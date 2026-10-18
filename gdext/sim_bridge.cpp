#include "sim_bridge.hpp"

#include <godot_cpp/core/class_db.hpp>

#include "core/hello.hpp"

namespace godot {

void SimBridge::_bind_methods() {
    ClassDB::bind_method(D_METHOD("hello"), &SimBridge::hello);
}

String SimBridge::hello() const {
    const std::string_view msg = corps::simHello();
    return String::utf8(msg.data(), static_cast<int64_t>(msg.size()));
}

}  // namespace godot
