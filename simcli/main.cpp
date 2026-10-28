// simcli: headless runner for the sim and its data pipeline.
//
//   simcli hello
//   simcli load-order [--game DIR]
//   simcli dump-tgi <vfs-path | file> [--game DIR]
//   simcli dump-defs [--game DIR] [--json] [--explain <IDS> [<field-path>]] [--report-unknown]
//                    [--strict] [--diags]
//   simcli show-def IDS                       compiled (typed) Def
//   simcli spawn-test [--regiment IDS] [--at X,Y] [--ticks N] [--seed S] [--out F.rpl]
//   simcli record --out F.rpl [--ticks N] [--seed S] [--commands-per-tick K] [--spawn IDS]
//                 [--map F.json | --gen SEED [--size N]]
//   simcli play F.rpl [--map F.json]
//   simcli determinism-check F.rpl [--every-tick] [--per-stage] [--map F.json]
//   simcli nav-bench [--map F.json | --gen SEED [--size N]] [--queries N] [--class C|all] [--seed S]
//                    [--budget B] [--optimal]
//   simcli scenario [--gen SEED --size N | --map F.json] [--ticks N] [--seed S] [--out F.rpl]
//   simcli perf [--gen SEED --size N | --map F.json] [--regiments N] [--ticks N] [--out F.rpl]
//   simcli duel REG_A REG_B|BUILDING [TERRAIN] [--distance D] [--formation-a F] [--formation-b F] [--entrench-a]
//               [--entrench-b] [--xp-a N] [--xp-b N] [--retreat-a S] [--retreat-b S] [--until-wiped]
//               [--max-seconds S] [--size N] [--seed S] [--tune name=value,...] [--out F.rpl] [--verbose]
//   simcli economy [NATION] [--minutes M] [--recruit IDS@S,...] [--research PROP@S,...] [--size N] [--seed S]
//                  [--out F.rpl]
//   simcli skirmish --map F.json [NATION_A [NATION_B]] [--minutes M] [--regiments N] [--regiments-b N]
//                   [--city-ratio R] [--seed S] [--out F.rpl]
//   simcli slice --map F.json [NATION [ENEMY]] [--money M] [--minutes M] [--seed S] [--out F.rpl] [--trace]
//   simcli diff a.state b.state               first differing entity / field of two state dumps (M8)
//   simcli bisect F.rpl [--map F.json] [--write-stages F] [--against F] [--perturb TICK:STAGE]
//
// The game install comes from --game, else from config/game_path.cfg (searched upward from the
// current directory and from the executable's directory).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/ascii.hpp"
#include "core/hello.hpp"
#include "sim_commands.hpp"
#include "data/tgi/pipeline.hpp"
#include "data/tgi/resolved_dump.hpp"
#include "vfs/dir_depot.hpp"
#include "vfs/path.hpp"
#include "vfs/vfs.hpp"

namespace fs = std::filesystem;
using namespace corps;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitDataErrors = 1;
constexpr int kExitUsage = 2;

void usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  simcli hello\n"
                 "  simcli load-order [--game DIR]\n"
                 "  simcli dump-tgi <vfs-path|file> [--game DIR]\n"
                 "  simcli dump-defs [--game DIR] [--json] [--explain <IDS> [<field-path>]]\n"
                 "                   [--report-unknown] [--strict] [--diags]\n"
                 "  simcli show-def IDS [--game DIR]\n"
                 "  simcli spawn-test [--regiment IDS] [--at X,Y] [--facing DEG] [--ticks N] [--seed S] [--out F.rpl]\n"
                 "  simcli record --out F.rpl [--ticks N] [--seed S] [--command-seed C] [--commands-per-tick K]\n"
                 "                [--spawn REGIMENT_IDS [--at X,Y]] [--map F.json | --gen SEED [--size N]]\n"
                 "  simcli play F.rpl [--map F.json]\n"
                 "  simcli determinism-check F.rpl [--every-tick] [--per-stage] [--map F.json]\n"
                 "  simcli nav-bench [--map F.json | --gen SEED [--size N]] [--queries N] [--class C|all]\n"
                 "                   [--seed S] [--budget B] [--optimal]\n"
                 "  simcli scenario [--gen SEED --size N | --map F.json] [--ticks N] [--seed S] [--out F.rpl]\n"
                 "  simcli perf [--gen SEED --size N | --map F.json] [--regiments N] [--ticks N] [--out F.rpl]\n"
                 "  simcli duel REG_A REG_B|BUILDING [TERRAIN] [--distance D] [--formation-a F] [--formation-b F]\n"
                 "              [--entrench-a] [--entrench-b] [--xp-a N] [--xp-b N] [--retreat-a S] [--retreat-b S]\n"
                 "              [--until-wiped] [--max-seconds S] [--size N] [--seed S] [--tune name=value,...]\n"
                 "              [--out F.rpl] [--verbose]\n"
                 "  simcli economy [NATION] [--minutes M] [--recruit IDS@S,...] [--research PROP@S,...]\n"
                 "                 [--size N] [--seed S] [--out F.rpl]\n"
                 "  simcli skirmish --map F.json [NATION_A [NATION_B]] [--minutes M] [--regiments N] [--regiments-b N]\n"
                 "                  [--city-ratio R] [--seed S] [--out F.rpl]\n"
                 "  simcli slice --map F.json [NATION [ENEMY]] [--money M] [--minutes M] [--seed S] [--out F.rpl]\n"
                 "  simcli diff a.state b.state\n"
                 "  simcli bisect F.rpl [--map F.json] [--write-stages F] [--against F] [--perturb TICK:STAGE]\n");
}

