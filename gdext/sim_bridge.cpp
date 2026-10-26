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

#include "ai/controller.hpp"
#include "core/ascii.hpp"
#include "core/hello.hpp"
#include "core/sim.hpp"
#include "data/def_compiler.hpp"
#include "data/json_lite.hpp"
#include "data/tgi/pipeline.hpp"
#include "nav/nav_world.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"
#include "systems/bases.hpp"
#include "systems/economy.hpp"
#include "systems/morale.hpp"
#include "systems/movement.hpp"
#include "systems/supply.hpp"
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
    std::map<std::string, std::string> strings;  // localization: lower-case key (no "|s") -> text
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
    case corps::Order::State::Engaged: return "engaged";
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
    case corps::SimEvent::Type::Hit: return "hit";
    case corps::SimEvent::Type::Recruited: return "recruited";
    case corps::SimEvent::Type::Unpacked: return "unpacked";
    case corps::SimEvent::Type::Packed: return "packed";
    case corps::SimEvent::Type::ResearchDone: return "research_done";
    case corps::SimEvent::Type::Bankrupt: return "bankrupt";
    case corps::SimEvent::Type::Solvent: return "solvent";
    case corps::SimEvent::Type::DeployFailed: return "deploy_failed";
    case corps::SimEvent::Type::Replaced: return "replaced";
    case corps::SimEvent::Type::CityCaptured: return "city_captured";
    case corps::SimEvent::Type::KingdomDefeated: return "kingdom_defeated";
    case corps::SimEvent::Type::TeamVictory: return "team_victory";
    case corps::SimEvent::Type::CityRatioVictory: return "city_ratio_victory";
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
    ClassDB::bind_method(D_METHOD("mount", "game_dir"), &SimBridge::mount);
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
    ClassDB::bind_method(D_METHOD("banner_instances", "alpha", "lift", "selected_orgs"), &SimBridge::banner_instances);
    ClassDB::bind_method(D_METHOD("record", "type", "ids"), &SimBridge::record);
    ClassDB::bind_method(D_METHOD("ability_record", "ability"), &SimBridge::ability_record);
    ClassDB::bind_method(D_METHOD("unit_record", "unit_def"), &SimBridge::unit_record);
    ClassDB::bind_method(D_METHOD("read_file", "path"), &SimBridge::read_file);
    ClassDB::bind_method(D_METHOD("queue_attack", "regiments", "target", "player"), &SimBridge::queue_attack);
    ClassDB::bind_method(D_METHOD("queue_attack_move", "regiments", "goal", "player"), &SimBridge::queue_attack_move);
    ClassDB::bind_method(D_METHOD("queue_retreat", "regiments", "goal", "player"), &SimBridge::queue_retreat);
    ClassDB::bind_method(D_METHOD("queue_start_player", "building_ids", "pos", "facing", "player"), &SimBridge::queue_start_player);
    ClassDB::bind_method(D_METHOD("queue_recruit", "building", "ids", "player"), &SimBridge::queue_recruit);
    ClassDB::bind_method(D_METHOD("queue_cancel_recruit", "building", "index", "player"), &SimBridge::queue_cancel_recruit);
    ClassDB::bind_method(D_METHOD("queue_unpack", "regiment", "pos", "facing", "player"), &SimBridge::queue_unpack);
    ClassDB::bind_method(D_METHOD("queue_pack", "building", "player"), &SimBridge::queue_pack);
    ClassDB::bind_method(D_METHOD("queue_research", "building", "property_ids", "player"), &SimBridge::queue_research);
    ClassDB::bind_method(D_METHOD("queue_cancel_research", "building", "player"), &SimBridge::queue_cancel_research);
    ClassDB::bind_method(D_METHOD("map_starts"), &SimBridge::map_starts);
    ClassDB::bind_method(D_METHOD("nation_thing", "nation", "placeholder"), &SimBridge::nation_thing);
    ClassDB::bind_method(D_METHOD("economy", "player"), &SimBridge::economy);
    ClassDB::bind_method(D_METHOD("structures"), &SimBridge::structures);
    ClassDB::bind_method(D_METHOD("structure_info", "building"), &SimBridge::structure_info);
    ClassDB::bind_method(D_METHOD("building_commands", "building", "player"), &SimBridge::building_commands);
    ClassDB::bind_method(D_METHOD("unpack_info", "regiment"), &SimBridge::unpack_info);
    ClassDB::bind_method(D_METHOD("placement", "building_ids", "at", "facing", "player", "ignore"), &SimBridge::placement);
    ClassDB::bind_method(D_METHOD("supply_zones"), &SimBridge::supply_zones);
    ClassDB::bind_method(D_METHOD("thing_info", "ids"), &SimBridge::thing_info);
    ClassDB::bind_method(D_METHOD("property_info", "ids"), &SimBridge::property_info);
    ClassDB::bind_method(D_METHOD("localize", "key"), &SimBridge::localize);
    ClassDB::bind_method(D_METHOD("read_file_for_nation", "path", "nation"), &SimBridge::read_file_for_nation);
    ClassDB::bind_method(D_METHOD("queue_match_rules", "teams", "city_ratio", "team_victory", "start_money", "player"),
                         &SimBridge::queue_match_rules);
    ClassDB::bind_method(D_METHOD("set_controller", "player", "kind", "nation"), &SimBridge::set_controller);
    ClassDB::bind_method(D_METHOD("drain_ai_notes"), &SimBridge::drain_ai_notes);
    ClassDB::bind_method(D_METHOD("save_replay", "path"), &SimBridge::save_replay);
}

