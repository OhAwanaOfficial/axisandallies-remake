#include "core/sim.hpp"

#include <algorithm>

#include "systems/stages.hpp"

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
    : config_(config), rng_(Rng::derive(config.matchSeed, static_cast<std::uint64_t>(RngStream::Match))) {}

void Sim::step(const std::vector<Command>& commands) {
    events_.clear();
    const auto after = [this](Stage s) {
        if (afterStage) afterStage(*this, s);
    };
    applyCommands(commands);
    after(Stage::ApplyCommands);
    runEconomy(*this);
    after(Stage::Economy);
    runTimers(*this);
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
    }
    reject();
}

void Sim::spawnRegiment(const Command& c, const SpawnRegimentPayload& p) {
    const auto reject = [&] { events_.push_back({SimEvent::Type::CommandRejected, EntityId{}, c.player, c.type}); };
    if (config_.defs == nullptr) return reject();
    const DefSet& defs = *config_.defs;
    const std::uint32_t regIndex = defs.regimentIndex(p.regimentIds);
    if (regIndex == kNoDef) return reject();
    const RegimentDef& reg = defs.regiments[regIndex];

    const EntityId org = world_.registry.create();
    if (org.isNull()) return reject();
    world_.transforms.set(org, {p.position, p.facing});
    world_.defRefs.set(org, {DefRef::Kind::Regiment, regIndex});
    events_.push_back({SimEvent::Type::Spawned, org, c.player, c.type});

    for (std::size_t s = 0; s < reg.slots.size(); ++s) {
        const ElementRef& el = reg.slots[s];
        if (el.kind == ElementKind::None) continue;  // empty slot, or an element the data doesn't define
        const EntityId e = world_.registry.create();
        if (e.isNull()) break;
        world_.transforms.set(e, {p.position + slotOffset(s).rotated(p.facing), p.facing});
        if (el.kind == ElementKind::Unit) {
            const Fixed hp = defs.units[el.index].health;
            world_.defRefs.set(e, {DefRef::Kind::Unit, el.index});
            world_.health.set(e, {hp, hp});
        } else {
            const Fixed hp = defs.buildings[el.index].health;
            world_.defRefs.set(e, {DefRef::Kind::Building, el.index});
            world_.health.set(e, {hp, hp});
        }
        world_.members.set(e, {org, static_cast<std::uint16_t>(s)});
        events_.push_back({SimEvent::Type::Spawned, e, c.player, c.type});
    }
}

void Sim::cleanup() {
    for (const EntityId id : world_.cleanup()) {
        events_.push_back({SimEvent::Type::Destroyed, id, 0, CommandType::Noop});
    }
}

}  // namespace corps
