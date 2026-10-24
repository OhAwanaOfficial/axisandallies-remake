#include "sim_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "core/ascii.hpp"
#include "core/hello.hpp"
#include "core/sim.hpp"
#include "data/def_compiler.hpp"
#include "data/json_lite.hpp"
#include "data/tgi/pipeline.hpp"
#include "nav/nav_world.hpp"
#include "session/state_io.hpp"
#include "systems/movement.hpp"
#include "vfs/path.hpp"
#include "vfs/vfs.hpp"

namespace godot {

using corps::EntityId;
using corps::Fixed;
using corps::FVec2;

struct SimBridge::Data {
    corps::vfs::Vfs vfs;
    std::unique_ptr<corps::tgi::TgiDatabase> db;
    corps::DefSet defs;
};

namespace {

constexpr double kTwoPi = 6.283185307179586;

double toF(Fixed f) {
    return static_cast<double>(f.raw) / 4294967296.0;
}
Fixed fromF(double v) {
    return Fixed::fromRaw(static_cast<int64_t>(std::llround(v * 4294967296.0)));
}
FVec2 fromV(const Vector2& v) {
    return {fromF(v.x), fromF(v.y)};
}
Vector2 toV(FVec2 v) {
    return {static_cast<real_t>(toF(v.x)), static_cast<real_t>(toF(v.y))};
}
float angleRad(corps::Angle a) {
    return static_cast<float>(a.units * kTwoPi / 65536.0);
}
corps::Angle fromRad(double r) {
    double u = std::fmod(r / kTwoPi * 65536.0, 65536.0);
    if (u < 0) u += 65536.0;
    return corps::Angle::fromUnits(static_cast<uint32_t>(std::llround(u)) & 0xFFFFu);
}
String str(std::string_view s) {
    return String::utf8(s.data(), static_cast<int64_t>(s.size()));
}
std::string utf8(const String& s) {
    const CharString c = s.utf8();
    return std::string(c.get_data(), static_cast<std::size_t>(c.length()));
}

const char* stateName(corps::Order::State s) {
    switch (s) {
    case corps::Order::State::Idle: return "idle";
    case corps::Order::State::WaitPath: return "wait_path";
    case corps::Order::State::Moving: return "moving";
    case corps::Order::State::Wheeling: return "wheeling";
    case corps::Order::State::Arriving: return "arriving";
    case corps::Order::State::Following: return "following";
    }
    return "?";
}

const char* eventName(corps::SimEvent::Type t) {
    switch (t) {
    case corps::SimEvent::Type::Spawned: return "spawned";
    case corps::SimEvent::Type::Destroyed: return "destroyed";
    case corps::SimEvent::Type::CommandRejected: return "rejected";
    case corps::SimEvent::Type::Arrived: return "arrived";
    case corps::SimEvent::Type::Blocked: return "blocked";
    case corps::SimEvent::Type::Fired: return "fired";
    case corps::SimEvent::Type::Died: return "died";
    case corps::SimEvent::Type::Routed: return "routed";
    case corps::SimEvent::Type::LevelUp: return "level_up";
    }
    return "?";
}

}  // namespace

SimBridge::SimBridge() = default;
SimBridge::~SimBridge() = default;

void SimBridge::_bind_methods() {
    ClassDB::bind_method(D_METHOD("hello"), &SimBridge::hello);
    ClassDB::bind_method(D_METHOD("start", "game_dir", "map_spec", "seed"), &SimBridge::start);
    ClassDB::bind_method(D_METHOD("last_error"), &SimBridge::last_error);
    ClassDB::bind_method(D_METHOD("map_info"), &SimBridge::map_info);
    ClassDB::bind_method(D_METHOD("terrain_table", "biome"), &SimBridge::terrain_table);
    ClassDB::bind_method(D_METHOD("unit_visual", "unit_def"), &SimBridge::unit_visual);
    ClassDB::bind_method(D_METHOD("regiment_def", "regiment_def"), &SimBridge::regiment_def);
    ClassDB::bind_method(D_METHOD("find_open_area", "near", "half"), &SimBridge::find_open_area);
    ClassDB::bind_method(D_METHOD("queue_spawn", "regiment_ids", "pos", "facing", "player"), &SimBridge::queue_spawn);
    ClassDB::bind_method(D_METHOD("queue_move", "regiments", "goal", "run", "has_facing", "facing", "player"), &SimBridge::queue_move);
    ClassDB::bind_method(D_METHOD("queue_stop", "regiments", "player"), &SimBridge::queue_stop);
    ClassDB::bind_method(D_METHOD("queue_formation", "regiments", "formation_ids", "player"), &SimBridge::queue_formation);
    ClassDB::bind_method(D_METHOD("queue_group_behaviour", "regiments", "division", "player"), &SimBridge::queue_group_behaviour);
    ClassDB::bind_method(D_METHOD("queue_blocker", "pos", "half_extent", "player"), &SimBridge::queue_blocker);
    ClassDB::bind_method(D_METHOD("step"), &SimBridge::step);
    ClassDB::bind_method(D_METHOD("tick"), &SimBridge::tick);
    ClassDB::bind_method(D_METHOD("state_hash"), &SimBridge::state_hash);
    ClassDB::bind_method(D_METHOD("snapshot"), &SimBridge::snapshot);
    ClassDB::bind_method(D_METHOD("drain_events"), &SimBridge::drain_events);
    ClassDB::bind_method(D_METHOD("regiment_info", "regiment"), &SimBridge::regiment_info);
    ClassDB::bind_method(D_METHOD("cost_grid", "move_class"), &SimBridge::cost_grid);
    ClassDB::bind_method(D_METHOD("set_view_heights", "heights", "verts_per_row", "tile"), &SimBridge::set_view_heights);
    ClassDB::bind_method(D_METHOD("unit_instances", "alpha", "selected_orgs"), &SimBridge::unit_instances);
}

String SimBridge::hello() const {
    return str(corps::simHello());
}

String SimBridge::last_error() const {
    return str(error);
}

bool SimBridge::start(const String& game_dir, const String& map_spec, int64_t seed) {
    error.clear();
    sim.reset();
    auto d = std::make_unique<Data>();
    std::vector<std::string> log;
    if (!corps::vfs::mountGameInstall(d->vfs, corps::vfs::pathFromUtf8(utf8(game_dir)), {}, log)) {
        error = "nothing could be mounted from " + utf8(game_dir);
        return false;
    }
    d->db = corps::tgi::loadTgiDatabase(d->vfs);
    corps::tgi::Diagnostics diags;
    d->defs = corps::compileDefs(*d->db, diags);

    auto m = std::make_unique<corps::nav::MapDef>();
    const std::string spec = utf8(map_spec);
    std::uint64_t genSeed = 0;
    std::uint32_t genSize = 0;
    biome = "temperate";
    if (corps::nav::parseGenSpec(spec, genSeed, genSize)) {
        *m = corps::nav::generateTestMap(genSeed, genSize);
    } else {
        std::ifstream in(corps::vfs::pathFromUtf8(spec), std::ios::binary);
        if (!in) {
            error = "cannot read map " + spec;
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        if (!corps::nav::loadRmpJson(text, *m, error)) return false;
        const corps::json::ParseResult pr = corps::json::parse(text);
        if (const corps::json::Value* b = pr.ok ? pr.value.get("biome") : nullptr; b != nullptr) biome = b->text;
    }
    data = std::move(d);
    map = std::move(m);
    sim = std::make_unique<corps::Sim>(corps::SimConfig{static_cast<std::uint64_t>(seed), &data->defs, map.get()});
    pending.clear();
    prev = capture();
    events.clear();
    std::fill(std::begin(seq), std::end(seq), uint16_t{0});
    return true;
}

// ------------------------------------------------------------------------------ static data

Dictionary SimBridge::map_info() const {
    Dictionary out;
    if (!map) return out;
    out["name"] = str(map->name);
    out["biome"] = str(biome);
    out["width"] = static_cast<int64_t>(map->width);
    out["height"] = static_cast<int64_t>(map->height);
    out["hash"] = String::num_uint64(map->mapHash, 16);
    out["height_width"] = static_cast<int64_t>(map->heightWidth);
    PackedFloat32Array heights;
    heights.resize(static_cast<int64_t>(map->heights.size()));
    for (std::size_t i = 0; i < map->heights.size(); ++i) heights.set(static_cast<int64_t>(i), static_cast<float>(toF(map->heights[i])));
    out["heights"] = heights;
    PackedStringArray names;
    for (const std::string& n : map->terrainNames) names.push_back(str(n));
    out["terrain_names"] = names;
    PackedInt32Array ids;
    PackedByteArray kinds;
    ids.resize(static_cast<int64_t>(map->cellCount()));
    kinds.resize(static_cast<int64_t>(map->cellCount()));
    for (std::size_t i = 0; i < map->cellCount(); ++i) {
        ids.set(static_cast<int64_t>(i), map->terrainId[i]);
        kinds.set(static_cast<int64_t>(i), static_cast<uint8_t>(map->kind[i]));
    }
    out["cell_terrain"] = ids;
    out["cell_kind"] = kinds;
    return out;
}

Dictionary SimBridge::terrain_table(const String& biomeName) const {
    Dictionary out;
    if (!data) return out;
    const corps::tgi::TgiDatabase& db = *data->db;
    const auto sym = [&](std::string_view s) { return db.symbols.find(s); };
    const auto text = [&](const corps::tgi::ResolvedNode* n, std::string_view key) -> std::string {
        if (n == nullptr) return {};
        const corps::tgi::Symbol k = sym(key);
        const corps::tgi::ResolvedField* f = k == corps::tgi::kNoSymbol ? nullptr : n->field(k);
        if (f == nullptr || !f->hasValue()) return {};
        std::string v(corps::trimAscii(f->text()));
        return corps::iequals(v, "null") ? std::string() : v;
    };
    const auto rec = [&](std::string_view type, std::string_view ids) -> const corps::tgi::Record* {
        const corps::tgi::DefIndex::ByIdsEntry* e = db.defs.find(sym(type), corps::asciiLowerCopy(ids));
        return e != nullptr ? &db.records[e->record] : nullptr;
    };
    const corps::tgi::Record* br = rec("Biome", utf8(biomeName));
    const corps::tgi::ResolvedNode* b = br != nullptr ? &br->tree : nullptr;
    Dictionary info;
    Dictionary variations;
    if (b != nullptr) {
        info["subfolder"] = str(text(b, "texture_subfolder_name"));
        info["detail_texture"] = str(text(b, "detail_texture"));
        info["macro_texture"] = str(text(b, "macro_texture"));
    }
    // Every Terrain record's variations: maps also use variations their biome record doesn't list.
    const corps::tgi::Symbol terrainSym = sym("Terrain");
    const corps::tgi::Symbol varSym = sym("Variation");
    for (const corps::tgi::DefIndex::ByIdsEntry& entry : db.defs.byIds) {
        if (entry.type != terrainSym) continue;
        const corps::tgi::Record& tr = db.records[entry.record];
        const corps::tgi::ResolvedNode* t = &tr.tree;
        // Variation textures are relative to the folder of the Terrain record's own file.
        std::string dir = tr.file != nullptr ? tr.file->path : std::string();
        dir = dir.substr(0, dir.find_last_of('/') == std::string::npos ? 0 : dir.find_last_of('/'));
        for (std::size_t v = 0; v < t->childCount(varSym); ++v) {
            const corps::tgi::ResolvedNode* vn = t->child(varSym, v);
            Dictionary e;
            const std::string tex = text(vn, "texture");
            e["texture"] = tex.empty() ? String() : str((dir.empty() ? "" : dir + "/") + tex);
            const PackedFloat64Array rgb = str(text(vn, "terrain_color")).split_floats(",");
            e["color"] = rgb.size() >= 3 ? Color(static_cast<float>(rgb[0]), static_cast<float>(rgb[1]), static_cast<float>(rgb[2]))
                                         : Color(0.5f, 0.5f, 0.5f);
            e["width"] = str(text(vn, "texture_width")).to_float();
            e["height"] = str(text(vn, "texture_height")).to_float();
            e["terrain"] = str(text(t, "IDS"));
            variations[str(corps::asciiLowerCopy(text(vn, "IDS")))] = e;
        }
    }
    out["biome"] = info;
    out["variations"] = variations;
    return out;
}

Dictionary SimBridge::unit_visual(int64_t unitDef) const {
    Dictionary out;
    if (!data || unitDef < 0 || static_cast<std::size_t>(unitDef) >= data->defs.units.size()) return out;
    const corps::UnitDef& u = data->defs.units[static_cast<std::size_t>(unitDef)];
    out["ids"] = str(u.ids);
    out["resource_prefix"] = str(u.resourcePrefix);
    out["move_class"] = static_cast<int64_t>(u.moveClass);
    out["radius"] = toF(u.blockingRadius);
    out["speed"] = toF(u.speedMax);
    // resource_prefix is relative to the folder of the TGI file that defines the unit.
    const corps::tgi::TgiDatabase& db = *data->db;
    std::string dir;
    if (const auto* e = db.defs.find(db.symbols.find("Thing"), corps::asciiLowerCopy(u.ids)); e != nullptr && db.records[e->record].file) {
        const std::string& p = db.records[e->record].file->path;
        dir = p.substr(0, p.find_last_of('/') == std::string::npos ? 0 : p.find_last_of('/'));
    }
    out["model"] = u.resourcePrefix.empty() ? String() : str((dir.empty() ? "" : dir + "/") + u.resourcePrefix + ".glb");
    return out;
}

Dictionary SimBridge::regiment_def(int64_t regimentDef) const {
    Dictionary out;
    if (!data || regimentDef < 0 || static_cast<std::size_t>(regimentDef) >= data->defs.regiments.size()) return out;
    const corps::RegimentDef& g = data->defs.regiments[static_cast<std::size_t>(regimentDef)];
    out["ids"] = str(g.ids);
    out["layout"] = str(g.layoutIds);
    PackedStringArray formations;
    if (g.layoutIndex != corps::kNoDef) {
        for (const auto& f : data->defs.layouts[g.layoutIndex].formations) formations.push_back(str(f.formationIds));
    }
    out["formations"] = formations;
    return out;
}

Vector2 SimBridge::find_open_area(const Vector2& near, int64_t half) const {
    if (!sim || sim->nav() == nullptr) return {-1, -1};
    const corps::nav::NavGrid& g = sim->nav()->grid;
    const auto h = static_cast<int32_t>(half);
    const auto wx = static_cast<int32_t>(near.x), wy = static_cast<int32_t>(near.y);
    int64_t best = INT64_MAX;
    Vector2 out{-1, -1};
    for (int32_t y = h; y < g.height() - h; ++y) {
        for (int32_t x = h; x < g.width() - h; ++x) {
            const int64_t d2 = static_cast<int64_t>(x - wx) * (x - wx) + static_cast<int64_t>(y - wy) * (y - wy);
            if (d2 >= best) continue;
            bool open = true;
            for (int32_t dy = -h; dy <= h && open; ++dy) {
                for (int32_t dx = -h; dx <= h && open; ++dx) {
                    const uint32_t c = g.index(x + dx, y + dy);
                    open = g.cost(corps::nav::MoveClass::Wheeled, c) != 0 && g.cost(corps::nav::MoveClass::Foot, c) != 0;
                }
            }
            if (open) {
                best = d2;
                out = {static_cast<real_t>(x) + 0.5f, static_cast<real_t>(y) + 0.5f};
            }
        }
    }
    return out;
}

// ------------------------------------------------------------------------------ commands

void SimBridge::queue(corps::Command c) {
    if (!sim) return;
    c.tick = sim->tick();
    c.seq = seq[c.player]++;
    pending.push_back(std::move(c));
}

std::vector<EntityId> SimBridge::regimentList(const PackedInt64Array& ids) const {
    std::vector<EntityId> out;
    for (int64_t i = 0; i < ids.size() && out.size() < corps::kMaxCommandRegiments; ++i) out.push_back(EntityId{static_cast<uint32_t>(ids[i])});
    return out;
}

void SimBridge::queue_spawn(const String& ids, const Vector2& pos, double facing, int64_t player) {
    queue(corps::makeSpawnRegiment(0, static_cast<uint8_t>(player), 0, utf8(ids), fromV(pos), fromRad(facing)));
}

void SimBridge::queue_move(const PackedInt64Array& regiments, const Vector2& goal, bool run, bool hasFacing, double facing,
                           int64_t player) {
    const auto p = static_cast<uint8_t>(player);
    // Commands carry at most six regiments (a group); bigger selections go as several groups.
    for (int64_t start = 0; start < regiments.size(); start += static_cast<int64_t>(corps::kMaxCommandRegiments)) {
        PackedInt64Array chunk = regiments.slice(start, start + static_cast<int64_t>(corps::kMaxCommandRegiments));
        std::vector<EntityId> regs = regimentList(chunk);
        corps::Command c = run ? corps::makeRun(0, p, 0, std::move(regs), fromV(goal))
                               : (hasFacing ? corps::makeMoveGroupFacing(0, p, 0, std::move(regs), fromV(goal), fromRad(facing))
                                            : corps::makeMoveGroup(0, p, 0, std::move(regs), fromV(goal)));
        if (run && hasFacing) {
            auto& payload = std::get<corps::MoveGroupPayload>(c.payload);
            payload.hasFacing = true;
            payload.facing = fromRad(facing);
        }
        queue(std::move(c));
    }
}

void SimBridge::queue_stop(const PackedInt64Array& regiments, int64_t player) {
    for (int64_t s = 0; s < regiments.size(); s += static_cast<int64_t>(corps::kMaxCommandRegiments)) {
        queue(corps::makeStop(0, static_cast<uint8_t>(player), 0, regimentList(regiments.slice(s, s + static_cast<int64_t>(corps::kMaxCommandRegiments)))));
    }
}

void SimBridge::queue_formation(const PackedInt64Array& regiments, const String& formationIds, int64_t player) {
    for (int64_t s = 0; s < regiments.size(); s += static_cast<int64_t>(corps::kMaxCommandRegiments)) {
        queue(corps::makeSetFormation(0, static_cast<uint8_t>(player), 0,
                                      regimentList(regiments.slice(s, s + static_cast<int64_t>(corps::kMaxCommandRegiments))), utf8(formationIds)));
    }
}

void SimBridge::queue_group_behaviour(const PackedInt64Array& regiments, bool division, int64_t player) {
    queue(corps::makeSetGroupBehaviour(0, static_cast<uint8_t>(player), 0, regimentList(regiments), division));
}

void SimBridge::queue_blocker(const Vector2& pos, const Vector2& half, int64_t player) {
    queue(corps::makeDebugPlaceBlocker(0, static_cast<uint8_t>(player), 0, fromV(pos), fromV(half)));
}

// ------------------------------------------------------------------------------ stepping

std::vector<SimBridge::Snap> SimBridge::capture() const {
    std::vector<Snap> out;
    if (!sim) return out;
    const corps::World& w = sim->world();
    out.reserve(w.transforms.size());
    for (std::size_t i = 0; i < w.transforms.size(); ++i) {
        const corps::Transform& t = w.transforms.data()[i];
        out.push_back({w.transforms.ids()[i].bits, static_cast<float>(toF(t.position.x)), static_cast<float>(toF(t.position.y)),
                       angleRad(t.facing)});
    }
    return out;
}

void SimBridge::step() {
    if (!sim) return;
    prev = capture();
    std::vector<corps::Command> cmds;
    cmds.swap(pending);
    sim->step(cmds);
    for (const corps::SimEvent& e : sim->events()) {
        Dictionary d;
        d["type"] = eventName(e.type);
        d["entity"] = static_cast<int64_t>(e.entity.bits);
        d["player"] = static_cast<int64_t>(e.player);
        d["command"] = static_cast<int64_t>(e.command);
        d["other"] = static_cast<int64_t>(e.other.bits);  // fired: the target; died: the killer
        d["tick"] = static_cast<int64_t>(sim->tick() - 1);
        events.push_back(d);
    }
}

int64_t SimBridge::tick() const {
    return sim ? sim->tick() : 0;
}

String SimBridge::state_hash() const {
    return sim ? String::num_uint64(corps::stateHash(*sim), 16) : String();
}

Array SimBridge::drain_events() {
    Array out = events;
    events = Array();
    return out;
}

Dictionary SimBridge::snapshot() const {
    Dictionary out;
    if (!sim) return out;
    const corps::World& w = sim->world();
    const int64_t n = static_cast<int64_t>(w.transforms.size());
    PackedInt64Array ids, org;
    PackedByteArray kind, player, moving;
    PackedInt32Array def;
    PackedVector2Array prevPos, curPos;
    PackedFloat32Array prevFacing, curFacing;
    ids.resize(n);
    org.resize(n);
    kind.resize(n);
    player.resize(n);
    moving.resize(n);
    def.resize(n);
    prevPos.resize(n);
    curPos.resize(n);
    prevFacing.resize(n);
    curFacing.resize(n);
    for (int64_t i = 0; i < n; ++i) {
        const EntityId id = w.transforms.ids()[static_cast<std::size_t>(i)];
        const corps::Transform& t = w.transforms.data()[static_cast<std::size_t>(i)];
        ids.set(i, id.bits);
        uint8_t k = 4;
        if (w.movers.has(id)) k = 0;
        else if (w.regiments.has(id)) k = 1;
        else if (w.groups.has(id)) k = 2;
        else if (w.footprints.has(id)) k = 3;
        kind.set(i, k);
        const corps::DefRef* dr = w.defRefs.get(id);
        def.set(i, dr != nullptr ? static_cast<int32_t>(dr->index) : -1);
        const corps::Member* m = w.members.get(id);
        org.set(i, m != nullptr ? m->organization.bits : 0);
        const corps::Regiment* r = w.regiments.get(m != nullptr ? m->organization : id);
        player.set(i, r != nullptr ? r->player : 0);
        const corps::Mover* mv = w.movers.get(id);
        moving.set(i, mv != nullptr && toF(mv->velocity.lengthSq()) > 0.0025 ? 1 : 0);
        const Vector2 cur = toV(t.position);
        const float cf = angleRad(t.facing);
        curPos.set(i, cur);
        curFacing.set(i, cf);
        const auto it = std::lower_bound(prev.begin(), prev.end(), id.bits, [](const Snap& s, uint32_t b) {
            return EntityId{s.bits}.index() < EntityId{b}.index();
        });
        if (it != prev.end() && it->bits == id.bits) {
            prevPos.set(i, Vector2(it->x, it->y));
            prevFacing.set(i, it->facing);
        } else {
            prevPos.set(i, cur);
            prevFacing.set(i, cf);
        }
    }
    out["tick"] = static_cast<int64_t>(sim->tick());
    out["id"] = ids;
    out["kind"] = kind;
    out["def"] = def;
    out["org"] = org;
    out["player"] = player;
    out["moving"] = moving;
    out["prev_pos"] = prevPos;
    out["cur_pos"] = curPos;
    out["prev_facing"] = prevFacing;
    out["cur_facing"] = curFacing;
    return out;
}

// ------------------------------------------------------------------------------ batched units

void SimBridge::set_view_heights(const PackedFloat32Array& heights, int64_t vertsPerRow, double tile) {
    viewHeights.assign(heights.ptr(), heights.ptr() + heights.size());
    viewRow = vertsPerRow;
    viewTile = static_cast<float>(tile);
}

// Same bilinear lookup as BattleTerrain.height_at().
float SimBridge::viewHeight(float x, float y) const {
    if (viewRow < 2 || viewHeights.size() < static_cast<std::size_t>(viewRow) * 2) return 0.0f;
    const int64_t tilesX = viewRow - 1;
    const int64_t tilesY = static_cast<int64_t>(viewHeights.size()) / viewRow - 1;
    const float fx = std::clamp(x / viewTile, 0.0f, static_cast<float>(tilesX) - 0.001f);
    const float fy = std::clamp(y / viewTile, 0.0f, static_cast<float>(tilesY) - 0.001f);
    const int64_t ix = static_cast<int64_t>(fx);
    const int64_t iy = static_cast<int64_t>(fy);
    const float tx = fx - static_cast<float>(ix);
    const float ty = fy - static_cast<float>(iy);
    const float* r0 = viewHeights.data() + iy * viewRow + ix;
    const float* r1 = r0 + viewRow;
    const float a = r0[0] + (r0[1] - r0[0]) * tx;
    const float b = r1[0] + (r1[1] - r1[0]) * tx;
    return a + (b - a) * ty;
}

Dictionary SimBridge::unit_instances(double alpha, const PackedInt64Array& selectedOrgs) const {
    Dictionary out;
    if (!sim) return out;
    std::vector<uint32_t> selected;
    selected.reserve(static_cast<std::size_t>(selectedOrgs.size()));
    for (int64_t i = 0; i < selectedOrgs.size(); ++i) selected.push_back(static_cast<uint32_t>(selectedOrgs[i]));
    std::sort(selected.begin(), selected.end());
    struct Lists {
        std::vector<float> units, rings;
    };
    std::map<int32_t, Lists> byDef;
    const float a = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
    const corps::World& w = sim->world();
    auto push = [](std::vector<float>& v, float c, float s, float x, float y, float z, float c0, float c1, float c2) {
        // MultiMesh 3D layout: basis rows with the origin as the fourth column, then custom data.
        const float row[16] = {c, 0, s, x, 0, 1, 0, y, -s, 0, c, z, c0, c1, c2, 0};
        v.insert(v.end(), row, row + 16);
    };
    for (std::size_t i = 0; i < w.transforms.size(); ++i) {
        const EntityId id = w.transforms.ids()[i];
        const corps::Mover* mv = w.movers.get(id);
        if (mv == nullptr) continue;
        const corps::Transform& t = w.transforms.data()[i];
        const corps::DefRef* dr = w.defRefs.get(id);
        const corps::Member* m = w.members.get(id);
        const corps::Regiment* r = w.regiments.get(m != nullptr ? m->organization : id);
        float x1 = static_cast<float>(toF(t.position.x));
        float y1 = static_cast<float>(toF(t.position.y));
        float f1 = angleRad(t.facing);
        float x0 = x1, y0 = y1, f0 = f1;
        const auto it = std::lower_bound(prev.begin(), prev.end(), id.bits, [](const Snap& s, uint32_t b) {
            return EntityId{s.bits}.index() < EntityId{b}.index();
        });
        if (it != prev.end() && it->bits == id.bits) {
            x0 = it->x;
            y0 = it->y;
            f0 = it->facing;
        }
        const float x = x0 + (x1 - x0) * a;
        const float y = y0 + (y1 - y0) * a;
        float df = std::fmod(f1 - f0, static_cast<float>(kTwoPi));
        df = std::fmod(2.0f * df, static_cast<float>(kTwoPi)) - df;  // shortest turn, as lerp_angle
        const float yaw = -(f0 + df * a);
        const float c = std::cos(yaw);
        const float s = std::sin(yaw);
        const float h = viewHeight(x, y);
        const float moving = toF(mv->velocity.lengthSq()) > 0.0025 ? 1.0f : 0.0f;
        const float phase = static_cast<float>((id.index() * 2654435761u) >> 8) / 16777216.0f;
        const float player = static_cast<float>(r != nullptr ? r->player : 0);
        Lists& l = byDef[dr != nullptr ? static_cast<int32_t>(dr->index) : -1];
        push(l.units, c, s, x, h, y, moving, phase, player);
        const uint32_t org = m != nullptr ? m->organization.bits : 0;
        if (!selected.empty() && std::binary_search(selected.begin(), selected.end(), org))
            push(l.rings, 1, 0, x, h + 0.08f, y, player, 0, 0);
    }
    for (const auto& [def, l] : byDef) {
        PackedFloat32Array units, rings;
        units.resize(static_cast<int64_t>(l.units.size()));
        std::copy(l.units.begin(), l.units.end(), units.ptrw());
        rings.resize(static_cast<int64_t>(l.rings.size()));
        std::copy(l.rings.begin(), l.rings.end(), rings.ptrw());
        Dictionary d;
        d["count"] = static_cast<int64_t>(l.units.size() / 16);
        d["units"] = units;
        d["ring_count"] = static_cast<int64_t>(l.rings.size() / 16);
        d["rings"] = rings;
        out[def] = d;
    }
    return out;
}

Dictionary SimBridge::regiment_info(int64_t regimentBits) const {
    Dictionary out;
    if (!sim) return out;
    const corps::World& w = sim->world();
    const EntityId id{static_cast<uint32_t>(regimentBits)};
    const corps::Regiment* r = w.registry.alive(id) ? w.regiments.get(id) : nullptr;
    if (r == nullptr) return out;
    const corps::Order* o = w.orders.get(id);
    const corps::DefRef* dr = w.defRefs.get(id);
    out["def"] = dr != nullptr ? static_cast<int64_t>(dr->index) : -1;
    out["player"] = static_cast<int64_t>(r->player);
    out["move_class"] = static_cast<int64_t>(r->moveClass);
    out["compression"] = static_cast<int64_t>(r->compression);
    out["running"] = r->runRestore != corps::Regiment::kNone;
    String formation;
    if (r->layout != corps::kNoDef && r->formation < data->defs.layouts[r->layout].formations.size()) {
        formation = str(data->defs.layouts[r->layout].formations[r->formation].formationIds);
    }
    out["formation"] = formation;
    out["state"] = o != nullptr ? stateName(o->state) : "?";
    out["blocked"] = o != nullptr && o->blocked;
    out["goal"] = o != nullptr ? toV(o->goal) : Vector2();
    const corps::Group* g = r->group.isNull() ? nullptr : w.groups.get(r->group);
    out["group"] = static_cast<int64_t>(r->group.bits);
    out["division"] = g != nullptr && g->division;
    PackedInt64Array units;
    PackedVector2Array slots;
    for (const auto& [unit, target] : corps::slotTargets(*sim, id)) {
        units.push_back(unit.bits);
        slots.push_back(toV(target));
    }
    out["units"] = units;
    out["slots"] = slots;
    // The path the regiment follows: its own, or its division's.
    const EntityId pathOwner = g != nullptr && g->division ? r->group : id;
    const corps::Path* p = w.paths.get(pathOwner);
    const corps::Order* po = w.orders.get(pathOwner);
    PackedVector2Array path;
    if (p != nullptr && sim->nav() != nullptr) {
        const corps::nav::NavGrid& grid = sim->nav()->grid;
        for (const uint32_t c : p->waypoints) path.push_back(Vector2(static_cast<real_t>(grid.xOf(c)) + 0.5f, static_cast<real_t>(grid.yOf(c)) + 0.5f));
    }
    out["path"] = path;
    out["path_status"] = p != nullptr ? static_cast<int64_t>(p->status) : -1;
    out["waypoint"] = po != nullptr ? static_cast<int64_t>(po->waypoint) : 0;
    const corps::Transform* anchor = w.transforms.get(id);
    out["anchor"] = anchor != nullptr ? toV(anchor->position) : Vector2();
    out["facing"] = anchor != nullptr ? angleRad(anchor->facing) : 0.0f;
    return out;
}

PackedByteArray SimBridge::cost_grid(int64_t moveClass) const {
    PackedByteArray out;
    if (!sim || sim->nav() == nullptr || moveClass < 0 || static_cast<std::size_t>(moveClass) >= corps::nav::kMoveClassCount) return out;
    const corps::nav::NavGrid& g = sim->nav()->grid;
    const auto c = static_cast<corps::nav::MoveClass>(moveClass);
    const int64_t n = static_cast<int64_t>(g.width()) * g.height();
    out.resize(n);
    for (int64_t i = 0; i < n; ++i) out.set(i, g.cost(c, static_cast<uint32_t>(i)));
    return out;
}

}  // namespace godot
