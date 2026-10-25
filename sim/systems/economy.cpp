#include "systems/economy.hpp"

#include <algorithm>

#include "core/sim.hpp"
#include "systems/bases.hpp"

namespace corps {

namespace {

void add(ResourceAmounts& to, const ResourceAmounts& a) {
    to.money = to.money + a.money;
    to.oil = to.oil + a.oil;
    to.ammo = to.ammo + a.ammo;
    to.unitLimitProvided = to.unitLimitProvided + a.unitLimitProvided;
    to.unitLimitConsumed = to.unitLimitConsumed + a.unitLimitConsumed;
    to.buildingLimitProvided = to.buildingLimitProvided + a.buildingLimitProvided;
    to.buildingLimitConsumed = to.buildingLimitConsumed + a.buildingLimitConsumed;
}

// Only the limits of `a` (reservations of regiments not yet recruited).
void addLimits(ResourceAmounts& to, const ResourceAmounts& a) {
    to.unitLimitConsumed = to.unitLimitConsumed + a.unitLimitConsumed;
    to.buildingLimitConsumed = to.buildingLimitConsumed + a.buildingLimitConsumed;
}

std::vector<std::uint32_t> indexesOf(const DefSet& d, const std::vector<std::string>& ids) {
    std::vector<std::uint32_t> out;
    for (const std::string& s : ids) {
        const std::uint32_t i = d.propertyIndex(s);
        if (i != kNoDef) out.push_back(i);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const std::vector<std::string>* defProperties(const Sim& sim, EntityId actor) {
    const DefSet* d = sim.config().defs;
    const DefRef* r = d != nullptr ? sim.world().defRefs.get(actor) : nullptr;
    if (r == nullptr) return nullptr;
    if (r->kind == DefRef::Kind::Unit && r->index < d->units.size()) return &d->units[r->index].properties;
    if (r->kind == DefRef::Kind::Building && r->index < d->buildings.size()) return &d->buildings[r->index].properties;
    if (r->kind == DefRef::Kind::Regiment && r->index < d->regiments.size()) return &d->regiments[r->index].properties;
    return nullptr;
}

bool appliesTo(const DefSet& d, std::uint32_t property, const std::vector<std::uint32_t>& own) {
    for (const std::string& req : d.properties[property].requiredProperties) {
        const std::uint32_t r = d.propertyIndex(req);
        if (r == kNoDef || !std::binary_search(own.begin(), own.end(), r)) return false;
    }
    return true;
}

// Sum of `attribute` ADD modifiers of the kingdom research that applies to the actor.
Fixed researchAdd(const Sim& sim, EntityId actor, const char* attribute) {
    const std::uint8_t p = ownerOf(sim, actor);
    if (p >= kMaxPlayers || sim.world().kingdoms[p].properties.empty()) return {};
    const DefSet& d = *sim.config().defs;
    const std::vector<std::string>* props = defProperties(sim, actor);
    const std::vector<std::uint32_t> own = props != nullptr ? indexesOf(d, *props) : std::vector<std::uint32_t>{};
    Fixed sum;
    for (const std::uint32_t k : sim.world().kingdoms[p].properties) {
        if (k >= d.properties.size() || !appliesTo(d, k, own)) continue;
        for (const ModifierDef& m : d.properties[k].modifiers) {
            if (m.op == ModifierDef::Op::Add && m.attribute == attribute) sum = sum + m.amount;
        }
    }
    return sum;
}

}  // namespace

std::uint8_t ownerOf(const Sim& sim, EntityId actor) {
    const World& w = sim.world();
    if (const Structure* s = w.structures.get(actor)) return s->player;
    if (const Regiment* r = w.regiments.get(actor)) return r->player;
    if (const Member* m = w.members.get(actor)) {
        if (const Regiment* r = w.regiments.get(m->organization)) return r->player;
    }
    return 0xFF;
}

std::array<Ledger, kMaxPlayers> ledgers(const Sim& sim) {
    std::array<Ledger, kMaxPlayers> out{};
    const DefSet* d = sim.config().defs;
    const World& w = sim.world();
    if (d == nullptr) return out;
    for (std::size_t p = 0; p < kMaxPlayers; ++p) {
        if (w.kingdoms[p].started) add(out[p].production, d->economy.startLocation);
    }
    // Every def-backed entity, ascending.
    for (std::size_t i = 0; i < w.defRefs.size(); ++i) {
        const EntityId id = w.defRefs.ids()[i];
        const DefRef& r = w.defRefs.data()[i];
        if (r.kind == DefRef::Kind::Regiment) {
            const Regiment* g = w.regiments.get(id);
            if (g == nullptr || g->player >= kMaxPlayers || r.index >= d->regiments.size()) continue;
            add(out[g->player].upkeep, d->regiments[r.index].upkeep);
        } else if (r.kind == DefRef::Kind::Unit) {
            const Member* m = w.members.get(id);
            const Regiment* g = m != nullptr ? w.regiments.get(m->organization) : nullptr;
            if (g == nullptr || g->player >= kMaxPlayers || r.index >= d->units.size()) continue;
            const UnitDef& u = d->units[r.index];
            add(out[g->player].upkeep, u.upkeep);
            add(out[g->player].production, u.production);
        } else if (r.kind == DefRef::Kind::Building) {
            const Structure* s = w.structures.get(id);
            if (s == nullptr || s->player >= kMaxPlayers || r.index >= d->buildings.size()) continue;
            const BuildingDef& b = d->buildings[r.index];
            add(out[s->player].upkeep, b.upkeep);
            if (s->state == Structure::State::Deployed) add(out[s->player].production, b.production);
        }
    }
    // Reservations: regiments queued, and the units not yet out of the one being recruited.
    for (std::size_t i = 0; i < w.structures.size(); ++i) {
        const Structure& s = w.structures.data()[i];
        if (s.player >= kMaxPlayers) continue;
        for (std::size_t q = 0; q < s.queue.size(); ++q) {
            const bool started = q == 0 && !s.recruit.isNull();
            addLimits(out[s.player].upkeep, recruitLimits(*d, s.queue[q].regiment, started ? s.nextElement : 0, !started));
        }
    }
    for (std::size_t p = 0; p < kMaxPlayers; ++p) {
        Ledger& l = out[p];
        const EconomyDef& e = d->economy;
        l.shortfallCost = max(-l.oilNet(), Fixed{}) * e.oilPrice + max(-l.ammoNet(), Fixed{}) * e.ammoPrice;
        l.unitLimit = min(l.production.unitLimitProvided, e.limitedMax);
        l.buildingLimit = l.production.buildingLimitProvided;
    }
    return out;
}

Ledger ledgerOf(const Sim& sim, std::uint8_t player) {
    return player < kMaxPlayers ? ledgers(sim)[player] : Ledger{};
}

void runEconomy(Sim& sim) {
    const DefSet* d = sim.config().defs;
    if (d == nullptr) return;
    World& w = sim.world();
    bool any = false;
    for (const Kingdom& k : w.kingdoms) any = any || k.started;
    if (!any) return;
    const std::array<Ledger, kMaxPlayers> all = ledgers(sim);
    constexpr std::int64_t kTicksPerMinute = 60 * kTicksPerSecond;
    for (std::size_t p = 0; p < kMaxPlayers; ++p) {
        Kingdom& k = w.kingdoms[p];
        if (!k.started) continue;
        const Fixed net = all[p].moneyNet();
        k.money = std::clamp(k.money + Fixed::fromRaw(net.raw / kTicksPerMinute), Fixed{}, d->economy.stockpileMax);
        const bool bankrupt = k.money <= Fixed{} && net < Fixed{};
        if (bankrupt != k.bankrupt) {
            k.bankrupt = bankrupt;
            sim.emit({bankrupt ? SimEvent::Type::Bankrupt : SimEvent::Type::Solvent, EntityId{}, static_cast<std::uint8_t>(p),
                      CommandType::Noop});
        }
    }
}

std::vector<std::uint32_t> effectiveProperties(const Sim& sim, EntityId actor) {
    const DefSet* d = sim.config().defs;
    if (d == nullptr) return {};
    const std::vector<std::string>* props = defProperties(sim, actor);
    std::vector<std::uint32_t> own = props != nullptr ? indexesOf(*d, *props) : std::vector<std::uint32_t>{};
    if (const Structure* s = sim.world().structures.get(actor)) own.insert(own.end(), s->properties.begin(), s->properties.end());
    std::sort(own.begin(), own.end());
    own.erase(std::unique(own.begin(), own.end()), own.end());
    const std::uint8_t p = ownerOf(sim, actor);
    if (p < kMaxPlayers) {
        std::vector<std::uint32_t> out = own;
        for (const std::uint32_t k : sim.world().kingdoms[p].properties) {
            if (k < d->properties.size() && appliesTo(*d, k, own)) out.push_back(k);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
    return own;
}

Fixed researchAttackValue(const Sim& sim, EntityId actor) { return researchAdd(sim, actor, "AV"); }
Fixed researchDefenseValue(const Sim& sim, EntityId actor) { return researchAdd(sim, actor, "DV"); }

}  // namespace corps
