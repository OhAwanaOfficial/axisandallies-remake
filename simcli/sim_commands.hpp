#pragma once

// simcli commands that run the sim: record / play / determinism-check / spawn-test / show-def (M3),
// nav-bench (M4).

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nav/map_def.hpp"

namespace simcli {

struct SimArgs {
    std::map<std::string, std::string> options;  // --name value (flags: "1")
    std::vector<std::string> positional;
    std::optional<std::filesystem::path> gameDir;

    std::string get(const std::string& name, const std::string& fallback = "") const;
    bool has(const std::string& name) const { return options.count(name) != 0; }
};

int cmdRecord(const SimArgs& a);
int cmdPlay(const SimArgs& a);
int cmdDeterminismCheck(const SimArgs& a);
int cmdSpawnTest(const SimArgs& a);
int cmdShowDef(const SimArgs& a);
int cmdNavBench(const SimArgs& a);
int cmdScenario(const SimArgs& a);
int cmdPerf(const SimArgs& a);
int cmdDuel(const SimArgs& a);
int cmdEconomy(const SimArgs& a);

// --map F.json (tools/rmp.py output) or --gen SEED [--size N]; out stays empty if neither is given.
// false (after printing why) if the map can't be loaded.
bool mapFromArgs(const SimArgs& a, std::optional<corps::nav::MapDef>& out);

}  // namespace simcli