void writeOut(std::string_view s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
}

std::optional<std::string> envVar(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) return std::nullopt;
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
}

std::optional<fs::path> readGamePathCfg(const fs::path& cfg) {
    std::ifstream in(cfg);
    if (!in) return std::nullopt;
    std::string line;
    while (std::getline(in, line)) {
        std::string_view t = trimAscii(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
        return vfs::pathFromUtf8(t);
    }
    return std::nullopt;
}

std::optional<fs::path> findGameDir(const std::optional<std::string>& explicitDir, const char* argv0) {
    if (explicitDir) return vfs::pathFromUtf8(*explicitDir);
    std::vector<fs::path> starts;
    std::error_code ec;
    starts.push_back(fs::current_path(ec));
    const fs::path exe = fs::absolute(fs::path(argv0), ec);
    if (!ec) starts.push_back(exe.parent_path());
    for (fs::path dir : starts) {
        for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
            const fs::path cfg = dir / "config" / "game_path.cfg";
            if (fs::is_regular_file(cfg, ec)) return readGamePathCfg(cfg);
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
    }
    return std::nullopt;
}

bool mountGame(vfs::Vfs& vfs, const fs::path& gameDir) {
    std::vector<fs::path> overlays;
    if (const auto profile = envVar("USERPROFILE")) {
        overlays.push_back(vfs::pathFromUtf8(*profile) / "Documents" / "Axis&Allies" / "data");
    }
    overlays.push_back(gameDir / "Userdata");
    std::vector<std::string> log;
    const bool ok = vfs::mountGameInstall(vfs, gameDir, overlays, log);
    for (const auto& l : log) {
        if (l.rfind("mount", 0) == 0) std::fprintf(stderr, "vfs: %s\n", l.c_str());
    }
    if (!ok) std::fprintf(stderr, "error: nothing could be mounted from %s\n", vfs::pathToUtf8(gameDir).c_str());
    return ok;
}

struct Args {
    std::string command;
    std::optional<std::string> game;
    std::vector<std::string> positional;
    bool json = false;
    bool reportUnknown = false;
    bool strict = false;
    bool diags = false;
    std::optional<std::string> explainIds;
    std::string explainPath;
    std::map<std::string, std::string> options;  // other --name [value] options (sim commands)
};

const std::set<std::string> kValueOptions = {"out", "ticks", "seed", "command-seed", "commands-per-tick", "regiments",
                                             "spawn", "at", "regiment", "facing",
                                             "map", "gen", "size", "queries", "class", "budget",
                                             "distance", "formation-a", "formation-b", "max-seconds", "tune",
                                             "xp-a", "xp-b", "retreat-a", "retreat-b", "minutes", "recruit", "research",
                                             "city-ratio", "regiments-b", "money",
                                             "write-stages", "against", "perturb"};
const std::set<std::string> kFlagOptions = {"every-tick", "per-stage", "optimal", "why", "entrench-a", "entrench-b", "verbose", "trace",
                                            "until-wiped"};

std::optional<Args> parseArgs(int argc, char** argv) {
    Args a;
    a.command = argc > 1 ? argv[1] : "hello";
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--game") {
            if (++i >= argc) return std::nullopt;
            a.game = argv[i];
        } else if (arg == "--json") {
            a.json = true;
        } else if (arg == "--report-unknown") {
            a.reportUnknown = true;
        } else if (arg == "--strict") {
            a.strict = true;
        } else if (arg == "--diags") {
            a.diags = true;
        } else if (arg == "--explain") {
            if (++i >= argc) return std::nullopt;
            a.explainIds = argv[i];
            if (i + 1 < argc && std::string_view(argv[i + 1]).rfind("--", 0) != 0) a.explainPath = argv[++i];
        } else if (arg.rfind("--", 0) == 0) {
            const std::string name(arg.substr(2));
            if (kValueOptions.count(name) != 0 && i + 1 < argc) {
                a.options[name] = argv[++i];
            } else if (kFlagOptions.count(name) != 0) {
                a.options[name] = "1";
            } else {
                std::fprintf(stderr, "unknown option %s\n", argv[i]);
                return std::nullopt;
            }
        } else {
            a.positional.emplace_back(arg);
        }
    }
    return a;
}

