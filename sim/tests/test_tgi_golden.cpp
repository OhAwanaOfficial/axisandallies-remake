// Golden tests against the player's own install (TGI_FORMAT §11.3). They read the game through
// config/game_path.cfg and are skipped when it is missing (CI has no game data). They assert
// hand-picked values and per-file hashes of the resolved output; no game text is committed.
//
// CORPS_PRINT_GOLDEN=1 prints the current per-file hashes (to refresh kGoldenHashes after a
// deliberate resolver change).

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

#include "combat/rules.hpp"
#include "core/ascii.hpp"
#include "core/sim.hpp"
#include "data/def_compiler.hpp"
#include "session/replay.hpp"
#include "session/state_io.hpp"
#include "systems/bases.hpp"
#include "systems/economy.hpp"
#include "systems/supply.hpp"
#include "data/tgi/resolved_dump.hpp"
#include "data/tgi/value_parse.hpp"
#include "tgi_fixture.hpp"
#include "vfs/path.hpp"

using namespace corps;
using namespace corps::tgi;
using namespace corps::fixed_literals;

namespace {

std::optional<std::filesystem::path> gameDir() {
    std::ifstream in(vfs::pathFromUtf8(CORPS_SOURCE_DIR "/config/game_path.cfg"));
    std::string line;
    while (in && std::getline(in, line)) {
        std::string_view t = trimAscii(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
        const std::filesystem::path p = vfs::pathFromUtf8(t);
        std::error_code ec;
        if (std::filesystem::is_regular_file(p / "Data.rwd", ec)) return p;
        return std::nullopt;
    }
    return std::nullopt;
}

struct Game {
    vfs::Vfs vfs;
    std::unique_ptr<TgiDatabase> db;
};

// Loaded once for all golden tests (the full load is the slow part, especially in Debug).
Game* game() {
    static std::unique_ptr<Game> loaded;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (const auto dir = gameDir()) {
            auto g = std::make_unique<Game>();
            std::vector<std::string> log;
            if (vfs::mountGameInstall(g->vfs, *dir, {}, log)) {
                g->db = loadTgiDatabase(g->vfs);
                loaded = std::move(g);
            }
        }
    }
    return loaded.get();
}

#define REQUIRE_GAME()                                                                      \
    Game* g = game();                                                                       \
    if (g == nullptr) {                                                                     \
        MESSAGE("skipped: no game install (config/game_path.cfg missing or has no Data.rwd)"); \
        return;                                                                             \
    }                                                                                       \
    const TgiDatabase& db = *g->db

Symbol sym(const TgiDatabase& db, std::string_view s) {
    return db.symbols.find(s);
}

const Record* rec(const TgiDatabase& db, std::string_view type, std::string_view ids) {
    const DefIndex::ByIdsEntry* e = db.defs.find(sym(db, type), ids);
    return e == nullptr ? nullptr : &db.records[e->record];
}

const ResolvedNode* node(const TgiDatabase& db, const ResolvedNode* n, std::string_view path) {
    std::size_t start = 0;
    while (n != nullptr && start < path.size()) {
        std::size_t slash = path.find('/', start);
        if (slash == std::string_view::npos) slash = path.size();
        std::string_view seg = path.substr(start, slash - start);
        std::size_t nth = 0;
        if (const std::size_t br = seg.find('['); br != std::string_view::npos) {
            nth = static_cast<std::size_t>(std::stoul(std::string(seg.substr(br + 1))));
            seg = seg.substr(0, br);
        }
        n = n->child(sym(db, seg), nth);
        start = slash + 1;
    }
    return n;
}

std::string val(const TgiDatabase& db, const ResolvedNode* n, std::string_view key) {
    if (n == nullptr) return "<no node>";
    const ResolvedField* f = n->field(sym(db, key));
    return (f != nullptr && f->hasValue()) ? std::string(f->text()) : std::string("<none>");
}

std::size_t diagsIn(const TgiDatabase& db, std::string_view file, DiagCode code) {
    std::size_t n = 0;
    for (const auto& d : db.diags.all()) n += (d.code == code && iequals(d.file, file)) ? 1 : 0;
    return n;
}

const LoadedFile* loaded(const TgiDatabase& db, std::string_view path) {
    for (const auto& f : db.files) {
        if (iequals(f.path, path)) return &f;
    }
    return nullptr;
}

// Canonical dump of everything a file contributes: its records, then the templates it defines.
std::uint64_t fileHash(const TgiDatabase& db, std::string_view path) {
    const LoadedFile* lf = loaded(db, path);
    if (lf == nullptr) return 0;
    std::string dump;
    for (const Record& r : db.records) {
        if (r.file == lf->ast.get()) dump += dumpCanonical(r.tree);
    }
    for (std::size_t t = 0; t < db.templates.size(); ++t) {
        const TemplateDef& d = db.templates.def(static_cast<TemplateId>(t));
        if (d.file == lf->ast.get()) dump += dumpCanonical(db.resolver->resolveTemplate(static_cast<TemplateId>(t)));
    }
    return fnv1a64(dump);
}

}  // namespace

TEST_CASE("golden: load order (1,860 files, PER/SAU mod from data/)") {
    REQUIRE_GAME();
    CHECK(db.files.size() == 1860);
    std::size_t fromData = 0;
    for (const auto& f : db.files) fromData += (f.depot == "data/") ? 1 : 0;
    CHECK(fromData == 219);
    CHECK(db.order.emptyWildcards.size() == 14);
    CHECK(db.order.lists.size() == 4);
    CHECK(db.diags.count(Severity::Error) == 0);
}

TEST_CASE("golden: diagnostics match TGI_FORMAT §10") {
    REQUIRE_GAME();
    CHECK(db.diags.count(DiagCode::HeaderMissingBracket) == 3);
    CHECK(db.diags.count(DiagCode::HeaderStrayEquals) == 2);
    CHECK(db.diags.count(DiagCode::EofUnclosedBlock) == 6);
    CHECK(db.diags.count(DiagCode::LateOpenBrace) == 1);
    CHECK(db.diags.count(DiagCode::ExtraCloseBrace) == 0);
    CHECK(db.diags.count(DiagCode::UnterminatedBlockComment) == 0);
    CHECK(db.diags.count(DiagCode::DoubledQuote) == 1);
    CHECK(db.diags.count(DiagCode::TemplateRedefined) == 2);
    CHECK(db.diags.count(DiagCode::UnknownTemplate) == 0);
    CHECK(db.diags.count(DiagCode::LstMissingFile) == 0);
    CHECK(db.diags.count(DiagCode::DuplicateIds) == 0);
    CHECK(db.diags.count(DiagCode::UndeclaredRepeat) == 1);
}

