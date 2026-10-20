// M4 part 1: maps, nav grid, labels, line of sight, HPA*, PathService (budget, fallback, stamps),
// sim integration and nav state in saves and replays.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "core/sim.hpp"
#include "data/json_lite.hpp"
#include "math/rng.hpp"
#include "nav/nav_world.hpp"
#include "session/bytes.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"
#include "test_env.hpp"
#include "vfs/path.hpp"

using namespace corps;
using namespace corps::nav;
using namespace corps::fixed_literals;

namespace {

// A hand-drawn map: '.' ground, '#' mountain, '~' water, 's' shallow, 'f' forest, 'r' road, 'b' beach.
MapDef asciiMap(const std::vector<std::string>& rows) {
    MapDef m;
    m.name = "ascii";
    m.height = static_cast<std::uint32_t>(rows.size());
    m.width = static_cast<std::uint32_t>(rows[0].size());
    m.terrainNames = {"t"};
    for (const std::string& r : rows) {
        for (const char ch : r) {
            TerrainKind k = TerrainKind::Ground;
            switch (ch) {
            case '#': k = TerrainKind::Mountain; break;
            case '~': k = TerrainKind::Water; break;
            case 's': k = TerrainKind::Shallow; break;
            case 'f': k = TerrainKind::Forest; break;
            case 'r': k = TerrainKind::Road; break;
            case 'b': k = TerrainKind::Beach; break;
            default: break;
            }
            m.kind.push_back(k);
        }
    }
    m.terrainId.assign(m.cellCount(), 0);
    REQUIRE(m.finalize());
    return m;
}

MapDef openMap(std::uint32_t w, std::uint32_t h) {
    return asciiMap(std::vector<std::string>(h, std::string(w, '.')));
}

// Every step of a cell path is a legal move; returns the summed step cost (-1 if illegal).
std::int64_t walkCost(const NavGrid& g, MoveClass c, const std::vector<std::uint32_t>& cells) {
    std::int64_t sum = 0;
    for (std::size_t i = 1; i < cells.size(); ++i) {
        const std::int32_t x = g.xOf(cells[i - 1]), y = g.yOf(cells[i - 1]);
        const std::int32_t dx = g.xOf(cells[i]) - x, dy = g.yOf(cells[i]) - y;
        int dir = -1;
        for (int d = 0; d < 8; ++d) {
            if (kDirX[d] == dx && kDirY[d] == dy) dir = d;
        }
        if (dir < 0) return -1;
        const std::int32_t s = g.stepCost(c, x, y, dir);
        if (s == 0) return -1;
        sum += s;
    }
    return sum;
}

void hashResult(ByteWriter& w, const PathResult& r) {
    w.u32(r.requestId);
    w.u8(static_cast<std::uint8_t>(r.status));
    w.u32(static_cast<std::uint32_t>(r.cost));
    w.u32(r.goal);
    w.u32(static_cast<std::uint32_t>(r.waypoints.size()));
    for (const std::uint32_t c : r.waypoints) w.u32(c);
    w.u32(static_cast<std::uint32_t>(r.cells.size()));
    for (const std::uint32_t c : r.cells) w.u32(c);
}

bool sameResult(const PathResult& a, const PathResult& b) {
    return a.status == b.status && a.cost == b.cost && a.goal == b.goal && a.waypoints == b.waypoints && a.cells == b.cells;
}

struct Query {
    std::uint32_t start, goal;
};

std::vector<Query> randomQueries(const NavGrid& g, MoveClass c, std::uint64_t seed, std::uint32_t n) {
    std::vector<std::uint32_t> passable;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(g.width() * g.height()); ++i) {
        if (g.cost(c, i) != 0) passable.push_back(i);
    }
    std::vector<Query> q;
    if (passable.empty()) return q;
    Rng rng = Rng::derive(seed, 0x51ULL + static_cast<std::uint64_t>(c));
    for (std::uint32_t i = 0; i < n; ++i) {
        q.push_back({passable[rng.below(static_cast<std::uint32_t>(passable.size()))],
                     rng.below(static_cast<std::uint32_t>(g.width() * g.height()))});
    }
    return q;
}

std::vector<PathResult> serveAll(NavWorld& nav, MoveClass c, const std::vector<Query>& qs, std::int32_t budget, std::uint32_t* ticks = nullptr) {
    PathService s;
    for (std::uint32_t i = 0; i < qs.size(); ++i) s.request(EntityId::make(i, 1), c, qs[i].start, qs[i].goal, 0);
    std::vector<PathResult> out;
    std::uint32_t t = 0;
    while (s.pending() != 0) {
        s.step(nav.hpa, budget, out);
        ++t;
    }
    if (ticks != nullptr) *ticks = t;
    return out;
}

}  // namespace

// ------------------------------------------------------------------------------------- json

