#include "ai/controller.hpp"

#include <algorithm>
#include <map>

#include "core/ascii.hpp"
#include "core/sim.hpp"
#include "nav/nav_world.hpp"
#include "systems/bases.hpp"
#include "systems/economy.hpp"
#include "systems/orders.hpp"
#include "systems/supply.hpp"

namespace corps::ai {

using namespace fixed_literals;

namespace {

constexpr std::uint32_t kThinkTicks = 10;      // once a second
constexpr std::uint32_t kReorderTicks = 150;   // re-send a busy regiment's order at most every 15 s

enum class Role : std::uint8_t { Home, City, Wave, Push };

constexpr std::size_t kPushGroup = 4;          // reinforcements join a push in groups of this many

struct Mine {
    EntityId id;
    FVec2 at;
    bool idle = false;             // no move order and no attack order
    bool orderable = true;         // not routing, retreating or exhausted (the sim refuses orders then)
    Engagement::Mode mode = Engagement::Mode::None;
    EntityId target;
};

const BuildingDef* buildingOf(const Sim& sim, EntityId id) {
    const DefSet& d = *sim.config().defs;
    const DefRef* r = sim.world().defRefs.get(id);
    return r != nullptr && r->kind == DefRef::Kind::Building && r->index < d.buildings.size() ? &d.buildings[r->index] : nullptr;
}

const RegimentDef* regimentOf(const Sim& sim, EntityId id) {
    const DefSet& d = *sim.config().defs;
    const DefRef* r = sim.world().defRefs.get(id);
    return r != nullptr && r->kind == DefRef::Kind::Regiment && r->index < d.regiments.size() ? &d.regiments[r->index] : nullptr;
}

// The building a regiment's trucks unpack into, or kNoDef.
std::uint32_t unpacksInto(const Sim& sim, EntityId org) {
    const World& w = sim.world();
    const DefSet& d = *sim.config().defs;
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        if (w.members.data()[i].organization != org) continue;
        const DefRef* r = w.defRefs.get(w.members.ids()[i]);
        if (r != nullptr && r->kind == DefRef::Kind::Unit && d.units[r->index].unpackTo != kNoDef) return d.units[r->index].unpackTo;
    }
    return kNoDef;
}

bool walkable(const nav::NavGrid& g, FVec2 p) {
    const auto x = static_cast<std::int32_t>(p.x.floorInt());
    const auto y = static_cast<std::int32_t>(p.y.floorInt());
    for (std::int32_t dy = -2; dy <= 2; ++dy) {
        for (std::int32_t dx = -2; dx <= 2; ++dx) {
            if (!g.passable(nav::MoveClass::Foot, x + dx, y + dy) || g.stampCount(g.index(x + dx, y + dy)) != 0) return false;
        }
    }
    return true;
}

// The walkable point nearest `p` on rings 2 m apart (p itself if none within 30 m).
FVec2 nearestWalkable(const Sim& sim, FVec2 p) {
    const nav::NavGrid& g = sim.nav()->grid;
    if (walkable(g, p)) return p;
    for (std::int32_t r = 2; r <= 30; r += 2) {
        for (std::uint32_t k = 0; k < 16; ++k) {
            const FVec2 c = p + FVec2{Fixed::fromInt(r), 0_fx}.rotated(Angle::fromUnits(k * 4096));
            if (walkable(g, c)) return c;
        }
    }
    return p;
}

FVec2 toward(FVec2 from, FVec2 to, Fixed by) {
    const Fixed len = distance(from, to);
    return len > Fixed{} ? from + (to - from) * (by / len) : from;
}

class RuleAi final : public Controller {
public:
    RuleAi(std::uint8_t player, std::string nation, Plan plan) : nation(std::move(nation)), plan(std::move(plan)) { me = player; }

