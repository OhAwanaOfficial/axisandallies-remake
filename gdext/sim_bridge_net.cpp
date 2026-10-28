// SimBridge's multiplayer door (M8): lockstep hands commands out and steps bundles in.

#include <algorithm>
#include <cstring>

#include <godot_cpp/core/class_db.hpp>

#include "ai/controller.hpp"
#include "core/sim.hpp"
#include "data/defs.hpp"
#include "net/packets.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"
#include "sim_bridge.hpp"

namespace godot {

namespace {

PackedByteArray toPacked(const std::vector<uint8_t>& b) {
    PackedByteArray out;
    out.resize(static_cast<int64_t>(b.size()));
    if (!b.empty()) std::memcpy(out.ptrw(), b.data(), b.size());
    return out;
}

std::vector<uint8_t> fromPacked(const PackedByteArray& p) {
    std::vector<uint8_t> out(static_cast<std::size_t>(p.size()));
    if (!out.empty()) std::memcpy(out.data(), p.ptr(), out.size());
    return out;
}

}  // namespace

PackedByteArray SimBridge::take_commands() {
    if (!sim) return {};
    for (const auto& ctl : controllers) {  // host-side players think on the current state
        std::vector<corps::Command> out;
        ctl->think(*sim, out);
        for (corps::Command& cmd : out) queue(std::move(cmd));
        for (const std::string& n : ctl->notes) aiNotes.push_back(String::utf8(n.c_str()));
        ctl->notes.clear();
    }
    // They're not applied here: the bundle that carries them is recorded when it is stepped.
    recorded.resize(recorded.size() - std::min(recorded.size(), pending.size()));
    std::vector<corps::Command> cmds;
    cmds.swap(pending);
    return toPacked(corps::net::encodeCommandList(cmds));
}

void SimBridge::step_commands(const PackedByteArray& commands) {
    if (!sim) return;
    std::vector<corps::Command> cmds;
    if (!corps::net::decodeCommandList(fromPacked(commands), sim->tick(), cmds)) {
        error = "malformed command list";
        cmds.clear();
    }
    recorded.insert(recorded.end(), cmds.begin(), cmds.end());
    pending = std::move(cmds);
    std::vector<std::unique_ptr<corps::ai::Controller>> keep;
    keep.swap(controllers);  // they spoke through take_commands already
    step();
    controllers.swap(keep);
}

int64_t SimBridge::state_hash_bits() const {
    return sim ? static_cast<int64_t>(corps::stateHash(*sim)) : 0;
}

PackedByteArray SimBridge::save_state() const {
    return sim ? toPacked(corps::saveState(*sim)) : PackedByteArray();
}

String SimBridge::sim_version() const {
    return String(corps::simVersion());
}

void SimBridge::debug_desync() {
    if (sim) sim->rng().nextU32();
}

void SimBridge::bind_net_methods() {
    bind_film_methods();
    ClassDB::bind_method(D_METHOD("debug_desync"), &SimBridge::debug_desync);
    ClassDB::bind_method(D_METHOD("take_commands"), &SimBridge::take_commands);
    ClassDB::bind_method(D_METHOD("step_commands", "commands"), &SimBridge::step_commands);
    ClassDB::bind_method(D_METHOD("state_hash_bits"), &SimBridge::state_hash_bits);
    ClassDB::bind_method(D_METHOD("save_state"), &SimBridge::save_state);
    ClassDB::bind_method(D_METHOD("data_hash"), &SimBridge::data_hash);
    ClassDB::bind_method(D_METHOD("sim_version"), &SimBridge::sim_version);
}

}  // namespace godot
