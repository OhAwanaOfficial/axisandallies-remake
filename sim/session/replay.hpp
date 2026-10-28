#pragma once

// Replays ("films", NETCODE §8): header + the command stream + periodic state hashes.
//
//   "CRPL" u16 formatVersion
//   header: str simVersion, u16 protocolVersion, u64 dataHash, str mapSpec, u64 mapHash,
//           u64 matchSeed, u32 tickCount, u16 hashInterval, u32 nPlayers, str player*, str tuning
//   (mapSpec: "" = no map, "gen:SEED:SIZE" = generated test map, "arena:TERRAIN:SIZE" = duel
//    arena, else the map's name; the player supplies the map and mapHash proves it is the same one.
//    tuning: the CombatTuning fields that differ from the defaults, "name=value,...")
//   "CMDS" u32 nCommands, (u32 tick, u16 seq, bytes encodedCommand)*  sorted by (tick, player, seq)
//   "HASH" u32 n, (u32 tick, u64 hash)*      hash of the state after that tick completed
//   ["KEYS" u32 n, (u32 tick, bytes state)*]  optional keyframes for seeking (Films, M8): the
//                                            canonical state after that tick; absent when empty,
//                                            so replays without keyframes are byte-identical
//   u64 xxh3 of every preceding byte (integrity)
//
// Uncompressed for now (zstd is planned; the format version will say so).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/command.hpp"
#include "core/sim.hpp"
#include "nav/map_def.hpp"

