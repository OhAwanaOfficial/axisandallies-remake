#pragma once

// simcli commands that run the sim (M3): record / play / determinism-check / spawn-test / show-def.

#include <filesystem>
#include <map>
#include <optional>
#include <string>

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

}  // namespace simcli
