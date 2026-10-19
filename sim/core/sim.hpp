#pragma once

// The simulation: State(T+1) = Step(State(T), Commands(T)) (ARCHITECTURE §3). 10 ticks per sim
// second; real-time pacing is the host's job (session/lockstep), so tools run it flat out.
// Every tick runs the same 13 stages in a fixed order (ARCHITECTURE §4.3); none can be skipped
// or reordered at runtime. Systems arrive in later milestones; until then their stages are empty.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "core/command.hpp"
#include "data/defs.hpp"
#include "ecs/world.hpp"
#include "math/rng.hpp"

namespace corps::nav {
struct MapDef;
struct NavWorld;
}  // namespace corps::nav

namespace corps {

constexpr std::uint32_t kTicksPerSecond = 10;  // ARCHITECTURE §4.3 / NETCODE §2 (original: SVars 16)

enum class Stage : std::uint8_t {
    ApplyCommands,
    Economy,
    Timers,       // construction / pack / unpack / recruit / research
    Orders,       // order state machines -> plans
    Pathfinding,
    Movement,
    Vision,
    Engagement,
    Abilities,
    Morale,       // morale / experience / entrenchment / combat efficiency
    Supply,       // supply, healing, replacement, under siege
    Capture,      // capture, objectives, victory
    Cleanup,      // deaths -> events, deferred destruction
    Count
};
constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::Count);
const char* stageName(Stage s);

struct SimEvent {
    enum class Type : std::uint8_t { Spawned, Destroyed, CommandRejected };
    Type type = Type::Spawned;
    EntityId entity;
    std::uint8_t player = 0;
    CommandType command = CommandType::Noop;
};

struct SimConfig {
    std::uint64_t matchSeed = 0;
    const DefSet* defs = nullptr;  // shared, immutable; may be null for data-free tests
    const nav::MapDef* map = nullptr;  // shared, immutable; null = no nav (paths and blockers rejected)
};

// Per-system RNG stream ids (Rng::derive(matchSeed, id)). Never renumber: replays depend on them.
enum class RngStream : std::uint64_t { Match = 1 };

class Sim {
public:
    explicit Sim(const SimConfig& config);
    ~Sim();
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;
    Sim(Sim&&) noexcept;
    Sim& operator=(Sim&&) noexcept;

    // Runs tick `tick()` with that tick's commands (any order; they are applied sorted by
    // (player, seq, position)) and advances. Commands for any other tick are rejected
    // deterministically. Which commands reach a tick is the session's job (CommandQueue, lockstep
    // bundles, replays); a command that hasn't arrived yet is not sim state.
    void step(const std::vector<Command>& commands = {});

    std::uint32_t tick() const noexcept { return tick_; }  // the next tick to run = ticks done
    const SimConfig& config() const noexcept { return config_; }
    World& world() noexcept { return world_; }
    const World& world() const noexcept { return world_; }
    Rng& rng() noexcept { return rng_; }
    const Rng& rng() const noexcept { return rng_; }
    // Events produced by the last step (the view drains these; not part of the state).
    const std::vector<SimEvent>& events() const noexcept { return events_; }
    // Navigation state for config().map (null without a map).
    nav::NavWorld* nav() noexcept { return nav_.get(); }
    const nav::NavWorld* nav() const noexcept { return nav_.get(); }
    // Nav cell under a world position, clamped to the map. Requires a map.
    std::uint32_t cellAt(FVec2 position) const;

    // Called after every stage of every tick (per-system hashing for determinism checks, test
    // hooks). Not part of the state. Empty by default.
    std::function<void(Sim&, Stage)> afterStage;

    // Restores the tick counter (save/load only).
    void restoreTick(std::uint32_t t) noexcept { tick_ = t; }

private:
    void applyCommands(const std::vector<Command>& commands);
    void apply(const Command& c);
    void spawnRegiment(const Command& c, const SpawnRegimentPayload& p);
    void placeBlocker(const Command& c, const DebugPlaceBlockerPayload& p);
    void requestPath(const Command& c, const DebugRequestPathPayload& p);
    void cleanup();

    SimConfig config_;
    std::uint32_t tick_ = 0;
    World world_;
    Rng rng_;
    std::vector<SimEvent> events_;
    std::unique_ptr<nav::NavWorld> nav_;
};

}  // namespace corps
