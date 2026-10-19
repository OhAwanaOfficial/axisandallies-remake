#pragma once

// Replays ("films", NETCODE §8): header + the command stream + periodic state hashes.
//
//   "CRPL" u16 formatVersion
//   header: str simVersion, u16 protocolVersion, u64 dataHash, u64 matchSeed, u32 tickCount,
//           u16 hashInterval, u32 nPlayers, str player*
//   "CMDS" u32 nCommands, (u32 tick, u16 seq, bytes encodedCommand)*  sorted by (tick, player, seq)
//   "HASH" u32 n, (u32 tick, u64 hash)*      hash of the state after that tick completed
//   u64 xxh3 of every preceding byte (integrity)
//
// Uncompressed for now (zstd is planned; the format version will say so).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/command.hpp"
#include "core/sim.hpp"

namespace corps {

constexpr std::uint16_t kReplayFormatVersion = 1;
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
    std::uint64_t matchSeed = 0;
    std::uint32_t tickCount = 0;
    std::uint16_t hashInterval = 10;
    std::vector<std::string> players;
};

struct Replay {
    ReplayHeader header;
    std::vector<Command> commands;  // each carries its tick and seq
    std::vector<TickHash> hashes;
};

std::vector<std::uint8_t> encodeReplay(const Replay& r);
bool decodeReplay(const std::vector<std::uint8_t>& bytes, Replay& out, std::string* error);

// The sim version string compiled into this build (git tree hash of sim/, "-dirty" if modified).
const char* simVersion();

struct RunOptions {
    bool everyTick = false;   // hash after every tick (default: every header.hashInterval ticks + last)
    bool perStage = false;    // also hash after every stage of every tick
    // Test hook: called after each stage; may perturb the sim to prove divergences are caught.
    std::function<void(Sim&, Stage)> afterStage;
};

struct RunResult {
    std::vector<TickHash> hashes;
    // perStage only: [tick][stage] hash of the state after that stage.
    std::vector<std::array<std::uint64_t, kStageCount>> stageHashes;
    std::uint64_t finalHash = 0;
    std::size_t rejectedSchedules = 0;  // commands the queue refused (malformed replay)
};

// Plays the replay on a fresh Sim. `defs` must be the data the replay was recorded with.
RunResult runReplay(const Replay& replay, const DefSet* defs, const RunOptions& options = {});

// Records a synthetic replay: `ticks` ticks of random test commands (spawn / destroy / move / RNG
// draws) chosen from a separate host-side RNG (`commandSeed`), issued with input delay 3 like real
// peers, against the entities that exist when they are issued. Optional `extra` commands are
// scheduled up front (e.g. SpawnRegiment from data).
Replay recordSynthetic(std::uint64_t matchSeed, std::uint64_t commandSeed, std::uint32_t ticks,
                       std::uint32_t commandsPerTick, const DefSet* defs, const std::vector<Command>& extra = {});

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

}  // namespace corps
