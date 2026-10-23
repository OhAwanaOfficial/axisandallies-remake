#include "nav/map_def.hpp"

#include <algorithm>

#include "core/ascii.hpp"
#include "data/json_lite.hpp"
#include "math/rng.hpp"
#include "session/bytes.hpp"

namespace corps::nav {

namespace {

constexpr const char* kClassNames[kMoveClassCount] = {"foot", "wheeled", "tracked", "naval", "amphibious"};
constexpr const char* kKindNames[kTerrainKindCount] = {"ground", "road", "forest", "beach", "shallow", "water", "mountain", "border"};

//                                             Foot Wheeled Tracked Naval Amphib
constexpr std::uint8_t kCost[kTerrainKindCount][kMoveClassCount] = {
    /* Ground   */ {16, 16, 16, 0, 16},
    /* Road     */ {16, 12, 12, 0, 16},
    /* Forest   */ {24, 48, 32, 0, 24},
    /* Beach    */ {16, 20, 20, 0, 16},
    /* Shallow  */ {0, 0, 0, 16, 16},
    /* Water    */ {0, 0, 0, 16, 16},
    /* Mountain */ {0, 0, 0, 0, 0},
    /* Border   */ {0, 0, 0, 0, 0},
};

}  // namespace

const char* moveClassName(MoveClass c) {
    const auto i = static_cast<std::size_t>(c);
    return i < kMoveClassCount ? kClassNames[i] : "?";
}

bool parseMoveClass(std::string_view name, MoveClass& out) {
    for (std::size_t i = 0; i < kMoveClassCount; ++i) {
        if (iequals(name, kClassNames[i])) {
            out = static_cast<MoveClass>(i);
            return true;
        }
    }
    return false;
}

const char* terrainKindName(TerrainKind k) {
    const auto i = static_cast<std::size_t>(k);
    return i < kTerrainKindCount ? kKindNames[i] : "?";
}

std::uint8_t terrainCost(TerrainKind k, MoveClass c) {
    return kCost[static_cast<std::size_t>(k)][static_cast<std::size_t>(c)];
}

bool MapDef::finalize() {
    const std::size_t n = cellCount();
    if (width == 0 || height == 0 || width > 4096 || height > 4096 || kind.size() != n || terrainId.size() != n) return false;
    for (const TerrainKind k : kind) {
        if (static_cast<std::size_t>(k) >= kTerrainKindCount) return false;
    }
    if (!heights.empty() && (heightWidth == 0 || heights.size() % heightWidth != 0)) return false;
    ByteWriter w;
    w.tag("NMAP");
    w.str(name);
    w.u32(width);
    w.u32(height);
    for (const TerrainKind k : kind) w.u8(static_cast<std::uint8_t>(k));
    for (const std::uint16_t t : terrainId) w.u16(t);
    w.u32(static_cast<std::uint32_t>(terrainNames.size()));
    for (const auto& s : terrainNames) w.str(s);
    w.u32(heightWidth);
    w.u32(static_cast<std::uint32_t>(heights.size()));
    for (const Fixed h : heights) w.fixed(h);
    mapHash = hashBytes(w.buf);
    return true;
}

// ------------------------------------------------------------------------------- generator

MapDef generateTestMap(std::uint64_t seed, std::uint32_t size) {
    using namespace fixed_literals;
    size = std::clamp<std::uint32_t>(size, 32, 1024);
    MapDef m;
    m.name = "gen:" + std::to_string(seed) + ":" + std::to_string(size);
    m.width = m.height = size;
    m.terrainNames = {"gen_ground", "gen_road", "gen_forest", "gen_beach", "gen_shallow", "gen_water", "gen_mountain", "gen_border"};
    const std::size_t n = m.cellCount();
    m.kind.assign(n, TerrainKind::Ground);
    Rng rng = Rng::derive(seed, 0x4D41504745ULL);  // "MAPGE"
    const auto S = static_cast<std::int32_t>(size);

    // Irregular blob: radius r scaled per 16-sector by 75..125%, membership by integer distance.
    const auto blob = [&](TerrainKind k, std::int32_t cx, std::int32_t cy, std::int32_t r, bool onlyGround) {
        std::int32_t scale[16];
        for (std::int32_t& sc : scale) sc = rng.range(75, 125);
        for (std::int32_t y = std::max(0, cy - r * 2); y < std::min(S, cy + r * 2); ++y) {
            for (std::int32_t x = std::max(0, cx - r * 2); x < std::min(S, cx + r * 2); ++x) {
                const std::int32_t dx = x - cx, dy = y - cy;
                const std::uint32_t sector = atan2(Fixed::fromInt(dy), Fixed::fromInt(dx)).units >> 12;
                const std::int64_t rr = static_cast<std::int64_t>(r) * scale[sector] / 100;
                if (static_cast<std::int64_t>(dx) * dx + static_cast<std::int64_t>(dy) * dy <= rr * rr) {
                    TerrainKind& c = m.kind[static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)];
                    if (!onlyGround || c == TerrainKind::Ground) c = k;
                }
            }
        }
    };
    const auto randomCentre = [&](std::int32_t margin) {
        return std::pair{rng.range(margin, S - 1 - margin), rng.range(margin, S - 1 - margin)};
    };
    const std::int32_t unit = std::max(4, S / 16);
    for (int i = 0, k = rng.range(1, 2); i < k; ++i) {  // oceans with shallows and beaches
        const auto [x, y] = randomCentre(unit);
        const std::int32_t r = rng.range(unit, unit * 2);
        blob(TerrainKind::Beach, x, y, r + 3, true);
        blob(TerrainKind::Shallow, x, y, r + 1, false);
        blob(TerrainKind::Water, x, y, r - 1, false);
    }
    for (int i = 0, k = rng.range(3, 5); i < k; ++i) {
        const auto [x, y] = randomCentre(unit);
        blob(TerrainKind::Mountain, x, y, rng.range(unit / 2, unit * 3 / 2), true);
    }
    for (int i = 0, k = rng.range(6, 10); i < k; ++i) {
        const auto [x, y] = randomCentre(unit / 2);
        blob(TerrainKind::Forest, x, y, rng.range(unit / 2, unit), true);
    }
    // Two roads: one row and one column, crossing only ground and forest.
    const std::int32_t ry = rng.range(unit, S - 1 - unit);
    const std::int32_t rx = rng.range(unit, S - 1 - unit);
    for (std::int32_t i = 0; i < S; ++i) {
        for (const std::size_t idx : {static_cast<std::size_t>(ry) * size + static_cast<std::size_t>(i),
                                      static_cast<std::size_t>(i) * size + static_cast<std::size_t>(rx)}) {
            if (m.kind[idx] == TerrainKind::Ground || m.kind[idx] == TerrainKind::Forest) m.kind[idx] = TerrainKind::Road;
        }
    }
    for (std::int32_t i = 0; i < S; ++i) {  // blocked border ring
        m.kind[static_cast<std::size_t>(i)] = TerrainKind::Border;
        m.kind[static_cast<std::size_t>(S - 1) * size + static_cast<std::size_t>(i)] = TerrainKind::Border;
        m.kind[static_cast<std::size_t>(i) * size] = TerrainKind::Border;
        m.kind[static_cast<std::size_t>(i) * size + static_cast<std::size_t>(S - 1)] = TerrainKind::Border;
    }
    m.terrainId.resize(n);
    for (std::size_t i = 0; i < n; ++i) m.terrainId[i] = static_cast<std::uint16_t>(m.kind[i]);
    m.finalize();
    return m;
}

