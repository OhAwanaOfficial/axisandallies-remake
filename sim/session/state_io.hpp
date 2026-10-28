#pragma once

// Canonical sim state (NETCODE §5, §8): one byte layout used for the state hash, save games and
// desync dumps. Fixed order, little-endian, fixed-width:
//
//   "CSIM" u32 formatVersion, u32 tick, u64 matchSeed, u64 dataHash, u64 mapHash, u64 combatTuningHash,
//          u64 rngState, u64 rngInc
//   "ENTS" registry: generations, slot states, FIFO free list, pending destruction (ascending)
//   one section per component pool, in World::forEachPool order, entries by ascending EntityId:
//     "XFRM" Transform   "DREF" DefRef   "HLTH" Health   "MEMB" Member   "FTPR" Footprint   "PATH" Path
//     "REGT" Regiment    "ORDR" Order    "MOVR" Mover    "GRUP" Group
//     "COMB" Combat      "ENGT" Engagement   "PROJ" Projectile   "STRC" Structure   "DPLY" Deploy
//     "SUPL" Supply
//   "KING" per player (kMaxPlayers): u8 active, fixed armyXp, u8 started, fixed money, u8 bankrupt,
//          properties, u8 team, u8 eliminated
//   "MTCH" match rules and result
//   "NAVS" u8 hasNav, then if set: u32 navVersion, u32 n + dirty clusters, PathService state
//          (queue, job in progress, budget debt, next id; see nav/path_service.cpp)
//
// The nav grid, stamps and HPA graph are not stored: they are rebuilt from the map and the live
// footprints on load.
//
// Events and hooks are outputs, not state. Commands for future ticks are not state either: they
// live in the session (CommandQueue / lockstep bundles), and peers receive them at different times.

#include <cstdint>
#include <string>
#include <vector>

#include "core/sim.hpp"

namespace corps {

// 2: mapHash, FTPR, PATH, NAVS; 3: REGT, ORDR, MOVR, GRUP (M4); 4: tuning hash, COMB, ENGT, PROJ (M5);
// 5: morale, efficiency, XP, entrenchment timer in ENGT, KING (M5 part 2); 6: STRC, DPLY, economy in
// KING (M6 part 1); 7: supply and capture in STRC, SUPL, team and elimination in KING, MTCH (M6 part 2);
// 8: starting money in MTCH (M6 close-out)
constexpr std::uint32_t kStateFormatVersion = 8;

std::vector<std::uint8_t> saveState(const Sim& sim);
std::uint64_t stateHash(const Sim& sim);

// Loads a saved state into `sim`, which must have been constructed with the same SimConfig (seed,
// DefSet and map). On failure returns false, sets `error`, and leaves `sim` unchanged.
bool loadState(Sim& sim, const std::vector<std::uint8_t>& bytes, std::string* error);

// The first difference between two canonical states (simcli diff, desync dumps; NETCODE §5). The
// states are walked item by item in canonical order (header fields, registry slots, one item per
// component per entity, kingdoms, match, nav); `where` names the first item that differs or exists
// in only one of them, e.g. "XFRM #12.3 position.y", "header rng.state", "KING player 1".
struct StateDiff {
    bool equal = true;
    bool malformed = false;  // a state didn't parse (format mismatch)
    std::string where;
    std::string a;  // that item's bytes in hex ("-": missing)
    std::string b;
    std::size_t differingItems = 0;  // items that differ in total
};
StateDiff diffStates(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b);

}  // namespace corps