String SimBridge::hello() const {
    return str(corps::simHello());
}

String SimBridge::last_error() const {
    return str(error);
}

bool SimBridge::mount(const String& game_dir) {
    error.clear();
    auto d = std::make_unique<Data>();
    std::vector<std::string> log;
    if (!corps::vfs::mountGameInstall(d->vfs, corps::vfs::pathFromUtf8(utf8(game_dir)), {}, log)) {
        error = "nothing could be mounted from " + utf8(game_dir);
        return false;
    }
    if (!sim) data = std::move(d);
    return true;
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
    // Localization: every Text record's fields ("key|s = text"); the first file loaded wins.
    for (const corps::tgi::Record& r : d->db->records) {
        if (!corps::iequals(r.typeSpelling, "text")) continue;
        for (const corps::tgi::ResolvedField& f : r.tree.fields) {
            if (!f.hasValue()) continue;
            std::string key = corps::asciiLowerCopy(f.spelling);
            if (const std::size_t bar = key.find('|'); bar != std::string::npos) key.resize(bar);
            d->strings.emplace(std::move(key), std::string(corps::trimAscii(f.text())));
        }
    }

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
    recorded.clear();
    controllers.clear();
    aiNotes = PackedStringArray();
    seed_ = static_cast<uint64_t>(seed);
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
    out["city_victory_ratio"] = toF(map->rule("CityVictoryRatio", data ? data->defs.economy.cityVictoryRatio : Fixed{}));
    out["start_money"] = toF(map->rule("ResourceStockpileDefault", data ? data->defs.economy.startMoney : Fixed{}));
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
    PackedInt32Array abilities;  // its weapons' DefSet abilities (FxRunner preloads their effects)
    for (const corps::WeaponDef& wd : u.weapons) {
        if (wd.ability != corps::kNoDef) abilities.push_back(static_cast<int32_t>(wd.ability));
    }
    out["abilities"] = abilities;
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
    recorded.push_back(c);
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

void SimBridge::queue_attack(const PackedInt64Array& regiments, int64_t target, int64_t player) {
    for (int64_t s = 0; s < regiments.size(); s += static_cast<int64_t>(corps::kMaxCommandRegiments)) {
        queue(corps::makeAttack(0, static_cast<uint8_t>(player), 0,
                                regimentList(regiments.slice(s, s + static_cast<int64_t>(corps::kMaxCommandRegiments))),
                                EntityId{static_cast<uint32_t>(target)}));
    }
}

void SimBridge::queue_attack_move(const PackedInt64Array& regiments, const Vector2& goal, int64_t player) {
    for (int64_t s = 0; s < regiments.size(); s += static_cast<int64_t>(corps::kMaxCommandRegiments)) {
        queue(corps::makeAttackMove(0, static_cast<uint8_t>(player), 0,
                                    regimentList(regiments.slice(s, s + static_cast<int64_t>(corps::kMaxCommandRegiments))), fromV(goal)));
    }
}

void SimBridge::queue_retreat(const PackedInt64Array& regiments, const Vector2& goal, int64_t player) {
    for (int64_t s = 0; s < regiments.size(); s += static_cast<int64_t>(corps::kMaxCommandRegiments)) {
        queue(corps::makeRetreat(0, static_cast<uint8_t>(player), 0,
                                 regimentList(regiments.slice(s, s + static_cast<int64_t>(corps::kMaxCommandRegiments))), fromV(goal)));
    }
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
    for (const auto& ctl : controllers) {  // host-side players: commands for this tick
        std::vector<corps::Command> out;
        ctl->think(*sim, out);
        for (corps::Command& cmd : out) queue(std::move(cmd));
        for (const std::string& n : ctl->notes) aiNotes.push_back(str(n));
        ctl->notes.clear();
    }
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
        d["tick"] = static_cast<int64_t>(sim->tick() - 1);
        d["other"] = static_cast<int64_t>(e.other.bits);  // fired: the target; died: the killer; hit: the shooter
        d["detail"] = static_cast<int64_t>(e.detail);     // fired / hit: the DefSet ability
        if (e.type == corps::SimEvent::Type::ResearchDone && e.detail < data->defs.properties.size()) {
            d["ids"] = str(data->defs.properties[e.detail].ids);
        }
        d["at"] = toV(e.at);
        d["facing"] = angleRad(e.facing);
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
        const corps::Structure* st = w.structures.get(id);
        const corps::Combat* cb = w.combats.get(id);
        player.set(i, st != nullptr ? st->player : (r != nullptr ? r->player : (cb != nullptr ? cb->player : 0)));
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

PackedFloat32Array SimBridge::banner_instances(double alpha, double lift, const PackedInt64Array& selectedOrgs) const {
    PackedFloat32Array out;
    if (!sim) return out;
    std::vector<uint32_t> selected;
    for (int64_t i = 0; i < selectedOrgs.size(); ++i) selected.push_back(static_cast<uint32_t>(selectedOrgs[i]));
    std::sort(selected.begin(), selected.end());
    const corps::World& w = sim->world();
    const float a = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
    // Per regiment: living units' interpolated centroid and health.
    struct Acc {
        float x = 0, y = 0, hp = 0, hpMax = 0;
        int n = 0;
    };
    std::map<uint32_t, Acc> acc;
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        const EntityId u = w.members.ids()[i];
        const corps::Health* h = w.health.get(u);
        const corps::Transform* t = w.transforms.get(u);
        if (h == nullptr || t == nullptr || h->current <= Fixed{} || !w.registry.alive(u)) continue;
        float x1 = static_cast<float>(toF(t->position.x)), y1 = static_cast<float>(toF(t->position.y));
        float x0 = x1, y0 = y1;
        const auto it = std::lower_bound(prev.begin(), prev.end(), u.bits,
                                         [](const Snap& s, uint32_t b) { return EntityId{s.bits}.index() < EntityId{b}.index(); });
        if (it != prev.end() && it->bits == u.bits) {
            x0 = it->x;
            y0 = it->y;
        }
        Acc& r = acc[w.members.data()[i].organization.bits];
        r.x += x0 + (x1 - x0) * a;
        r.y += y0 + (y1 - y0) * a;
        r.hp += static_cast<float>(toF(h->current));
        r.hpMax += static_cast<float>(toF(h->max));
        ++r.n;
    }
    for (const auto& [bits, r] : acc) {
        const EntityId id{bits};
        const corps::Regiment* reg = w.registry.alive(id) ? w.regiments.get(id) : nullptr;
        const corps::Engagement* e = w.engagements.get(id);
        if (reg == nullptr || e == nullptr || r.n == 0) continue;
        float& top = regimentHealthMax[bits];
        top = std::max(top, r.hpMax);
        const float cx = r.x / static_cast<float>(r.n), cy = r.y / static_cast<float>(r.n);
        const float h = viewHeight(cx, cy) + static_cast<float>(lift);
        const float moraleMax = static_cast<float>(toF(corps::moraleMaxOf(*sim, id)));
        const float fills[4] = {1.0f, top > 0 ? r.hp / top : 0.0f, moraleMax > 0 ? static_cast<float>(toF(e->morale)) / moraleMax : 0.0f,
                                static_cast<float>(toF(e->efficiency))};
        float flags = 0;
        if (e->state == corps::Engagement::State::Routing) flags += 1;
        if (e->entrenched) flags += 2;
        if (std::binary_search(selected.begin(), selected.end(), bits)) flags += 4;
        for (int k = 0; k < 4; ++k) {
            const float row[16] = {1, 0, 0, cx, 0, 1, 0, h, 0, 0, 1, cy, std::clamp(fills[k], 0.0f, 1.0f), static_cast<float>(k),
                                   static_cast<float>(reg->player), flags};
            for (const float v : row) out.push_back(v);
        }
    }
    return out;
}

// ------------------------------------------------------------------------------ records for the view

Dictionary SimBridge::nodeDict(const void* p) const {
    const auto& n = *static_cast<const corps::tgi::ResolvedNode*>(p);
    Dictionary d;
    d["_name"] = str(corps::asciiLowerCopy(n.spelling));
    for (const corps::tgi::ResolvedField& f : n.fields) {
        if (f.hasValue()) d[str(corps::asciiLowerCopy(f.spelling))] = str(corps::trimAscii(f.text()));
    }
    Array children;
    for (const corps::tgi::NodeRef& c : n.children) {
        if (c) children.push_back(nodeDict(c.get()));
    }
    d["_children"] = children;
    return d;
}

Dictionary SimBridge::record(const String& type, const String& ids) const {
    if (!data || !data->db) return Dictionary();
    const corps::tgi::TgiDatabase& db = *data->db;
    const corps::tgi::Symbol t = db.symbols.find(corps::asciiLowerCopy(utf8(type)));
    if (t == corps::tgi::kNoSymbol) return Dictionary();
    const auto* e = db.defs.find(t, corps::asciiLowerCopy(utf8(ids)));
    return e != nullptr ? nodeDict(&db.records[e->record].tree) : Dictionary();
}

Dictionary SimBridge::unit_record(int64_t unitDef) const {
    if (!data || unitDef < 0 || static_cast<std::size_t>(unitDef) >= data->defs.units.size()) return Dictionary();
    return record("thing", str(data->defs.units[static_cast<std::size_t>(unitDef)].ids));
}

Dictionary SimBridge::ability_record(int64_t ability) const {
    if (!data || !data->db || ability < 0 || static_cast<std::size_t>(ability) >= data->defs.abilities.size()) return Dictionary();
    const corps::AbilityDef& ab = data->defs.abilities[static_cast<std::size_t>(ability)];
    const std::size_t hash = ab.ids.find('#');
    if (hash == std::string::npos) return record("abilitydefinition", str(ab.ids));
    // Inline: the n-th Ability slot of the owner's PerformerComponent, then DefenderComponent
    // (numbered as the def compiler numbers them).
    const corps::tgi::TgiDatabase& db = *data->db;
    const auto* e = db.defs.find(db.symbols.find("thing"), corps::asciiLowerCopy(ab.ownerIds));
    if (e == nullptr) return Dictionary();
    const std::size_t want = static_cast<std::size_t>(std::strtoul(ab.ids.c_str() + hash + 1, nullptr, 10));
    std::size_t k = 0;
    const corps::tgi::ResolvedNode& thing = db.records[e->record].tree;
    for (const char* comp : {"performercomponent", "defendercomponent"}) {
        const corps::tgi::Symbol cs = db.symbols.find(comp);
        const corps::tgi::Symbol as = db.symbols.find("ability");
        const corps::tgi::Symbol ds = db.symbols.find("abilitydefinition");
        const corps::tgi::ResolvedNode* cn = cs != corps::tgi::kNoSymbol ? thing.child(cs) : nullptr;
        if (cn == nullptr || as == corps::tgi::kNoSymbol) continue;
        for (std::size_t i = 0; i < cn->childCount(as); ++i, ++k) {
            if (k != want) continue;
            const corps::tgi::ResolvedNode* def = cn->child(as, i)->child(ds);
            return def != nullptr ? nodeDict(def) : Dictionary();
        }
    }
    return Dictionary();
}

PackedByteArray SimBridge::read_file(const String& path) const {
    PackedByteArray out;
    if (!data) return out;
    std::string bytes;
    if (!data->vfs.read(utf8(path), bytes)) return out;
    out.resize(static_cast<int64_t>(bytes.size()));
    std::copy(bytes.begin(), bytes.end(), out.ptrw());
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
    // Combat (M5): health, morale, efficiency, experience, engagement.
    float hp = 0, hpMax = 0;
    int64_t alive = 0;
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        if (w.members.data()[i].organization != id) continue;
        const corps::Health* h = w.health.get(w.members.ids()[i]);
        if (h == nullptr || h->current <= Fixed{}) continue;
        hp += static_cast<float>(toF(h->current));
        hpMax += static_cast<float>(toF(h->max));
        ++alive;
    }
    float& top = regimentHealthMax[id.bits];
    top = std::max(top, hpMax);
    out["units_alive"] = alive;
    out["health"] = hp;
    out["health_max"] = top;
    if (const corps::Engagement* e = w.engagements.get(id)) {
        static const char* kEngagement[] = {"idle", "engaging", "stalemate", "reform", "regroup", "retreating", "routing", "exhausted"};
        static const char* kMode[] = {"none", "attack", "attack_move"};
        out["engagement"] = kEngagement[static_cast<int>(e->state)];
        out["mode"] = kMode[static_cast<int>(e->mode)];
        out["morale"] = toF(e->morale);
        out["morale_max"] = toF(corps::moraleMaxOf(*sim, id));
        out["efficiency"] = toF(e->efficiency);
        out["xp"] = toF(e->xp);
        out["level"] = static_cast<int64_t>(e->level);
        out["entrenched"] = e->entrenched;
        out["attack_target"] = static_cast<int64_t>(e->target.bits);
    }
    const corps::Supply* sp = w.supplies.get(id);
    out["in_supply"] = sp != nullptr && sp->inSupply;
    out["besieged"] = sp != nullptr && sp->besieged;
    if (dr != nullptr && dr->index < data->defs.regiments.size()) {
        const corps::RegimentDef& rd = data->defs.regiments[dr->index];
        out["breakpoint"] = toF(rd.moraleBreakpoint);
        PackedFloat32Array levels;
        for (const corps::ExperienceLevelDef& l : rd.experienceLevels) levels.push_back(static_cast<float>(toF(l.required)));
        out["level_xp"] = levels;
    }
    return out;
}

