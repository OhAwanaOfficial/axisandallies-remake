// TGI_FORMAT §9.1: integer-only, exact value parsing. Expected Q32.32 raw values were computed with
// exact rational arithmetic (round to nearest, ties to even).

#include <doctest/doctest.h>

#include "data/tgi/value_parse.hpp"
#include "math/fixed.hpp"

using namespace corps;
using namespace corps::tgi;

namespace {

std::int64_t rawOf(std::string_view s) {
    const FixedParse p = parseFixed(s);
    REQUIRE_MESSAGE(p.ok, s);
    return p.value.raw;
}

}  // namespace

TEST_CASE("fixed: every accepted spelling gives exact bits") {
    CHECK(rawOf("1") == 4294967296LL);
    CHECK(rawOf("1.0f") == 4294967296LL);  // C float suffix ignored (SVars.tgi uses it on fixed vars)
    CHECK(rawOf(".5") == 2147483648LL);
    CHECK(rawOf(".5f") == 2147483648LL);
    CHECK(rawOf("+2") == 8589934592LL);
    CHECK(rawOf("-1") == -4294967296LL);
    CHECK(rawOf("5.") == 21474836480LL);
    CHECK(rawOf("25%") == 1073741824LL);
    CHECK(rawOf("133%") == 5712306504LL);
    CHECK(rawOf(".3333") == 1431512600LL);
    CHECK(rawOf("-.3333") == -1431512600LL);
    CHECK(rawOf("0.1") == 429496730LL);
    CHECK(rawOf("12.345") == 53021371269LL);
    CHECK(rawOf("  7  ") == 30064771072LL);
}

TEST_CASE("fixed: one rounding, ties to even") {
    // exactly half an ulp (2^-33) rounds down to even 0; 1.5 ulp rounds up to even 2
    CHECK(rawOf("0.000000000116415321826934814453125") == 0);
    CHECK(rawOf("0.000000000349245965480804443359375") == 2);
    // a long expansion is still exact
    CHECK(rawOf("0.33333333333333333333333333333333333333") == 1431655765LL);
}

TEST_CASE("fixed: range and junk") {
    CHECK(parseFixed("2147483647").ok);
    CHECK_FALSE(parseFixed("2147483648").ok);
    CHECK(parseFixed("-2147483648").ok);
    CHECK(parseFixed("-2147483648").value.raw == INT64_MIN);
    CHECK_FALSE(parseFixed("2147483647.99999999999").ok);  // rounds up past the maximum
    CHECK_FALSE(parseFixed("1e5").ok);                     // no exponents
    CHECK_FALSE(parseFixed("").ok);
    CHECK_FALSE(parseFixed(".").ok);
    CHECK_FALSE(parseFixed("abc").ok);
}

TEST_CASE("fixed: decimal comma takes the leading number with a warning") {
    const FixedParse p = parseFixed("0,5");
    CHECK(p.ok);
    CHECK(p.decimalComma);
    CHECK(p.value.raw == 0);

    Diagnostics diags;
    const Fixed v = readFixed("0,5", Fixed::fromInt(9), {&diags, "f.tgi", 17, "decal_fade_time"});
    CHECK(v.raw == 0);
    CHECK(diags.count(DiagCode::DecimalComma) == 1);
    CHECK(readFixed("nope", Fixed::fromInt(9), {&diags, "f.tgi", 18, "k"}) == Fixed::fromInt(9));
    CHECK(diags.count(DiagCode::BadValue) == 1);
}

TEST_CASE("formatFixed prints exact decimals") {
    CHECK(formatFixed(Fixed::fromRaw(rawOf("3.5"))) == "3.5");
    CHECK(formatFixed(Fixed::fromRaw(rawOf("-1.25"))) == "-1.25");
    CHECK(formatFixed(Fixed::fromInt(400)) == "400");
    CHECK(formatFixed(Fixed::fromRaw(1)) == "0.00000000023283064365386962890625");
}

TEST_CASE("int, bool, null") {
    CHECK(parseInt("-12") == -12);
    CHECK(parseInt("+3") == 3);
    CHECK(parseInt("9223372036854775807") == INT64_MAX);
    CHECK_FALSE(parseInt("9223372036854775808").has_value());
    CHECK_FALSE(parseInt("1.0").has_value());
    CHECK_FALSE(parseInt("+").has_value());

    CHECK(parseBool("TRUE") == true);
    CHECK(parseBool("Yes") == true);
    CHECK(parseBool("1") == true);
    CHECK(parseBool("no") == false);
    CHECK(parseBool("False") == false);
    CHECK(parseBool("0") == false);
    CHECK_FALSE(parseBool("maybe").has_value());

    CHECK(isNull("null"));
    CHECK(isNull("NULL"));
    CHECK_FALSE(isNull("none"));  // SAI uses 'none' as a literal
    CHECK_FALSE(isNull(""));
}

TEST_CASE("points: comma-separated Fixed components") {
    const PointParse p3 = parsePoint("-1, -1, -1", 3);
    CHECK(p3.ok);
    CHECK_FALSE(p3.countMismatch);
    CHECK(p3.v[2] == Fixed::fromInt(-1));

    const PointParse dots = parsePoint(".6,.6,.6", 3);
    CHECK(dots.ok);
    CHECK(dots.v[0].raw == rawOf(".6"));

    const PointParse pct = parsePoint("0%, 50%", 2);
    CHECK(pct.ok);
    CHECK(pct.v[1].raw == 2147483648LL);

    const PointParse shortP = parsePoint("1, 2", 3);  // missing components are 0, with a warning
    CHECK(shortP.countMismatch);
    CHECK(shortP.v[2].raw == 0);

    Diagnostics diags;
    readPoint("1,2,3,4", 3, {&diags, "f.tgi", 1, "p"});
    CHECK(diags.count(DiagCode::BadValue) == 1);
}

TEST_CASE("word lists and localized refs") {
    const auto w = splitWordList("+enemy, -air,-mine  -water");
    REQUIRE(w.size() == 4);
    CHECK(w[0] == "+enemy");
    CHECK(w[3] == "-water");
    CHECK(splitWordList("a.mp3,b.mp3").size() == 2);
    CHECK(splitWordList("+constructable +friend").size() == 2);

    CHECK(localizedKey("#ids_key") == std::string_view("ids_key"));
    CHECK_FALSE(localizedKey("literal text").has_value());
}
