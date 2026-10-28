#include "net/packets.hpp"

#include "session/bytes.hpp"

namespace corps::net {

namespace {

void encodeBody(const CmdPacket& p, ByteWriter& w) {
    w.u32(p.tick);
    w.u16(p.seq);
    w.u8(p.player);
    w.u8(static_cast<std::uint8_t>(p.commands.size()));
    for (const Command& c : p.commands) encodeCommand(c, w.buf);
}

void encodeBody(const TickBundle& b, ByteWriter& w) {
    w.u32(b.tick);
    putVarint(w.buf, b.commands.size());
    for (const Command& c : b.commands) {
        putVarint(w.buf, c.seq);
        encodeCommand(c, w.buf);
    }
    putVarint(w.buf, b.controls.size());
    for (const Control& c : b.controls) {
        w.u16(static_cast<std::uint16_t>(c.type));
        w.u8(c.player);
        putVarint(w.buf, c.value);
    }
}

void encodeBody(const HashReport& h, ByteWriter& w) {
    w.u32(h.tick);
    w.u8(h.player);
    w.u64(h.hash);
}
void encodeBody(const Ping& p, ByteWriter& w) {
    w.u32(p.id);
    w.u32(p.sentMs);
}
void encodeBody(const Pong& p, ByteWriter& w) {
    w.u32(p.id);
    w.u32(p.sentMs);
}
void encodeBody(const Chat& c, ByteWriter& w) {
    w.u8(c.player);
    const std::size_t n = c.text.size() < kMaxChatBytes ? c.text.size() : kMaxChatBytes;
    putVarint(w.buf, n);
    w.buf.insert(w.buf.end(), c.text.begin(), c.text.begin() + static_cast<std::ptrdiff_t>(n));
}

MsgKind kindOf(const Message& m) {
    switch (m.index()) {
    case 0: return MsgKind::CmdPacket;
    case 1: return MsgKind::TickBundle;
    case 2: return MsgKind::HashReport;
    case 3: return MsgKind::Ping;
    case 4: return MsgKind::Pong;
    default: return MsgKind::Chat;
    }
}

// Varint bounded by `max`.
bool getBounded(const std::vector<std::uint8_t>& in, std::size_t& pos, std::uint64_t max, std::uint64_t& v) {
    return getVarint(in, pos, v) && v <= max;
}

bool isControlType(std::uint16_t t) {
    return t >= static_cast<std::uint16_t>(ControlType::SetInputDelay) && t <= static_cast<std::uint16_t>(ControlType::DesyncDetected);
}

}  // namespace

std::vector<std::uint8_t> encodeMessage(const Message& m) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(kindOf(m)));
    std::visit([&w](const auto& body) { encodeBody(body, w); }, m);
    return std::move(w.buf);
}

bool decodeMessage(const std::vector<std::uint8_t>& bytes, Message& out) {
    if (bytes.empty()) return false;
    ByteReader r(bytes, 1);
    std::uint64_t v = 0;
    switch (static_cast<MsgKind>(bytes[0])) {
    case MsgKind::CmdPacket: {
        CmdPacket p;
        p.tick = r.u32();
        p.seq = r.u16();
        p.player = r.u8();
        const std::uint8_t count = r.u8();
        if (!r.ok || p.player >= kMaxNetPlayers || count > kMaxCommandsPerPlayerTick) return false;
        std::size_t pos = r.position();
        for (std::uint8_t i = 0; i < count; ++i) {
            Command c;
            if (!decodeCommand(bytes, pos, c)) return false;
            c.tick = p.tick;
            c.seq = i;
            p.commands.push_back(std::move(c));
        }
        if (pos != bytes.size()) return false;
        out = std::move(p);
        return true;
    }
    case MsgKind::TickBundle: {
        TickBundle b;
        b.tick = r.u32();
        if (!r.ok) return false;
        std::size_t pos = r.position();
        std::uint64_t n = 0;
        if (!getBounded(bytes, pos, kMaxBundleCommands, n)) return false;
        for (std::uint64_t i = 0; i < n; ++i) {
            Command c;
            if (!getBounded(bytes, pos, 0xFFFF, v) || !decodeCommand(bytes, pos, c)) return false;
            c.tick = b.tick;
            c.seq = static_cast<std::uint16_t>(v);
            b.commands.push_back(std::move(c));
        }
        if (!getBounded(bytes, pos, kMaxBundleControls, n)) return false;
        for (std::uint64_t i = 0; i < n; ++i) {
            ByteReader cr(bytes, pos);
            const std::uint16_t type = cr.u16();
            const std::uint8_t player = cr.u8();
            if (!cr.ok || !isControlType(type)) return false;
            pos = cr.position();
            if (!getBounded(bytes, pos, 0xFFFFFFFFu, v)) return false;
            b.controls.push_back({static_cast<ControlType>(type), player, static_cast<std::uint32_t>(v)});
        }
        if (pos != bytes.size()) return false;
        out = std::move(b);
        return true;
    }
    case MsgKind::HashReport: {
        HashReport h;
        h.tick = r.u32();
        h.player = r.u8();
        h.hash = r.u64();
        if (!r.ok || r.remaining() != 0) return false;
        out = h;
        return true;
    }
    case MsgKind::Ping:
    case MsgKind::Pong: {
        const std::uint32_t id = r.u32();
        const std::uint32_t sent = r.u32();
        if (!r.ok || r.remaining() != 0) return false;
        if (bytes[0] == static_cast<std::uint8_t>(MsgKind::Ping)) {
            out = Ping{id, sent};
        } else {
            out = Pong{id, sent};
        }
        return true;
    }
    case MsgKind::Chat: {
        Chat c;
        c.player = r.u8();
        if (!r.ok) return false;
        std::size_t pos = r.position();
        if (!getBounded(bytes, pos, kMaxChatBytes, v) || bytes.size() - pos != v) return false;
        c.text.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.end());
        out = std::move(c);
        return true;
    }
    }
    return false;
}

std::vector<std::uint8_t> encodeCommandList(const std::vector<Command>& commands) {
    std::vector<std::uint8_t> out;
    putVarint(out, commands.size());
    for (const Command& c : commands) {
        putVarint(out, c.seq);
        encodeCommand(c, out);
    }
    return out;
}

bool decodeCommandList(const std::vector<std::uint8_t>& bytes, std::uint32_t tick, std::vector<Command>& out, std::size_t max) {
    out.clear();
    std::size_t pos = 0;
    std::uint64_t n = 0;
    std::uint64_t v = 0;
    if (!getBounded(bytes, pos, max, n)) return false;
    for (std::uint64_t i = 0; i < n; ++i) {
        Command c;
        if (!getBounded(bytes, pos, 0xFFFF, v) || !decodeCommand(bytes, pos, c)) return false;
        c.tick = tick;
        c.seq = static_cast<std::uint16_t>(v);
        out.push_back(std::move(c));
    }
    return pos == bytes.size();
}

}  // namespace corps::net