// ------------------------------------------------------------------------------ bases and HUD (M6 part 3)

namespace {

const char* structureStateName(corps::Structure::State s) {
    switch (s) {
    case corps::Structure::State::Deployed: return "deployed";
    case corps::Structure::State::Unpacking: return "unpacking";
    case corps::Structure::State::Packing: return "packing";
    }
    return "?";
}

std::string dirOf(const std::string& path) {
    const std::size_t s = path.find_last_of('/');
    return s == std::string::npos ? std::string() : path.substr(0, s);
}

}  // namespace

void SimBridge::queue_start_player(const String& ids, const Vector2& pos, double facing, int64_t player) {
    queue(corps::makeStartPlayer(0, static_cast<uint8_t>(player), 0, utf8(ids), fromV(pos), fromRad(facing)));
}
void SimBridge::queue_recruit(int64_t building, const String& ids, int64_t player) {
    queue(corps::makeRecruit(0, static_cast<uint8_t>(player), 0, EntityId{static_cast<uint32_t>(building)}, utf8(ids)));
}
void SimBridge::queue_cancel_recruit(int64_t building, int64_t index, int64_t player) {
    queue(corps::makeCancelRecruit(0, static_cast<uint8_t>(player), 0, EntityId{static_cast<uint32_t>(building)},
                                   static_cast<uint8_t>(std::clamp<int64_t>(index, 0, 255))));
}
void SimBridge::queue_unpack(int64_t regiment, const Vector2& pos, double facing, int64_t player) {
    queue(corps::makeUnpack(0, static_cast<uint8_t>(player), 0, EntityId{static_cast<uint32_t>(regiment)}, fromV(pos), fromRad(facing)));
}
void SimBridge::queue_pack(int64_t building, int64_t player) {
    queue(corps::makePack(0, static_cast<uint8_t>(player), 0, EntityId{static_cast<uint32_t>(building)}));
}
void SimBridge::queue_research(int64_t building, const String& ids, int64_t player) {
    queue(corps::makeResearch(0, static_cast<uint8_t>(player), 0, EntityId{static_cast<uint32_t>(building)}, utf8(ids)));
}
void SimBridge::queue_cancel_research(int64_t building, int64_t player) {
    queue(corps::makeCancelResearch(0, static_cast<uint8_t>(player), 0, EntityId{static_cast<uint32_t>(building)}));
}

