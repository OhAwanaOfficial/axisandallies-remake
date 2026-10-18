// TGI_FORMAT §7–8: template binding, overlay rules, provenance, records and identity.

#include <doctest/doctest.h>

#include "data/tgi/resolved_dump.hpp"
#include "tgi_fixture.hpp"

using namespace corps;
using namespace corps::tgi;
using test::at;
using test::loadTgi;
using test::value;

namespace {

// Engine-like base templates shared by several tests.
const char* const kBase =
    "[Template Base]\n"
    "{\n"
    "  string IDS\n"
    "  string category = BASE\n"
    "  fixed speed = 1\n"
    "  [list Property Template=PropRef]\n"
    "  [optional Body Template=BodyT]\n"
    "  [Flags]\n"
    "  {\n"
    "    flag blocking = false\n"
    "    flag flying = false\n"
    "  }\n"
    "}\n";

const char* const kParts =
    "[Template PropRef]\n"
    "{\n"
    "  string property_IDS\n"
    "}\n"
    "[Template BodyT]\n"
    "{\n"
    "  fixed health = 10\n"
    "  fixed defense = 0\n"
    "}\n";

}  // namespace

TEST_CASE("scalars: later layer replaces; provenance points at the writer") {
    const auto l = loadTgi({{"t.tgi", std::string(kParts) + kBase +
                                          "[Template Mid inherit=Base]\n"
                                          "{\n"
                                          "  speed = 2\n"
                                          "  category = MID\n"
                                          "}\n"},
                            {"u.tgi",
                             "[Thing Template=Mid]\n"
                             "{\n"
                             "  IDS = unit_a\n"
                             "  speed = 3.5\n"
                             "}\n"}});
    const Record* r = l.record("unit_a");
    REQUIRE(r != nullptr);
    CHECK(value(l, &r->tree, "speed") == "3.5");
    CHECK(value(l, &r->tree, "category") == "MID");
    const ResolvedField* speed = r->tree.field(l.sym("speed"));
    CHECK(speed->type == TypeWord::Fixed);  // declared in Base, value from the instance
    CHECK(speed->valueOrigin == kOriginInstance);
    CHECK(l.db->pathOf(speed->value->loc.file) == "u.tgi");
    CHECK(speed->value->loc.line == 4);
    const ResolvedField* cat = r->tree.field(l.sym("category"));
    CHECK(l.db->originName(cat->valueOrigin) == "Mid");
    CHECK(l.db->diags.empty());
}

TEST_CASE("lists append: inherited items first, then local ones in source order") {
    const auto l = loadTgi({{"t.tgi", std::string(kParts) + kBase +
                                          "[Template Tank inherit=Base]\n"
                                          "{\n"
                                          "  [Property]\n"
                                          "  property_IDS = vehicle\n"
                                          "  [Property]\n"
                                          "  property_IDS = tank\n"
                                          "}\n"},
                            {"u.tgi",
                             "[Thing Template=Tank]\n"
                             "{\n"
                             "  IDS = t1\n"
                             "  [Property]\n"
                             "  property_IDS = front\n"
                             "  [Property]\n"
                             "  property_IDS = support\n"
                             "}\n"}});
    const Record* r = l.record("t1");
    REQUIRE(r != nullptr);
    REQUIRE(r->tree.childCount(l.sym("Property")) == 4);
    CHECK(value(l, at(l, r->tree, "Property[0]"), "property_IDS") == "vehicle");
    CHECK(value(l, at(l, r->tree, "Property[1]"), "property_IDS") == "tank");
    CHECK(value(l, at(l, r->tree, "Property[2]"), "property_IDS") == "front");
    CHECK(value(l, at(l, r->tree, "Property[3]"), "property_IDS") == "support");
    // items start from the list's item template (schema carried over)
    CHECK(at(l, r->tree, "Property[2]")->field(l.sym("property_IDS"))->type == TypeWord::String);
}

TEST_CASE("named child without template=: deep merge; optional child starts from its declaration") {
    const auto l = loadTgi({{"t.tgi", std::string(kParts) + kBase +
                                          "[Template Mid inherit=Base]\n"
                                          "{\n"
                                          "  [Flags]\n"
                                          "  {\n"
                                          "    blocking = true\n"
                                          "  }\n"
                                          "  [Body]\n"
                                          "  defense = 2\n"
                                          "}\n"},
                            {"u.tgi",
                             "[Thing Template=Mid]\n"
                             "{\n"
                             "  IDS = m1\n"
                             "  [flags]\n"  // names match case-insensitively
                             "  flying = true\n"
                             "  [BODY]\n"
                             "  health = 400\n"
                             "}\n"}});
    const Record* r = l.record("m1");
    REQUIRE(r != nullptr);
    CHECK(r->tree.childCount(l.sym("Flags")) == 1);
    CHECK(value(l, at(l, r->tree, "Flags"), "blocking") == "true");
    CHECK(value(l, at(l, r->tree, "Flags"), "flying") == "true");
    const ResolvedNode* body = at(l, r->tree, "Body");
    REQUIRE(body != nullptr);
    CHECK(value(l, body, "health") == "400");
    CHECK(value(l, body, "defense") == "2");
    CHECK(body->field(l.sym("health"))->type == TypeWord::Fixed);  // from BodyT via the optional decl
}

