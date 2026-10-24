#pragma once

// The only door between Godot and the sim (ARCHITECTURE §4.4). It owns the Sim and the data it
// was built from; the view reads snapshots, events and debug queries, and changes the game only by
// queueing Commands, which the next step() applies. Floats appear only here, on the view side of
// the bridge: everything handed to the sim is Fixed.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include "core/command.hpp"

namespace corps {
class Sim;
struct DefSet;
namespace nav {
struct MapDef;
}
}  // namespace corps

namespace godot {

class SimBridge : public RefCounted {
    GDCLASS(SimBridge, RefCounted)

protected:
    static void _bind_methods();

public:
    SimBridge();
    ~SimBridge() override;

    String hello() const;

    // Loads the game install (VFS, TGI, Defs) and a map ("gen:SEED:SIZE" or a tools/rmp.py
    // map.json path), and creates the Sim. false + last_error() on failure.
    bool start(const String& game_dir, const String& map_spec, int64_t seed);
    String last_error() const;

    // --- static data (view) -----------------------------------------------------------------
    Dictionary map_info() const;
    Dictionary terrain_table(const String& biome) const;
    Dictionary unit_visual(int64_t unit_def) const;
    Dictionary regiment_def(int64_t regiment_def) const;
    Vector2 find_open_area(const Vector2& near, int64_t half) const;

    // --- commands (applied by the next step) --------------------------------------------------
    void queue_spawn(const String& regiment_ids, const Vector2& pos, double facing, int64_t player);
    void queue_move(const PackedInt64Array& regiments, const Vector2& goal, bool run, bool has_facing, double facing,
                    int64_t player);
    void queue_stop(const PackedInt64Array& regiments, int64_t player);
    void queue_formation(const PackedInt64Array& regiments, const String& formation_ids, int64_t player);
    void queue_group_behaviour(const PackedInt64Array& regiments, bool division, int64_t player);
    void queue_blocker(const Vector2& pos, const Vector2& half_extent, int64_t player);
    void queue_attack(const PackedInt64Array& regiments, int64_t target, int64_t player);
    void queue_attack_move(const PackedInt64Array& regiments, const Vector2& goal, int64_t player);
    void queue_retreat(const PackedInt64Array& regiments, const Vector2& goal, int64_t player);

    // --- stepping and reading ----------------------------------------------------------------
    void step();
    int64_t tick() const;
    String state_hash() const;
    Dictionary snapshot() const;
    Array drain_events();
    Dictionary regiment_info(int64_t regiment) const;
    PackedByteArray cost_grid(int64_t move_class) const;

    // --- batched unit rendering (view only) ---------------------------------------------------
    // The terrain's vertex heights ((tiles + 1)^2, row-major, `tile` units apart), so instance
    // transforms sit on the same ground the view draws.
    void set_view_heights(const PackedFloat32Array& heights, int64_t verts_per_row, double tile);
    // Per unit def: {"count", "units": 16 floats per unit (MultiMesh 3D transform + custom data:
    // moving, animation phase 0..1, player, 0), "ring_count", "rings": the same for units whose
    // organization is in `selected_orgs`}, placed at render interpolation `alpha`.
    Dictionary unit_instances(double alpha, const PackedInt64Array& selected_orgs) const;
    // Regiment banners: 4 instances per regiment with living units (flag, health, morale,
    // efficiency bars), MultiMesh 3D transform (position only, `lift` above the ground) + custom
    // data (fill 0..1, kind 0..3, player, flags: 1 routing, 2 entrenched, 4 selected).
    PackedFloat32Array banner_instances(double alpha, double lift, const PackedInt64Array& selected_orgs) const;

    // --- view data for effects and panels (M5 part 3) ----------------------------------------
    // A resolved TGI record as nested Dictionaries: field (lower case) -> text, "_name" -> the node
    // name, "_children" -> Array of child Dictionaries. {} if there is none.
    Dictionary record(const String& type, const String& ids) const;
    // The AbilityDefinition node of a DefSet ability (inline ones from their owner's record).
    Dictionary ability_record(int64_t ability) const;
    // A unit def's Thing record.
    Dictionary unit_record(int64_t unit_def) const;
    // Raw bytes of a file in the game's VFS (sounds); empty if missing.
    PackedByteArray read_file(const String& path) const;

private:
    struct Snap {
        uint32_t bits = 0;
        float x = 0, y = 0, facing = 0;
    };
    void queue(corps::Command c);
    std::vector<corps::EntityId> regimentList(const PackedInt64Array& ids) const;
    std::vector<Snap> capture() const;
    float viewHeight(float x, float y) const;
    Dictionary nodeDict(const void* resolvedNode) const;

    struct Data;
    std::unique_ptr<Data> data;
    std::unique_ptr<corps::nav::MapDef> map;
    std::unique_ptr<corps::Sim> sim;
    std::vector<corps::Command> pending;
    std::vector<Snap> prev;
    std::vector<float> viewHeights;
    int64_t viewRow = 0;
    float viewTile = 2.0f;
    mutable std::map<uint32_t, float> regimentHealthMax;  // most health each regiment had (banner / panel)
    Array events;
    uint16_t seq[256] = {};
    std::string error;
    std::string biome;
};

}  // namespace godot