TEST_CASE("golden: per_tank_medium (M1 acceptance: health 400, AP 65, range 20)") {
    REQUIRE_GAME();
    const Record* r = rec(db, "Thing", "per_tank_medium");
    REQUIRE(r != nullptr);
    CHECK(std::string(describeChain(db, *r)).find("AAUnitPERVehicleMediumTank") != std::string::npos);
    const ResolvedNode* t = &r->tree;
    CHECK(val(db, t, "category") == "UNIT");
    CHECK(val(db, node(db, t, "BodyComponent"), "health") == "400");
    CHECK(val(db, node(db, t, "BodyComponent"), "defense") == "6");
    CHECK(val(db, node(db, t, "MoverComponent"), "speed_max") == "3.5");
    CHECK(val(db, node(db, t, "MoverComponent"), "turn_radius") == "0.9");
    const ResolvedNode* def = node(db, t, "PerformerComponent/Ability[0]/AbilityDefinition");
    REQUIRE(def != nullptr);
    CHECK(db.originName(def->base) == "AAMediumTankGun");
    CHECK(val(db, def, "range") == "20");
    CHECK(val(db, def, "recharge_time") == "6");
    CHECK(val(db, def, "targets") == "+enemy, -air, -mine, -water");
    CHECK(val(db, node(db, def, "Effect[0]"), "amount") == "65");
    CHECK(val(db, node(db, def, "Effect[0]"), "damage_type_ids") == "armor_piercing");
    CHECK(val(db, node(db, t, "PerformerComponent/Turret[0]"), "yaw_rotation_per_sec_degrees") == "37");
    const char* props[] = {"vehicle", "tank", "nation_per", "front", "support", "combat_unit"};
    const ResolvedNode* pc = node(db, t, "PropertiesComponent");
    REQUIRE(pc != nullptr);
    REQUIRE(pc->childCount(sym(db, "Property")) == 6);
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(val(db, pc->child(sym(db, "Property"), i), "property_IDS") == props[i]);
    }
    // the number parses to the exact Fixed value
    CHECK(parseFixed(val(db, node(db, t, "MoverComponent"), "speed_max")).value.raw == 15032385536LL);
}

TEST_CASE("golden: PER infantry and regiment from the user's mod") {
    REQUIRE_GAME();
    CHECK(val(db, node(db, &rec(db, "Thing", "per_infantry")->tree, "BodyComponent"), "health") == "200");
    CHECK(rec(db, "Thing", "per_infantry_amphib") != nullptr);
    const Record* reg = rec(db, "Thing", "regiment_per_tank_medium");
    REQUIRE(reg != nullptr);
    CHECK(val(db, &reg->tree, "category") == "ORGANIZATION");
    const ResolvedNode* org = node(db, &reg->tree, "OrganizationComponent");
    REQUIRE(org != nullptr);
    CHECK(db.originName(org->base) == "AAVehicleOrganizationComponent");
    CHECK(val(db, org, "layout_IDS") == "regiment_vehicle_light");
}

TEST_CASE("golden: ger_engineer repair ability is rebased (hammer FX once)") {
    REQUIRE_GAME();
    const ResolvedNode* def = node(db, &rec(db, "Thing", "ger_engineer")->tree, "PerformerComponent/Ability[1]/AbilityDefinition");
    REQUIRE(def != nullptr);
    CHECK(db.originName(def->base) == "AARepairAction");
    CHECK(def->childCount(sym(db, "WhenDo")) == 2);
}

TEST_CASE("golden: usa_carrier late '{' keeps [Size] fields and balances") {
    REQUIRE_GAME();
    CHECK(diagsIn(db, "Units/USA/usa_carrier.tgi", DiagCode::LateOpenBrace) == 1);
    const ResolvedNode* size = node(db, &rec(db, "Thing", "usa_carrier")->tree, "Size");
    CHECK(val(db, size, "scale_min") == "1.0");
    CHECK(val(db, size, "blocking_radius") == "11");
    CHECK(val(db, size, "target_rect_height") == "16");
}

TEST_CASE("golden: jpn_truck_armor is closed at EOF and still resolves") {
    REQUIRE_GAME();
    CHECK(diagsIn(db, "Units/JPN/jpn_truck_armor.tgi", DiagCode::EofUnclosedBlock) == 1);
    CHECK(val(db, node(db, &rec(db, "Thing", "jpn_truck_armor")->tree, "BodyComponent"), "health") == "500");
}

TEST_CASE("golden: translate.tgi keeps 182 ordered records under non-nesting comments") {
    REQUIRE_GAME();
    std::size_t n = 0;
    for (const Record& r : db.records) n += iequals(r.typeSpelling, "Translate") ? 1 : 0;
    CHECK(n == 182);
    CHECK(db.registry.policy(sym(db, "Translate")) == RecordPolicy::Ordered);
}

TEST_CASE("golden: template_object schema (types and defaults)") {
    REQUIRE_GAME();
    const TemplateId obj = db.templates.lookup(sym(db, "Object"));
    REQUIRE(obj >= 0);
    const ResolvedNode& o = db.resolver->resolveTemplate(obj);
    CHECK(val(db, &o, "category") == "OBJECT");
    const ResolvedNode* size = o.child(sym(db, "Size"));
    REQUIRE(size != nullptr);
    CHECK(size->field(sym(db, "blocking_shape"))->type == TypeWord::String);
    CHECK(val(db, size, "blocking_shape") == "CIRCLE");
    const ResolvedNode* view = o.child(sym(db, "View"));
    REQUIRE(view != nullptr);
    REQUIRE(view->decl(sym(db, "Aspect")) != nullptr);
    CHECK(view->decl(sym(db, "Aspect"))->list);
    CHECK(loaded(db, "Templates/template_object.tgi")->encoding == TextEncoding::Cp1252);
}