Array SimBridge::map_starts() const {
    Array out;
    if (!map) return out;
    for (const corps::MapStart& s : corps::mapStarts(*map)) {
        Dictionary d;
        d["player"] = static_cast<int64_t>(s.player);
        d["placeholder"] = str(s.placeholder);
        d["position"] = toV(s.position);
        d["facing"] = angleRad(s.facing);
        out.push_back(d);
    }
    return out;
}

String SimBridge::nation_thing(const String& nation, const String& placeholder) const {
    return data ? str(corps::placeholderThing(data->defs, utf8(nation), utf8(placeholder))) : String();
}

Dictionary SimBridge::economy(int64_t player) const {
    Dictionary out;
    if (!sim || player < 0 || player >= static_cast<int64_t>(corps::kMaxPlayers)) return out;
    const corps::Kingdom& k = sim->world().kingdoms[static_cast<std::size_t>(player)];
    const corps::Ledger l = corps::ledgerOf(*sim, static_cast<uint8_t>(player));
    out["started"] = k.started;
    out["money"] = toF(k.money);
    out["money_net"] = toF(l.moneyNet());
    out["money_production"] = toF(l.production.money);
    out["money_upkeep"] = toF(l.upkeep.money);
    out["oil"] = toF(l.oilNet());
    out["ammo"] = toF(l.ammoNet());
    out["oil_production"] = toF(l.production.oil);
    out["oil_upkeep"] = toF(l.upkeep.oil);
    out["ammo_production"] = toF(l.production.ammo);
    out["ammo_upkeep"] = toF(l.upkeep.ammo);
    out["shortfall"] = toF(l.shortfallCost);
    out["unit_used"] = toF(l.upkeep.unitLimitConsumed);
    out["unit_limit"] = toF(l.unitLimit);
    out["building_used"] = toF(l.upkeep.buildingLimitConsumed);
    out["building_limit"] = toF(l.buildingLimit);
    out["bankrupt"] = k.bankrupt;
    out["eliminated"] = k.eliminated;
    out["team"] = static_cast<int64_t>(k.team);
    out["army_xp"] = toF(k.armyXp);
    const corps::Match& m = sim->world().match;
    out["match_over"] = m.over;
    out["winner"] = static_cast<int64_t>(m.winner);
    out["city_ratio"] = toF(m.cityVictoryRatio);
    out["end_tick"] = static_cast<int64_t>(m.endTick);
    PackedStringArray techs;  // GLOBAL research granted
    for (const std::uint32_t p : k.properties) {
        if (p < data->defs.properties.size()) techs.push_back(str(data->defs.properties[p].ids));
    }
    out["technologies"] = techs;
    return out;
}