namespace corps {

constexpr std::uint16_t kReplayFormatVersion = 3;  // 2: mapSpec + mapHash (M4); 3: combat tuning (M5)
constexpr std::uint16_t kProtocolVersion = 1;

struct TickHash {
    std::uint32_t tick = 0;  // the tick that just completed
    std::uint64_t hash = 0;
    friend bool operator==(const TickHash& a, const TickHash& b) noexcept { return a.tick == b.tick && a.hash == b.hash; }
};

struct ReplayHeader {
    std::string simVersion;
    std::uint16_t protocolVersion = kProtocolVersion;
    std::uint64_t dataHash = 0;
    std::string mapSpec;
    std::uint64_t mapHash = 0;
    std::uint64_t matchSeed = 0;
    std::uint32_t tickCount = 0;
    std::uint16_t hashInterval = 10;
    std::vector<std::string> players;
    std::string tuning;  // formatCombatTuning(); "" = the defaults
};

struct Keyframe {
    std::uint32_t tick = 0;           // the tick that just completed
    std::vector<std::uint8_t> state;  // saveState() after it
    friend bool operator==(const Keyframe& a, const Keyframe& b) noexcept { return a.tick == b.tick && a.state == b.state; }
};

struct Replay {
    ReplayHeader header;
    std::vector<Command> commands;  // each carries its tick and seq
    std::vector<TickHash> hashes;
    std::vector<Keyframe> keyframes;  // ascending ticks; may be empty
};

std::vector<std::uint8_t> encodeReplay(const Replay& r);
bool decodeReplay(const std::vector<std::uint8_t>& bytes, Replay& out, std::string* error);

// The sim version string compiled into this build (git tree hash of sim/, "-dirty" if modified).
const char* simVersion();

struct RunOptions {
    const nav::MapDef* map = nullptr;  // the map the replay was recorded on (header.mapHash)
    bool everyTick = false;   // hash after every tick (default: every header.hashInterval ticks + last)
    bool perStage = false;    // also hash after every stage of every tick
    // Test hook: called after each stage; may perturb the sim to prove divergences are caught.
    std::function<void(Sim&, Stage)> afterStage;
    std::uint32_t keyframeEvery = 0;  // > 0: keep a keyframe after every N-th tick (N-1, 2N-1, ...)
};

struct RunResult {
    std::vector<TickHash> hashes;
    // perStage only: [tick][stage] hash of the state after that stage.
    std::vector<std::array<std::uint64_t, kStageCount>> stageHashes;
    std::uint64_t finalHash = 0;
    std::size_t rejectedSchedules = 0;  // commands the queue refused (malformed replay)
    std::vector<Keyframe> keyframes;    // keyframeEvery only
};

// Plays the replay on a fresh Sim. `defs` must be the data the replay was recorded with.
RunResult runReplay(const Replay& replay, const DefSet* defs, const RunOptions& options = {});

// Records a synthetic replay: `ticks` ticks of random test commands (spawn / destroy / move / RNG
// draws) chosen from a separate host-side RNG (`commandSeed`), issued with input delay 3 like real
// peers, against the entities that exist when they are issued. Optional `extra` commands are
// scheduled up front (e.g. SpawnRegiment from data). With a map, positions fall inside it and the
// mix adds blockers (DebugPlaceBlocker) and path requests (DebugRequestPath, every move class).
Replay recordSynthetic(std::uint64_t matchSeed, std::uint64_t commandSeed, std::uint32_t ticks,
                       std::uint32_t commandsPerTick, const DefSet* defs, const std::vector<Command>& extra = {},
                       const nav::MapDef* map = nullptr);

// The map a replay needs: regenerates "gen:" specs; any other spec must come from `supplied`
// (e.g. simcli --map). Checks mapHash. out = nullopt for map-free replays. false + error on mismatch.
bool resolveReplayMap(const ReplayHeader& header, const nav::MapDef* supplied, std::optional<nav::MapDef>& out,
                      std::string& error);

struct Divergence {
    bool found = false;
    std::uint32_t tick = 0;
    int stage = -1;  // first diverging stage when both runs have per-stage hashes
    std::uint64_t a = 0;
    std::uint64_t b = 0;
};
Divergence firstDivergence(const RunResult& a, const RunResult& b);
// Compares a run against the hashes stored in the replay (only ticks present in both).
Divergence divergenceFromRecorded(const Replay& replay, const RunResult& run);

// Film playback (NETCODE §8): steps a replay tick by tick and seeks, from the latest keyframe at or
// before the target (else from the start). Host-side; owns its Sim.
class ReplayPlayer {
public:
    // `defs` and `map` must outlive the player (the data and map the replay was recorded with).
    // `slot`: where the Sim lives (a SimBridge's), else the player keeps its own; seeking may
    // replace the Sim in it.
    ReplayPlayer(const Replay& replay, const DefSet* defs, const nav::MapDef* map, std::unique_ptr<Sim>* slot = nullptr);
    ~ReplayPlayer();
    ReplayPlayer(const ReplayPlayer&) = delete;
    ReplayPlayer& operator=(const ReplayPlayer&) = delete;

    // Runs the next tick; false at the end of the film.
    bool step();
    // The next tick's commands, for a caller that steps sim() itself (a view that wants the events).
    std::vector<Command> takeTickCommands();
    // Positions the film so that tick() == target (clamped to the film's length).
    void seek(std::uint32_t target);
    std::uint32_t tick() const;  // ticks done
    std::uint32_t length() const noexcept { return replay_.header.tickCount; }
    Sim& sim() { return **sim_; }
    const Sim& sim() const { return **sim_; }
    // Ticks re-simulated by the last seek (keyframes keep this under one minute of play).
    std::uint32_t lastSeekTicks() const noexcept { return lastSeekTicks_; }

private:
    void restart(std::uint32_t fromTick, const std::vector<std::uint8_t>* state);

    Replay replay_;
    const DefSet* defs_;
    const nav::MapDef* map_;
    CombatTuning tuning_;
    std::unique_ptr<Sim> own_;
    std::unique_ptr<Sim>* sim_;
    std::size_t nextCommand_ = 0;  // index into replay_.commands (sorted by tick)
    std::uint32_t lastSeekTicks_ = 0;
};

// Keyframes every 60 s of game time (NETCODE §8).
constexpr std::uint32_t kFilmKeyframeTicks = 60 * kTicksPerSecond;

}  // namespace corps
