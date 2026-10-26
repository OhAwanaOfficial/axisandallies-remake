#include "core/sim.hpp"

#include <algorithm>

#include "combat/rules.hpp"
#include "nav/nav_world.hpp"
#include "systems/bases.hpp"
#include "systems/combat.hpp"
#include "systems/economy.hpp"
#include "systems/morale.hpp"
#include "systems/movement.hpp"
#include "systems/orders.hpp"
#include "systems/stages.hpp"
#include "systems/supply.hpp"

namespace corps {

using namespace fixed_literals;

namespace {

constexpr const char* kStageNames[kStageCount] = {
    "ApplyCommands", "Economy", "Timers", "Orders", "Pathfinding", "Movement", "Vision",
    "Engagement", "Abilities", "Morale", "Supply", "Capture", "Cleanup",
};

// Placeholder unit placement inside a regiment until formations land (M4, PATHFINDING): rows of
// three, 1.5 m apart, behind the regiment anchor in its own frame (+x = facing).
FVec2 slotOffset(std::size_t slot) {
    const auto row = static_cast<std::int32_t>(slot / 3);
    const auto col = static_cast<std::int32_t>(slot % 3) - 1;
    return {-1.5_fx * row, 1.5_fx * col};
}

}  // namespace

const char* stageName(Stage s) {
    const auto i = static_cast<std::size_t>(s);
    return i < kStageCount ? kStageNames[i] : "?";
}

Sim::Sim(const SimConfig& config)
    : config_(config), rng_(Rng::derive(config.matchSeed, static_cast<std::uint64_t>(RngStream::Match))) {
    if (config_.map != nullptr) nav_ = std::make_unique<nav::NavWorld>(*config_.map);
    moveCache_ = std::make_unique<MovementCache>();
    if (config_.tuning != nullptr) tuning_ = *config_.tuning;
    config_.tuning = nullptr;  // the Sim keeps its own copy; tuning() is the one to read
    combatRules_ = std::make_unique<CombatRules>(config_.defs, config_.map, tuning_);
    if (config_.defs != nullptr) {
        // Victory rules: the map's (RULES) over the SVars; SetMatchRules may replace them at tick 0.
        const EconomyDef& e = config_.defs->economy;
        world_.match.cityVictoryRatio = config_.map != nullptr ? config_.map->rule("CityVictoryRatio", e.cityVictoryRatio) : e.cityVictoryRatio;
        world_.match.teamVictory = e.teamVictory;
    }
    spawnMapCities(*this);    // part of map load: the map's cities, before the clearance is derived
    refreshClearance(*this);  // part of map load, not of the first tick
}

Sim::~Sim() = default;
Sim::Sim(Sim&&) noexcept = default;
Sim& Sim::operator=(Sim&&) noexcept = default;

std::uint32_t Sim::cellAt(FVec2 p) const {
    const nav::NavGrid& g = nav_->grid;
    const auto clampAxis = [](Fixed v, std::int32_t n) {
        return static_cast<std::int32_t>(std::clamp<std::int64_t>(v.floorInt(), 0, n - 1));
    };
    return g.index(clampAxis(p.x, g.width()), clampAxis(p.y, g.height()));
}

void Sim::step(const std::vector<Command>& commands) {
    events_.clear();
    const auto after = [this](Stage s) {
        if (afterStage) afterStage(*this, s);
    };
    applyCommands(commands);
    after(Stage::ApplyCommands);
    runEconomy(*this);
    after(Stage::Economy);
    runBases(*this);
    after(Stage::Timers);
    runOrders(*this);
    after(Stage::Orders);
    runPathfinding(*this);
    after(Stage::Pathfinding);
    runMovement(*this);
    after(Stage::Movement);
    runVision(*this);
    after(Stage::Vision);
    runEngagement(*this);
    after(Stage::Engagement);
    runAbilities(*this);
    after(Stage::Abilities);
    runMorale(*this);
    after(Stage::Morale);
    runSupply(*this);
    after(Stage::Supply);
    runCapture(*this);
    after(Stage::Capture);
    cleanup();
    ++tick_;  // the tick is complete before the Cleanup hook (hashes describe the finished tick)
    after(Stage::Cleanup);
}

void Sim::applyCommands(const std::vector<Command>& commands) {
    // Canonical order: (player, seq), ties by position in the bundle.
    std::vector<std::size_t> order(commands.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const Command& x = commands[a];
        const Command& y = commands[b];
        return x.player != y.player ? x.player < y.player : x.seq < y.seq;
    });
    for (const std::size_t i : order) {
        const Command& c = commands[i];
        if (c.tick != tick_) {
            events_.push_back({SimEvent::Type::CommandRejected, EntityId{}, c.player, c.type});
            continue;
        }
        apply(c);
    }
}