TEST_CASE("an optional child is absent until used") {
    const auto l = loadTgi({{"t.tgi", std::string(kParts) + kBase},
                            {"u.tgi", "[Thing Template=Base]\n{\n  IDS = bare\n}\n"}});
    const Record* r = l.record("bare");
    REQUIRE(r != nullptr);
    CHECK(at(l, r->tree, "Body") == nullptr);
    CHECK(r->tree.decl(l.sym("Body")) != nullptr);
}

TEST_CASE("named child WITH template= over an inherited one: rebase, not merge") {
    const auto l = loadTgi({{"t.tgi",
                             "[Template Fx]\n{\n  string sound\n}\n"
                             "[Template Def]\n"
                             "{\n"
                             "  fixed range\n"
                             "  [list WhenDo Template=Fx]\n"
                             "}\n"
                             "[Template HammerDef inherit=Def]\n"
                             "{\n"
                             "  range = 1\n"
                             "  [WhenDo]\n"
                             "  sound = hammer\n"
                             "}\n"
                             "[Template RepairDef inherit=Def]\n"
                             "{\n"
                             "  range = 5\n"
                             "  [WhenDo]\n"
                             "  sound = hammer\n"
                             "}\n"
                             "[Template Ability]\n"
                             "{\n"
                             "  [AbilityDefinition Template=HammerDef]\n"
                             "  extra = 1\n"
                             "}\n"},
                            {"u.tgi",
                             "[Thing Template=Ability]\n"
                             "{\n"
                             "  IDS = eng\n"
                             "  [AbilityDefinition Template=RepairDef]\n"
                             "  {\n"
                             "    IDS = null\n"
                             "  }\n"
                             "}\n"}});
    const Record* r = l.record("eng");
    REQUIRE(r != nullptr);
    const ResolvedNode* def = at(l, r->tree, "AbilityDefinition");
    REQUIRE(def != nullptr);
    CHECK(r->tree.childCount(l.sym("AbilityDefinition")) == 1);
    CHECK(value(l, def, "range") == "5");
    CHECK(value(l, def, "extra") == "<none>");             // inherited child content dropped
    CHECK(def->childCount(l.sym("WhenDo")) == 1);           // merge would give the hammer FX twice
    CHECK(l.db->originName(def->base) == "RepairDef");
}

TEST_CASE("bare key clears the inherited value and marks the field required") {
    const auto l = loadTgi({{"t.tgi",
                             "[Template A]\n{\n  texture = x.dds\n  amount = 3\n}\n"
                             "[Template B inherit=A]\n{\n  texture\n}\n"},
                            {"u.tgi", "[Thing Template=B]\n{\n  IDS = c\n}\n"}});
    const Record* r = l.record("c");
    REQUIRE(r != nullptr);
    const ResolvedField* tex = r->tree.field(l.sym("texture"));
    REQUIRE(tex != nullptr);
    CHECK_FALSE(tex->hasValue());
    CHECK(tex->required);
    CHECK(tex->cleared != nullptr);
    CHECK(value(l, &r->tree, "amount") == "3");
}

TEST_CASE("typed declaration without a value keeps the inherited value") {
    const auto l = loadTgi({{"t.tgi",
                             "[Template A]\n{\n  speed = 4\n}\n"
                             "[Template B inherit=A]\n{\n  fixed speed\n}\n"},
                            {"u.tgi", "[Thing Template=B]\n{\n  IDS = d\n}\n"}});
    const ResolvedField* f = l.record("d")->tree.field(l.sym("speed"));
    CHECK(f->type == TypeWord::Fixed);
    CHECK(f->text() == "4");
}

