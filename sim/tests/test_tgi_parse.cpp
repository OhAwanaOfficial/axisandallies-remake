// TGI_FORMAT §2–5: decoding, comments, line classes, headers, fields and block structure.

#include <doctest/doctest.h>

#include "data/tgi/lexer.hpp"
#include "data/tgi/parser.hpp"
#include "data/tgi/text_decode.hpp"

using namespace corps;
using namespace corps::tgi;

namespace {

struct Parsed {
    SymbolTable symbols;
    Diagnostics diags;
    AstFile file;
};

Parsed parse(std::string_view text) {
    Parsed p;
    p.file = parseTgi(text, 0, "test.tgi", p.symbols, p.diags);
    return p;
}

const AstField* field(const AstNode& n, std::string_view key) {
    for (const auto& f : n.fields) {
        if (f.key == key) return &f;
    }
    return nullptr;
}

}  // namespace

// --- §2 decoding -----------------------------------------------------------------------------------

TEST_CASE("decode: BOM means UTF-8 and is stripped") {
    const DecodedText d = decodeText("\xEF\xBB\xBFkey = \xC3\xA9");
    CHECK(d.encoding == TextEncoding::Utf8Bom);
    CHECK(d.utf8 == "key = \xC3\xA9");
}

TEST_CASE("decode: valid UTF-8 stays, ASCII is UTF-8") {
    CHECK(decodeText("plain").encoding == TextEncoding::Utf8);
    CHECK(decodeText("caf\xC3\xA9").encoding == TextEncoding::Utf8);
}

TEST_CASE("decode: otherwise Windows-1252 (copyright, smart quotes, e-acute)") {
    const DecodedText d = decodeText("\xA9 2002 \x93q\x94 \xE9");
    CHECK(d.encoding == TextEncoding::Cp1252);
    CHECK(d.utf8 == "\xC2\xA9 2002 \xE2\x80\x9Cq\xE2\x80\x9D \xC3\xA9");
    CHECK_FALSE(isValidUtf8("\xC0\xAF"));      // overlong
    CHECK_FALSE(isValidUtf8("\xED\xA0\x80"));  // surrogate
}

// --- §3.1 comments ---------------------------------------------------------------------------------

TEST_CASE("comments: ';' and '//' to end of line, after headers and values") {
    const auto p = parse(
        "[Slot]\t;; front\n"
        "icon = null ;; %sIcon.tga\n"
        "a = 1 // trailing\n"
        "// whole line\n");
    REQUIRE(p.file.nodes.size() == 1);
    const AstNode& n = p.file.nodes[0];
    CHECK(n.name == "Slot");
    CHECK(field(n, "icon")->value == "null");
    CHECK(field(n, "a")->value == "1");
    CHECK(n.fields.size() == 2);
}

TEST_CASE("comments: '/* */' does not nest; the first '*/' closes") {
    const auto p = parse(
        "/* header comment\n"
        "[Template Hidden]\n"
        "/* inner opener is ignored\n"
        "*/\n"
        "[Visible]\n"
        "x = 1\n");
    REQUIRE(p.file.nodes.size() == 1);
    CHECK(p.file.nodes[0].name == "Visible");
    CHECK(p.file.nodes[0].loc.line == 5);  // line numbers survive multi-line comments
}

TEST_CASE("comments: unterminated '/*' runs to EOF with a warning") {
    const auto p = parse(
        "[A]\n"
        "x = 1\n"
        "/* never closed\n"
        "[B]\n");
    CHECK(p.file.nodes.size() == 1);
    CHECK(p.diags.count(DiagCode::UnterminatedBlockComment) == 1);
}

TEST_CASE("comments: quote-aware; markers inside strings are text") {
    const auto p = parse(
        "[S]\n"
        "sub = \"wait; then // go /* now */\"\n"
        "cmd = \"#key\" ;\"old text\"\n");
    const AstNode& n = p.file.nodes[0];
    CHECK(field(n, "sub")->value == "wait; then // go /* now */");
    CHECK(field(n, "cmd")->value == "#key");
}

TEST_CASE("line endings: CRLF, LF and lone CR") {
    const auto p = parse("[A]\r\nx = 1\ry = 2\nz = 3");
    const AstNode& n = p.file.nodes[0];
    CHECK(n.fields.size() == 3);
    CHECK(field(n, "y")->loc.line == 3);
    CHECK(field(n, "z")->loc.line == 4);
}

// --- §3.3 headers ----------------------------------------------------------------------------------