void Sim::apply(const Command& c) {
    const auto reject = [&] { events_.push_back({SimEvent::Type::CommandRejected, EntityId{}, c.player, c.type}); };
    switch (c.type) {
    case CommandType::Noop:
        return;
    case CommandType::SpawnRegiment:
        spawnRegiment(c, std::get<SpawnRegimentPayload>(c.payload));
        return;
    case CommandType::DebugSpawn: {
        const auto& p = std::get<DebugSpawnPayload>(c.payload);
        const EntityId id = world_.registry.create();
        if (id.isNull()) return reject();
        world_.transforms.set(id, {p.position, p.facing});
        events_.push_back({SimEvent::Type::Spawned, id, c.player, c.type});
        return;
    }
    case CommandType::DebugDestroy:
        if (!world_.registry.destroyLater(std::get<DebugDestroyPayload>(c.payload).target)) reject();
        return;
    case CommandType::DebugSetTransform: {
        const auto& p = std::get<DebugSetTransformPayload>(c.payload);
        Transform* t = world_.registry.alive(p.target) ? world_.transforms.get(p.target) : nullptr;
        if (t == nullptr) return reject();
        *t = {p.position, p.facing};
        return;
    }
    case CommandType::DebugRngDraw: {
        const std::uint16_t n = std::get<DebugRngDrawPayload>(c.payload).count;
        for (std::uint16_t i = 0; i < n; ++i) rng_.nextU32();
        return;
    }
    case CommandType::DebugPlaceBlocker:
        placeBlocker(c, std::get<DebugPlaceBlockerPayload>(c.payload));
        return;
    case CommandType::DebugRequestPath:
        requestPath(c, std::get<DebugRequestPathPayload>(c.payload));
        return;
    case CommandType::MoveGroup:
    case CommandType::Run:
        applyMoveGroup(*this, c, std::get<MoveGroupPayload>(c.payload), c.type == CommandType::Run);
        return;
    case CommandType::Stop:
        applyStop(*this, c, std::get<RegimentListPayload>(c.payload));
        return;
    case CommandType::SetFormation:
        applySetFormation(*this, c, std::get<SetFormationPayload>(c.payload));
        return;
    case CommandType::SetGroupBehaviour:
        applySetGroupBehaviour(*this, c, std::get<SetGroupBehaviourPayload>(c.payload));
        return;
    case CommandType::Attack:
        applyAttack(*this, c, std::get<AttackPayload>(c.payload));
        return;
    case CommandType::AttackMove:
        applyAttackMove(*this, c, std::get<MoveGroupPayload>(c.payload));
        return;
    case CommandType::DebugSpawnBuilding:
        spawnBuilding(c, std::get<DebugSpawnBuildingPayload>(c.payload));
        return;
    case CommandType::DebugSetEntrenched:
        applySetEntrenched(*this, c, std::get<DebugSetEntrenchedPayload>(c.payload));
        return;
    case CommandType::Retreat:
        applyRetreat(*this, c, std::get<MoveGroupPayload>(c.payload));
        return;
    case CommandType::DebugSetExperience:
        applySetExperience(*this, c, std::get<DebugSetExperiencePayload>(c.payload));
        return;
    case CommandType::StartPlayer:
        applyStartPlayer(*this, c, std::get<BaseOrderPayload>(c.payload));
        return;
    case CommandType::Recruit:
        applyRecruit(*this, c, std::get<BaseOrderPayload>(c.payload));
        return;
    case CommandType::CancelRecruit:
        applyCancelRecruit(*this, c, std::get<BaseOrderPayload>(c.payload));
        return;
    case CommandType::Unpack:
        applyUnpack(*this, c, std::get<BaseOrderPayload>(c.payload));
        return;
    case CommandType::Pack:
        applyPack(*this, c, std::get<BaseOrderPayload>(c.payload));
        return;
    case CommandType::Research:
        applyResearch(*this, c, std::get<BaseOrderPayload>(c.payload));
        return;
    case CommandType::CancelResearch:
        applyCancelResearch(*this, c, std::get<BaseOrderPayload>(c.payload));
        return;
    case CommandType::DebugSetMoney:
        applyDebugSetMoney(*this, c, std::get<BaseOrderPayload>(c.payload));
        return;
    case CommandType::DebugGrantProperty:
        applyDebugGrantProperty(*this, c, std::get<BaseOrderPayload>(c.payload));
        return;
    case CommandType::SetMatchRules:
        applySetMatchRules(*this, c, std::get<MatchRulesPayload>(c.payload));
        return;
    case CommandType::DebugSetHealth:
        applyDebugSetHealth(*this, c, std::get<BaseOrderPayload>(c.payload));
        return;
    case CommandType::DebugSetOwner:
        applyDebugSetOwner(*this, c, std::get<BaseOrderPayload>(c.payload));
        return;
    }
    reject();
}