    void think(const Sim& sim, std::vector<Command>& out) override {
        if (sim.config().defs == nullptr || sim.nav() == nullptr || me >= kMaxPlayers) return;
        if (sim.tick() < next) return;
        next = sim.tick() + kThinkTicks;
        const World& w = sim.world();
        const Kingdom& k = w.kingdoms[me];
        if (!k.started || k.eliminated || w.match.over) return;
        if (nextWave == 0 && plan.firstWaveTicks > 0) nextWave = sim.tick() + plan.firstWaveTicks;
        survey(sim);
        if (corps.isNull()) return;
        deployTrucks(sim, out);
        buildBase(sim, out);
        recruitArmy(sim, out);
        takeCities(sim, out);
        defend(sim, out);
        waves(sim, out);
        push(sim, out);
    }

private:
    std::string nation;
    Plan plan;
    std::uint32_t next = 0;
    std::uint32_t nextWave = 0;
    std::uint32_t waveNo = 0;
    std::size_t rosterNext = 0;
    bool pushing = false;
    EntityId pushTarget;
    std::vector<std::string> blockedNoted;     // roster entries whose blocker was noted
    std::map<EntityId, Role> roles;            // army regiments' jobs
    std::map<EntityId, EntityId> cityOf;       // city holder -> its city
    std::map<EntityId, std::uint32_t> ordered; // regiment -> tick of its last order
    std::map<EntityId, std::uint32_t> tries;   // city holder -> approaches tried
    // This think's survey.
    EntityId corps;
    FVec2 base;
    std::vector<EntityId> divisionHqs;         // deployed
    std::vector<std::string> owned;            // building ids owned or on their way (trucks, queue)
    std::vector<Mine> army;                    // fighting regiments, ascending
    std::vector<Mine> trucks;                  // truck regiments, ascending
    std::vector<std::pair<EntityId, FVec2>> enemyHqs;  // HQ structures, then HQ regiments
    std::vector<FVec2> enemyUnits;             // anchors of hostile regiments
    FVec2 enemyBase;
    bool haveEnemyBase = false;

    void note(const Sim& sim, std::string s) {
        notes.push_back(std::to_string(sim.tick() / 600) + ":" + (sim.tick() / 10 % 60 < 10 ? "0" : "") +
                        std::to_string(sim.tick() / 10 % 60) + " p" + std::to_string(me) + " " + std::move(s));
    }

    void survey(const Sim& sim) {
        const World& w = sim.world();
        const DefSet& d = *sim.config().defs;
        corps = EntityId{};
        divisionHqs.clear();
        owned.clear();
        army.clear();
        trucks.clear();
        enemyHqs.clear();
        enemyUnits.clear();
        haveEnemyBase = false;
        std::vector<EntityId> forming;
        for (std::size_t i = 0; i < w.structures.size(); ++i) {
            const EntityId id = w.structures.ids()[i];
            const Structure& s = w.structures.data()[i];
            const BuildingDef* b = buildingOf(sim, id);
            const Transform* t = w.transforms.get(id);
            if (b == nullptr || t == nullptr || !w.registry.alive(id)) continue;
            if (s.player == me) {
                owned.push_back(b->ids);
                for (const RecruitOrder& q : s.queue) {
                    for (const RecruitElement& e : recruitPlan(d, q.regiment)) {
                        if (e.element.kind == ElementKind::Unit && d.units[e.element.index].unpackTo != kNoDef) {
                            owned.push_back(d.buildings[d.units[e.element.index].unpackTo].ids);
                        }
                    }
                }
                if (!s.recruit.isNull()) forming.push_back(s.recruit);
                if (b->providesCorps) {
                    corps = id;
                    base = t->position;
                } else if (b->hq && s.state == Structure::State::Deployed) {
                    divisionHqs.push_back(id);
                }
            } else if (s.player < kMaxPlayers && w.hostile(s.player, me)) {
                if (b->hq) enemyHqs.push_back({id, t->position});
                if (b->providesCorps || !haveEnemyBase) {
                    enemyBase = t->position;
                    haveEnemyBase = true;
                }
            }
        }
        std::sort(forming.begin(), forming.end());
        for (std::size_t i = 0; i < w.regiments.size(); ++i) {
            const EntityId id = w.regiments.ids()[i];
            const Regiment& r = w.regiments.data()[i];
            const Transform* t = w.transforms.get(id);
            const RegimentDef* rd = regimentOf(sim, id);
            if (t == nullptr || rd == nullptr || !w.registry.alive(id)) continue;
            if (r.player != me) {
                if (r.player < kMaxPlayers && w.hostile(r.player, me)) {
                    enemyUnits.push_back(t->position);
                    if (rd->hq) enemyHqs.push_back({id, t->position});
                }
                continue;
            }
            if (std::binary_search(forming.begin(), forming.end(), id)) continue;
            const Order* o = w.orders.get(orderOwner(sim, id));
            const Engagement* e = w.engagements.get(id);
            Mine m{id, t->position};
            m.mode = e != nullptr ? e->mode : Engagement::Mode::None;
            m.target = e != nullptr ? e->target : EntityId{};
            m.orderable = e == nullptr || (e->state != Engagement::State::Routing && e->state != Engagement::State::Retreating &&
                                           e->state != Engagement::State::Exhausted);
            m.idle = (o == nullptr || o->state == Order::State::Idle) && m.mode == Engagement::Mode::None && m.orderable;
            const std::uint32_t into = unpacksInto(sim, id);
            if (into != kNoDef) {
                owned.push_back(d.buildings[into].ids);
                trucks.push_back(m);
            } else if (!rd->hq) {
                army.push_back(m);
                if (!roles.count(id)) roles[id] = Role::Home;
            }
        }
        // Forget the dead.
        for (auto it = roles.begin(); it != roles.end();) {
            it = w.registry.alive(it->first) ? std::next(it) : roles.erase(it);
        }
        for (auto it = cityOf.begin(); it != cityOf.end();) {
            it = w.registry.alive(it->first) && roles.count(it->first) && roles[it->first] == Role::City ? std::next(it) : cityOf.erase(it);
        }
    }