TEST_CASE("sequential binding: each file sees the definition loaded before it") {
    const auto l = loadTgi({{"base.tgi", "[Template Gun]\n{\n  range = 1\n}\n"},
                            {"regiment_a.tgi",
                             "[Template Reg inherit=Gun]\n{\n  loadout = light\n}\n"
                             "[Thing Template=Reg]\n{\n  IDS = reg_a\n}\n"},
                            {"regiment_b.tgi",
                             "[Template Reg inherit=Gun]\n{\n  loadout = heavy\n}\n"
                             "[Thing Template=Reg]\n{\n  IDS = reg_b\n}\n"}});
    CHECK(value(l, &l.record("reg_a")->tree, "loadout") == "light");  // "global last wins" would say heavy
    CHECK(value(l, &l.record("reg_b")->tree, "loadout") == "heavy");
    CHECK(l.count(DiagCode::TemplateRedefined) == 1);
}

TEST_CASE("binding is fixed: redefining a parent later doesn't change bound children") {
    const auto l = loadTgi({{"a.tgi",
                             "[Template P]\n{\n  v = old\n}\n"
                             "[Template C inherit=P]\n{\n}\n"
                             "[Template P]\n{\n  v = new\n}\n"
                             "[Thing Template=C]\n{\n  IDS = x\n}\n"
                             "[Thing Template=P]\n{\n  IDS = y\n}\n"}});
    CHECK(value(l, &l.record("x")->tree, "v") == "old");
    CHECK(value(l, &l.record("y")->tree, "v") == "new");
}

TEST_CASE("a template may extend the previous definition of its own name") {
    const auto l = loadTgi({{"a.tgi",
                             "[Template P]\n{\n  a = 1\n}\n"
                             "[Template P inherit=P]\n{\n  b = 2\n}\n"
                             "[Thing Template=P]\n{\n  IDS = z\n}\n"}});
    CHECK(value(l, &l.record("z")->tree, "a") == "1");
    CHECK(value(l, &l.record("z")->tree, "b") == "2");
}

TEST_CASE("forward or unknown template reference: error, empty base") {
    const auto l = loadTgi({{"a.tgi",
                             "[Thing Template=Later]\n{\n  IDS = early\n  own = 1\n}\n"
                             "[Template Later]\n{\n  inherited = 1\n}\n"}});
    const Record* r = l.record("early");
    REQUIRE(r != nullptr);
    CHECK(value(l, &r->tree, "own") == "1");
    CHECK(value(l, &r->tree, "inherited") == "<none>");
    CHECK(l.count(DiagCode::UnknownTemplate) == 1);
}

TEST_CASE("a template that reaches itself is a cycle error, resolved with an empty base") {
    const auto l = loadTgi({{"a.tgi",
                             "[Template Loop]\n{\n  x = 1\n  [Child Template=Loop]\n  y = 2\n}\n"
                             "[Thing Template=Loop]\n{\n  IDS = loop\n}\n"}});
    const Record* r = l.record("loop");
    REQUIRE(r != nullptr);
    CHECK(value(l, &r->tree, "x") == "1");
    CHECK(value(l, at(l, r->tree, "Child"), "y") == "2");
    CHECK(value(l, at(l, r->tree, "Child"), "x") == "<none>");
    CHECK(l.count(DiagCode::TemplateCycle) == 1);
}

TEST_CASE("top-level node without template= uses a same-named template; else empty") {
    const auto l = loadTgi({{"a.tgi",
                             "[Template NameList]\n{\n  string IDS\n  int min = 1\n}\n"
                             "[NameList]\n{\n  IDS = names\n}\n"
                             "[SVars]\n{\n  int ticks = 16\n}\n"}});
    CHECK(value(l, &l.record("names")->tree, "min") == "1");
    CHECK(l.db->records.size() == 2);
    CHECK(l.db->records[1].tree.base == kNoTemplate);
}

TEST_CASE("list with an inline item schema") {
    const auto l = loadTgi({{"a.tgi",
                             "[Template Map]\n{\n  [list Biome]\n  {\n    string IDS\n    int weight = 5\n  }\n}\n"
                             "[Thing Template=Map]\n{\n  IDS = map\n  [Biome]\n  IDS = snow\n  [Biome]\n  IDS = sand\n"
                             "  weight = 7\n}\n"}});
    const Record* r = l.record("map");
    REQUIRE(r->tree.childCount(l.sym("Biome")) == 2);
    CHECK(value(l, at(l, r->tree, "Biome[0]"), "weight") == "5");
    CHECK(value(l, at(l, r->tree, "Biome[1]"), "weight") == "7");
    CHECK(at(l, r->tree, "Biome[0]")->field(l.sym("weight"))->type == TypeWord::Int);
}