int finishWithDiagnostics(const tgi::Diagnostics& diags, bool strict, bool printAll) {
    if (printAll || strict) {
        for (const auto& d : diags.all()) {
            if (printAll || d.severity != tgi::Severity::Info) {
                std::fprintf(stderr, "%s\n", tgi::formatDiagnostic(d).c_str());
            }
        }
    }
    const std::size_t errors = diags.count(tgi::Severity::Error);
    const std::size_t warnings = diags.count(tgi::Severity::Warning);
    std::fprintf(stderr, "diagnostics: %zu error(s), %zu warning(s), %zu info\n", errors, warnings,
                 diags.count(tgi::Severity::Info));
    const std::string summary = tgi::summarizeDiagnostics(diags);
    if (!summary.empty()) std::fprintf(stderr, "%s", summary.c_str());
    if (errors > 0 || (strict && warnings > 0)) return kExitDataErrors;
    return kExitOk;
}

int cmdLoadOrder(const Args& args, const fs::path& gameDir) {
    vfs::Vfs vfs;
    if (!mountGame(vfs, gameDir)) return kExitDataErrors;
    tgi::Diagnostics diags;
    const tgi::LoadOrder order = tgi::buildLoadOrder(vfs, diags);
    for (std::size_t i = 0; i < order.files.size(); ++i) {
        const tgi::LoadEntry& e = order.files[i];
        std::string depot = vfs.depot(e.file->depot).name();
        if (depot.size() < 10) depot.resize(10, ' ');
        std::printf("%5zu  %s  %s\n", i, depot.c_str(), e.file->path.c_str());
    }
    std::fprintf(stderr, "%zu files from %zu lists; %zu wildcard spec(s) matched nothing (silent)\n",
                 order.files.size(), order.lists.size(), order.emptyWildcards.size());
    for (const auto& l : order.lists) std::fprintf(stderr, "  list %s\n", l.c_str());
    return finishWithDiagnostics(diags, args.strict, args.diags);
}

int cmdDumpTgi(const Args& args, const std::optional<fs::path>& gameDir) {
    if (args.positional.empty()) {
        usage();
        return kExitUsage;
    }
    const std::string& target = args.positional[0];
    std::string bytes;
    std::string displayPath = target;
    std::error_code ec;
    if (fs::is_regular_file(vfs::pathFromUtf8(target), ec)) {
        if (!vfs::readFileBytes(vfs::pathFromUtf8(target), bytes)) {
            std::fprintf(stderr, "cannot read %s\n", target.c_str());
            return kExitDataErrors;
        }
    } else {
        if (!gameDir) {
            std::fprintf(stderr, "%s is not a file and no game install is configured\n", target.c_str());
            return kExitUsage;
        }
        vfs::Vfs vfs;
        if (!mountGame(vfs, *gameDir)) return kExitDataErrors;
        const vfs::VfsFile* f = vfs.find(target);
        if (f == nullptr || !vfs.read(*f, bytes)) {
            std::fprintf(stderr, "not found in the VFS: %s\n", target.c_str());
            return kExitDataErrors;
        }
        displayPath = f->path;
        std::fprintf(stderr, "from depot %s\n", vfs.depot(f->depot).name().c_str());
    }
    tgi::SymbolTable symbols;
    tgi::Diagnostics diags;
    const tgi::DecodedText text = tgi::decodeText(bytes);
    const tgi::AstFile ast = tgi::parseTgi(text.utf8, 0, displayPath, symbols, diags);
    std::fprintf(stderr, "encoding %s, %zu top-level nodes, %u nodes\n", std::string(tgi::encodingName(text.encoding)).c_str(),
                 ast.nodes.size(), ast.nodeCount);
    writeOut(tgi::dumpAst(ast));
    return finishWithDiagnostics(diags, args.strict, true);
}