TEST_CASE("golden: SVars singleton merge, SVars_AA overrides 36 vars, 1.0f on fixed vars") {
    REQUIRE_GAME();
    REQUIRE(db.defs.singletons.size() >= 1);
    const DefIndex::Singleton* sv = nullptr;
    for (const auto& s : db.defs.singletons) {
        if (s.type == sym(db, "SVars")) sv = &s;
    }
    REQUIRE(sv != nullptr);
    CHECK(sv->records.size() == 2);
    CHECK(val(db, &sv->merged, "GameTicksPerGameSecond") == "16");
    CHECK(val(db, &sv->merged, "UnitSupplyEnabled") == "true");  // false in SVars.tgi
    CHECK(val(db, &sv->merged, "RetreatingTime") == "10");       // 25 in SVars.tgi
    CHECK(val(db, &sv->merged, "ExperienceFactionGainMultiplier") == "1.0f");
    CHECK(parseFixed("1.0f").value == Fixed::fromInt(1));
    CHECK(diagsIn(db, "Game/SVars_AA.tgi", DiagCode::TypeRedeclared) == 0);  // bool vs flag
}

TEST_CASE("golden: economic_aid effects stack (list append)") {
    REQUIRE_GAME();
    const Record* r = rec(db, "AbilityDefinition", "gbr_economic_aid");
    REQUIRE(r != nullptr);
    REQUIRE(r->tree.childCount(sym(db, "Effect")) == 2);
    CHECK(val(db, r->tree.child(sym(db, "Effect"), 0), "hide_from_tooltip") == "true");
    CHECK(val(db, r->tree.child(sym(db, "Effect"), 0), "object_ids") == "smoke_grenade");
    CHECK(val(db, r->tree.child(sym(db, "Effect"), 1), "object_ids") == "gbr_economic_aid");
}

TEST_CASE("golden: header recoveries (stray '=', missing ']')") {
    REQUIRE_GAME();
    CHECK(diagsIn(db, "Game/denizen_types.tgi", DiagCode::HeaderStrayEquals) == 2);
    const Record* bomber = rec(db, "DenizenType", "denizen_type_bomber");
    REQUIRE(bomber != nullptr);
    CHECK(db.originName(bomber->tree.base) == "DenizenType");
    CHECK(diagsIn(db, "Templates/template_ability_special_op_aa.tgi", DiagCode::HeaderMissingBracket) == 2);
}

TEST_CASE("golden: localisation (BOM, spaced keys, ';' inside quotes)") {
    REQUIRE_GAME();
    const LoadedFile* lf = loaded(db, "Localization/strings_data_AA.tgi");
    REQUIRE(lf != nullptr);
    CHECK(lf->encoding == TextEncoding::Utf8Bom);
    const Record* text = nullptr;
    for (const Record& r : db.records) {
        if (r.file == lf->ast.get()) text = &r;
    }
    REQUIRE(text != nullptr);
    CHECK(text->tree.fields.size() == 8010);
    CHECK(text->tree.field(sym(db, "mg_territories_Aleutian Islands_name")) != nullptr);
    const std::string sub = val(db, &text->tree, "sounds_speech_Subtitle_text_7");
    CHECK(sub.find("; next thing you know") != std::string::npos);
}

TEST_CASE("golden: NameList uses the same-named template implicitly") {
    REQUIRE_GAME();
    const Record* r = rec(db, "NameList", "namelist_alaska");
    REQUIRE(r != nullptr);
    CHECK(r->node->hasParentRef == false);
    CHECK(db.originName(r->tree.base) == "NameList");
}

TEST_CASE("golden: sequential redefinition gives each paratrooper regiment its own loadout") {
    REQUIRE_GAME();
    const ResolvedNode* a = &rec(db, "Thing", "regiment_rus_paratrooper")->tree;
    const ResolvedNode* b = &rec(db, "Thing", "regiment_rus_paratrooper_heavy")->tree;
    CHECK(val(db, node(db, a, "OrganizationComponent/Slot[3]"), "element_ids") == "rus_paratrooper");
    CHECK(val(db, node(db, b, "OrganizationComponent/Slot[3]"), "element_ids") == "rus_paratrooper_mortar");
    CHECK(val(db, node(db, a, "EconomyComponent/Cost"), "money") == "150");
    CHECK(val(db, node(db, b, "EconomyComponent/Cost"), "money") == "200");
}

TEST_CASE("golden: scorch_marks bare key and decimal comma") {
    REQUIRE_GAME();
    const Record* r = rec(db, "VFX", "scorch_mark_small_vfx");
    REQUIRE(r != nullptr);
    const ResolvedField* tex = r->tree.field(sym(db, "texture"));
    REQUIRE(tex != nullptr);
    CHECK(tex->required);  // declared bare in the template, filled by the instance
    CHECK(tex->hasValue());
    CHECK(val(db, &r->tree, "decal_fade_time") == "0,5");  // the resolver keeps raw text
    CHECK(parseFixed("0,5").decimalComma);
}

TEST_CASE("golden: template_sai ('//' comment, '{}' bodies) and game_events '|*' keys") {
    REQUIRE_GAME();
    CHECK(loaded(db, "Templates/template_sai.tgi") != nullptr);
    CHECK(db.diags.count(DiagCode::UnparsedLine) == 0);
    const Record* ev = rec(db, "GameEvent", "building_construction_complete");
    REQUIRE(ev != nullptr);
    CHECK(val(db, &ev->tree, "game_message|*") == "#game_events_building_construction_complete_game_message|*");
}

TEST_CASE("golden: resource_list's CostList is used from other files") {
    REQUIRE_GAME();
    const ResolvedNode* cost = node(db, &rec(db, "Thing", "per_tank_medium")->tree, "PerformerComponent/Ability[0]/Cost");
    REQUIRE(cost != nullptr);
    CHECK(db.originName(cost->base) == "CostList");
    CHECK(iequals(db.templates.def(cost->base).file->path, "Game/resource_list.tgi"));
}