namespace {

Dictionary structureDict(const corps::Sim& sim, EntityId id) {
    Dictionary d;
    const corps::World& w = sim.world();
    const corps::Structure* s = w.structures.get(id);
    const corps::DefRef* r = w.defRefs.get(id);
    const corps::Transform* t = w.transforms.get(id);
    const corps::DefSet& defs = *sim.config().defs;
    if (s == nullptr || r == nullptr || t == nullptr || r->index >= defs.buildings.size()) return d;
    const corps::BuildingDef& b = defs.buildings[r->index];
    d["id"] = static_cast<int64_t>(id.bits);
    d["ids"] = str(b.ids);
    d["def"] = static_cast<int64_t>(r->index);
    d["player"] = static_cast<int64_t>(s->player);
    d["state"] = structureStateName(s->state);
    d["position"] = toV(t->position);
    d["facing"] = angleRad(t->facing);
    const corps::Health* h = w.health.get(id);
    d["health"] = h != nullptr ? toF(h->current) : 0.0;
    d["health_max"] = h != nullptr ? toF(h->max) : 0.0;
    if (const corps::Footprint* fp = w.footprints.get(id)) d["rect"] = Rect2i(fp->x0, fp->y0, fp->x1 - fp->x0 + 1, fp->y1 - fp->y0 + 1);
    d["city"] = corps::isCity(defs, r->index);
    d["in_supply"] = s->inSupply;
    d["under_siege"] = s->underSiege;
    d["capturer"] = static_cast<int64_t>(s->capturer);
    d["capture"] = std::min(1.0, static_cast<double>(s->captureTicks) / 50.0);  // systems/supply: 5 s
    return d;
}

}  // namespace

Array SimBridge::structures() const {
    Array out;
    if (!sim) return out;
    const corps::World& w = sim->world();
    for (std::size_t i = 0; i < w.structures.size(); ++i) {
        const EntityId id = w.structures.ids()[i];
        if (!w.registry.alive(id)) continue;
        Dictionary d = structureDict(*sim, id);
        if (!d.is_empty()) out.push_back(d);
    }
    return out;
}