TEST_CASE("json_lite parses rmp.py-style documents strictly") {
    const json::ParseResult ok = json::parse(R"({"a": [1, -2, 3.5e2], "b": {"c": "x\"y\u00e9"}, "d": true, "e": null})");
    REQUIRE(ok.ok);
    const json::Value* a = ok.value.get("a");
    REQUIRE(a != nullptr);
    REQUIRE(a->isArray());
    CHECK(a->items.size() == 3);
    CHECK(a->items[1].asInt() == -2);
    CHECK_FALSE(a->items[2].asInt().has_value());  // not an integer
    CHECK(ok.value.get("b")->get("c")->text == "x\"y\xC3\xA9");
    CHECK(ok.value.get("missing") == nullptr);
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "01", "[1] x", "\"\\q\"", "{\"a\":1,}", "nul", "-", "1.", "1e", "--1", "1-2"}) {
        CAPTURE(std::string(bad));
        CHECK_FALSE(json::parse(bad).ok);
    }
    CHECK_FALSE(json::parse(std::string(200, '[') + std::string(200, ']')).ok);  // depth limit
}

// ------------------------------------------------------------------------------------- maps

TEST_CASE("generated test maps are deterministic and contain every feature") {
    const MapDef a = generateTestMap(7, 128);
    const MapDef b = generateTestMap(7, 128);
    const MapDef c = generateTestMap(8, 128);
    CHECK(a.name == "gen:7:128");
    CHECK(a.mapHash == b.mapHash);
    CHECK(a.kind == b.kind);
    CHECK(a.mapHash != c.mapHash);
    std::array<std::size_t, kTerrainKindCount> counts{};
    for (const TerrainKind k : a.kind) ++counts[static_cast<std::size_t>(k)];
    for (std::size_t k = 0; k < kTerrainKindCount; ++k) {
        CAPTURE(terrainKindName(static_cast<TerrainKind>(k)));
        CHECK(counts[k] > 0);
    }
    for (std::uint32_t i = 0; i < a.width; ++i) {  // border ring
        CHECK(a.kind[i] == TerrainKind::Border);
        CHECK(a.kind[(a.height - 1) * a.width + i] == TerrainKind::Border);
        CHECK(a.kind[i * a.width] == TerrainKind::Border);
    }
    std::uint64_t seed = 0;
    std::uint32_t size = 0;
    REQUIRE(parseGenSpec(a.name, seed, size));
    CHECK(seed == 7);
    CHECK(size == 128);
    CHECK(generateTestMap(seed, size).mapHash == a.mapHash);
    CHECK_FALSE(parseGenSpec("gen:7", seed, size));
    CHECK_FALSE(parseGenSpec("gen:x:128", seed, size));
    CHECK_FALSE(parseGenSpec("gen:7:99999", seed, size));
    CHECK_FALSE(parseGenSpec("golden", seed, size));
    CHECK(generateTestMap(1, 5).width == 32);  // clamped
}

TEST_CASE("rmp.py JSON loads into 2x2 cells per tile with kinds and heights") {
    // 2x2 tiles: ground, forest / water (shallows), mountain.
    const std::string j = R"({"source": "Maps/test.RMP", "tiles_per_side": 2, "layers": {
        "terrain_names": ["grass", "forest_01", "water_shallows", "rock"],
        "tile_terrain_index": [0, 1, 2, 3],
        "tile_class_bits": [0, 0, 32, 16],
        "vertex_height_mm": [0, 1000, 2000, 0, 0, 0, 500, 500, 500],
        "vertex_mountain_mm": [0, 0, 0, 0, 0, 0, 0, 0, 250]}})";
    MapDef m;
    std::string err;
    REQUIRE_MESSAGE(loadRmpJson(j, m, err), err);
    CHECK(m.name == "Maps/test.RMP");
    CHECK(m.width == 4);
    CHECK(m.height == 4);
    CHECK(m.kind[0] == TerrainKind::Ground);
    CHECK(m.kind[1 * 4 + 1] == TerrainKind::Ground);
    CHECK(m.kind[0 * 4 + 2] == TerrainKind::Forest);
    CHECK(m.kind[3 * 4 + 3 - 3] == TerrainKind::Shallow);  // (0, 3)
    CHECK(m.kind[3 * 4 + 3] == TerrainKind::Mountain);
    CHECK(m.heightWidth == 3);
    CHECK(m.heights[1] == 1_fx);
    CHECK(m.heights[8] == Fixed::fromRatio(750, 1000));
    CHECK(m.mapHash != 0);

    const std::string noHeights = R"({"tiles_per_side": 1, "layers": {"terrain_names": ["a"],
        "tile_terrain_index": [0], "tile_class_bits": [0]}})";
    CHECK_FALSE(loadRmpJson(noHeights, m, err));
    CHECK(err.find("vertex_height_mm") != std::string::npos);
    CHECK_FALSE(loadRmpJson(R"({"tiles_per_side": 1, "layers": {"terrain_names": ["a"], "tile_terrain_index": [5],
        "tile_class_bits": [0], "vertex_height_mm": [0,0,0,0], "vertex_mountain_mm": [0,0,0,0]}})", m, err));
    CHECK_FALSE(loadRmpJson("[1, 2]", m, err));
}