    bool owns(const std::string& ids) const {
        return std::any_of(owned.begin(), owned.end(), [&](const std::string& o) { return iequals(o, ids); });
    }

    void order(const Sim& sim, std::vector<Command>& out, Command c, const std::vector<EntityId>& regs) {
        out.push_back(std::move(c));
        for (const EntityId r : regs) ordered[r] = sim.tick();
    }

    bool recentlyOrdered(const Sim& sim, EntityId r) const {
        const auto it = ordered.find(r);
        return it != ordered.end() && sim.tick() < it->second + kReorderTicks;
    }

    // Up to 6 regiments per command.
    template <class Make>
    void inGroups(const Sim& sim, std::vector<Command>& out, const std::vector<EntityId>& regs, Make make) {
        for (std::size_t s = 0; s < regs.size(); s += kMaxCommandRegiments) {
            const std::vector<EntityId> g(regs.begin() + static_cast<std::ptrdiff_t>(s),
                                          regs.begin() + static_cast<std::ptrdiff_t>(std::min(regs.size(), s + kMaxCommandRegiments)));
            order(sim, out, make(g), g);
        }
    }

    // No building stamp within kRoom cells of the rect: the placement rule's 2 m gap can wall
    // regiments in between buildings (and put form-up points inside them).
    static bool roomy(const Sim& sim, const Footprint& r) {
        const nav::NavGrid& g = sim.nav()->grid;
        constexpr std::int32_t kRoom = 6;
        for (std::int32_t y = std::max(0, r.y0 - kRoom); y <= std::min(g.height() - 1, r.y1 + kRoom); ++y) {
            for (std::int32_t x = std::max(0, r.x0 - kRoom); x <= std::min(g.width() - 1, r.x1 + kRoom); ++x) {
                if (g.stampCount(g.index(x, y)) != 0) return false;
            }
        }
        return true;
    }