TEST_CASE("golden: per-file resolved hashes (archive files) and determinism") {
    REQUIRE_GAME();
    struct Golden {
        const char* path;
        std::uint64_t hash;
    };
    static const Golden kGoldenHashes[] = {
#include "golden_hashes.inc"
    };
#ifdef _MSC_VER
    char* env = nullptr;
    std::size_t envLen = 0;
    const bool print = _dupenv_s(&env, &envLen, "CORPS_PRINT_GOLDEN") == 0 && env != nullptr;
    std::free(env);
#else
    const bool print = std::getenv("CORPS_PRINT_GOLDEN") != nullptr;
#endif
    for (const Golden& gh : kGoldenHashes) {
        const LoadedFile* lf = loaded(db, gh.path);
        REQUIRE_MESSAGE(lf != nullptr, std::string(gh.path));
        const std::uint64_t h = fileHash(db, gh.path);
        if (print) std::printf("    {\"%s\", 0x%sULL},\n", gh.path, hex64(h).c_str());
        if (lf->depot != "Data.rwd") continue;  // the user's own data/ may change; values are checked above
        CHECK_MESSAGE(h == gh.hash, (std::string(gh.path) + " resolved hash is now 0x" + hex64(h)));
    }

    // A second, independent load gives a byte-identical definition set.
    vfs::Vfs again;
    std::vector<std::string> log;
    REQUIRE(vfs::mountGameInstall(again, *gameDir(), {}, log));
    const auto db2 = loadTgiDatabase(again);
    CHECK(fnv1a64(dumpDefinitionSet(*db2)) == fnv1a64(dumpDefinitionSet(db)));
}

// ------------------------------------------------------------------------------------- M3: Def compiler

namespace {
const DefSet* compiledDefs() {
    static std::unique_ptr<DefSet> defs;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (Game* g = game()) {
            Diagnostics diags;
            defs = std::make_unique<DefSet>(compileDefs(*g->db, diags));
        }
    }
    return defs.get();
}
}  // namespace

#define REQUIRE_DEFS()                                                                    \
    const DefSet* defsPtr = compiledDefs();                                               \
    if (defsPtr == nullptr) {                                                             \
        MESSAGE("skipped: no game install (config/game_path.cfg missing or has no Data.rwd)"); \
        return;                                                                           \
    }                                                                                     \
    const DefSet& defs = *defsPtr

TEST_CASE("golden defs: per_tank_medium UnitDef (health 400, AP 65, range 20)") {
    REQUIRE_DEFS();
    const UnitDef* u = defs.unit("per_tank_medium");
    REQUIRE(u != nullptr);
    CHECK(u->category == "UNIT");
    CHECK(u->health == Fixed::fromInt(400));
    CHECK(u->defense == Fixed::fromInt(6));
    CHECK(formatFixed(u->speedMax) == "3.5");
    REQUIRE(u->abilities.size() == 1);
    const AbilityDef& ab = defs.abilities[u->abilities[0]];
    CHECK(ab.range == Fixed::fromInt(20));
    CHECK(ab.rechargeTime == Fixed::fromInt(6));
    REQUIRE(ab.effects.size() == 1);
    CHECK(ab.effects[0].amount == Fixed::fromInt(65));
    CHECK(ab.effects[0].damageType == "armor_piercing");
    CHECK(std::find(ab.targets.begin(), ab.targets.end(), "-air") != ab.targets.end());
    const std::vector<std::string> props = {"vehicle", "tank", "nation_per", "front", "support", "combat_unit"};
    CHECK(u->properties == props);
}

TEST_CASE("golden defs: regiment_per_infantry slots resolve to PER units") {
    REQUIRE_DEFS();
    const RegimentDef* g = defs.regiment("regiment_per_infantry");
    REQUIRE(g != nullptr);
    REQUIRE(g->slots.size() == 6);
    const char* expect[] = {"per_infantry", "per_infantry", "per_infantry", "per_infantry", "per_infantry_mg", "per_infantry_co"};
    for (std::size_t i = 0; i < 6; ++i) {
        REQUIRE(g->slots[i].kind == ElementKind::Unit);
        CHECK(defs.units[g->slots[i].index].ids == expect[i]);
    }
    CHECK(g->cost.money == Fixed::fromInt(50));
    CHECK(g->upkeep.ammo == Fixed::fromInt(10));
    CHECK(g->moraleBreakpoint == Fixed::fromInt(20));
    CHECK(g->layoutIds == "regiment_infantry");
    CHECK(defs.stats.missingSlotUnits == 0);
    // HQ regiments hold a TRUCK (packed) or the HQ building (unpacked).
    const RegimentDef* hq = defs.regiment("regiment_gbr_hq_airborne_unpacked");
    REQUIRE(hq != nullptr);
    CHECK(std::any_of(hq->slots.begin(), hq->slots.end(), [](const ElementRef& e) { return e.kind == ElementKind::Building; }));
}

TEST_CASE("golden defs: buildings, nations, SVars, and the PER/SAU mod") {
    REQUIRE_DEFS();
    const BuildingDef* corps = defs.building("per_corps");
    REQUIRE(corps != nullptr);
    CHECK(corps->health == Fixed::fromInt(3500));
    CHECK(std::find(corps->recruits.begin(), corps->recruits.end(), "regiment_per_hq_infantry") != corps->recruits.end());
    const NationDef* per = defs.nation("nation_per");
    REQUIRE(per != nullptr);
    CHECK(per->factions == std::vector<std::string>{"montgomery", "wingate", "mountbatten", "wavell"});
    CHECK(defs.nation("nation_sau") != nullptr);
    CHECK(defs.unit("sau_tank_medium") != nullptr);
    CHECK(defs.svars.getInt("GameTicksPerGameSecond", 0) == 16);           // SVars.tgi
    CHECK(defs.svars.getFixed("MoraleBreakerBase", Fixed{}) == Fixed::fromInt(3));  // SVars_AA.tgi overrides
    CHECK(defs.svars.getBool("UnitSupplyEnabled", false));
    CHECK(defs.units.size() > 400);
    CHECK(defs.regiments.size() > 200);
    CHECK(defs.stats.badValues == 0);
}

