#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// The only door between Godot and the sim. The view never mutates sim state.
class SimBridge : public RefCounted {
    GDCLASS(SimBridge, RefCounted)

protected:
    static void _bind_methods();

public:
    String hello() const;
};

}  // namespace godot