    // Trucks out of the HQ unpack on the first roomy spot round the Corps HQ (rings of 6 m from
    // 18 m, starting towards the enemy).
    void deployTrucks(const Sim& sim, std::vector<Command>& out) {
        const World& w = sim.world();
        for (const Mine& t : trucks) {
            if (w.deploys.has(t.id) || !t.idle) continue;
            const std::uint32_t building = unpacksInto(sim, t.id);
            const Angle start = haveEnemyBase ? (enemyBase - base).angle() : Angle{};
            bool placed = false;
            for (std::int32_t r = 18; r <= 60 && !placed; r += 6) {
                for (std::uint32_t k = 0; k < 16 && !placed; ++k) {
                    const FVec2 at = base + FVec2{Fixed::fromInt(r), 0_fx}.rotated(start + Angle::fromUnits(k * 4096));
                    const Placement pl = placeBuilding(sim, building, at, Angle{}, me, t.id);
                    if (!pl.ok || !roomy(sim, pl.rect)) continue;
                    order(sim, out, makeUnpack(0, me, 0, t.id, at, Angle{}), {t.id});
                    note(sim, "unpack " + sim.config().defs->buildings[building].ids);
                    placed = true;
                }
            }
        }
    }

    // The Corps HQ deploys the plan's division HQs in order, then depots while oil or ammo is short.
    void buildBase(const Sim& sim, std::vector<Command>& out) {
        const Structure& s = *sim.world().structures.get(corps);
        if (s.state != Structure::State::Deployed || !s.queue.empty()) return;
        const DefSet& d = *sim.config().defs;
        const BuildingDef& b = *buildingOf(sim, corps);
        const auto deploy = [&](const std::string& building) {
            for (const RecruitRef& ref : b.recruitRefs) {
                bool match = false;
                for (const RecruitElement& e : recruitPlan(d, ref.regiment)) {
                    const std::uint32_t into = e.element.kind == ElementKind::Unit ? d.units[e.element.index].unpackTo : kNoDef;
                    match = match || (into != kNoDef && iequals(d.buildings[into].ids, building));
                }
                if (!match) continue;
                const Blocker why = recruitBlocker(sim, me, corps, ref.ids);
                if (!why.ok()) {  // wait for it (money); say so if it's something else
                    if (why.code != "money" && std::find(blockedNoted.begin(), blockedNoted.end(), ref.ids) == blockedNoted.end()) {
                        blockedNoted.push_back(ref.ids);
                        note(sim, "can't deploy " + building + ": " + why.code + (why.detail.empty() ? "" : " " + why.detail));
                    }
                    return true;
                }
                out.push_back(makeRecruit(0, me, 0, corps, ref.ids));
                note(sim, "deploy " + building);
                return true;
            }
            return false;
        };
        for (const std::string& wanted : plan.buildings) {
            if (!owns(wanted) && deploy(wanted)) return;
        }
        if (!plan.depots) return;
        const Ledger l = ledgerOf(sim, me);
        if (l.oilNet() < Fixed{} && !owns(nation + "_depot_oil") && deploy(nation + "_depot_oil")) return;
        if (l.ammoNet() < Fixed{} && !owns(nation + "_depot_ammo")) deploy(nation + "_depot_ammo");
    }

    // Oil or ammo is short and its depot isn't built or on its way.
    bool depotWanted(const Sim& sim) const {
        if (!plan.depots) return false;
        const Ledger l = ledgerOf(sim, me);
        return (l.oilNet() < Fixed{} && !owns(nation + "_depot_oil")) || (l.ammoNet() < Fixed{} && !owns(nation + "_depot_ammo"));
    }