void Sim::spawnRegiment(const Command& c, const SpawnRegimentPayload& p) {
    const std::uint32_t regIndex = config_.defs != nullptr ? config_.defs->regimentIndex(p.regimentIds) : kNoDef;
    if (regIndex == kNoDef || spawnRegimentAt(c.player, regIndex, p.position, p.facing, c.type).isNull()) reject(c);
}

EntityId Sim::spawnRegimentAt(std::uint8_t player, std::uint32_t regIndex, FVec2 position, Angle facing, CommandType cause) {
    const DefSet& defs = *config_.defs;
    const RegimentDef& reg = defs.regiments[regIndex];

    const EntityId org = world_.registry.create();
    if (org.isNull()) return org;
    world_.transforms.set(org, {position, facing});
    world_.defRefs.set(org, {DefRef::Kind::Regiment, regIndex});
    Regiment regiment;
    regiment.layout = reg.layoutIndex;
    regiment.player = player;
    const LayoutDef* layout = reg.layoutIndex != kNoDef ? &defs.layouts[reg.layoutIndex] : nullptr;
    if (layout != nullptr) {
        const std::uint32_t f = layout->formation(layout->defaultFormationIds);
        regiment.formation = static_cast<std::uint16_t>(f != kNoDef ? f : 0);
    }
    events_.push_back({SimEvent::Type::Spawned, org, player, cause});

    // Regiment slot s fills layout slot s, which holds `number` elements (PER infantry: 9 men).
    // Member::slot is the layout element; slots past the layout use the legacy grid placement.
    bool anyClass = false;
    const auto addUnit = [&](const ElementRef& el, std::uint32_t element, FVec2 offset) {
        const EntityId e = world_.registry.create();
        if (e.isNull()) return false;
        world_.transforms.set(e, {position + offset.rotated(facing), facing});
        if (el.kind == ElementKind::Unit) {
            const UnitDef& u = defs.units[el.index];
            world_.defRefs.set(e, {DefRef::Kind::Unit, el.index});
            world_.health.set(e, {u.health, u.health});
            world_.movers.set(e, {});
            const auto cls = static_cast<std::uint8_t>(u.moveClass);
            regiment.moveClass = anyClass ? restrictiveClass(regiment.moveClass, cls) : cls;
            anyClass = true;
        } else {
            const Fixed hp = defs.buildings[el.index].health;
            world_.defRefs.set(e, {DefRef::Kind::Building, el.index});
            world_.health.set(e, {hp, hp});
        }
        world_.members.set(e, {org, static_cast<std::uint16_t>(element)});
        attachCombat(*this, e, player);
        events_.push_back({SimEvent::Type::Spawned, e, player, cause});
        return true;
    };
    const std::size_t layoutElements = layout != nullptr ? layout->elementSlot.size() : 0;
    std::uint32_t extra = 0;
    for (std::size_t s = 0; s < reg.slots.size(); ++s) {
        const ElementRef& el = reg.slots[s];
        if (el.kind == ElementKind::None) continue;  // empty slot, or an element the data doesn't define
        if (layout != nullptr && s < layout->slots.size()) {
            for (std::uint32_t e = 0; e < layoutElements; ++e) {
                if (layout->elementSlot[e] == s && !addUnit(el, e, layoutOffset(defs, regiment, regiment.formation, e))) break;
            }
        } else if (!addUnit(el, static_cast<std::uint32_t>(layoutElements) + extra, slotOffset(s))) {
            break;
        } else {
            ++extra;
        }
    }
    world_.regiments.set(org, regiment);
    Order idle;
    idle.facing = facing;
    idle.goal = position;
    world_.orders.set(org, idle);
    attachEngagement(*this, org);
    initRegimentMorale(*this, org);
    return org;
}