Dictionary SimBridge::structure_info(int64_t building) const {
    if (!sim) return Dictionary();
    const corps::World& w = sim->world();
    const EntityId id{static_cast<uint32_t>(building)};
    if (!w.registry.alive(id) || !w.structures.has(id)) return Dictionary();
    Dictionary d = structureDict(*sim, id);
    if (d.is_empty()) return d;
    const corps::Structure& s = *w.structures.get(id);
    const corps::DefSet& defs = data->defs;
    const corps::BuildingDef& b = defs.buildings[static_cast<std::size_t>(int64_t(d["def"]))];
    const auto ticks = [](Fixed seconds) { return std::max(1.0, toF(seconds) * corps::kTicksPerSecond); };
    double progress = 1.0;
    if (s.state == corps::Structure::State::Unpacking) progress = 1.0 - s.stateTicks / ticks(b.unpackingLength);
    if (s.state == corps::Structure::State::Packing) progress = 1.0 - s.stateTicks / ticks(b.packingLength);
    d["progress"] = std::clamp(progress, 0.0, 1.0);
    Array queueOut;
    for (const corps::RecruitOrder& o : s.queue) {
        Dictionary q;
        const corps::RegimentDef& g = defs.regiments[o.regiment];
        const Dictionary ti = thing_info(str(g.ids));
        q["ids"] = str(g.ids);
        q["name"] = ti.get("name", str(g.ids));
        q["icon"] = ti.get("icon", String());
        q["paid"] = toF(o.paid);
        queueOut.push_back(q);
    }
    d["queue"] = queueOut;
    double recruit = 0.0;
    if (!s.queue.empty() && !s.recruit.isNull()) {
        const std::size_t n = std::max<std::size_t>(1, corps::recruitPlan(defs, s.queue.front().regiment).size());
        const double interval = ticks(defs.regiments[s.queue.front().regiment].recruitTime) / static_cast<double>(n);
        recruit = (s.nextElement + std::clamp(1.0 - s.recruitTicks / interval, 0.0, 1.0)) / static_cast<double>(n);
    }
    d["recruit_progress"] = std::clamp(recruit, 0.0, 1.0);
    if (s.research != corps::Structure::kNone && s.research < b.research.size()) {
        const corps::ResearchItemDef& r = b.research[s.research];
        Dictionary rd = property_info(str(r.propertyIds));
        rd["ids"] = str(r.propertyIds);
        rd["progress"] = std::clamp(1.0 - s.researchTicks / ticks(r.time), 0.0, 1.0);
        d["research"] = rd;
    }
    Dictionary prod;
    prod["money"] = toF(b.production.money);
    prod["oil"] = toF(b.production.oil);
    prod["ammo"] = toF(b.production.ammo);
    d["production"] = prod;
    d["supply_range"] = toF(b.supplyRange);
    return d;
}

Array SimBridge::building_commands(int64_t building, int64_t player) const {
    Array out;
    if (!sim) return out;
    const corps::World& w = sim->world();
    const EntityId id{static_cast<uint32_t>(building)};
    const corps::DefRef* r = w.registry.alive(id) && w.structures.has(id) ? w.defRefs.get(id) : nullptr;
    const corps::DefSet& defs = data->defs;
    if (r == nullptr || r->index >= defs.buildings.size()) return out;
    const corps::BuildingDef& b = defs.buildings[r->index];
    const auto p = static_cast<uint8_t>(player);
    const auto describe = [&](Dictionary& d, const corps::Blocker& why) {
        d["enabled"] = why.ok();
        d["code"] = str(why.code);
        d["detail"] = str(why.detail);
        String name;
        if (why.code == "requires_structure") name = thing_info(str(why.detail)).get("name", str(why.detail));
        if (why.code == "requires_property") name = property_info(str(why.detail)).get("name", str(why.detail));
        d["detail_name"] = name;
    };
    for (const corps::RecruitRef& ref : b.recruitRefs) {
        const corps::RegimentDef& g = defs.regiments[ref.regiment];
        bool truck = false;
        for (const corps::RecruitElement& e : corps::recruitPlan(defs, ref.regiment)) {
            truck = truck || (e.element.kind == corps::ElementKind::Unit && defs.units[e.element.index].unpackTo != corps::kNoDef);
        }
        // A bare truck (a convoy) is shown as its truck: its record has the name and icon.
        const Dictionary ti = thing_info(str(g.convoy ? ref.ids : g.ids));
        Dictionary d;
        d["kind"] = truck ? "deploy" : "recruit";
        d["ids"] = str(ref.ids);
        d["name"] = ti.get("name", str(ref.ids));
        d["icon"] = ti.get("icon", String());
        d["description"] = ti.get("description", String());
        d["cost"] = toF(corps::recruitCost(defs, ref.regiment));
        d["time"] = toF(g.recruitTime);
        describe(d, corps::recruitBlocker(*sim, p, id, ref.ids));
        out.push_back(d);
    }
    for (const corps::ResearchItemDef& item : b.research) {
        if (item.scope == corps::ResearchItemDef::Scope::Support) continue;  // settlements: not yet
        Dictionary d = property_info(str(item.propertyIds));
        d["kind"] = "research";
        d["ids"] = str(item.propertyIds);
        d["cost"] = toF(item.cost.money);
        d["time"] = toF(item.time);
        describe(d, corps::researchBlocker(*sim, p, id, item.propertyIds));
        out.push_back(d);
    }
    if (b.mobile && b.packedRegiment != corps::kNoDef) {
        Dictionary d;
        d["kind"] = "pack";
        d["ids"] = str(b.ids);
        d["name"] = localize("#actor_commands_pack_name");
        d["icon"] = "UI/%s/Game/ActionButtons/IconPack.png";
        d["description"] = localize("#actor_commands_pack_tooltip");
        d["cost"] = 0.0;
        d["time"] = toF(b.packingLength);
        describe(d, corps::packBlocker(*sim, p, id));
        out.push_back(d);
    }
    return out;
}