    // The roster is recruited strictly in turn, one regiment queued at a time per division HQ, up to
    // the cap: the next entry waits for money or unit limit rather than letting a cheaper one go
    // first (tanks would never come); an entry nothing can recruit (a missing requirement, no HQ
    // offering it yet) is skipped. While a planned building is still missing, at most two
    // regiments: the building's truck needs money and a unit-limit slot of its own.
    void recruitArmy(const Sim& sim, std::vector<Command>& out) {
        if (plan.roster.empty() || divisionHqs.empty()) return;
        const World& w = sim.world();
        std::size_t queued = 0;
        for (const EntityId hq : divisionHqs) queued += w.structures.get(hq)->queue.size();
        const bool buildingMissing = std::any_of(plan.buildings.begin(), plan.buildings.end(), [&](const std::string& h) { return !owns(h); });
        const std::size_t cap = buildingMissing ? std::min<std::size_t>(plan.armyCap, 2) : plan.armyCap;
        if (army.size() + queued >= cap) return;
        if (!buildingMissing && depotWanted(sim) && army.size() + queued >= 2) return;  // save for the depot
        for (std::size_t k = 0; k < plan.roster.size(); ++k) {
            const std::string& ids = plan.roster[rosterNext];
            EntityId at;
            Blocker why{"unknown", ""};
            for (const EntityId hq : divisionHqs) {
                const BuildingDef& b = *buildingOf(sim, hq);
                if (!std::any_of(b.recruitRefs.begin(), b.recruitRefs.end(), [&](const RecruitRef& r) { return iequals(r.ids, ids); })) continue;
                why = recruitBlocker(sim, me, hq, ids);
                at = hq;
                if (why.ok() || why.code == "money" || why.code == "unit_limit" || why.code == "queue_full") break;
            }
            if (why.ok() && w.structures.get(at)->queue.empty()) {
                out.push_back(makeRecruit(0, me, 0, at, ids));
                note(sim, "recruit " + ids);
                rosterNext = (rosterNext + 1) % plan.roster.size();
                return;
            }
            if (why.ok() || why.code == "money" || why.code == "unit_limit" || why.code == "queue_full") return;  // its turn: wait
            if (!at.isNull() && std::find(blockedNoted.begin(), blockedNoted.end(), ids) == blockedNoted.end()) {
                blockedNoted.push_back(ids);
                note(sim, "can't recruit " + ids + ": " + why.code + (why.detail.empty() ? "" : " " + why.detail));
            }
            rosterNext = (rosterNext + 1) % plan.roster.size();  // skip it
        }
    }