TEST_CASE("golden defs: layouts, formations and group formations from Organizations/ and group_formations.tgi") {
    REQUIRE_DEFS();
    CHECK(defs.layouts.size() >= 8);
    const std::uint32_t li = defs.layoutIndex("regiment_infantry");
    REQUIRE(li != kNoDef);
    const LayoutDef& inf = defs.layouts[li];
    CHECK(inf.elementSlot.size() == 9);
    CHECK(inf.defaultFormationIds == "regiment_defensive");
    REQUIRE(inf.formations.size() == 3);
    for (const LayoutFormationDef& f : inf.formations) {
        CHECK(f.formationIndex != kNoDef);
        CHECK(f.offsets.size() == 9);
    }
    // Captain at the anchor; data (x, y) = (right, forward) → sim (forward, left).
    const LayoutFormationDef& agg = inf.formations[inf.formation("regiment_aggressive")];
    CHECK(agg.offsets[8] == FVec2{});
    CHECK(agg.offsets[0] == FVec2{5.25_fx, 1_fx});  // data offset -1,+5.25
    CHECK(defs.formations[defs.formationIndex("regiment_aggressive")].speedMultiplier == 0.75_fx);
    CHECK(defs.formations[defs.formationIndex("regiment_defensive")].speedMultiplier == 1_fx);
    CHECK(defs.formations[defs.formationIndex("regiment_column")].speedMultiplier == 1.25_fx);
    for (const char* g : {"group_aggressive", "group_defensive", "group_column"}) {
        const std::uint32_t gi = defs.groupFormationIndex(g);
        REQUIRE(gi != kNoDef);
        CHECK(defs.groupFormations[gi].offsets.size() == 6);
    }
    CHECK(defs.regiment("regiment_per_infantry")->layoutIndex == li);
    CHECK(defs.unit("per_infantry")->moveClass == UnitMoveClass::Foot);
    CHECK(defs.unit("ger_tank_medium")->moveClass == UnitMoveClass::Tracked);
    CHECK(defs.unit("ger_tank_medium")->turnType == TurnType::InPlace);
    CHECK(defs.unit("usa_halftrack")->moveClass == UnitMoveClass::Wheeled);
    CHECK(defs.unit("usa_battleship")->moveClass == UnitMoveClass::Naval);
    CHECK(defs.unit("gbr_fighter")->moveClass == UnitMoveClass::Air);
}

TEST_CASE("golden defs: compilation is deterministic (same data hash twice)") {
    REQUIRE_DEFS();
    Diagnostics diags;
    const DefSet again = compileDefs(*game()->db, diags);
    CHECK(again.dataHash == defs.dataHash);
    CHECK(serializeDefs(again) == serializeDefs(defs));
}

TEST_CASE("golden spawn-test: regiment_per_infantry, 100 ticks, stable hash, replay agrees") {
    REQUIRE_DEFS();
    Sim sim({1, &defs});
    sim.step({makeSpawnRegiment(0, 0, 0, "regiment_per_infantry", {Fixed::fromInt(100), Fixed::fromInt(50)}, Angle{})});
    // The layout expands slot `number`s: 3+1+1+1 riflemen, 2 MG, 1 captain = 9 men + the regiment.
    CHECK(sim.world().registry.aliveCount() == 10);
    CHECK(sim.world().health.size() == 9);
    CHECK(sim.world().regiments.size() == 1);
    for (std::uint32_t t = 1; t < 100; ++t) sim.step();
    const std::uint64_t h = stateHash(sim);
    Replay rep;
    rep.header.matchSeed = 1;
    rep.header.tickCount = 100;
    rep.header.dataHash = defs.dataHash;
    rep.commands.push_back(makeSpawnRegiment(0, 0, 0, "regiment_per_infantry", {Fixed::fromInt(100), Fixed::fromInt(50)}, Angle{}));
    CHECK(runReplay(rep, &defs).finalHash == h);
}

// ------------------------------------------------------------------------------------- M5: combat data

TEST_CASE("golden defs: combat data (turrets, weapons, projectiles, resistances, terrain, SVars)") {
    REQUIRE_DEFS();
    using namespace fixed_literals;
    const UnitDef* tank = defs.unit("ger_tank_medium");
    REQUIRE(tank != nullptr);
    REQUIRE(tank->turrets.size() == 1);
    CHECK(tank->turrets[0].yawRate == 37_fx);
    CHECK(tank->turrets[0].resetTime == 10_fx);
    REQUIRE(tank->weapons.size() == 1);
    CHECK(tank->weapons[0].turret == 0);
    CHECK(tank->weapons[0].priority == 20_fx);
    CHECK(tank->weapons[0].fuzzyRange == 3_fx);
    CHECK(tank->checkDamageResistance);
    const AbilityDef& gun = defs.abilities[tank->weapons[0].ability];
    CHECK(gun.projectileIds == "medium_tank_projectile");
    REQUIRE(gun.projectileIndex != kNoDef);
    CHECK(defs.projectiles[gun.projectileIndex].speed == 50_fx);
    CHECK(defs.unit("usa_halftrack")->weapons[0].turret == WeaponDef::kNoTurret);  // a local gun
    const BuildingDef* bunker = defs.building("ger_bunker");
    REQUIRE(bunker != nullptr);
    CHECK(bunker->controlRange == 20_fx);
    REQUIRE(bunker->weapons.size() == 1);  // DefenderComponent
    const AbilityDef& mg = defs.abilities[bunker->weapons[0].ability];
    CHECK(mg.maxTargets == 4);
    CHECK(mg.attenuationRangeMin == 0.5_fx);
    CHECK(mg.attenuationFactorMax == 0.5_fx);
    for (const char* dt : {"gunfire", "armor_piercing", "explosive", "flame"}) CHECK(defs.damageTypeIndex(dt) != kNoDef);
    CHECK(defs.targetTypeIndex("air") != kNoDef);
    CHECK(defs.targetTypeIndex("infantry") != kNoDef);
    const PropertyDef& vehicle = defs.properties[defs.propertyIndex("vehicle")];
    REQUIRE(vehicle.resist.size() == 2);
    CHECK(vehicle.resist[0].first == "gunfire");
    CHECK(vehicle.resist[0].second == 0.4_fx);
    const PropertyDef& dug = defs.properties[defs.propertyIndex("infantry_entrench")];
    CHECK(dug.defenseAdd == 4_fx);
    CHECK(dug.requiredProperties == std::vector<std::string>{"infantry"});
    const std::uint32_t forest = defs.terrainOfName("arctic_forest01");
    REQUIRE(forest != kNoDef);
    CHECK(defs.terrains[forest].properties == std::vector<std::string>{"terrain_forest_infantry", "terrain_forest_vehicles"});
    // Resistances add up across properties: a tank resists gunfire 0.4 (vehicle) + 0.2 (tank).
    const CombatRules rules(&defs, nullptr, CombatTuning{});
    const CombatProfile& tp = *rules.unit(defs.unitIndex("ger_tank_medium"));
    CHECK(tp.resist[defs.damageTypeIndex("gunfire")] == 0.4_fx + 0.2_fx);
    CHECK(tp.defense == 7_fx);
    CHECK(defs.svars.getFixed("PrimarySampleTime", {}) == 5_fx);
    CHECK(defs.svars.getFixed("CAITargetInRangeZOCMultiplier", {}) == 0.6_fx);
    CHECK(defs.svars.getBool("KillTargetIsHighestPriority", false));
}