TEST_CASE("same name repeated with no list declaration: treated as a list, reported once") {
    const auto l = loadTgi({{"a.tgi",
                             "[Template Mover]\n{\n  [WhenStartMoving]\n  base = 1\n}\n"
                             "[Thing Template=Mover]\n{\n  IDS = mv\n"
                             "  [WhenStartMoving]\n  location = a\n"
                             "  [WhenStartMoving]\n  location = b\n}\n"}});
    const Record* r = l.record("mv");
    REQUIRE(r->tree.childCount(l.sym("WhenStartMoving")) == 3);
    CHECK(value(l, at(l, r->tree, "WhenStartMoving[0]"), "base") == "1");
    CHECK(value(l, at(l, r->tree, "WhenStartMoving[1]"), "location") == "a");
    CHECK(value(l, at(l, r->tree, "WhenStartMoving[2]"), "location") == "b");
    CHECK(l.count(DiagCode::UndeclaredRepeat) == 1);
}

TEST_CASE("redeclaring a field with another type warns; bool and flag are the same type") {
    const auto l = loadTgi({{"a.tgi",
                             "[SVars]\n{\n  bool a = true\n  int b = 1\n}\n"
                             "[SVars]\n{\n  flag a = false\n  fixed b = 2\n}\n"}});
    CHECK(l.count(DiagCode::TypeRedeclared) == 1);
}

TEST_CASE("records: by-IDS last wins with a warning; singletons merge; ordered types keep all") {
    const auto l = loadTgi({{"a.tgi",
                             "[Thing]\n{\n  IDS = dup\n  v = 1\n}\n"
                             "[SVars]\n{\n  fixed speed = 1.0f\n  int keep = 3\n}\n"
                             "[Translate]\n{\n  ids_old = a\n}\n"},
                            {"b.tgi",
                             "[Thing]\n{\n  ids = DUP\n  v = 2\n}\n"
                             "[SVars]\n{\n  speed = 2\n}\n"
                             "[Translate]\n{\n  ids_old = b\n}\n"
                             "[Gizmo]\n{\n  IDS = g\n}\n"}});
    const TgiDatabase& db = *l.db;
    CHECK(l.count(DiagCode::DuplicateIds) == 1);
    const DefIndex::ByIdsEntry* e = db.defs.find(l.sym("Thing"), "Dup");
    REQUIRE(e != nullptr);
    CHECK(value(l, &db.records[e->record].tree, "v") == "2");
    REQUIRE(db.defs.singletons.size() == 1);
    CHECK(value(l, &db.defs.singletons[0].merged, "speed") == "2");
    CHECK(value(l, &db.defs.singletons[0].merged, "keep") == "3");
    CHECK(db.defs.singletons[0].merged.field(l.sym("speed"))->type == TypeWord::Fixed);
    CHECK(db.defs.ordered.size() == 3);  // two Translate records + the unknown Gizmo
    REQUIRE(db.defs.unknownTypes.size() == 1);
    CHECK(db.records[db.defs.unknownTypes[0]].typeSpelling == "Gizmo");
    CHECK(reportUnknown(db).find("Gizmo") != std::string::npos);
}

TEST_CASE("explain shows the chain, the winning value and every earlier write") {
    const auto l = loadTgi({{"t.tgi",
                             "[Template Attack]\n{\n  fixed range = 1\n}\n"
                             "[Template Gun inherit=Attack]\n{\n  range = 18\n}\n"},
                            {"u.tgi", "[Thing Template=Gun]\n{\n  IDS = tank\n  range = 20\n}\n"}},
                           /*trackHistory=*/true);
    const Record* r = l.record("tank");
    std::string out;
    REQUIRE(explainRecord(*l.db, *r, "range", out));
    CHECK(out.find("chain: Thing (u.tgi:1) -> Gun (t.tgi:5) -> Attack (t.tgi:1)") != std::string::npos);
    CHECK(out.find("range = 20") != std::string::npos);
    CHECK(out.find("was 18  t.tgi:7 (Gun)") != std::string::npos);
    CHECK(out.find("was 1  t.tgi:3 (Attack)") != std::string::npos);
    std::string missing;
    CHECK_FALSE(explainRecord(*l.db, *r, "Nope.x", missing));
}

TEST_CASE("the resolved dump is deterministic across independent loads") {
    const test::Files files = {{"t.tgi", std::string(kParts) + kBase},
                               {"u.tgi", "[Thing Template=Base]\n{\n  IDS = q\n  [Property]\n  property_IDS = p\n}\n"}};
    const auto a = loadTgi(files);
    const auto b = loadTgi(files);
    const std::string da = dumpDefinitionSet(*a.db);
    CHECK(da == dumpDefinitionSet(*b.db));
    CHECK(da.find("== Thing q") != std::string::npos);
    CHECK(definitionsJson(*a.db) == definitionsJson(*b.db));
}