TEST_CASE("Golden Fields from tools/rmp.py loads and paths (skipped without cache/maps)") {
    std::ifstream in(vfs::pathFromUtf8(CORPS_SOURCE_DIR "/cache/maps/golden_fields/map.json"), std::ios::binary);
    if (!in) {
        MESSAGE("cache/maps/golden_fields/map.json absent (run tools/rmp.py); skipped");
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    MapDef m;
    std::string err;
    REQUIRE_MESSAGE(loadRmpJson(text, m, err), err);
    CHECK(m.width == 256);
    CHECK(m.height == 256);
    std::array<std::size_t, kTerrainKindCount> counts{};
    for (const TerrainKind k : m.kind) ++counts[static_cast<std::size_t>(k)];
    CHECK(counts[static_cast<std::size_t>(TerrainKind::Ground)] > 10000);
    CHECK(counts[static_cast<std::size_t>(TerrainKind::Water)] + counts[static_cast<std::size_t>(TerrainKind::Shallow)] > 0);
    NavWorld nav(m);
    const auto qs = randomQueries(nav.grid, MoveClass::Tracked, 5, 50);
    for (const PathResult& r : serveAll(nav, MoveClass::Tracked, qs, kPathBudgetPerTick)) {
        REQUIRE((r.status == PathStatus::Found || r.status == PathStatus::FoundNearest));
        CHECK(walkCost(nav.grid, MoveClass::Tracked, r.cells) == r.cost);
    }
}

// ------------------------------------------------------------------------------------- grid

TEST_CASE("cost grid, labels and the no-corner-cutting rule") {
    const MapDef m = asciiMap({
        "..#..",
        "..#..",
        "###..",
        "...#.",
        "....#",
    });
    NavGrid g(m);
    const auto at = [&](int x, int y) { return g.index(x, y); };
    CHECK(g.cost(MoveClass::Foot, at(0, 0)) == kCostUnit);
    CHECK(g.cost(MoveClass::Foot, at(2, 0)) == 0);
    CHECK(g.cost(MoveClass::Naval, at(0, 0)) == 0);
    // top-left pocket is closed off; (3,3)-(4,4) diagonal gap is not a passage (no corner cutting)
    CHECK(g.label(MoveClass::Foot, at(0, 0)) == g.label(MoveClass::Foot, at(1, 1)));
    CHECK(g.label(MoveClass::Foot, at(0, 0)) != g.label(MoveClass::Foot, at(4, 0)));
    CHECK(g.label(MoveClass::Foot, at(0, 4)) != g.label(MoveClass::Foot, at(4, 3)));
    CHECK(g.label(MoveClass::Foot, at(2, 0)) == 0);
    CHECK(g.stepCost(MoveClass::Foot, 3, 4, 6) == 0);  // NE from (3,4) to (4,3) cuts two corners
    CHECK(g.stepCost(MoveClass::Foot, 0, 0, 4) == kDiagonal * 2 * kCostUnit);
    const CellRect& box = g.labelBox(MoveClass::Foot, g.label(MoveClass::Foot, at(0, 0)));
    CHECK(box.x0 == 0);
    CHECK(box.y0 == 0);
    CHECK(box.x1 == 1);
    CHECK(box.y1 == 1);

    // Forest costs more on wheels; roads less.
    const MapDef t = asciiMap({"frb~s"});
    NavGrid tg(t);
    CHECK(tg.cost(MoveClass::Wheeled, 0) > tg.cost(MoveClass::Foot, 0));
    CHECK(tg.cost(MoveClass::Wheeled, 1) < kCostUnit);
    CHECK(tg.cost(MoveClass::Amphibious, 3) != 0);
    CHECK(tg.cost(MoveClass::Naval, 4) != 0);
    CHECK(tg.cost(MoveClass::Foot, 4) == 0);
    CHECK(tg.minCost(MoveClass::Wheeled) == tg.cost(MoveClass::Wheeled, 1));
}

TEST_CASE("line of sight: supercover, corners and cost ceiling") {
    const MapDef m = asciiMap({
        "..........",
        "....#.....",
        "..........",
        ".....ff...",
        "..........",
    });
    NavGrid g(m);
    const auto at = [&](int x, int y) { return g.index(x, y); };
    const std::uint8_t any = 255;
    CHECK(g.lineOfSight(MoveClass::Foot, at(0, 0), at(9, 0), any));
    CHECK_FALSE(g.lineOfSight(MoveClass::Foot, at(0, 1), at(9, 1), any));       // through the rock
    CHECK_FALSE(g.lineOfSight(MoveClass::Foot, at(3, 0), at(5, 2), any));       // exact corner past (4,1)
    CHECK(g.lineOfSight(MoveClass::Foot, at(0, 2), at(9, 2), any));
    CHECK(g.lineOfSight(MoveClass::Foot, at(0, 3), at(9, 3), any));
    CHECK_FALSE(g.lineOfSight(MoveClass::Foot, at(0, 3), at(9, 3), kCostUnit));  // forest above the ceiling
    CHECK(g.lineOfSight(MoveClass::Foot, at(2, 2), at(2, 2), any));
    CHECK(g.lineOfSight(MoveClass::Foot, at(9, 4), at(0, 0), any) == g.lineOfSight(MoveClass::Foot, at(0, 0), at(9, 4), any));
}

TEST_CASE("smoothing keeps only corners and every shortcut has line of sight") {
    const MapDef m = openMap(40, 40);
    NavWorld nav(m);
    const PathResult r = nav.paths.solveNow(nav.hpa, MoveClass::Foot, nav.grid.index(1, 1), nav.grid.index(38, 30));
    REQUIRE(r.status == PathStatus::Found);
    CHECK(r.waypoints.size() == 2);  // open field: one straight line
    CHECK(r.waypoints.front() == nav.grid.index(1, 1));
    CHECK(r.waypoints.back() == nav.grid.index(38, 30));
    CHECK(r.cost == octile(37, 29) * 2 * kCostUnit);
}

// ------------------------------------------------------------------------------------- HPA*

TEST_CASE("HPA* paths are legal, end at the goal and stay close to optimal") {
    for (const std::uint64_t seed : {3ULL, 11ULL, 29ULL}) {
        const MapDef m = generateTestMap(seed, 160);
        NavWorld nav(m);
        SearchScratch scratch;
        for (std::size_t ci = 0; ci < kMoveClassCount; ++ci) {
            const auto c = static_cast<MoveClass>(ci);
            CAPTURE(seed);
            CAPTURE(std::string(moveClassName(c)));
            const auto qs = randomQueries(nav.grid, c, seed, 60);
            const auto results = serveAll(nav, c, qs, kPathBudgetPerTick);
            REQUIRE(results.size() == qs.size());
            std::int64_t ratioSum = 0;
            int n = 0;
            for (std::size_t i = 0; i < results.size(); ++i) {
                const PathResult& r = results[i];
                REQUIRE((r.status == PathStatus::Found || r.status == PathStatus::FoundNearest));
                REQUIRE(!r.cells.empty());
                CHECK(r.cells.front() == qs[i].start);
                CHECK(r.cells.back() == r.goal);
                CHECK(r.waypoints.front() == r.cells.front());
                CHECK(r.waypoints.back() == r.cells.back());
                CHECK(walkCost(nav.grid, c, r.cells) == r.cost);
                for (std::size_t k = 1; k < r.waypoints.size(); ++k) {
                    CHECK(nav.grid.lineOfSight(c, r.waypoints[k - 1], r.waypoints[k], 255));
                }
                if (r.status == PathStatus::Found) {
                    CHECK(r.goal == qs[i].goal);
                } else {
                    CHECK(nav.grid.label(c, qs[i].goal) != nav.grid.label(c, qs[i].start));
                    CHECK(nav.grid.label(c, r.goal) == nav.grid.label(c, qs[i].start));
                }
                const GridPath opt = gridAStar(nav.grid, c, r.cells.front(), r.goal, wholeMap(nav.grid), scratch);
                REQUIRE(opt.found);
                CHECK(r.cost >= opt.cost);
                if (opt.cost > 0) {
                    const std::int64_t permille = static_cast<std::int64_t>(r.cost) * 1000 / opt.cost;
                    CAPTURE(opt.cost);
                    CAPTURE(nav.grid.xOf(r.cells.front()));
                    CAPTURE(nav.grid.yOf(r.cells.front()));
                    CAPTURE(nav.grid.xOf(r.goal));
                    CAPTURE(nav.grid.yOf(r.goal));
                    CHECK(permille <= 1250);
                    ratioSum += permille;
                    ++n;
                }
            }
            if (n > 0) CHECK(ratioSum / n <= 1050);
        }
    }
}

TEST_CASE("results do not depend on the expansion budget (suspension is invisible)") {
    const MapDef m = generateTestMap(5, 128);
    NavWorld nav(m);
    for (const MoveClass c : {MoveClass::Foot, MoveClass::Naval}) {
        const auto qs = randomQueries(nav.grid, c, 77, 40);
        std::uint32_t ticksBig = 0, ticksSmall = 0;
        const auto big = serveAll(nav, c, qs, kPathBudgetPerTick, &ticksBig);
        const auto small = serveAll(nav, c, qs, 37, &ticksSmall);
        const auto one = serveAll(nav, c, qs, 1);
        REQUIRE(big.size() == qs.size());
        REQUIRE(small.size() == qs.size());
        REQUIRE(one.size() == qs.size());
        CHECK(ticksSmall > ticksBig);
        for (std::size_t i = 0; i < qs.size(); ++i) {
            CHECK(big[i].requestId == small[i].requestId);
            CHECK(sameResult(big[i], small[i]));
            CHECK(sameResult(big[i], one[i]));
            CHECK(sameResult(big[i], nav.paths.solveNow(nav.hpa, c, qs[i].start, qs[i].goal)));
        }
    }
}

TEST_CASE("PathService queue order is (tick, requester, id) and debt carries over") {
    const MapDef m = openMap(64, 64);
    NavWorld nav(m);
    PathService s;
    const std::uint32_t a = s.request(EntityId::make(9, 1), MoveClass::Foot, 0, 4000, 5);
    const std::uint32_t b = s.request(EntityId::make(2, 1), MoveClass::Foot, 0, 4000, 5);
    const std::uint32_t c = s.request(EntityId::make(30, 1), MoveClass::Foot, 0, 4000, 4);
    std::vector<PathResult> out;
    while (s.pending() != 0) s.step(nav.hpa, 1000000, out);
    REQUIRE(out.size() == 3);
    CHECK(out[0].requestId == c);
    CHECK(out[1].requestId == b);
    CHECK(out[2].requestId == a);

    // One long search with a tiny budget overruns; the overrun is paid back on the next ticks.
    s.request(EntityId::make(1, 1), MoveClass::Foot, nav.grid.index(1, 1), nav.grid.index(62, 62), 0);
    out.clear();
    s.step(nav.hpa, 10, out);
    CHECK(s.debt() > 0);
    const std::int32_t debt = s.debt();
    s.step(nav.hpa, 10, out);
    CHECK(s.debt() == std::max(0, debt - 10));
}

TEST_CASE("unreachable goals fall back to the nearest reachable cell; blocked starts move off") {
    const MapDef m = asciiMap({
        "..........",
        "..........",
        "...~~~~...",
        "...~~~~...",
        "...~~~~...",
        "..........",
    });
    NavWorld nav(m);
    const auto at = [&](int x, int y) { return nav.grid.index(x, y); };
    // Foot to the middle of the lake: nearest shore cell by distance², ties by cell index.
    const PathResult r = nav.paths.solveNow(nav.hpa, MoveClass::Foot, at(0, 0), at(4, 3));
    REQUIRE(r.status == PathStatus::FoundNearest);
    CHECK(r.goal == at(4, 1));  // (4,1) and (2,3) both at d²=4; (4,1) has the lower index... check below
    std::int64_t bestD2 = INT64_MAX;
    std::uint32_t best = 0;
    for (std::uint32_t cell = 0; cell < m.cellCount(); ++cell) {
        if (nav.grid.label(MoveClass::Foot, cell) != nav.grid.label(MoveClass::Foot, at(0, 0))) continue;
        const std::int64_t dx = nav.grid.xOf(cell) - 4, dy = nav.grid.yOf(cell) - 3;
        if (dx * dx + dy * dy < bestD2) {
            bestD2 = dx * dx + dy * dy;
            best = cell;
        }
    }
    CHECK(r.goal == best);
    // Naval from the lake to land: nearest water cell.
    const PathResult n = nav.paths.solveNow(nav.hpa, MoveClass::Naval, at(5, 3), at(9, 0));
    REQUIRE(n.status == PathStatus::FoundNearest);
    CHECK(n.goal == at(6, 2));
    // Naval with its start on land moves to the nearest water cell first.
    const PathResult s = nav.paths.solveNow(nav.hpa, MoveClass::Naval, at(0, 3), at(5, 3));
    REQUIRE(s.status == PathStatus::Found);
    CHECK(s.cells.front() == at(3, 3));
    // Out-of-range cells are a NoStart, not a crash.
    CHECK(nav.paths.solveNow(nav.hpa, MoveClass::Foot, 999999, 0).status == PathStatus::NoStart);
    // A class with no passable cell at all.
    const MapDef dry = openMap(8, 8);
    NavWorld dryNav(dry);
    CHECK(dryNav.paths.solveNow(dryNav.hpa, MoveClass::Naval, 0, 5).status == PathStatus::NoStart);
}

TEST_CASE("stamping reroutes, unstamping restores, and the incremental rebuild equals a full one") {
    const MapDef m = openMap(96, 64);
    NavWorld nav(m);
    const auto at = [&](int x, int y) { return nav.grid.index(x, y); };
    const PathResult before = nav.paths.solveNow(nav.hpa, MoveClass::Tracked, at(5, 30), at(90, 30));
    REQUIRE(before.status == PathStatus::Found);
    const std::uint32_t v0 = nav.hpa.version();

    const CellRect wall{40, 10, 43, 55};
    nav.grid.stamp(wall);
    CHECK(!nav.grid.dirtyClusters().empty());
    CHECK(std::is_sorted(nav.grid.dirtyClusters().begin(), nav.grid.dirtyClusters().end()));
    nav.hpa.rebuildDirty();
    CHECK(nav.grid.dirtyClusters().empty());
    CHECK(nav.hpa.version() == v0 + 1);
    const PathResult around = nav.paths.solveNow(nav.hpa, MoveClass::Tracked, at(5, 30), at(90, 30));
    REQUIRE(around.status == PathStatus::Found);
    CHECK(around.cost > before.cost);
    for (const std::uint32_t cell : around.cells) CHECK(nav.grid.stampCount(cell) == 0);
    CHECK(walkCost(nav.grid, MoveClass::Tracked, around.cells) == around.cost);

    // Graph equals a from-scratch build with the same stamps.
    NavWorld fresh(m);
    fresh.grid.stamp(wall);
    fresh.hpa.rebuildAll();
    for (std::size_t ci = 0; ci < kMoveClassCount; ++ci) {
        const auto c = static_cast<MoveClass>(ci);
        for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(nav.grid.clustersX() * nav.grid.clustersY()); ++k) {
            const ClusterGraph& x = nav.hpa.cluster(c, k);
            const ClusterGraph& y = fresh.hpa.cluster(c, k);
            REQUIRE(x.nodes == y.nodes);
            REQUIRE(x.edges.size() == y.edges.size());
            for (std::size_t i = 0; i < x.edges.size(); ++i) {
                REQUIRE(x.edges[i].size() == y.edges[i].size());
                for (std::size_t e = 0; e < x.edges[i].size(); ++e) {
                    CHECK(x.edges[i][e].cell == y.edges[i][e].cell);
                    CHECK(x.edges[i][e].cost == y.edges[i][e].cost);
                    CHECK(x.edges[i][e].node == y.edges[i][e].node);
                    CHECK(nav.hpa.nodeCell(c, x.edges[i][e].node) == x.edges[i][e].cell);
                }
            }
        }
        for (std::uint32_t cell = 0; cell < m.cellCount(); ++cell) {
            if (nav.grid.label(c, cell) != fresh.grid.label(c, cell)) {
                FAIL_CHECK("labels differ");
                break;
            }
        }
    }

    // Stamps are ref-counted: two overlapping stamps need two unstamps.
    nav.grid.stamp({40, 20, 41, 21});
    nav.grid.unstamp(wall);
    CHECK(nav.grid.stampCount(at(40, 20)) == 1);
    nav.grid.unstamp({40, 20, 41, 21});
    nav.hpa.rebuildDirty();
    const PathResult after = nav.paths.solveNow(nav.hpa, MoveClass::Tracked, at(5, 30), at(90, 30));
    CHECK(sameResult(after, before));
}