TEST_CASE("headers: template definition, data node, schema declarations") {
    const ParsedHeader t = parseHeaderLine("[Template FooUnit inherit=Unit]");
    CHECK(t.kind == NodeKind::Template);
    CHECK(t.name == "FooUnit");
    CHECK(t.parentRef == "Unit");

    const ParsedHeader d = parseHeaderLine("[Thing Template = FooUnit]");  // spaces around '='
    CHECK(d.kind == NodeKind::Data);
    CHECK(d.name == "Thing");
    CHECK(d.parentRef == "FooUnit");

    const ParsedHeader l = parseHeaderLine("[list Turret Template=Turret]");
    CHECK(l.kind == NodeKind::Decl);
    CHECK(l.listMod);
    CHECK_FALSE(l.optionalMod);
    CHECK(l.name == "Turret");

    const ParsedHeader ol = parseHeaderLine("[Optional List Thing]");
    CHECK(ol.kind == NodeKind::Decl);
    CHECK(ol.listMod);
    CHECK(ol.optionalMod);
    CHECK(ol.name == "Thing");

    const ParsedHeader plain = parseHeaderLine("[List]");  // a lone modifier word is a name
    CHECK(plain.kind == NodeKind::Data);
    CHECK(plain.name == "List");
}

TEST_CASE("headers: template= and inherit= are synonyms; both → template= wins with a warning") {
    CHECK(parseHeaderLine("[WhenDo Inherit=FX]").parentRef == "FX");
    const ParsedHeader both = parseHeaderLine("[X inherit=A template=B]");
    CHECK(both.parentRef == "B");
    CHECK(both.templateAndInherit);
    const ParsedHeader lang = parseHeaderLine("[Text language=english]");
    REQUIRE(lang.attrs.size() == 1);
    CHECK(lang.attrs[0].key == "language");
    CHECK(lang.attrs[0].value == "english");
    CHECK_FALSE(lang.hasParentRef);
}

TEST_CASE("headers: missing ']' ends at end of line; stray '=' is dropped") {
    const auto p = parse(
        "[Template Foo inherit=Bar\n"
        "{\n"
        "}\n"
        "[Kind = Template=Foo]\n");
    REQUIRE(p.file.nodes.size() == 2);
    CHECK(p.file.nodes[0].name == "Foo");
    CHECK(p.file.nodes[0].parentRef == "Bar");
    CHECK(p.diags.count(DiagCode::HeaderMissingBracket) == 1);
    CHECK(p.file.nodes[1].name == "Kind");
    CHECK(p.file.nodes[1].parentRef == "Foo");
    CHECK(p.diags.count(DiagCode::HeaderStrayEquals) == 1);
}

// --- §3.4–3.5 fields -------------------------------------------------------------------------------

TEST_CASE("fields: typed keys, literal keys with spaces and '|', '=' split") {
    const ParsedField typed = parseFieldLine("Point3 offset = 0, 0, 1");
    CHECK(typed.type == TypeWord::Point3);
    CHECK(typed.key == "offset");
    CHECK(typed.value == "0, 0, 1");

    const ParsedField spaced = parseFieldLine("mg_place_Big Island_name = \"x\"");
    CHECK(spaced.type == TypeWord::None);
    CHECK(spaced.key == "mg_place_Big Island_name");

    CHECK(parseFieldLine("game_message|* = 1").key == "game_message|*");
    CHECK(parseFieldLine("key_[ = a").key == "key_[");
    CHECK(parseFieldLine("ui_failed|s = b").key == "ui_failed|s");
    CHECK(parseFieldLine("a = b = c").value == "b = c");  // first '=' only
}

TEST_CASE("fields: declarations without '=' and bare keys") {
    const ParsedField decl = parseFieldLine("point2 location");
    CHECK(decl.ok);
    CHECK(decl.type == TypeWord::Point2);
    CHECK_FALSE(decl.hasValue);

    const ParsedField bare = parseFieldLine("texture");
    CHECK(bare.ok);
    CHECK(bare.type == TypeWord::None);
    CHECK_FALSE(bare.hasValue);

    CHECK_FALSE(parseFieldLine("three loose words").ok);
    CHECK(parseTypeWord("KF32") == TypeWord::KF32);
    CHECK(parseTypeWord("Fixed") == TypeWord::Fixed);
}

TEST_CASE("quoted values: only the outer pair is removed, no escapes") {
    CHECK(parseFieldLine("text = \"@Billy \"BMC\" McCain\"").value == "@Billy \"BMC\" McCain");
    CHECK(parseFieldLine("t = \"a\\nb %s\"").value == "a\\nb %s");
    CHECK(parseFieldLine("t = \"\"").value.empty());
    CHECK(parseFieldLine("t = \"\"").quoted);

    const ParsedField dq = parseFieldLine("help = \"#some_help_string\"\"");
    CHECK(dq.value == "#some_help_string");
    CHECK(dq.doubledQuote);
}