    // One home regiment per city the team doesn't hold; it stands by the city centre.
    void takeCities(const Sim& sim, std::vector<Command>& out) {
        if (!plan.takeCities) return;
        const World& w = sim.world();
        const DefSet& d = *sim.config().defs;
        for (std::size_t i = 0; i < w.structures.size(); ++i) {
            const EntityId city = w.structures.ids()[i];
            const DefRef* r = w.defRefs.get(city);
            if (r == nullptr || r->kind != DefRef::Kind::Building || !isCity(d, r->index)) continue;
            const FVec2 at = w.transforms.get(city)->position;
            const bool ours = w.teamOf(w.structures.data()[i].player) == w.teamOf(me);
            EntityId holder;
            for (const auto& [reg, c] : cityOf) {
                if (c == city) holder = reg;
            }
            if (ours && holder.isNull()) continue;
            if (holder.isNull()) {  // the nearest home regiment
                const Mine* best = nullptr;
                for (const Mine& m : army) {
                    if (roles[m.id] == Role::Home && m.idle && (best == nullptr || distance(m.at, at) < distance(best->at, at))) best = &m;
                }
                if (best == nullptr) continue;
                holder = best->id;
                roles[holder] = Role::City;
                cityOf[holder] = city;
                note(sim, "take the city at " + std::to_string(at.x.roundInt()) + "," + std::to_string(at.y.roundInt()));
            }
            const auto it = std::find_if(army.begin(), army.end(), [&](const Mine& m) { return m.id == holder; });
            if (it == army.end() || !it->idle || distance(it->at, at) <= 9_fx || recentlyOrdered(sim, holder)) continue;
            // The capture needs a unit within half the footprint + 4 m of the centre: stand 8 m out,
            // on the walkable point nearest the regiment, the next one on each retry.
            std::vector<std::pair<Fixed, FVec2>> spots;
            const nav::NavGrid& g = sim.nav()->grid;
            for (std::uint32_t k = 0; k < 16; ++k) {
                const FVec2 p = at + FVec2{8_fx, 0_fx}.rotated(Angle::fromUnits(k * 4096));
                const auto x = static_cast<std::int32_t>(p.x.floorInt()), y = static_cast<std::int32_t>(p.y.floorInt());
                if (g.passable(nav::MoveClass::Foot, x, y) && g.stampCount(g.index(x, y)) == 0) spots.push_back({distance(p, it->at), p});
            }
            if (spots.empty()) continue;
            std::stable_sort(spots.begin(), spots.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
            const std::uint32_t attempt = tries[holder]++;
            order(sim, out, makeMoveGroup(0, me, 0, {holder}, spots[attempt % spots.size()].second), {holder});
        }
    }

    // Home regiments attack-move onto enemies near any own building.
    void defend(const Sim& sim, std::vector<Command>& out) {
        const World& w = sim.world();
        const Fixed radius = Fixed::fromInt(plan.defendRadius);
        bool found = false;
        FVec2 threat;
        for (std::size_t i = 0; i < w.structures.size() && !found; ++i) {
            if (w.structures.data()[i].player != me) continue;
            const FVec2 at = w.transforms.get(w.structures.ids()[i])->position;
            for (const FVec2& e : enemyUnits) {
                if (distance(e, at) <= radius) {
                    threat = e;
                    found = true;
                    break;
                }
            }
        }
        if (!found) return;
        std::vector<EntityId> regs;
        for (const Mine& m : army) {
            if (roles[m.id] == Role::Home && m.idle) regs.push_back(m.id);
        }
        if (regs.empty()) return;
        note(sim, "defend at " + std::to_string(threat.x.roundInt()) + "," + std::to_string(threat.y.roundInt()) + " with " +
                      std::to_string(regs.size()));
        inGroups(sim, out, regs, [&](const std::vector<EntityId>& g) { return makeAttackMove(0, me, 0, g, threat); });
    }

    // Every few minutes all but the home guard go at the enemy Corps HQ; idle wave regiments go again.
    void waves(const Sim& sim, std::vector<Command>& out) {
        if (plan.firstWaveTicks == 0 || !haveEnemyBase) return;
        if (sim.tick() >= nextWave) {
            nextWave = sim.tick() + plan.waveTicks;
            std::vector<EntityId> home;
            for (const Mine& m : army) {
                if (roles[m.id] == Role::Home) home.push_back(m.id);
            }
            if (home.size() > plan.keepHome) {
                const std::vector<EntityId> wave(home.begin() + plan.keepHome, home.end());
                for (const EntityId r : wave) roles[r] = Role::Wave;
                ++waveNo;
                note(sim, "wave " + std::to_string(waveNo) + ": " + std::to_string(wave.size()) + " regiments");
            }
        }
        std::vector<EntityId> go;
        for (const Mine& m : army) {
            if (roles[m.id] == Role::Wave && m.idle && m.orderable && !recentlyOrdered(sim, m.id)) go.push_back(m.id);
        }
        if (go.empty()) return;
        const FVec2 goal = nearestWalkable(sim, toward(enemyBase, base, 14_fx));
        inGroups(sim, out, go, [&](const std::vector<EntityId>& g) { return makeAttackMove(0, me, 0, g, goal); });
    }

    // Once the army is big enough: every regiment but the city holders attacks the nearest enemy HQ.
    void push(const Sim& sim, std::vector<Command>& out) {
        if (plan.pushAt == 0 || enemyHqs.empty()) return;
        std::size_t fighters = 0;
        for (const Mine& m : army) fighters += roles[m.id] != Role::City;
        if (!pushing && fighters >= plan.pushAt) {
            pushing = true;
            for (const Mine& m : army) {
                if (roles[m.id] != Role::City) roles[m.id] = Role::Push;
            }
            note(sim, "push with " + std::to_string(fighters) + " regiments");
        }
        if (!pushing) return;
        std::vector<EntityId> fresh;  // reinforcements at home join in groups
        for (const Mine& m : army) {
            if (roles[m.id] == Role::Home && m.idle) fresh.push_back(m.id);
        }
        if (fresh.size() >= kPushGroup) {
            for (const EntityId r : fresh) roles[r] = Role::Push;
            note(sim, "reinforce the push with " + std::to_string(fresh.size()));
        }
        // The target: the enemy Corps HQ first (it deploys every other HQ), then the HQ nearest our
        // base (buildings, then packed HQs).
        const auto rank = [&](const std::pair<EntityId, FVec2>& h) {
            const BuildingDef* b = buildingOf(sim, h.first);
            return b != nullptr && b->providesCorps ? 0 : (b != nullptr ? 1 : 2);
        };
        // Sticky: the push keeps its target until it is gone (moving HQ trucks would otherwise
        // change places every second).
        auto nearest = std::find_if(enemyHqs.begin(), enemyHqs.end(), [&](const auto& h) { return h.first == pushTarget; });
        if (nearest == enemyHqs.end()) {
            nearest = std::min_element(enemyHqs.begin(), enemyHqs.end(), [&](const auto& a, const auto& b) {
                if (rank(a) != rank(b)) return rank(a) < rank(b);
                const Fixed da = distance(a.second, base), db = distance(b.second, base);
                return da != db ? da < db : a.first < b.first;
            });
            pushTarget = nearest->first;
        }
        const EntityId target = nearest->first;
        const FVec2 targetAt = nearest->second;
        // Far away: attack-move to a walkable point near it (fighting what is on the way); close:
        // attack it. (An Attack order on a building from across the map can stall on the way.)
        const FVec2 approach = nearestWalkable(sim, toward(targetAt, base, 14_fx));
        std::vector<EntityId> close, far;
        for (const Mine& m : army) {
            if (roles[m.id] != Role::Push || !m.orderable) continue;
            if (distance(m.at, targetAt) <= 30_fx) {
                const bool onIt = m.mode == Engagement::Mode::Attack && m.target == target;
                if (!onIt && (m.idle || !recentlyOrdered(sim, m.id))) close.push_back(m.id);
            } else {
                const bool onIt = m.mode == Engagement::Mode::AttackMove && !m.idle;
                if (!onIt && (m.idle || !recentlyOrdered(sim, m.id))) far.push_back(m.id);
            }
        }
        const std::string name = buildingOf(sim, target) != nullptr ? buildingOf(sim, target)->ids : "an HQ convoy";
        if (!far.empty()) {
            note(sim, "advance on " + name + " with " + std::to_string(far.size()));
            inGroups(sim, out, far, [&](const std::vector<EntityId>& g) { return makeAttackMove(0, me, 0, g, approach); });
        }
        if (!close.empty()) {
            note(sim, "attack " + name + " with " + std::to_string(close.size()));
            inGroups(sim, out, close, [&](const std::vector<EntityId>& g) { return makeAttack(0, me, 0, g, target); });
        }
    }
};

}  // namespace

Plan dummyPlan(const std::string& nation) {
    Plan p;
    p.buildings = {nation + "_hq_infantry", nation + "_motor_pool"};
    p.roster = {"regiment_" + nation + "_infantry", "regiment_" + nation + "_infantry_at"};
    p.armyCap = 5;
    p.firstWaveTicks = 6 * 600;  // 6 minutes
    p.waveTicks = 4 * 600;       // then every 4
    p.keepHome = 2;
    return p;
}

Plan slicePlan(const std::string& nation) {
    Plan p;
    // The data's chain: AT infantry needs the motor pool; the Armour HQ needs the Mechanized HQ.
    p.buildings = {nation + "_hq_infantry", nation + "_motor_pool", nation + "_hq_mechanized", nation + "_hq_armor"};
    p.roster = {"regiment_" + nation + "_infantry", "regiment_" + nation + "_tank_light", "regiment_" + nation + "_infantry_at",
                "regiment_" + nation + "_tank_medium"};
    p.armyCap = 12;
    p.takeCities = true;
    p.pushAt = 8;
    return p;
}

std::unique_ptr<Controller> makeController(std::uint8_t player, const std::string& nation, const Plan& plan) {
    return std::make_unique<RuleAi>(player, asciiLowerCopy(nation), plan);
}

std::unique_ptr<Controller> makeDummyAi(std::uint8_t player, const std::string& nation) {
    return makeController(player, nation, dummyPlan(asciiLowerCopy(nation)));
}

std::unique_ptr<Controller> makeSlicePlayer(std::uint8_t player, const std::string& nation) {
    return makeController(player, nation, slicePlan(asciiLowerCopy(nation)));
}

}  // namespace corps::ai