TEST_CASE("golden duel: GER vs GBR rifle regiments on open ground fight to a finish, deterministically") {
    REQUIRE_DEFS();
    using namespace fixed_literals;
    const nav::MapDef map = nav::generateArenaMap("open", 96);
    std::uint64_t hashes[2] = {0, 0};
    for (int run = 0; run < 2; ++run) {
        Sim sim({1, &defs, &map});
        sim.step({makeSpawnRegiment(0, 0, 0, "regiment_ger_infantry", {33_fx, 48_fx}, Angle{}),
                  makeSpawnRegiment(0, 1, 0, "regiment_gbr_infantry", {63_fx, 48_fx}, Angle::fromUnits(Angle::kHalf))});
        const std::vector<EntityId> regs = sim.world().regiments.ids();
        REQUIRE(regs.size() == 2);
        sim.step({makeAttack(1, 0, 1, {regs[0]}, regs[1]), makeAttack(1, 1, 1, {regs[1]}, regs[0])});
        std::size_t deaths = 0;
        while (sim.tick() < 6000 && sim.world().registry.alive(regs[0]) && sim.world().registry.alive(regs[1])) {
            sim.step();
            for (const SimEvent& e : sim.events()) deaths += e.type == SimEvent::Type::Died;
        }
        CHECK(sim.world().registry.alive(regs[0]) != sim.world().registry.alive(regs[1]));  // one side won
        CHECK(deaths >= 9);
        hashes[run] = stateHash(sim);
    }
    CHECK(hashes[0] == hashes[1]);
}

TEST_CASE("golden defs: morale, experience levels, entrenchment, retreat formation") {
    REQUIRE_DEFS();
    using namespace fixed_literals;
    CHECK(defs.unit("ger_infantry")->moraleValue == 5_fx);
    CHECK(defs.unit("ger_tank_medium")->moraleValue == 20_fx);
    const RegimentDef* inf = defs.regiment("regiment_ger_infantry");
    REQUIRE(inf != nullptr);
    CHECK(inf->moraleBreakpoint == 20_fx);
    CHECK(inf->entrenchmentTime == 30_fx);
    REQUIRE(inf->experienceLevels.size() == 4);  // Recruit, Regular, Veteran, Elite
    CHECK(inf->experienceLevels[1].required == 150_fx);
    CHECK(inf->experienceLevels[3].required == 600_fx);
    CHECK(inf->experienceLevels[3].attack == 16_fx);
    CHECK(inf->experienceLevels[3].defense == 3_fx);
    CHECK(inf->experienceLevels[2].morale == 0.5_fx);
    const LayoutDef& lay = defs.layouts[inf->layoutIndex];
    REQUIRE(lay.retreatFormation != kNoDef);
    CHECK(lay.formations[lay.retreatFormation].formationIds == "regiment_column");  // the last organized one
    const PropertyDef& dug = defs.properties[defs.propertyIndex("infantry_entrench")];
    REQUIRE(dug.modifiers.size() == 2);
    CHECK(dug.modifiers[1].attribute == "MORALE_LOSS_RATE");
    CHECK(dug.modifiers[1].amount == 0.5_fx);
    const AbilityDef* mortar = nullptr;
    for (const std::uint32_t a : defs.unit("gbr_infantry_mortar")->abilities) mortar = &defs.abilities[a];
    REQUIRE(mortar != nullptr);
    const CombatRules rules(&defs, nullptr, CombatTuning{});
    CHECK(rules.ability(static_cast<std::uint32_t>(mortar - defs.abilities.data()))->moraleLoss == 0.75_fx);  // a morale breaker
    CHECK(defs.svars.getFixed("AttackEfficiencyPointsPerSecondFalling", {}) == 0.02_fx);
    CHECK(defs.svars.getFixed("MoraleDeathFactor", {}) == 0.25_fx);
}