// --- §5 blocks -------------------------------------------------------------------------------------

TEST_CASE("braceless body: fields up to the next header or '}', indentation ignored") {
    const auto p = parse(
        "[Thing]\n"
        "{\n"
        "    [Property]\n"
        "    property_IDS = front\n"
        "  [Property]\n"
        "property_IDS = support\n"
        "    after = 1\n"
        "}\n");
    REQUIRE(p.file.nodes.size() == 1);
    const AstNode& thing = p.file.nodes[0];
    REQUIRE(thing.children.size() == 2);
    CHECK(thing.fields.empty());
    CHECK_FALSE(thing.children[0].braced);
    CHECK(field(thing.children[0], "property_IDS")->value == "front");
    CHECK(thing.children[1].fields.size() == 2);  // 'after' belongs to the braceless node
}

TEST_CASE("late '{' turns an open braceless node into a braced one") {
    const auto p = parse(
        "[Thing]\n"
        "{\n"
        "  [Size]\n"
        "  a = 1\n"
        "  b = 2\n"
        "  {\n"
        "    [Shape]\n"
        "    c = 3\n"
        "  }\n"
        "  d = 4\n"
        "}\n");
    REQUIRE(p.file.nodes.size() == 1);
    const AstNode& thing = p.file.nodes[0];
    REQUIRE(thing.children.size() == 1);
    const AstNode& size = thing.children[0];
    CHECK(size.braced);
    CHECK(size.fields.size() == 2);
    REQUIRE(size.children.size() == 1);
    CHECK(field(thing, "d") != nullptr);  // braces balance under this reading
    CHECK(p.diags.count(DiagCode::LateOpenBrace) == 1);
    CHECK(p.diags.count(DiagCode::ExtraCloseBrace) == 0);
}

TEST_CASE("'{}' on one line is an empty body; text after '{' is the body's first line") {
    const auto p = parse(
        "[A]\n"
        "{}\n"
        "[B]\n"
        "{\tprefix = X/Y\n"
        "  more = 1\n"
        "}\n");
    REQUIRE(p.file.nodes.size() == 2);
    CHECK(p.file.nodes[0].braced);
    CHECK(p.file.nodes[0].fields.empty());
    CHECK(field(p.file.nodes[1], "prefix")->value == "X/Y");
    CHECK(p.file.nodes[1].fields.size() == 2);
    CHECK(p.diags.empty());
}

TEST_CASE("unclosed blocks at EOF are closed with one warning per file") {
    const auto p = parse(
        "[A]\n"
        "{\n"
        "  [B]\n"
        "  {\n"
        "    x = 1\n");
    REQUIRE(p.file.nodes.size() == 1);
    CHECK(p.file.nodes[0].children.size() == 1);
    CHECK(p.diags.count(DiagCode::EofUnclosedBlock) == 1);
}

TEST_CASE("extra '}' at root is ignored; anonymous '{' block is dropped") {
    const auto p = parse(
        "[A]\n"
        "{\n"
        "}\n"
        "}\n"
        "{\n"
        "  lost = 1\n"
        "}\n"
        "[B]\n");
    CHECK(p.file.nodes.size() == 2);
    CHECK(p.diags.count(DiagCode::ExtraCloseBrace) == 1);
    CHECK(p.diags.count(DiagCode::AnonymousBlock) == 1);
}

TEST_CASE("text after '}' is re-read as a line, with a warning") {
    const auto p = parse(
        "[A]\n"
        "{\n"
        "} [B]\n"
        "x = 1\n");
    REQUIRE(p.file.nodes.size() == 2);
    CHECK(field(p.file.nodes[1], "x") != nullptr);
    CHECK(p.diags.count(DiagCode::TextAfterBrace) == 1);
}

TEST_CASE("nested template and root-level declaration are flagged") {
    const auto p = parse(
        "[list Stray]\n"
        "[Outer]\n"
        "{\n"
        "  [Template Inner]\n"
        "}\n");
    CHECK(p.diags.count(DiagCode::DeclAtRoot) == 1);
    CHECK(p.diags.count(DiagCode::NestedTemplate) == 1);
}

TEST_CASE("pre-order node indexes follow header order") {
    const auto p = parse(
        "[A]\n"
        "{\n"
        "  [B]\n"
        "  [C]\n"
        "}\n"
        "[D]\n");
    CHECK(p.file.nodeCount == 4);
    CHECK(p.file.nodes[0].index == 0);
    CHECK(p.file.nodes[0].children[0].index == 1);
    CHECK(p.file.nodes[0].children[1].index == 2);
    CHECK(p.file.nodes[1].index == 3);
}
