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

#include "combat/tuning.hpp"
#include "core/command.hpp"
#include "data/defs.hpp"
#include "ecs/world.hpp"
#include "math/rng.hpp"

namespace corps::nav {
struct MapDef;
struct NavWorld;
}  // namespace corps::nav

namespace corps {

struct MovementCache;
class CombatRules;

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
    // Arrived / Blocked: a regiment's (or division's) move order finished at, or short of, its goal.
    // Fired: `entity` shot at `other` with ability `detail` (M5). Hit: a shot of ability `detail`
    // from `other` landed `at` (on `entity`, the primary target, or nothing). Died: unit or building
    // `entity` was killed by `other`, `at` facing `facing`. Routed: regiment `entity` broke (its last
    // attacker: `other`). LevelUp: regiment `entity` gained an experience level.
    // Bases (M6): Recruited: regiment `entity` has all its units (building `other`, regiment def
    // `detail`). Unpacked / Packed: building `entity` finished unpacking / packed into regiment
    // `other`. ResearchDone: building `entity` granted property `detail`. Bankrupt / Solvent: player
    // `player`'s kingdom went bankrupt / recovered. DeployFailed: truck regiment `entity` arrived
    // where its building no longer fits.
    // Supply and victory (M6 part 2): Replaced: unit `entity` joined regiment `other` as a free
    // replacement. CityCaptured: city `entity` passed to `player` (from player `detail`; 255: nobody),
    // taken by regiment `other`. KingdomDefeated: `player` lost its last HQ. TeamVictory /
    // CityRatioVictory: the match is won by team (or player) `detail` (255: nobody is left).
    // Events feed the view (sounds, effects, death models, HUD); they are outputs, not state.
    enum class Type : std::uint8_t {
        Spawned, Destroyed, CommandRejected, Arrived, Blocked, Fired, Died, Routed, LevelUp, Hit,
        Recruited, Unpacked, Packed, ResearchDone, Bankrupt, Solvent, DeployFailed,
        Replaced, CityCaptured, KingdomDefeated, TeamVictory, CityRatioVictory
    };
    Type type = Type::Spawned;
    EntityId entity;
    std::uint8_t player = 0;
    CommandType command = CommandType::Noop;
    EntityId other;
    std::uint32_t detail = 0;
    FVec2 at;
    Angle facing;
};

struct SimConfig {
    std::uint64_t matchSeed = 0;
    const DefSet* defs = nullptr;  // shared, immutable; may be null for data-free tests
    const nav::MapDef* map = nullptr;  // shared, immutable; null = no nav (paths and blockers rejected)
    const CombatTuning* tuning = nullptr;  // combat constants (null: the defaults); copied by the Sim
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
    // Derived movement data (clearance); not state. Valid while a map is set.
    MovementCache& moveCache() noexcept { return *moveCache_; }
    // Combat constants (config().tuning or the defaults) and lookups derived from defs + map.
    const CombatTuning& tuning() const noexcept { return tuning_; }
    const CombatRules& combatRules() const noexcept { return *combatRules_; }

    // Appends an output event (systems use it; the view drains events()).
    void emit(const SimEvent& e) { events_.push_back(e); }
    void reject(const Command& c) { events_.push_back({SimEvent::Type::CommandRejected, EntityId{}, c.player, c.type}); }

    // Called after every stage of every tick (per-system hashing for determinism checks, test
    // hooks). Not part of the state. Empty by default.
    std::function<void(Sim&, Stage)> afterStage;

    // Restores the tick counter (save/load only).
    void restoreTick(std::uint32_t t) noexcept { tick_ = t; }

    // Creates regiment def `regiment` for `player` with all its units at `position` (SpawnRegiment;
    // a building packing onto its trucks). Returns the regiment, or null if no entity was left.
    EntityId spawnRegimentAt(std::uint8_t player, std::uint32_t regiment, FVec2 position, Angle facing, CommandType cause);

private:
    void applyCommands(const std::vector<Command>& commands);
    void apply(const Command& c);
    void spawnRegiment(const Command& c, const SpawnRegimentPayload& p);
    void placeBlocker(const Command& c, const DebugPlaceBlockerPayload& p);
    void requestPath(const Command& c, const DebugRequestPathPayload& p);
    void spawnBuilding(const Command& c, const DebugSpawnBuildingPayload& p);
    void cleanup();

    SimConfig config_;
    std::uint32_t tick_ = 0;
    World world_;
    Rng rng_;
    std::vector<SimEvent> events_;
    std::unique_ptr<nav::NavWorld> nav_;
    std::unique_ptr<MovementCache> moveCache_;
    CombatTuning tuning_;
    std::unique_ptr<CombatRules> combatRules_;
};

}  // namespace corps