TEST_CASE("a job restarts when the graph changes while it is suspended") {
    const MapDef m = openMap(128, 64);
    NavWorld nav(m);
    const auto at = [&](int x, int y) { return nav.grid.index(x, y); };
    PathService s;
    s.request(EntityId::make(1, 1), MoveClass::Foot, at(2, 32), at(125, 32), 0);
    std::vector<PathResult> out;
    s.step(nav.hpa, 1, out);  // runs the abstract search, then suspends before refining
    REQUIRE(out.empty());
    nav.grid.stamp({60, 0, 61, 50});
    nav.hpa.rebuildDirty();
    while (s.pending() != 0) s.step(nav.hpa, 1, out);
    REQUIRE(out.size() == 1);
    CHECK(sameResult(out[0], nav.paths.solveNow(nav.hpa, MoveClass::Foot, at(2, 32), at(125, 32))));
    for (const std::uint32_t cell : out[0].cells) CHECK(nav.grid.stampCount(cell) == 0);
}

// ------------------------------------------------------------------------------------- golden

TEST_CASE("golden path hashes on generated maps (cross-compiler, cross-platform)") {
    // Regenerate with CORPS_PRINT_GOLDEN=1 after a deliberate change to the map generator, the
    // cost table, HPA*, fallback or smoothing.
    struct Golden {
        std::uint64_t seed;
        std::uint32_t size;
        std::uint64_t mapHash;
        std::uint64_t pathsHash;
    };
    const Golden golden[] = {
        {1, 128, 0x2cb7186371c9b72bULL, 0x19f81ecd1752bcfbULL},
        {2, 192, 0x3e825325fef8669cULL, 0x94b152b48a95fbf0ULL},
        {3, 256, 0xa45fa95b4d5edc03ULL, 0xaa375c3f5bea4754ULL},
    };
    const bool print = testEnvSet("CORPS_PRINT_GOLDEN");
    for (const Golden& gd : golden) {
        const MapDef m = generateTestMap(gd.seed, gd.size);
        NavWorld nav(m);
        ByteWriter w;
        for (std::size_t ci = 0; ci < kMoveClassCount; ++ci) {
            const auto c = static_cast<MoveClass>(ci);
            for (const PathResult& r : serveAll(nav, c, randomQueries(nav.grid, c, gd.seed, 40), kPathBudgetPerTick)) hashResult(w, r);
        }
        const std::uint64_t h = hashBytes(w.buf);
        if (print) {
            std::printf("        {%llu, %u, 0x%016llxULL, 0x%016llxULL},\n", static_cast<unsigned long long>(gd.seed), gd.size,
                        static_cast<unsigned long long>(m.mapHash), static_cast<unsigned long long>(h));
            continue;
        }
        CAPTURE(gd.seed);
        CHECK(m.mapHash == gd.mapHash);
        CHECK(h == gd.pathsHash);
    }
}

