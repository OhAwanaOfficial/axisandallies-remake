// simcli nav-bench and the shared --map / --gen map options (M4, PATHFINDING §8).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "data/tgi/value_parse.hpp"
#include "math/rng.hpp"
#include "nav/nav_world.hpp"
#include "session/bytes.hpp"
#include "sim_commands.hpp"
#include "vfs/path.hpp"

namespace simcli {

using namespace corps;

namespace {

std::uint64_t u64Arg(const SimArgs& a, const char* name, std::uint64_t fallback) {
    const std::string s = a.get(name);
    if (s.empty()) return fallback;
    const auto v = tgi::parseInt(s);
    return v && *v >= 0 ? static_cast<std::uint64_t>(*v) : fallback;
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

bool mapFromArgs(const SimArgs& a, std::optional<nav::MapDef>& out) {
    out.reset();
    if (a.has("map")) {
        const std::string path = a.get("map");
        std::ifstream in(vfs::pathFromUtf8(path), std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot read map %s\n", path.c_str());
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        nav::MapDef m;
        std::string err;
        if (!nav::loadRmpJson(text, m, err)) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
            return false;
        }
        out = std::move(m);
    } else if (a.has("gen")) {
        out = nav::generateTestMap(u64Arg(a, "gen", 1), static_cast<std::uint32_t>(u64Arg(a, "size", 256)));
    }
    return true;
}

// simcli nav-bench [--map F.json | --gen SEED [--size N]] [--queries N] [--class C|all] [--seed S]
//                  [--budget B] [--optimal]
//   Random start/goal pairs through the PathService (budgeted, like the sim). Reports expansions
//   (avg/p50/p99/max), host time, path lengths, fallbacks and a hash of every result.
int cmdNavBench(const SimArgs& a) {
    std::optional<nav::MapDef> map;
    if (!mapFromArgs(a, map)) return 1;
    if (!map) map = nav::generateTestMap(1, 256);
    const auto queries = static_cast<std::uint32_t>(u64Arg(a, "queries", 1000));
    const std::uint64_t seed = u64Arg(a, "seed", 1);
    const auto budget = static_cast<std::int32_t>(std::max<std::uint64_t>(1, u64Arg(a, "budget", nav::kPathBudgetPerTick)));
    const std::string clsArg = a.get("class", "all");
    std::vector<nav::MoveClass> classes;
    if (clsArg == "all") {
        for (std::size_t c = 0; c < nav::kMoveClassCount; ++c) classes.push_back(static_cast<nav::MoveClass>(c));
    } else {
        nav::MoveClass c{};
        if (!nav::parseMoveClass(clsArg, c)) {
            std::fprintf(stderr, "unknown --class %s (foot, wheeled, tracked, naval, amphibious, all)\n", clsArg.c_str());
            return 2;
        }
        classes.push_back(c);
    }

    const auto tBuild = std::chrono::steady_clock::now();
    nav::NavWorld nav(*map);
    const double buildMs = msSince(tBuild);
    const nav::NavGrid& g = nav.grid;
    std::printf("map %s: %ux%u cells, map hash %016llx, %dx%d clusters, built in %.1f ms\n", map->name.c_str(), map->width,
                map->height, static_cast<unsigned long long>(map->mapHash), g.clustersX(), g.clustersY(), buildMs);

    std::uint64_t allHash = 0;
    int rc = 0;
    for (const nav::MoveClass cls : classes) {
        // Starts on passable cells of this class; goals anywhere (blocked / other islands -> fallback).
        std::vector<std::uint32_t> passable;
        for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(map->cellCount()); ++c) {
            if (g.cost(cls, c) != 0) passable.push_back(c);
        }
        std::printf("\n[%s] %zu passable cells, %zu nodes, %zu edges\n", nav::moveClassName(cls), passable.size(),
                    nav.hpa.nodeCount(cls), nav.hpa.edgeCount(cls));
        if (passable.empty()) continue;
        Rng rng = Rng::derive(seed, 0x4E4156ULL + static_cast<std::uint64_t>(cls));
        nav::PathService service;
        for (std::uint32_t q = 0; q < queries; ++q) {
            const std::uint32_t s = passable[rng.below(static_cast<std::uint32_t>(passable.size()))];
            const std::uint32_t goal = rng.below(static_cast<std::uint32_t>(map->cellCount()));
            service.request(EntityId::make(q, 1), cls, s, goal, 0);
        }
        std::vector<nav::PathResult> results;
        std::uint32_t ticks = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (service.pending() != 0) {
            service.step(nav.hpa, budget, results);
            ++ticks;
        }
        const double ms = msSince(t0);

        std::vector<std::uint32_t> exp;
        std::uint64_t refined = 0, waypoints = 0;
        std::size_t found = 0, nearest = 0, unreachable = 0, noStart = 0;
        ByteWriter w;
        for (const nav::PathResult& r : results) {
            exp.push_back(r.expansions);
            w.u32(r.requestId);
            w.u8(static_cast<std::uint8_t>(r.status));
            w.u32(static_cast<std::uint32_t>(r.cost));
            w.u32(static_cast<std::uint32_t>(r.waypoints.size()));
            for (const std::uint32_t c : r.waypoints) w.u32(c);
            switch (r.status) {
            case nav::PathStatus::Found: ++found; break;
            case nav::PathStatus::FoundNearest: ++nearest; break;
            case nav::PathStatus::Unreachable: ++unreachable; break;
            case nav::PathStatus::NoStart: ++noStart; break;
            }
            refined += r.cells.size();
            waypoints += r.waypoints.size();
        }
        std::sort(exp.begin(), exp.end());
        std::uint64_t sum = 0;
        for (const std::uint32_t e : exp) sum += e;
        const auto pct = [&](std::size_t p) { return exp.empty() ? 0u : exp[std::min(exp.size() - 1, exp.size() * p / 100)]; };
        const std::size_t withPath = found + nearest;
        const std::uint64_t h = hashBytes(w.buf);
        allHash = allHash * 0x100000001B3ULL ^ h;
        std::printf("  %zu queries in %u ticks (budget %d): %.1f ms total, %.3f ms/query\n", results.size(), ticks, budget, ms,
                    results.empty() ? 0.0 : ms / static_cast<double>(results.size()));
        std::printf("  expansions avg %llu  p50 %u  p99 %u  max %u\n",
                    static_cast<unsigned long long>(exp.empty() ? 0 : sum / exp.size()), pct(50), pct(99), exp.empty() ? 0u : exp.back());
        std::printf("  found %zu, nearest-fallback %zu, unreachable %zu, no-start %zu\n", found, nearest, unreachable, noStart);
        if (withPath != 0) {
            std::printf("  avg path %llu cells -> %llu waypoints after smoothing\n",
                        static_cast<unsigned long long>(refined / withPath), static_cast<unsigned long long>(waypoints / withPath));
        }
        std::printf("  paths hash %016llx\n", static_cast<unsigned long long>(h));

        if (a.has("optimal")) {  // HPA* cost vs full-grid A* (mean <= 1.05x, worst <= 1.25x)
            nav::SearchScratch scratch;
            std::uint64_t ratioSum = 0;
            std::uint32_t worst = 0, n = 0;
            std::int32_t worstLen = 0;
            for (const nav::PathResult& r : results) {
                if (r.waypoints.empty() || r.cost == 0) continue;
                const nav::GridPath opt = gridAStar(g, cls, r.waypoints.front(), r.goal, nav::wholeMap(g), scratch);
                if (!opt.found || opt.cost == 0) continue;
                const auto permille = static_cast<std::uint32_t>(static_cast<std::int64_t>(r.cost) * 1000 / opt.cost);
                ratioSum += permille;
                if (permille > worst) {
                    worst = permille;
                    worstLen = opt.cost;
                }
                ++n;
            }
            if (n != 0) {
                std::printf("  cost vs optimal: avg %.3fx, worst %.3fx (optimal cost %d) over %u paths\n",
                            static_cast<double>(ratioSum) / n / 1000.0, worst / 1000.0, worstLen, n);
                if (worst > 1250 || ratioSum / n > 1050) rc = 1;  // the bounds sim/tests/test_nav.cpp enforces
            }
        }
    }
    std::printf("\nNAV BENCH HASH %016llx\n", static_cast<unsigned long long>(allHash));
    return rc;
}

}  // namespace simcli
