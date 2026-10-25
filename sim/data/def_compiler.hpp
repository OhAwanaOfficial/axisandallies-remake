#pragma once

// Def compiler (ARCHITECTURE §4.2, TGI_FORMAT §8): resolved TGI records -> typed, immutable Defs.
//
//   Thing, category UNIT or TRUCK             -> UnitDef (+ its inline abilities -> AbilityDef)
//   Thing, category ORGANIZATION              -> RegimentDef (slots -> unit/truck or building)
//   Thing, category BUILDING                  -> BuildingDef
//   Thing, category START_LOC                 -> EconomyDef::startLocation
//   Resource (ammo, oil PurchaseCost)         -> EconomyDef prices
//   AbilityDefinition (top level)             -> AbilityDef
//   Nation                                    -> NationDef
//   SVars (singleton, SVars.tgi + SVars_AA)   -> SVarsDef
//
// Numbers go through the exact integer parsers (data/tgi/value_parse); a value that doesn't parse
// is reported (bad-value, with file:line) and the field keeps its default.

#include "data/defs.hpp"
#include "data/tgi/pipeline.hpp"

namespace corps {

DefSet compileDefs(const tgi::TgiDatabase& db, tgi::Diagnostics& diags);

// Links bases (M6): trucks to the buildings they unpack into, buildings to their truck and HQ
// regiments, recruit lists to regiments; adds a "convoy_<truck>" regiment for every truck no
// regiment holds (and re-sorts the regiments). compileDefs calls it; hand-built DefSets (tests)
// call it after filling units, regiments (slots resolved) and buildings, sorted by IDS.
void linkBases(DefSet& defs, tgi::Diagnostics& diags);

}  // namespace corps