// ------------------------------------------------------------------------------------- sim

TEST_CASE("Sim: blockers stamp the grid, path requests come back as Path components") {
    const MapDef m = openMap(80, 80);
    Sim sim({3, nullptr, &m});
    REQUIRE(sim.nav() != nullptr);
    sim.step({makeDebugSpawn(0, 0, 0, {5_fx, 40_fx}, Angle{}), makeDebugPlaceBlocker(0, 0, 1, {40_fx, 40_fx}, {2_fx, 30_fx})});
    const EntityId unit = sim.world().transforms.ids()[0];
    const EntityId blocker = sim.world().footprints.ids()[0];
    const Footprint& fp = sim.world().footprints.data()[0];
    CHECK(fp.x0 == 38);
    CHECK(fp.x1 == 41);
    CHECK(fp.y0 == 10);
    CHECK(fp.y1 == 69);
    CHECK(sim.nav()->grid.stampCount(sim.nav()->grid.index(40, 40)) == 1);
    CHECK(sim.nav()->grid.dirtyClusters().empty());  // rebuilt in this tick's Pathfinding stage

    sim.step({makeDebugRequestPath(1, 0, 0, unit, {75_fx, 40_fx}, static_cast<std::uint8_t>(MoveClass::Foot))});
    const Path* p = sim.world().paths.get(unit);
    REQUIRE(p != nullptr);
    CHECK(p->status == Path::Status::Found);  // one tick: the budget covers it
    REQUIRE(p->waypoints.size() >= 3);        // around the wall
    CHECK(p->waypoints.front() == sim.cellAt({5_fx, 40_fx}));
    CHECK(p->waypoints.back() == sim.cellAt({75_fx, 40_fx}));

    // Destroying the blocker unstamps it at Cleanup; the next tick's rebuild opens the way.
    const std::int32_t aroundCost = p->cost;
    sim.step({makeDebugDestroy(2, 0, 0, blocker)});
    CHECK(sim.nav()->grid.stampCount(sim.nav()->grid.index(40, 40)) == 0);
    CHECK_FALSE(sim.nav()->grid.dirtyClusters().empty());
    sim.step({makeDebugRequestPath(3, 0, 0, unit, {75_fx, 40_fx}, static_cast<std::uint8_t>(MoveClass::Foot))});
    const Path* straight = sim.world().paths.get(unit);
    CHECK(straight->cost < aroundCost);
    CHECK(straight->cost <= 70 * kStraight * 2 * kCostUnit * 11 / 10);  // about the straight row
    CHECK(straight->waypoints.size() <= 3);  // 70 cells > the 64-cell smoothing window

    // Bad requests are rejected, not crashes.
    sim.step({makeDebugRequestPath(4, 0, 0, blocker, {1_fx, 1_fx}, 0), makeDebugRequestPath(4, 0, 1, unit, {1_fx, 1_fx}, 9),
              makeDebugPlaceBlocker(4, 0, 2, {500_fx, 500_fx}, {1_fx, 1_fx}), makeDebugPlaceBlocker(4, 0, 3, {5_fx, 5_fx}, {0_fx, 1_fx})});
    const auto rejected = std::count_if(sim.events().begin(), sim.events().end(),
                                        [](const SimEvent& e) { return e.type == SimEvent::Type::CommandRejected; });
    CHECK(rejected == 4);

    Sim noMap({3, nullptr});
    noMap.step({makeDebugSpawn(0, 0, 0, {}, Angle{})});
    noMap.step({makeDebugRequestPath(1, 0, 0, noMap.world().transforms.ids()[0], {}, 0), makeDebugPlaceBlocker(1, 0, 1, {}, {1_fx, 1_fx})});
    CHECK(noMap.events().size() == 2);
}