std::uint64_t hostClockMicros() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

int cmdDumpDefs(const Args& args, const fs::path& gameDir) {
    vfs::Vfs vfs;
    if (!mountGame(vfs, gameDir)) return kExitDataErrors;
    tgi::TgiOptions options;
    options.resolve.trackHistory = args.explainIds.has_value();
    options.hostClockMicros = &hostClockMicros;
    const auto t0 = std::chrono::steady_clock::now();
    const auto db = tgi::loadTgiDatabase(vfs, options);
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    int rc = kExitOk;
    if (args.explainIds) {
        const auto matches = db->findRecords("", *args.explainIds);
        if (matches.empty()) {
            std::fprintf(stderr, "no record with IDS '%s'\n", args.explainIds->c_str());
            rc = kExitDataErrors;
        }
        for (const tgi::Record* r : matches) {
            std::string out;
            const bool ok = tgi::explainRecord(*db, *r, args.explainPath, out);
            writeOut(out);
            if (!ok) rc = kExitDataErrors;
        }
    } else if (args.json) {
        writeOut(tgi::definitionsJson(*db));
    } else if (!args.reportUnknown) {
        writeOut(tgi::dumpDefinitionSet(*db));
    }
    if (args.reportUnknown) writeOut(tgi::reportUnknown(*db));

    const std::string canon = tgi::dumpDefinitionSet(*db);
    std::fprintf(stderr, "%zu files, %zu templates, %zu records (%zu by IDS, %zu singleton types, %zu ordered)\n",
                 db->files.size(), db->templates.size(), db->records.size(), db->defs.byIds.size(),
                 db->defs.singletons.size(), db->defs.ordered.size());
    std::fprintf(stderr, "resolved hash %s   load+resolve %lld ms\n", tgi::hex64(tgi::fnv1a64(canon)).c_str(),
                 static_cast<long long>(ms));
    for (const auto& t : db->timings) {
        std::fprintf(stderr, "  %-18s %6llu ms\n", t.phase, static_cast<unsigned long long>(t.micros / 1000));
    }
    const int drc = finishWithDiagnostics(db->diags, args.strict, args.diags);
    return rc != kExitOk ? rc : drc;
}

}  // namespace

int main(int argc, char** argv) {
    const auto args = parseArgs(argc, argv);
    if (!args) {
        usage();
        return kExitUsage;
    }
    if (args->command == "hello") {
        std::printf("%.*s\n", static_cast<int>(corps::simHello().size()), corps::simHello().data());
        return kExitOk;
    }
    const bool needsGame = args->command == "load-order" || args->command == "dump-defs";
    const std::optional<fs::path> gameDir = findGameDir(args->game, argv[0]);
    if (needsGame && !gameDir) {
        std::fprintf(stderr, "no game install: pass --game DIR or create config/game_path.cfg\n");
        return kExitUsage;
    }
    if (args->command == "load-order") return cmdLoadOrder(*args, *gameDir);
    if (args->command == "dump-tgi") return cmdDumpTgi(*args, gameDir);
    if (args->command == "dump-defs") return cmdDumpDefs(*args, *gameDir);
    simcli::SimArgs sa;
    sa.options = args->options;
    sa.positional = args->positional;
    sa.gameDir = gameDir;
    if (args->command == "record") return simcli::cmdRecord(sa);
    if (args->command == "play") return simcli::cmdPlay(sa);
    if (args->command == "determinism-check") return simcli::cmdDeterminismCheck(sa);
    if (args->command == "spawn-test") return simcli::cmdSpawnTest(sa);
    if (args->command == "show-def") return simcli::cmdShowDef(sa);
    if (args->command == "nav-bench") return simcli::cmdNavBench(sa);
    if (args->command == "scenario") return simcli::cmdScenario(sa);
    if (args->command == "perf") return simcli::cmdPerf(sa);
    if (args->command == "duel") return simcli::cmdDuel(sa);
    if (args->command == "economy") return simcli::cmdEconomy(sa);
    if (args->command == "skirmish") return simcli::cmdSkirmish(sa);
    if (args->command == "slice") return simcli::cmdSlice(sa);
    if (args->command == "diff") return simcli::cmdDiff(sa);
    if (args->command == "bisect") return simcli::cmdBisect(sa);
    usage();
    return kExitUsage;
}