MapDef generateArenaMap(std::string_view terrain, std::uint32_t size) {
    size = std::clamp<std::uint32_t>(size, 32, 1024);
    MapDef m;
    const std::string name = terrain.empty() ? std::string("open") : std::string(terrain);
    m.name = "arena:" + name + ":" + std::to_string(size);
    m.width = m.height = size;
    m.terrainNames = {name, "gen_border"};
    const bool wooded = name.find("forest") != std::string::npos || name.find("jungle") != std::string::npos;
    const std::size_t n = m.cellCount();
    m.kind.assign(n, wooded ? TerrainKind::Forest : TerrainKind::Ground);
    m.terrainId.assign(n, 0);
    const auto S = static_cast<std::size_t>(size);
    for (std::size_t i = 0; i < S; ++i) {  // blocked border ring
        for (const std::size_t idx : {i, (S - 1) * S + i, i * S, i * S + S - 1}) {
            m.kind[idx] = TerrainKind::Border;
            m.terrainId[idx] = 1;
        }
    }
    m.finalize();
    return m;
}

bool parseArenaSpec(std::string_view spec, std::string& terrain, std::uint32_t& size) {
    if (spec.substr(0, 6) != "arena:") return false;
    spec.remove_prefix(6);
    const std::size_t colon = spec.rfind(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    const std::string_view digits = spec.substr(colon + 1);
    if (digits.empty() || digits.size() > 4) return false;
    std::uint32_t v = 0;
    for (const char ch : digits) {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + static_cast<std::uint32_t>(ch - '0');
    }
    if (v > 1024) return false;
    terrain = std::string(spec.substr(0, colon));
    size = v;
    return true;
}

bool parseGenSpec(std::string_view spec, std::uint64_t& seed, std::uint32_t& size) {
    if (spec.substr(0, 4) != "gen:") return false;
    spec.remove_prefix(4);
    const std::size_t colon = spec.find(':');
    if (colon == std::string_view::npos) return false;
    const auto digits = [](std::string_view s, std::uint64_t max, std::uint64_t& v) {
        if (s.empty() || s.size() > 20) return false;
        v = 0;
        for (const char ch : s) {
            if (ch < '0' || ch > '9') return false;
            const auto d = static_cast<std::uint64_t>(ch - '0');
            if (v > (max - d) / 10) return false;
            v = v * 10 + d;
        }
        return true;
    };
    std::uint64_t sz = 0;
    if (!digits(spec.substr(0, colon), UINT64_MAX, seed) || !digits(spec.substr(colon + 1), 1024, sz)) return false;
    size = static_cast<std::uint32_t>(sz);
    return true;
}

// ------------------------------------------------------------------------------- rmp.py JSON

namespace {

bool intArray(const json::Value* v, std::size_t expect, std::vector<std::int64_t>& out) {
    if (v == nullptr || !v->isArray() || (expect != 0 && v->items.size() != expect)) return false;
    out.clear();
    out.reserve(v->items.size());
    for (const json::Value& it : v->items) {
        const auto i = it.asInt();
        if (!i) return false;
        out.push_back(*i);
    }
    return true;
}

TerrainKind kindFor(std::int64_t classBits, const std::string& name) {
    constexpr std::int64_t kBorder = 0x01, kMountain = 0x10, kWater = 0x20;
    const std::string n = asciiLowerCopy(name);
    if (classBits & kBorder) return TerrainKind::Border;
    if (classBits & kMountain) return TerrainKind::Mountain;
    if (classBits & kWater) return n.find("shallow") != std::string::npos ? TerrainKind::Shallow : TerrainKind::Water;
    if (n.find("trans") != std::string::npos) return TerrainKind::Ground;  // transition tiles: open ground
    if (n.find("shore") != std::string::npos || n.find("beach") != std::string::npos) return TerrainKind::Beach;
    if (n.find("forest") != std::string::npos || n.find("jungle") != std::string::npos) return TerrainKind::Forest;
    if (n.find("road") != std::string::npos) return TerrainKind::Road;
    return TerrainKind::Ground;
}

}  // namespace

bool loadRmpJson(std::string_view text, MapDef& out, std::string& error) {
    const json::ParseResult pr = json::parse(text);
    if (!pr.ok) {
        error = "map json: " + pr.error;
        return false;
    }
    const json::Value& root = pr.value;
    const json::Value* layers = root.get("layers");
    const json::Value* tiles = root.get("tiles_per_side");
    const json::Value* src = root.get("source");
    if (layers == nullptr || tiles == nullptr || !tiles->asInt()) {
        error = "map json: missing layers or tiles_per_side (not tools/rmp.py output?)";
        return false;
    }
    const auto n = static_cast<std::size_t>(*tiles->asInt());
    if (n == 0 || n > 2048) {
        error = "map json: bad tiles_per_side";
        return false;
    }
    std::vector<std::int64_t> index, cls, h, mt;
    const json::Value* names = layers->get("terrain_names");
    if (!intArray(layers->get("tile_terrain_index"), n * n, index) || !intArray(layers->get("tile_class_bits"), n * n, cls) ||
        names == nullptr || !names->isArray()) {
        error = "map json: missing or malformed tile layers";
        return false;
    }
    if (!intArray(layers->get("vertex_height_mm"), (n + 1) * (n + 1), h) ||
        !intArray(layers->get("vertex_mountain_mm"), (n + 1) * (n + 1), mt)) {
        error = "map json: no integer height layers (re-run tools/rmp.py; it now writes vertex_height_mm)";
        return false;
    }
    MapDef m;
    m.name = src != nullptr && src->type == json::Value::Type::String ? src->text : "rmp";
    for (const json::Value& s : names->items) m.terrainNames.push_back(s.text);
    m.width = m.height = static_cast<std::uint32_t>(n * 2);
    m.kind.assign(m.cellCount(), TerrainKind::Ground);
    m.terrainId.assign(m.cellCount(), 0);
    for (std::size_t ty = 0; ty < n; ++ty) {
        for (std::size_t tx = 0; tx < n; ++tx) {
            const std::size_t t = ty * n + tx;
            if (index[t] < 0 || static_cast<std::size_t>(index[t]) >= m.terrainNames.size()) {
                error = "map json: terrain index out of range";
                return false;
            }
            const TerrainKind k = kindFor(cls[t], m.terrainNames[static_cast<std::size_t>(index[t])]);
            for (std::size_t dy = 0; dy < 2; ++dy) {
                for (std::size_t dx = 0; dx < 2; ++dx) {
                    const std::size_t c = (ty * 2 + dy) * m.width + tx * 2 + dx;
                    m.kind[c] = k;
                    m.terrainId[c] = static_cast<std::uint16_t>(index[t]);
                }
            }
        }
    }
    // View-only heights: one vertex per tile corner (2 world units apart), ground = base + mountain.
    m.heightWidth = static_cast<std::uint32_t>(n + 1);
    m.heights.reserve(h.size());
    for (std::size_t i = 0; i < h.size(); ++i) m.heights.push_back(Fixed::fromRatio(h[i] + mt[i], 1000));
    if (!m.finalize()) {
        error = "map json: inconsistent map";
        return false;
    }
    out = std::move(m);
    return true;
}

}  // namespace corps::nav