TEST_CASE("golden terrain defense on a real map: forest cells of Golden Fields give infantry DV +4") {
    REQUIRE_DEFS();
    using namespace fixed_literals;
    const std::string path = CORPS_SOURCE_DIR "/cache/maps/golden_fields/map.json";
    std::ifstream in(vfs::pathFromUtf8(path), std::ios::binary);
    if (!in) {
        MESSAGE("skipped: no " << path << " (tools/rmp.py output)");
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    nav::MapDef map;
    std::string err;
    REQUIRE_MESSAGE(nav::loadRmpJson(text, map, err), err);
    const CombatRules rules(&defs, &map, CombatTuning{});
    const CombatProfile& rifle = *rules.unit(defs.unitIndex("ger_infantry"));
    std::uint32_t forest = 0xFFFFFFFFu, open = 0xFFFFFFFFu;
    for (std::uint32_t i = 0; i < map.terrainNames.size(); ++i) {
        const std::string& n = map.terrainNames[i];
        if (forest == 0xFFFFFFFFu && n.find("forest") != std::string::npos && defs.terrainOfName(n) != kNoDef) forest = i;
        if (open == 0xFFFFFFFFu && n.find("dirt0") != std::string::npos && defs.terrainOfName(n) != kNoDef) open = i;
    }
    REQUIRE(forest != 0xFFFFFFFFu);
    REQUIRE(open != 0xFFFFFFFFu);
    CHECK(rules.statsOf(rifle, forest, false).defense == rules.statsOf(rifle, open, false).defense + 4_fx);
    const std::uint32_t ap = defs.damageTypeIndex("armor_piercing");
    CHECK(rules.statsOf(rifle, forest, false).resist[ap] > rules.statsOf(rifle, open, false).resist[ap]);
}

TEST_CASE("golden defs: bases and economy (GBR Corps HQ, division HQ truck and regiments, convoys, research, start location)") {
    REQUIRE_DEFS();
    using namespace fixed_literals;
    const BuildingDef& corps = *defs.building("gbr_corps");
    CHECK(corps.shape == FootprintShape::Square);
    CHECK(corps.squareSize == 10_fx);
    CHECK(corps.padding == 1_fx);
    CHECK(corps.tileLocked);
    CHECK(corps.axisLocked);
    CHECK(corps.providesCorps);
    CHECK(corps.mobile);
    CHECK(corps.packHealthMin == 0.5_fx);
    CHECK(corps.packingLength == 10_fx);  // not GAME_DESIGN's 7 s: each building has its own
    CHECK(corps.cost.money == 450_fx);
    CHECK(corps.production.money == 25_fx);
    CHECK(corps.production.oil == 10_fx);
    CHECK(corps.production.ammo == 20_fx);
    REQUIRE(corps.research.size() == 6);
    CHECK(corps.research[0].propertyIds == "tech_improved_logistics");
    CHECK(corps.research[0].scope == ResearchItemDef::Scope::Global);
    CHECK(corps.research[0].time == 60_fx);
    CHECK(corps.research[0].cost.money == 50_fx);
    CHECK(corps.research[0].property == defs.propertyIndex("tech_improved_logistics"));
    const auto recruits = [&](const BuildingDef& b, const char* ids) {
        return std::any_of(b.recruitRefs.begin(), b.recruitRefs.end(), [&](const RecruitRef& r) { return defs.regiments[r.regiment].ids == ids; });
    };
    CHECK(recruits(corps, "regiment_gbr_hq_infantry"));
    CHECK(recruits(corps, "convoy_gbr_truck_engineer"));  // a bare truck, recruited as its convoy
    CHECK(corps.packedRegiment == defs.regimentIndex("convoy_gbr_truck_corps"));
    const BuildingDef& hq = *defs.building("gbr_hq_infantry");
    CHECK(hq.truck == defs.unitIndex("gbr_truck_infantry"));
    CHECK(hq.packedRegiment == defs.regimentIndex("regiment_gbr_hq_infantry"));
    CHECK(hq.unpackedRegiment == defs.regimentIndex("regiment_gbr_hq_infantry_unpacked"));
    CHECK(hq.packingLength == 8_fx);
    CHECK(hq.creationOffset == FVec2{1.5_fx, -2.5_fx});    // DeploymentInfo 2.5, 1.5 (right, forward)
    CHECK(hq.deploymentOffset == FVec2{6_fx, -2.5_fx});
    CHECK(recruits(hq, "regiment_gbr_infantry"));
    CHECK(defs.units[hq.truck].unpackTo == defs.buildingIndex("gbr_hq_infantry"));
    CHECK(defs.units[hq.truck].cost.money == 225_fx);
    const UnitDef& heavy = *defs.unit("gbr_tank_heavy");
    CHECK(heavy.requiredStructureIds == std::vector<std::string>{"gbr_motor_pool"});
    CHECK(heavy.requiredProperties == std::vector<std::string>{"tech_heavy_tank"});
    const EconomyDef& e = defs.economy;
    CHECK(e.startMoney == 500_fx);
    CHECK(e.stockpileMax == 99999_fx);
    CHECK(e.limitedMax == 20_fx);
    CHECK(e.buildingPadding == 2_fx);
    CHECK(e.ammoPrice == 2_fx);
    CHECK(e.oilPrice == 3_fx);
    CHECK(e.startLocation.buildingLimitProvided == 20_fx);
}

TEST_CASE("golden economy: a GBR base earns, recruits a division HQ that unpacks, and fills an infantry regiment unit by unit") {
    REQUIRE_DEFS();
    using namespace fixed_literals;
    const nav::MapDef map = nav::generateArenaMap("open", 160);
    Sim sim({1, &defs, &map});
    std::uint16_t seq = 0;
    const auto at = [&](Command c) {
        c.tick = sim.tick();
        c.seq = seq++;
        return c;
    };
    sim.step({at(makeStartPlayer(0, 0, 0, "gbr_corps", {80_fx, 80_fx}, Angle{}))});
    REQUIRE(sim.world().structures.size() == 1);
    const EntityId corps = sim.world().structures.ids()[0];
    Ledger l = ledgerOf(sim, 0);
    CHECK(l.moneyNet() == 25_fx);
    CHECK(l.oilNet() == 10_fx);
    CHECK(l.ammoNet() == 20_fx);
    CHECK(l.buildingLimit == 20_fx);
    sim.step({at(makeRecruit(0, 0, 0, corps, "regiment_gbr_hq_infantry"))});
    CHECK(sim.world().kingdoms[0].money < 276_fx);  // 225 for the truck
    EntityId truckReg;
    for (int i = 0; i < 400 && truckReg.isNull(); ++i) {
        sim.step();
        for (const SimEvent& ev : sim.events()) {
            if (ev.type == SimEvent::Type::Recruited) truckReg = ev.entity;
        }
    }
    REQUIRE_FALSE(truckReg.isNull());
    sim.step({at(makeUnpack(0, 0, 0, truckReg, {110_fx, 80_fx}, Angle{}))});
    EntityId hq;
    for (int i = 0; i < 1500 && hq.isNull(); ++i) {
        sim.step();
        for (const SimEvent& ev : sim.events()) {
            if (ev.type == SimEvent::Type::Unpacked) hq = ev.entity;
        }
    }
    REQUIRE_FALSE(hq.isNull());
    l = ledgerOf(sim, 0);
    CHECK(l.unitLimit == 5_fx);
    CHECK(l.moneyNet() == 50_fx);
    sim.step({at(makeRecruit(0, 0, 0, hq, "regiment_gbr_infantry"))});
    EntityId inf;
    std::size_t units = 0;
    for (int i = 0; i < 400 && inf.isNull(); ++i) {
        sim.step();
        for (const SimEvent& ev : sim.events()) {
            if (ev.type == SimEvent::Type::Spawned && ev.command == CommandType::Recruit && sim.world().members.has(ev.entity)) ++units;
            if (ev.type == SimEvent::Type::Recruited) inf = ev.entity;
        }
    }
    REQUIRE_FALSE(inf.isNull());
    CHECK(units == recruitPlan(defs, defs.regimentIndex("regiment_gbr_infantry")).size());
    CHECK(ledgerOf(sim, 0).ammoNet() == 30_fx - 10_fx);  // corps 20 + HQ 10, infantry upkeep 10
    // Heavy tanks: not this HQ's, and they need a motor pool and tech_heavy_tank anyway.
    sim.step({at(makeRecruit(0, 0, 0, hq, "regiment_gbr_tank_heavy"))});
    CHECK(std::any_of(sim.events().begin(), sim.events().end(), [](const SimEvent& ev) { return ev.type == SimEvent::Type::CommandRejected; }));
}

TEST_CASE("golden defs: supply, cities and victory data (supply ranges, resupply rates, cities, SVars)") {
    REQUIRE_DEFS();
    using namespace fixed_literals;
    const BuildingDef& corps = *defs.building("gbr_corps");
    CHECK(corps.supplyRange == 60_fx);
    CHECK(corps.hq);
    CHECK_FALSE(corps.supplyRelay);
    const BuildingDef& city = *defs.building("mediterranean_city_center01_large");
    CHECK(city.cityZoneRadius == 42_fx);
    CHECK(city.supplyRange == 55_fx);
    CHECK(city.captureable);
    CHECK(city.production.money == 15_fx);
    CHECK(isCity(defs, defs.buildingIndex("mediterranean_city_center01_large")));
    CHECK_FALSE(isCity(defs, defs.buildingIndex("med_villageblock12x12_02")));  // a village block: scenery
    CHECK(abs(defs.unit("gbr_infantry")->resupplyRate - 0.3_fx) < 0.0001_fx);
    const RegimentDef& inf = *defs.regiment("regiment_gbr_infantry");
    CHECK(inf.supplyUsed);
    CHECK(abs(inf.shortfallDamageRate - 0.01_fx) < 0.0001_fx);
    CHECK(defs.regiment("regiment_gbr_hq_infantry")->hq);
    const EconomyDef& e = defs.economy;
    CHECK(e.siegeDamageSeconds == 7_fx);
    CHECK(e.underSiegeRate == 0.25_fx);
    CHECK(e.supplyUnderSiege);
    CHECK(e.moraleSupplyBonus == 1_fx);
    CHECK(abs(e.captureHealthFactor - 0.3_fx) < 0.0001_fx);
    CHECK(e.teamVictory);
    CHECK(placeholderThing(defs, "ger", "placeholder_corpsunpacked") == "ger_corps");
}

TEST_CASE("golden skirmish on Golden Fields: the map's towns are cities, both kingdoms start at their placeholders, a city falls") {
    REQUIRE_DEFS();
    using namespace fixed_literals;
    const std::string path = CORPS_SOURCE_DIR "/cache/maps/golden_fields/map.json";
    std::ifstream in(vfs::pathFromUtf8(path), std::ios::binary);
    if (!in) {
        MESSAGE("skipped: no " << path << " (tools/rmp.py output)");
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    nav::MapDef map;
    std::string err;
    REQUIRE_MESSAGE(nav::loadRmpJson(text, map, err), err);
    REQUIRE(map.kingdoms.size() == 3);
    CHECK(map.rule("ResourceStockpileDefault", 0_fx) == 1000_fx);
    const std::vector<MapStart> starts = mapStarts(map);
    REQUIRE(starts.size() == 2);
    CHECK(starts[0].position == FVec2{205_fx, 145_fx});
    CHECK(starts[1].position == FVec2{27_fx, 129_fx});
    Sim sim({1, &defs, &map});
    std::vector<EntityId> cities;
    for (std::size_t i = 0; i < sim.world().structures.size(); ++i) cities.push_back(sim.world().structures.ids()[i]);
    REQUIRE(cities.size() == 2);  // chaparral_village_town_center -> mediterranean_city_center01_large, twice
    for (const EntityId c : cities) {
        CHECK(sim.world().structures.get(c)->player == kNeutral);
        CHECK(defs.buildings[sim.world().defRefs.get(c)->index].ids == "mediterranean_city_center01_large");
    }
    std::uint16_t seq = 0;
    const auto at = [&](Command c) {
        c.tick = sim.tick();
        c.seq = seq++;
        return c;
    };
    const FVec2 cityPos = sim.world().transforms.get(cities[0])->position;  // (210, 74)
    sim.step({at(makeStartPlayer(0, 0, 0, "gbr_corps", starts[0].position, starts[0].facing)),
              at(makeStartPlayer(0, 1, 0, "ger_corps", starts[1].position, starts[1].facing)),
              at(makeSpawnRegiment(0, 0, 0, "regiment_gbr_infantry", cityPos + FVec2{0_fx, 12_fx}, Angle{}))});
    for (const SimEvent& ev : sim.events()) CHECK(ev.type != SimEvent::Type::CommandRejected);
    CHECK(sim.world().kingdoms[0].money > 999_fx);  // the map's starting money
    EntityId captured;
    for (int i = 0; i < 300 && captured.isNull(); ++i) {
        sim.step();
        for (const SimEvent& ev : sim.events()) {
            if (ev.type == SimEvent::Type::CityCaptured) captured = ev.entity;
        }
    }
    REQUIRE(captured == cities[0]);
    CHECK(sim.world().structures.get(captured)->player == 0);
    CHECK(ledgerOf(sim, 0).production.money == 25_fx + 15_fx);
    // Both HQs supply their surroundings.
    const std::vector<SupplyZone> zones = supplyZones(sim);
    CHECK(zones.size() == 3);
}