Dictionary SimBridge::unpack_info(int64_t regiment) const {
    Dictionary out;
    if (!sim) return out;
    const corps::World& w = sim->world();
    const EntityId id{static_cast<uint32_t>(regiment)};
    if (!w.registry.alive(id) || !w.regiments.has(id)) return out;
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        if (w.members.data()[i].organization != id) continue;
        const corps::DefRef* r = w.defRefs.get(w.members.ids()[i]);
        if (r == nullptr || r->kind != corps::DefRef::Kind::Unit || data->defs.units[r->index].unpackTo == corps::kNoDef) continue;
        const corps::BuildingDef& b = data->defs.buildings[data->defs.units[r->index].unpackTo];
        const Dictionary ti = thing_info(str(b.ids));
        out["building"] = static_cast<int64_t>(data->defs.units[r->index].unpackTo);
        out["ids"] = str(b.ids);
        out["name"] = ti.get("name", str(b.ids));
        out["icon"] = ti.get("icon", String());
        out["unpacking_length"] = toF(b.unpackingLength);
        out["deploying"] = w.deploys.has(id);
        break;
    }
    return out;
}

Dictionary SimBridge::placement(const String& ids, const Vector2& at, double facing, int64_t player, int64_t ignore) const {
    Dictionary out;
    if (!sim) return out;
    const std::uint32_t bi = data->defs.buildingIndex(utf8(ids));
    if (bi == corps::kNoDef) return out;
    const corps::Placement pl = corps::placeBuilding(*sim, bi, fromV(at), fromRad(facing), static_cast<uint8_t>(player),
                                                     EntityId{static_cast<uint32_t>(ignore)});
    out["ok"] = pl.ok;
    out["why"] = String(pl.why);
    out["center"] = toV(pl.center);
    out["facing"] = angleRad(pl.facing);
    out["rect"] = Rect2i(pl.rect.x0, pl.rect.y0, pl.rect.x1 - pl.rect.x0 + 1, pl.rect.y1 - pl.rect.y0 + 1);
    return out;
}

Array SimBridge::supply_zones() const {
    Array out;
    if (!sim) return out;
    for (const corps::SupplyZone& z : corps::supplyZones(*sim)) {
        Dictionary d;
        d["source"] = static_cast<int64_t>(z.source.bits);
        d["center"] = toV(z.center);
        d["radius"] = toF(z.radius);
        d["team"] = static_cast<int64_t>(z.team);
        d["active"] = z.active;
        d["besieged"] = z.besieged;
        out.push_back(d);
    }
    return out;
}

Dictionary SimBridge::thing_info(const String& ids) const {
    Dictionary out;
    if (!data || !data->db) return out;
    const corps::tgi::TgiDatabase& db = *data->db;
    const auto* e = db.defs.find(db.symbols.find("thing"), corps::asciiLowerCopy(utf8(ids)));
    if (e == nullptr) return out;
    const corps::tgi::Record& rec = db.records[e->record];
    const auto field = [&](const corps::tgi::ResolvedNode* n, std::string_view key) -> std::string {
        const corps::tgi::Symbol k = db.symbols.find(key);
        const corps::tgi::ResolvedField* f = n != nullptr && k != corps::tgi::kNoSymbol ? n->field(k) : nullptr;
        if (f == nullptr || !f->hasValue()) return {};
        std::string v(corps::trimAscii(f->text()));
        return corps::iequals(v, "null") ? std::string() : v;
    };
    const corps::tgi::Symbol viewSym = db.symbols.find("view");
    const corps::tgi::ResolvedNode* view = viewSym != corps::tgi::kNoSymbol ? rec.tree.child(viewSym) : nullptr;
    out["name"] = localize(str(field(&rec.tree, "name")));
    out["description"] = localize(str(field(&rec.tree, "description")));
    out["icon"] = str(field(view, "icon"));
    out["minimap_icon"] = str(field(view, "minimap_icon"));
    const std::string prefix = field(view, "resource_prefix");
    const std::string dir = rec.file != nullptr ? dirOf(rec.file->path) : std::string();
    out["model"] = prefix.empty() ? String() : str(prefix.front() == '/' ? prefix.substr(1) + ".glb" : (dir.empty() ? "" : dir + "/") + prefix + ".glb");
    return out;
}

Dictionary SimBridge::property_info(const String& ids) const {
    Dictionary out;
    if (!data || !data->db) return out;
    const corps::tgi::TgiDatabase& db = *data->db;
    const auto* e = db.defs.find(db.symbols.find("property"), corps::asciiLowerCopy(utf8(ids)));
    if (e == nullptr) return out;
    const corps::tgi::Record& rec = db.records[e->record];
    const auto field = [&](std::string_view key) -> std::string {
        const corps::tgi::Symbol k = db.symbols.find(key);
        const corps::tgi::ResolvedField* f = k != corps::tgi::kNoSymbol ? rec.tree.field(k) : nullptr;
        return f != nullptr && f->hasValue() ? std::string(corps::trimAscii(f->text())) : std::string();
    };
    out["name"] = localize(str(field("name")));
    out["description"] = localize(str(field("description")));
    // action_button_icon is relative to the Property file's folder (Properties/TechIcons/...).
    std::string icon = field("action_button_icon");
    if (icon.empty()) icon = field("icon");
    if (!icon.empty() && icon.front() != '/') {
        const std::string dir = rec.file != nullptr ? dirOf(rec.file->path) : std::string();
        if (!dir.empty() && data->vfs.find(dir + "/" + icon) != nullptr) icon = dir + "/" + icon;
    }
    out["icon"] = str(icon);
    return out;
}

