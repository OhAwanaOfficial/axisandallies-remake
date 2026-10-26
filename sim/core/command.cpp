#include "core/command.hpp"

#include <algorithm>

namespace corps {

namespace {

Command base(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, CommandType type) {
    Command c;
    c.tick = tick;
    c.player = player;
    c.seq = seq;
    c.type = type;
    return c;
}

std::uint64_t zigzag(std::int64_t v) {
    return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
}

std::int64_t unzigzag(std::uint64_t v) {
    return static_cast<std::int64_t>(v >> 1) ^ -static_cast<std::int64_t>(v & 1u);
}

void putFixed(std::vector<std::uint8_t>& out, Fixed f) {
    putVarint(out, zigzag(f.raw));
}

void putVec(std::vector<std::uint8_t>& out, FVec2 v) {
    putFixed(out, v.x);
    putFixed(out, v.y);
}

bool getFixed(const std::vector<std::uint8_t>& in, std::size_t& pos, Fixed& f) {
    std::uint64_t v = 0;
    if (!getVarint(in, pos, v)) return false;
    f = Fixed::fromRaw(unzigzag(v));
    return true;
}

bool getVec(const std::vector<std::uint8_t>& in, std::size_t& pos, FVec2& v) {
    return getFixed(in, pos, v.x) && getFixed(in, pos, v.y);
}

bool getU(const std::vector<std::uint8_t>& in, std::size_t& pos, std::uint64_t max, std::uint64_t& v) {
    return getVarint(in, pos, v) && v <= max;
}

void putIds(std::vector<std::uint8_t>& out, const std::vector<EntityId>& ids) {
    putVarint(out, ids.size());
    for (const EntityId id : ids) putVarint(out, id.bits);
}

bool getIds(const std::vector<std::uint8_t>& in, std::size_t& pos, std::vector<EntityId>& ids) {
    std::uint64_t n = 0, v = 0;
    if (!getU(in, pos, kMaxCommandRegiments, n) || n == 0) return false;
    ids.clear();
    for (std::uint64_t i = 0; i < n; ++i) {
        if (!getU(in, pos, 0xFFFFFFFFu, v)) return false;
        ids.push_back(EntityId{static_cast<std::uint32_t>(v)});
    }
    return true;
}

void putString(std::vector<std::uint8_t>& out, const std::string& s) {
    putVarint(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

bool getString(const std::vector<std::uint8_t>& in, std::size_t& pos, std::string& s) {
    std::uint64_t n = 0;
    if (!getU(in, pos, 256, n) || pos + n > in.size()) return false;
    s.assign(in.begin() + static_cast<std::ptrdiff_t>(pos), in.begin() + static_cast<std::ptrdiff_t>(pos + n));
    pos += n;
    return true;
}

}  // namespace

Command makeNoop(std::uint32_t tick, std::uint8_t player, std::uint16_t seq) {
    return base(tick, player, seq, CommandType::Noop);
}

Command makeSpawnRegiment(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::string ids, FVec2 pos, Angle facing) {
    Command c = base(tick, player, seq, CommandType::SpawnRegiment);
    c.payload = SpawnRegimentPayload{std::move(ids), pos, facing};
    return c;
}

Command makeDebugSpawn(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, FVec2 pos, Angle facing) {
    Command c = base(tick, player, seq, CommandType::DebugSpawn);
    c.payload = DebugSpawnPayload{pos, facing};
    return c;
}

Command makeDebugDestroy(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId target) {
    Command c = base(tick, player, seq, CommandType::DebugDestroy);
    c.payload = DebugDestroyPayload{target};
    return c;
}

Command makeDebugSetTransform(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId target, FVec2 pos, Angle facing) {
    Command c = base(tick, player, seq, CommandType::DebugSetTransform);
    c.payload = DebugSetTransformPayload{target, pos, facing};
    return c;
}

Command makeDebugRngDraw(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::uint16_t count) {
    Command c = base(tick, player, seq, CommandType::DebugRngDraw);
    c.payload = DebugRngDrawPayload{count};
    return c;
}

Command makeDebugPlaceBlocker(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, FVec2 pos, FVec2 halfExtent) {
    Command c = base(tick, player, seq, CommandType::DebugPlaceBlocker);
    c.payload = DebugPlaceBlockerPayload{pos, halfExtent};
    return c;
}

Command makeDebugRequestPath(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId target, FVec2 goal,
                             std::uint8_t moveClass) {
    Command c = base(tick, player, seq, CommandType::DebugRequestPath);
    c.payload = DebugRequestPathPayload{target, goal, moveClass};
    return c;
}

Command makeMoveGroup(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::vector<EntityId> regiments, FVec2 goal) {
    Command c = base(tick, player, seq, CommandType::MoveGroup);
    c.payload = MoveGroupPayload{std::move(regiments), goal, false, Angle{}};
    return c;
}

Command makeMoveGroupFacing(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::vector<EntityId> regiments, FVec2 goal,
                            Angle facing) {
    Command c = base(tick, player, seq, CommandType::MoveGroup);
    c.payload = MoveGroupPayload{std::move(regiments), goal, true, facing};
    return c;
}

Command makeRun(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::vector<EntityId> regiments, FVec2 goal) {
    Command c = base(tick, player, seq, CommandType::Run);
    c.payload = MoveGroupPayload{std::move(regiments), goal, false, Angle{}};
    return c;
}

Command makeStop(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::vector<EntityId> regiments) {
    Command c = base(tick, player, seq, CommandType::Stop);
    c.payload = RegimentListPayload{std::move(regiments)};
    return c;
}

Command makeSetFormation(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::vector<EntityId> regiments,
                         std::string formationIds) {
    Command c = base(tick, player, seq, CommandType::SetFormation);
    c.payload = SetFormationPayload{std::move(regiments), std::move(formationIds)};
    return c;
}

Command makeSetGroupBehaviour(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::vector<EntityId> regiments,
                              bool division) {
    Command c = base(tick, player, seq, CommandType::SetGroupBehaviour);
    c.payload = SetGroupBehaviourPayload{std::move(regiments), division};
    return c;
}

Command makeAttack(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::vector<EntityId> regiments, EntityId target) {
    Command c = base(tick, player, seq, CommandType::Attack);
    c.payload = AttackPayload{std::move(regiments), target};
    return c;
}

Command makeAttackMove(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::vector<EntityId> regiments, FVec2 goal) {
    Command c = base(tick, player, seq, CommandType::AttackMove);
    c.payload = MoveGroupPayload{std::move(regiments), goal, false, Angle{}};
    return c;
}

Command makeDebugSpawnBuilding(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::string ids, FVec2 pos, Angle facing) {
    Command c = base(tick, player, seq, CommandType::DebugSpawnBuilding);
    c.payload = DebugSpawnBuildingPayload{std::move(ids), pos, facing};
    return c;
}

Command makeDebugSetEntrenched(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::vector<EntityId> regiments,
                               bool entrenched) {
    Command c = base(tick, player, seq, CommandType::DebugSetEntrenched);
    c.payload = DebugSetEntrenchedPayload{std::move(regiments), entrenched};
    return c;
}

Command makeRetreat(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::vector<EntityId> regiments, FVec2 goal) {
    Command c = base(tick, player, seq, CommandType::Retreat);
    c.payload = MoveGroupPayload{std::move(regiments), goal, false, Angle{}};
    return c;
}

Command makeDebugSetExperience(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::vector<EntityId> regiments, Fixed xp) {
    Command c = base(tick, player, seq, CommandType::DebugSetExperience);
    c.payload = DebugSetExperiencePayload{std::move(regiments), xp};
    return c;
}

void putVarint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

bool getVarint(const std::vector<std::uint8_t>& in, std::size_t& pos, std::uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (pos >= in.size()) return false;
        const std::uint8_t b = in[pos++];
        v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return shift < 63 || b <= 1;  // reject bits beyond 64
    }
    return false;
}

namespace {

Command baseOrder(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, CommandType type, BaseOrderPayload p) {
    Command c = base(tick, player, seq, type);
    c.payload = std::move(p);
    return c;
}

}  // namespace

Command makeStartPlayer(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::string corpsIds, FVec2 pos, Angle facing) {
    BaseOrderPayload p;
    p.ids = std::move(corpsIds);
    p.position = pos;
    p.facing = facing;
    return baseOrder(tick, player, seq, CommandType::StartPlayer, std::move(p));
}

Command makeRecruit(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId building, std::string ids) {
    BaseOrderPayload p;
    p.target = building;
    p.ids = std::move(ids);
    return baseOrder(tick, player, seq, CommandType::Recruit, std::move(p));
}

Command makeCancelRecruit(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId building, std::uint8_t index) {
    BaseOrderPayload p;
    p.target = building;
    p.index = index;
    return baseOrder(tick, player, seq, CommandType::CancelRecruit, std::move(p));
}

Command makeUnpack(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId regiment, FVec2 pos, Angle facing) {
    BaseOrderPayload p;
    p.target = regiment;
    p.position = pos;
    p.facing = facing;
    return baseOrder(tick, player, seq, CommandType::Unpack, std::move(p));
}

Command makePack(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId building) {
    BaseOrderPayload p;
    p.target = building;
    return baseOrder(tick, player, seq, CommandType::Pack, std::move(p));
}

Command makeResearch(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId building, std::string propertyIds) {
    BaseOrderPayload p;
    p.target = building;
    p.ids = std::move(propertyIds);
    return baseOrder(tick, player, seq, CommandType::Research, std::move(p));
}

Command makeCancelResearch(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId building) {
    BaseOrderPayload p;
    p.target = building;
    return baseOrder(tick, player, seq, CommandType::CancelResearch, std::move(p));
}

Command makeDebugSetMoney(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, Fixed money) {
    BaseOrderPayload p;
    p.amount = money;
    return baseOrder(tick, player, seq, CommandType::DebugSetMoney, std::move(p));
}

Command makeSetMatchRules(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::vector<std::uint8_t> teams,
                          Fixed cityVictoryRatio, bool teamVictory, Fixed startMoney) {
    Command c = base(tick, player, seq, CommandType::SetMatchRules);
    c.payload = MatchRulesPayload{std::move(teams), cityVictoryRatio, teamVictory, startMoney};
    return c;
}

Command makeDebugSetHealth(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId target, Fixed health) {
    BaseOrderPayload p;
    p.target = target;
    p.amount = health;
    return baseOrder(tick, player, seq, CommandType::DebugSetHealth, std::move(p));
}

Command makeDebugSetOwner(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, EntityId structure, std::uint8_t owner) {
    BaseOrderPayload p;
    p.target = structure;
    p.index = owner;
    return baseOrder(tick, player, seq, CommandType::DebugSetOwner, std::move(p));
}

Command makeDebugGrantProperty(std::uint32_t tick, std::uint8_t player, std::uint16_t seq, std::string propertyIds) {
    BaseOrderPayload p;
    p.ids = std::move(propertyIds);
    return baseOrder(tick, player, seq, CommandType::DebugGrantProperty, std::move(p));
}

void encodeCommand(const Command& c, std::vector<std::uint8_t>& out) {
    out.push_back(static_cast<std::uint8_t>(c.type));
    out.push_back(c.player);
    switch (c.type) {
    case CommandType::Noop:
        break;
    case CommandType::SpawnRegiment: {
        const auto& p = std::get<SpawnRegimentPayload>(c.payload);
        putVarint(out, p.regimentIds.size());
        out.insert(out.end(), p.regimentIds.begin(), p.regimentIds.end());
        putVec(out, p.position);
        putVarint(out, p.facing.units);
        break;
    }
    case CommandType::DebugSpawn: {
        const auto& p = std::get<DebugSpawnPayload>(c.payload);
        putVec(out, p.position);
        putVarint(out, p.facing.units);
        break;
    }
    case CommandType::DebugDestroy:
        putVarint(out, std::get<DebugDestroyPayload>(c.payload).target.bits);
        break;
    case CommandType::DebugSetTransform: {
        const auto& p = std::get<DebugSetTransformPayload>(c.payload);
        putVarint(out, p.target.bits);
        putVec(out, p.position);
        putVarint(out, p.facing.units);
        break;
    }
    case CommandType::DebugRngDraw:
        putVarint(out, std::get<DebugRngDrawPayload>(c.payload).count);
        break;
    case CommandType::DebugPlaceBlocker: {
        const auto& p = std::get<DebugPlaceBlockerPayload>(c.payload);
        putVec(out, p.position);
        putVec(out, p.halfExtent);
        break;
    }
    case CommandType::DebugRequestPath: {
        const auto& p = std::get<DebugRequestPathPayload>(c.payload);
        putVarint(out, p.target.bits);
        putVec(out, p.goal);
        putVarint(out, p.moveClass);
        break;
    }
    case CommandType::MoveGroup:
    case CommandType::Run:
    case CommandType::AttackMove:
    case CommandType::Retreat: {
        const auto& p = std::get<MoveGroupPayload>(c.payload);
        putIds(out, p.regiments);
        putVec(out, p.goal);
        putVarint(out, p.hasFacing ? 1 : 0);
        if (p.hasFacing) putVarint(out, p.facing.units);
        break;
    }
    case CommandType::Stop:
        putIds(out, std::get<RegimentListPayload>(c.payload).regiments);
        break;
    case CommandType::SetFormation: {
        const auto& p = std::get<SetFormationPayload>(c.payload);
        putIds(out, p.regiments);
        putString(out, p.formationIds);
        break;
    }
    case CommandType::SetGroupBehaviour: {
        const auto& p = std::get<SetGroupBehaviourPayload>(c.payload);
        putIds(out, p.regiments);
        putVarint(out, p.division ? 1 : 0);
        break;
    }
    case CommandType::Attack: {
        const auto& p = std::get<AttackPayload>(c.payload);
        putIds(out, p.regiments);
        putVarint(out, p.target.bits);
        break;
    }
    case CommandType::DebugSpawnBuilding: {
        const auto& p = std::get<DebugSpawnBuildingPayload>(c.payload);
        putString(out, p.buildingIds);
        putVec(out, p.position);
        putVarint(out, p.facing.units);
        break;
    }
    case CommandType::DebugSetEntrenched: {
        const auto& p = std::get<DebugSetEntrenchedPayload>(c.payload);
        putIds(out, p.regiments);
        putVarint(out, p.entrenched ? 1 : 0);
        break;
    }
    case CommandType::DebugSetExperience: {
        const auto& p = std::get<DebugSetExperiencePayload>(c.payload);
        putIds(out, p.regiments);
        putFixed(out, p.xp);
        break;
    }
    case CommandType::StartPlayer: {
        const auto& p = std::get<BaseOrderPayload>(c.payload);
        putString(out, p.ids);
        putVec(out, p.position);
        putVarint(out, p.facing.units);
        break;
    }
    case CommandType::Recruit:
    case CommandType::Research: {
        const auto& p = std::get<BaseOrderPayload>(c.payload);
        putVarint(out, p.target.bits);
        putString(out, p.ids);
        break;
    }
    case CommandType::CancelRecruit: {
        const auto& p = std::get<BaseOrderPayload>(c.payload);
        putVarint(out, p.target.bits);
        putVarint(out, p.index);
        break;
    }
    case CommandType::Unpack: {
        const auto& p = std::get<BaseOrderPayload>(c.payload);
        putVarint(out, p.target.bits);
        putVec(out, p.position);
        putVarint(out, p.facing.units);
        break;
    }
    case CommandType::Pack:
    case CommandType::CancelResearch:
        putVarint(out, std::get<BaseOrderPayload>(c.payload).target.bits);
        break;
    case CommandType::DebugSetMoney:
        putFixed(out, std::get<BaseOrderPayload>(c.payload).amount);
        break;
    case CommandType::DebugGrantProperty:
        putString(out, std::get<BaseOrderPayload>(c.payload).ids);
        break;
    case CommandType::SetMatchRules: {
        const auto& p = std::get<MatchRulesPayload>(c.payload);
        putVarint(out, p.teams.size());
        for (const std::uint8_t t : p.teams) putVarint(out, t);
        putFixed(out, p.cityVictoryRatio);
        putVarint(out, p.teamVictory ? 1 : 0);
        putFixed(out, p.startMoney);
        break;
    }
    case CommandType::DebugSetHealth: {
        const auto& p = std::get<BaseOrderPayload>(c.payload);
        putVarint(out, p.target.bits);
        putFixed(out, p.amount);
        break;
    }
    case CommandType::DebugSetOwner: {
        const auto& p = std::get<BaseOrderPayload>(c.payload);
        putVarint(out, p.target.bits);
        putVarint(out, p.index);
        break;
    }
    }
}

bool decodeCommand(const std::vector<std::uint8_t>& in, std::size_t& pos, Command& out) {
    if (pos + 2 > in.size()) return false;
    out.type = static_cast<CommandType>(in[pos++]);
    out.player = in[pos++];
    std::uint64_t v = 0;
    switch (out.type) {
    case CommandType::Noop:
        out.payload = NoPayload{};
        return true;
    case CommandType::SpawnRegiment: {
        SpawnRegimentPayload p;
        if (!getU(in, pos, 256, v) || pos + v > in.size()) return false;
        p.regimentIds.assign(in.begin() + static_cast<std::ptrdiff_t>(pos), in.begin() + static_cast<std::ptrdiff_t>(pos + v));
        pos += v;
        if (!getVec(in, pos, p.position) || !getU(in, pos, 0xFFFF, v)) return false;
        p.facing = Angle::fromUnits(static_cast<std::uint32_t>(v));
        out.payload = std::move(p);
        return true;
    }
    case CommandType::DebugSpawn: {
        DebugSpawnPayload p;
        if (!getVec(in, pos, p.position) || !getU(in, pos, 0xFFFF, v)) return false;
        p.facing = Angle::fromUnits(static_cast<std::uint32_t>(v));
        out.payload = p;
        return true;
    }
    case CommandType::DebugDestroy:
        if (!getU(in, pos, 0xFFFFFFFFu, v)) return false;
        out.payload = DebugDestroyPayload{EntityId{static_cast<std::uint32_t>(v)}};
        return true;
    case CommandType::DebugSetTransform: {
        DebugSetTransformPayload p;
        if (!getU(in, pos, 0xFFFFFFFFu, v)) return false;
        p.target = EntityId{static_cast<std::uint32_t>(v)};
        if (!getVec(in, pos, p.position) || !getU(in, pos, 0xFFFF, v)) return false;
        p.facing = Angle::fromUnits(static_cast<std::uint32_t>(v));
        out.payload = p;
        return true;
    }
    case CommandType::DebugRngDraw:
        if (!getU(in, pos, 0xFFFF, v)) return false;
        out.payload = DebugRngDrawPayload{static_cast<std::uint16_t>(v)};
        return true;
    case CommandType::DebugPlaceBlocker: {
        DebugPlaceBlockerPayload p;
        if (!getVec(in, pos, p.position) || !getVec(in, pos, p.halfExtent)) return false;
        out.payload = p;
        return true;
    }
    case CommandType::DebugRequestPath: {
        DebugRequestPathPayload p;
        if (!getU(in, pos, 0xFFFFFFFFu, v)) return false;
        p.target = EntityId{static_cast<std::uint32_t>(v)};
        if (!getVec(in, pos, p.goal) || !getU(in, pos, 0xFF, v)) return false;
        p.moveClass = static_cast<std::uint8_t>(v);
        out.payload = p;
        return true;
    }
    case CommandType::MoveGroup:
    case CommandType::Run:
    case CommandType::AttackMove:
    case CommandType::Retreat: {
        MoveGroupPayload p;
        if (!getIds(in, pos, p.regiments) || !getVec(in, pos, p.goal) || !getU(in, pos, 1, v)) return false;
        p.hasFacing = v != 0;
        if (p.hasFacing) {
            if (!getU(in, pos, 0xFFFF, v)) return false;
            p.facing = Angle::fromUnits(static_cast<std::uint32_t>(v));
        }
        out.payload = std::move(p);
        return true;
    }
    case CommandType::Stop: {
        RegimentListPayload p;
        if (!getIds(in, pos, p.regiments)) return false;
        out.payload = std::move(p);
        return true;
    }
    case CommandType::SetFormation: {
        SetFormationPayload p;
        if (!getIds(in, pos, p.regiments) || !getString(in, pos, p.formationIds)) return false;
        out.payload = std::move(p);
        return true;
    }
    case CommandType::SetGroupBehaviour: {
        SetGroupBehaviourPayload p;
        if (!getIds(in, pos, p.regiments) || !getU(in, pos, 1, v)) return false;
        p.division = v != 0;
        out.payload = std::move(p);
        return true;
    }
    case CommandType::Attack: {
        AttackPayload p;
        if (!getIds(in, pos, p.regiments) || !getU(in, pos, 0xFFFFFFFFu, v)) return false;
        p.target = EntityId{static_cast<std::uint32_t>(v)};
        out.payload = std::move(p);
        return true;
    }
    case CommandType::DebugSpawnBuilding: {
        DebugSpawnBuildingPayload p;
        if (!getString(in, pos, p.buildingIds) || !getVec(in, pos, p.position) || !getU(in, pos, 0xFFFF, v)) return false;
        p.facing = Angle::fromUnits(static_cast<std::uint32_t>(v));
        out.payload = std::move(p);
        return true;
    }
    case CommandType::DebugSetEntrenched: {
        DebugSetEntrenchedPayload p;
        if (!getIds(in, pos, p.regiments) || !getU(in, pos, 1, v)) return false;
        p.entrenched = v != 0;
        out.payload = std::move(p);
        return true;
    }
    case CommandType::DebugSetExperience: {
        DebugSetExperiencePayload p;
        if (!getIds(in, pos, p.regiments) || !getFixed(in, pos, p.xp)) return false;
        out.payload = std::move(p);
        return true;
    }
    case CommandType::StartPlayer: {
        BaseOrderPayload p;
        if (!getString(in, pos, p.ids) || !getVec(in, pos, p.position) || !getU(in, pos, 0xFFFF, v)) return false;
        p.facing = Angle::fromUnits(static_cast<std::uint32_t>(v));
        out.payload = std::move(p);
        return true;
    }
    case CommandType::Recruit:
    case CommandType::Research: {
        BaseOrderPayload p;
        if (!getU(in, pos, 0xFFFFFFFFu, v)) return false;
        p.target = EntityId{static_cast<std::uint32_t>(v)};
        if (!getString(in, pos, p.ids)) return false;
        out.payload = std::move(p);
        return true;
    }
    case CommandType::CancelRecruit: {
        BaseOrderPayload p;
        if (!getU(in, pos, 0xFFFFFFFFu, v)) return false;
        p.target = EntityId{static_cast<std::uint32_t>(v)};
        if (!getU(in, pos, 0xFF, v)) return false;
        p.index = static_cast<std::uint8_t>(v);
        out.payload = std::move(p);
        return true;
    }
    case CommandType::Unpack: {
        BaseOrderPayload p;
        if (!getU(in, pos, 0xFFFFFFFFu, v)) return false;
        p.target = EntityId{static_cast<std::uint32_t>(v)};
        if (!getVec(in, pos, p.position) || !getU(in, pos, 0xFFFF, v)) return false;
        p.facing = Angle::fromUnits(static_cast<std::uint32_t>(v));
        out.payload = std::move(p);
        return true;
    }
    case CommandType::Pack:
    case CommandType::CancelResearch: {
        BaseOrderPayload p;
        if (!getU(in, pos, 0xFFFFFFFFu, v)) return false;
        p.target = EntityId{static_cast<std::uint32_t>(v)};
        out.payload = std::move(p);
        return true;
    }
    case CommandType::DebugSetMoney: {
        BaseOrderPayload p;
        if (!getFixed(in, pos, p.amount)) return false;
        out.payload = std::move(p);
        return true;
    }
    case CommandType::DebugGrantProperty: {
        BaseOrderPayload p;
        if (!getString(in, pos, p.ids)) return false;
        out.payload = std::move(p);
        return true;
    }
    case CommandType::SetMatchRules: {
        MatchRulesPayload p;
        std::uint64_t n = 0;
        if (!getU(in, pos, 8, n)) return false;
        for (std::uint64_t i = 0; i < n; ++i) {
            if (!getU(in, pos, 7, v)) return false;
            p.teams.push_back(static_cast<std::uint8_t>(v));
        }
        if (!getFixed(in, pos, p.cityVictoryRatio) || !getU(in, pos, 1, v)) return false;
        p.teamVictory = v != 0;
        if (!getFixed(in, pos, p.startMoney)) return false;
        out.payload = std::move(p);
        return true;
    }
    case CommandType::DebugSetHealth: {
        BaseOrderPayload p;
        if (!getU(in, pos, 0xFFFFFFFFu, v)) return false;
        p.target = EntityId{static_cast<std::uint32_t>(v)};
        if (!getFixed(in, pos, p.amount)) return false;
        out.payload = std::move(p);
        return true;
    }
    case CommandType::DebugSetOwner: {
        BaseOrderPayload p;
        if (!getU(in, pos, 0xFFFFFFFFu, v)) return false;
        p.target = EntityId{static_cast<std::uint32_t>(v)};
        if (!getU(in, pos, 0xFF, v)) return false;
        p.index = static_cast<std::uint8_t>(v);
        out.payload = std::move(p);
        return true;
    }
    }
    return false;  // unknown type
}

bool CommandQueue::schedule(const Command& c, std::uint32_t nextTick) {
    if (c.tick < nextTick) return false;
    // Stable insert after every command with the same (tick, player, seq): arrival order breaks ties.
    const auto it = std::upper_bound(queue.begin(), queue.end(), c, [](const Command& a, const Command& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.player != b.player) return a.player < b.player;
        return a.seq < b.seq;
    });
    queue.insert(it, c);
    return true;
}

std::vector<Command> CommandQueue::take(std::uint32_t tick) {
    const auto end = std::find_if(queue.begin(), queue.end(), [tick](const Command& c) { return c.tick != tick; });
    // The queue is sorted by tick and nothing older than `tick` can be left (schedule rejects it).
    std::vector<Command> out(queue.begin(), end);
    queue.erase(queue.begin(), end);
    return out;
}

}  // namespace corps