// A building on its own (duels, tests; base building arrives in M6): it blocks the cells under
// its blocking radius and defends itself with its DefenderComponent weapons.
void Sim::spawnBuilding(const Command& c, const DebugSpawnBuildingPayload& p) {
    const auto reject = [&] { events_.push_back({SimEvent::Type::CommandRejected, EntityId{}, c.player, c.type}); };
    if (config_.defs == nullptr || !nav_) return reject();
    const std::uint32_t bi = config_.defs->buildingIndex(p.buildingIds);
    if (bi == kNoDef) return reject();
    const BuildingDef& b = config_.defs->buildings[bi];
    const nav::NavGrid& g = nav_->grid;
    const Fixed r = max(b.blockingRadius, 0.5_fx);
    const std::int64_t x0 = std::max<std::int64_t>((p.position.x - r).floorInt(), 0);
    const std::int64_t y0 = std::max<std::int64_t>((p.position.y - r).floorInt(), 0);
    const std::int64_t x1 = std::min<std::int64_t>((p.position.x + r).ceilInt() - 1, g.width() - 1);
    const std::int64_t y1 = std::min<std::int64_t>((p.position.y + r).ceilInt() - 1, g.height() - 1);
    if (x1 < x0 || y1 < y0) return reject();
    const EntityId id = world_.registry.create();
    if (id.isNull()) return reject();
    world_.transforms.set(id, {p.position, p.facing});
    world_.defRefs.set(id, {DefRef::Kind::Building, bi});
    world_.health.set(id, {b.health, b.health});
    const Footprint fp{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0), static_cast<std::int32_t>(x1),
                       static_cast<std::int32_t>(y1)};
    world_.footprints.set(id, fp);
    nav_->grid.stamp({fp.x0, fp.y0, fp.x1, fp.y1});
    attachCombat(*this, id, c.player);
    if (c.player < kMaxPlayers) world_.kingdoms[c.player].active = true;
    events_.push_back({SimEvent::Type::Spawned, id, c.player, c.type});
}

void Sim::placeBlocker(const Command& c, const DebugPlaceBlockerPayload& p) {
    const auto reject = [&] { events_.push_back({SimEvent::Type::CommandRejected, EntityId{}, c.player, c.type}); };
    if (!nav_ || p.halfExtent.x <= Fixed{} || p.halfExtent.y <= Fixed{}) return reject();
    const nav::NavGrid& g = nav_->grid;
    // Cells overlapped by [pos - half, pos + half), clamped to the map.
    const std::int64_t x0 = std::max<std::int64_t>((p.position.x - p.halfExtent.x).floorInt(), 0);
    const std::int64_t y0 = std::max<std::int64_t>((p.position.y - p.halfExtent.y).floorInt(), 0);
    const std::int64_t x1 = std::min<std::int64_t>((p.position.x + p.halfExtent.x).ceilInt() - 1, g.width() - 1);
    const std::int64_t y1 = std::min<std::int64_t>((p.position.y + p.halfExtent.y).ceilInt() - 1, g.height() - 1);
    if (x1 < x0 || y1 < y0) return reject();
    const EntityId id = world_.registry.create();
    if (id.isNull()) return reject();
    const Footprint fp{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0), static_cast<std::int32_t>(x1),
                       static_cast<std::int32_t>(y1)};
    world_.transforms.set(id, {p.position, Angle{}});
    world_.footprints.set(id, fp);
    nav_->grid.stamp({fp.x0, fp.y0, fp.x1, fp.y1});
    events_.push_back({SimEvent::Type::Spawned, id, c.player, c.type});
}

void Sim::requestPath(const Command& c, const DebugRequestPathPayload& p) {
    const auto reject = [&] { events_.push_back({SimEvent::Type::CommandRejected, EntityId{}, c.player, c.type}); };
    const Transform* t = world_.registry.alive(p.target) ? world_.transforms.get(p.target) : nullptr;
    if (!nav_ || t == nullptr || p.moveClass >= nav::kMoveClassCount) return reject();
    const auto cls = static_cast<nav::MoveClass>(p.moveClass);
    const std::uint32_t id = nav_->paths.request(p.target, cls, cellAt(t->position), cellAt(p.goal), tick_);
    Path path;
    path.status = Path::Status::Pending;
    path.moveClass = p.moveClass;
    path.requestId = id;
    world_.paths.set(p.target, std::move(path));  // supersedes any earlier request of this entity
}

void Sim::cleanup() {
    const auto unstamp = [this](EntityId id) {
        const Footprint* fp = world_.footprints.get(id);
        if (nav_ && fp != nullptr) nav_->grid.unstamp({fp->x0, fp->y0, fp->x1, fp->y1});
        onEntityDestroyed(*this, id);  // leave groups
        // A building that dies drops the regiment it was filling if no unit has joined yet (it
        // would stay forever, empty; an HQ one would keep its kingdom in the game). It goes in the
        // next tick's cleanup.
        if (const Structure* s = world_.structures.get(id); s != nullptr && !s->recruit.isNull() && world_.registry.alive(s->recruit)) {
            bool members = false;
            for (const Member& m : world_.members.data()) members = members || m.organization == s->recruit;
            if (!members) world_.registry.destroyLater(s->recruit);
        }
    };
    for (const EntityId id : world_.cleanup(unstamp)) {
        events_.push_back({SimEvent::Type::Destroyed, id, 0, CommandType::Noop});
    }
}

}  // namespace corps