TEST_CASE("new nav commands round-trip through the byte encoding") {
    const Command cmds[] = {makeDebugPlaceBlocker(4, 1, 2, {-3_fx, 7_fx}, {2_fx, 1_fx}),
                            makeDebugRequestPath(9, 0, 3, EntityId::make(17, 3), {100_fx, 5_fx}, 4)};
    for (const Command& c : cmds) {
        std::vector<std::uint8_t> bytes;
        encodeCommand(c, bytes);
        Command back;
        back.tick = c.tick;
        back.seq = c.seq;
        std::size_t pos = 0;
        REQUIRE(decodeCommand(bytes, pos, back));
        CHECK(pos == bytes.size());
        CHECK(back == c);
        for (std::size_t cut = 0; cut < bytes.size(); ++cut) {  // truncations fail cleanly
            std::vector<std::uint8_t> shorter(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
            std::size_t p2 = 0;
            Command x;
            CHECK_FALSE(decodeCommand(shorter, p2, x));
        }
    }
}

TEST_CASE("save/load mid-search and with pending dirty clusters continues identically") {
    const MapDef m = generateTestMap(21, 128);
    // Scenario: 4 units and 2 blockers; 60 path requests at tick 1 (several ticks of budget);
    // a new blocker at tick 3 (graph changes under suspended jobs); blocker destroyed at tick 5
    // (unstamped at Cleanup: dirty clusters pending across the tick boundary).
    std::vector<std::vector<Command>> ticks(16);
    Rng host(9, 9);
    std::uint16_t seq = 0;
    for (int i = 0; i < 4; ++i) {
        ticks[0].push_back(makeDebugSpawn(0, 0, seq++, {Fixed::fromInt(20 + 25 * i), Fixed::fromInt(20 + 25 * i)}, Angle{}));
    }
    ticks[0].push_back(makeDebugPlaceBlocker(0, 0, seq++, {64_fx, 64_fx}, {3_fx, 20_fx}));
    ticks[0].push_back(makeDebugPlaceBlocker(0, 0, seq++, {40_fx, 100_fx}, {12_fx, 2_fx}));
    for (int i = 0; i < 60; ++i) {
        const EntityId unit = EntityId::make(static_cast<std::uint32_t>(i % 4), 1);
        ticks[1].push_back(makeDebugRequestPath(1, 0, seq++, unit, {Fixed::fromInt(host.range(2, 125)), Fixed::fromInt(host.range(2, 125))},
                                                static_cast<std::uint8_t>(host.below(static_cast<std::uint32_t>(kMoveClassCount)))));
    }
    ticks[3].push_back(makeDebugPlaceBlocker(3, 0, seq++, {90_fx, 40_fx}, {8_fx, 8_fx}));
    ticks[5].push_back(makeDebugDestroy(5, 0, seq++, EntityId::make(4, 1)));

    const auto runFrom = [&](Sim& s, std::uint32_t from, std::uint32_t to) {
        for (std::uint32_t t = from; t < to; ++t) s.step(ticks[t]);
    };
    Sim ref({5, nullptr, &m});
    runFrom(ref, 0, 16);
    const std::uint64_t want = stateHash(ref);

    bool sawActive = false, sawDirty = false;
    std::vector<std::uint8_t> saved3;
    Sim a({5, nullptr, &m});
    for (std::uint32_t cut = 1; cut < 9; ++cut) {
        CAPTURE(cut);
        a.step(ticks[cut - 1]);  // a has now run ticks [0, cut)
        sawActive |= a.nav()->paths.pending() != 0;
        sawDirty |= !a.nav()->grid.dirtyClusters().empty();
        const auto saved = saveState(a);
        Sim b({5, nullptr, &m});
        std::string err;
        REQUIRE_MESSAGE(loadState(b, saved, &err), err);
        CHECK(saveState(b) == saved);
        runFrom(b, cut, 16);
        CHECK(stateHash(b) == want);
        if (cut == 3) saved3 = saved;
    }
    // Loading into a sim that already has other footprints swaps the stamps correctly.
    {
        std::string err;
        REQUIRE(loadState(a, saved3, &err));  // a is at tick 8 with its own blockers
        CHECK(saveState(a) == saved3);
        runFrom(a, 3, 16);
        CHECK(stateHash(a) == want);
    }
    CHECK(sawActive);
    CHECK(sawDirty);
    std::size_t done = 0;
    for (const Path& p : ref.world().paths.data()) done += p.status != Path::Status::Pending;
    CHECK(done == 4);  // each unit's latest request finished

    // A synthetic map-backed run matches its recording.
    // Wrong map refuses to load.
    const MapDef other = generateTestMap(22, 128);
    Sim c({5, nullptr, &other});
    std::string err;
    CHECK_FALSE(loadState(c, saveState(ref), &err));
    Sim d({5, nullptr});
    CHECK_FALSE(loadState(d, saveState(ref), &err));
}

TEST_CASE("map-backed replays record path traffic and resolve their map") {
    const MapDef m = generateTestMap(13, 64);
    const Replay rep = recordSynthetic(13, 14, 100, 4, nullptr, {}, &m);
    CHECK(rep.header.mapSpec == "gen:13:64");
    CHECK(rep.header.mapHash == m.mapHash);
    std::size_t paths = 0, blockers = 0;
    for (const Command& c : rep.commands) {
        paths += c.type == CommandType::DebugRequestPath;
        blockers += c.type == CommandType::DebugPlaceBlocker;
    }
    CHECK(paths > 50);
    CHECK(blockers > 10);

    std::vector<std::uint8_t> bytes = encodeReplay(rep);
    Replay back;
    std::string err;
    REQUIRE(decodeReplay(bytes, back, &err));
    std::optional<MapDef> resolved;
    REQUIRE(resolveReplayMap(back.header, nullptr, resolved, err));
    REQUIRE(resolved.has_value());
    CHECK(resolved->mapHash == m.mapHash);
    RunOptions opt;
    opt.map = &*resolved;
    opt.everyTick = true;
    const RunResult r1 = runReplay(back, nullptr, opt);
    const RunResult r2 = runReplay(back, nullptr, opt);
    CHECK_FALSE(firstDivergence(r1, r2).found);
    CHECK_FALSE(divergenceFromRecorded(back, r1).found);
    // Some paths actually completed in the run.
    Sim sim({13, nullptr, &m});
    CommandQueue q;
    for (const Command& c : back.commands) q.schedule(c, 0);
    for (std::uint32_t t = 0; t < 100; ++t) sim.step(q.take(t));
    std::size_t done = 0;
    for (const Path& p : sim.world().paths.data()) done += p.status != Path::Status::Pending;
    CHECK(done > 0);

    const MapDef wrong = generateTestMap(14, 64);
    CHECK_FALSE(resolveReplayMap(back.header, &wrong, resolved, err));
    Replay named = back;
    named.header.mapSpec = "Maps/Somewhere.RMP";
    CHECK_FALSE(resolveReplayMap(named.header, nullptr, resolved, err));
    CHECK(err.find("--map") != std::string::npos);
    REQUIRE(resolveReplayMap(named.header, &m, resolved, err));
    Replay plain = recordSynthetic(1, 2, 10, 2, nullptr);
    REQUIRE(resolveReplayMap(plain.header, nullptr, resolved, err));
    CHECK_FALSE(resolved.has_value());
}

TEST_CASE("committed nav replay plays identically on this compiler (cross-platform fixture)") {
    // sim/tests/fixtures/synthetic_nav_300.rpl: map gen:2026:128, 300 ticks, 4 commands per tick
    // (spawns, blockers, path requests of every class), recorded with MSVC x64.
    const std::string path = CORPS_SOURCE_DIR "/sim/tests/fixtures/synthetic_nav_300.rpl";
    const MapDef m = generateTestMap(2026, 128);
    std::ifstream in(vfs::pathFromUtf8(path), std::ios::binary);
    if (testEnvSet("CORPS_WRITE_FIXTURES") || !in) {
        const Replay rep = recordSynthetic(2026, 2027, 300, 4, nullptr, {}, &m);
        std::ofstream out(vfs::pathFromUtf8(path), std::ios::binary);
        const auto b = encodeReplay(rep);
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        FAIL_CHECK("fixture (re)written; commit it and re-run");
        return;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    Replay rep;
    std::string err;
    REQUIRE(decodeReplay(bytes, rep, &err));
    std::optional<MapDef> map;
    REQUIRE(resolveReplayMap(rep.header, nullptr, map, err));
    RunOptions opt;
    opt.map = &*map;
    const RunResult run = runReplay(rep, nullptr, opt);
    CHECK_FALSE(divergenceFromRecorded(rep, run).found);
    CHECK(run.finalHash == rep.hashes.back().hash);
    const Replay again = recordSynthetic(2026, 2027, 300, 4, nullptr, {}, &m);
    CHECK(again.commands == rep.commands);
    CHECK(again.hashes == rep.hashes);
}
