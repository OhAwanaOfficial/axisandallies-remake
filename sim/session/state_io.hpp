#pragma once

// Canonical sim state (NETCODE §5, §8): one byte layout used for the state hash, save games and
// desync dumps. Fixed order, little-endian, fixed-width:
//
//   "CSIM" u32 formatVersion, u32 tick, u64 matchSeed, u64 dataHash, u64 rngState, u64 rngInc
//   "ENTS" registry: generations, slot states, FIFO free list, pending destruction (ascending)
//   one section per component pool, in World::forEachPool order, entries by ascending EntityId:
//     "XFRM" Transform   "DREF" DefRef   "HLTH" Health   "MEMB" Member
//
// Events and hooks are outputs, not state. Commands for future ticks are not state either: they
// live in the session (CommandQueue / lockstep bundles), and peers receive them at different times.

#include <cstdint>
#include <string>
#include <vector>

#include "core/sim.hpp"

namespace corps {

constexpr std::uint32_t kStateFormatVersion = 1;

std::vector<std::uint8_t> saveState(const Sim& sim);
std::uint64_t stateHash(const Sim& sim);

// Loads a saved state into `sim`, which must have been constructed with the same SimConfig (seed
// and DefSet). On failure returns false, sets `error`, and leaves `sim` unchanged.
bool loadState(Sim& sim, const std::vector<std::uint8_t>& bytes, std::string* error);

}  // namespace corps