String SimBridge::localize(const String& key) const {
    std::string k = utf8(key);
    if (k.empty() || !data) return key;
    if (k.front() == '#') k.erase(0, 1);
    else if (k.find(' ') != std::string::npos) return key;  // already text
    std::string lower = corps::asciiLowerCopy(k);
    if (const std::size_t bar = lower.find('|'); bar != std::string::npos) lower.resize(bar);
    const auto it = data->strings.find(lower);
    if (it == data->strings.end()) return str(k);
    std::string text = it->second;
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
    for (std::size_t p = text.find("\\n"); p != std::string::npos; p = text.find("\\n", p + 1)) text.replace(p, 2, "\n");
    return str(text);
}

PackedByteArray SimBridge::read_file_for_nation(const String& path, const String& nation) const {
    PackedByteArray out;
    if (!data) return out;
    std::string key = corps::asciiLowerCopy(utf8(path));
    while (!key.empty() && key.front() == '/') key.erase(0, 1);
    for (char& ch : key) if (ch == '\\') ch = '/';
    const std::string skin = "skins/" + corps::asciiLowerCopy(utf8(nation)) + ".rwd";
    const corps::vfs::Vfs& vfs = data->vfs;
    for (std::size_t i = vfs.depotCount(); i-- > 0;) {
        const corps::vfs::Depot& d = vfs.depot(i);
        const std::string name = corps::asciiLowerCopy(d.name());
        if (corps::istartsWith(name, "skins/") && name != skin) continue;
        const std::vector<corps::vfs::DepotEntry>& es = d.entries();
        const auto it = std::lower_bound(es.begin(), es.end(), key, [](const corps::vfs::DepotEntry& e, const std::string& k) { return e.key < k; });
        if (it == es.end() || it->key != key) continue;
        std::string bytes;
        if (!d.read(static_cast<std::size_t>(it - es.begin()), bytes)) return out;
        out.resize(static_cast<int64_t>(bytes.size()));
        std::copy(bytes.begin(), bytes.end(), out.ptrw());
        return out;
    }
    return out;
}

// ------------------------------------------------------------------------------ skirmish (M6 close-out)

void SimBridge::queue_match_rules(const PackedInt32Array& teams, double cityRatio, bool teamVictory, double startMoney, int64_t player) {
    std::vector<uint8_t> t;
    for (int64_t i = 0; i < teams.size(); ++i) t.push_back(static_cast<uint8_t>(std::clamp<int32_t>(teams[i], 0, 255)));
    queue(corps::makeSetMatchRules(0, static_cast<uint8_t>(player), 0, std::move(t), fromF(cityRatio), teamVictory, fromF(startMoney)));
}

bool SimBridge::set_controller(int64_t player, const String& kind, const String& nation) {
    if (player < 0 || player >= static_cast<int64_t>(corps::kMaxPlayers)) return false;
    const auto p = static_cast<uint8_t>(player);
    controllers.erase(std::remove_if(controllers.begin(), controllers.end(), [&](const auto& c) { return c->player() == p; }),
                      controllers.end());
    const std::string k = utf8(kind);
    if (k.empty()) return true;
    if (k == "dummy") controllers.push_back(corps::ai::makeDummyAi(p, utf8(nation)));
    else if (k == "slice") controllers.push_back(corps::ai::makeSlicePlayer(p, utf8(nation)));
    else return false;
    // Ascending players: their commands go in a fixed order.
    std::sort(controllers.begin(), controllers.end(), [](const auto& a, const auto& b) { return a->player() < b->player(); });
    return true;
}

PackedStringArray SimBridge::drain_ai_notes() {
    PackedStringArray out = aiNotes;
    aiNotes = PackedStringArray();
    return out;
}

Dictionary SimBridge::save_replay(const String& path) const {
    Dictionary out;
    out["ok"] = false;
    out["path"] = path;
    if (!sim || !map || !data) {
        out["error"] = "no match";
        return out;
    }
    corps::Replay rep;
    rep.header.simVersion = corps::simVersion();
    rep.header.dataHash = data->defs.dataHash;
    rep.header.mapSpec = map->name;
    rep.header.mapHash = map->mapHash;
    rep.header.matchSeed = seed_;
    rep.header.tickCount = sim->tick();
    rep.header.players = {"player 0", "player 1"};
    rep.commands = recorded;  // pending ones (queued for a tick not run yet) are past tickCount: dropped
    rep.commands.erase(std::remove_if(rep.commands.begin(), rep.commands.end(), [&](const corps::Command& c) { return c.tick >= sim->tick(); }),
                       rep.commands.end());
    corps::RunOptions opt;
    opt.map = map.get();
    const corps::RunResult run = corps::runReplay(rep, &data->defs, opt);
    rep.hashes = run.hashes;
    const uint64_t live = corps::stateHash(*sim);
    out["ticks"] = static_cast<int64_t>(rep.header.tickCount);
    out["commands"] = static_cast<int64_t>(rep.commands.size());
    out["hash"] = String::num_uint64(live, 16);
    if (run.finalHash != live) {
        out["error"] = "the replay doesn't reproduce the live match";
        return out;
    }
    const auto bytes = corps::encodeReplay(rep);
    std::ofstream f(corps::vfs::pathFromUtf8(utf8(path)), std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) {
        out["error"] = "cannot write " + path;
        return out;
    }
    out["ok"] = true;
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
