// TGI_FORMAT §6: index*.lst load order.

#include <doctest/doctest.h>

#include "data/tgi/lst_loader.hpp"
#include "tgi_fixture.hpp"

using namespace corps;
using namespace corps::tgi;

namespace {

std::vector<std::string> paths(const LoadOrder& o) {
    std::vector<std::string> out;
    for (const auto& e : o.files) out.push_back(e.file->path);
    return out;
}

}  // namespace

TEST_CASE("lst: nested lists are processed in place; comments and blanks skipped") {
    auto v = test::makeVfs({
        {"index_aa.lst", ";; header\r\nfirst.tgi\r\n\r\nsub.lst ; nested\r\nlast.tgi\r\n"},
        {"sub.lst", "Dir/*.tgi\n"},
        {"first.tgi", ""},
        {"last.tgi", ""},
        {"Dir/b.tgi", ""},
        {"Dir/a.tgi", ""},
    });
    Diagnostics diags;
    const LoadOrder o = buildLoadOrder(*v, diags);
    CHECK(paths(o) == std::vector<std::string>{"first.tgi", "Dir/a.tgi", "Dir/b.tgi", "last.tgi"});
    CHECK(o.lists == std::vector<std::string>{"index_aa.lst", "sub.lst"});
    CHECK(o.files[1].listPath == "sub.lst");
    CHECK(diags.empty());
}

TEST_CASE("lst: every root index*.lst runs, in sorted order") {
    auto v = test::makeVfs({{"index_b.lst", "b.tgi\n"}, {"INDEX_a.lst", "a.tgi\n"}, {"a.tgi", ""}, {"b.tgi", ""},
                            {"sub/index_c.lst", "c.tgi\n"}});
    Diagnostics diags;
    CHECK(paths(buildLoadOrder(*v, diags)) == std::vector<std::string>{"a.tgi", "b.tgi"});
}

TEST_CASE("lst: wildcard order is upper-case ordinal; '*' is not recursive") {
    auto v = test::makeVfs({{"index.lst", "U/*.tgi\n"},
                            {"U/per_car_armored_co.tgi", ""},
                            {"U/per_carrier.tgi", ""},
                            {"U/Deep/x.tgi", ""}});
    Diagnostics diags;
    CHECK(paths(buildLoadOrder(*v, diags)) ==
          std::vector<std::string>{"U/per_carrier.tgi", "U/per_car_armored_co.tgi"});
}

TEST_CASE("lst: empty wildcard is silent, missing explicit file warns, duplicates skip") {
    auto v = test::makeVfs({{"index.lst", "Nothing/*.tgi\nmissing.tgi\na.tgi\nA.TGI\n"}, {"a.tgi", ""}});
    Diagnostics diags;
    const LoadOrder o = buildLoadOrder(*v, diags);
    CHECK(paths(o) == std::vector<std::string>{"a.tgi"});
    CHECK(o.emptyWildcards.size() == 1);
    CHECK(diags.count(DiagCode::LstMissingFile) == 1);
    CHECK(diags.count(DiagCode::LstDuplicate) == 1);
    CHECK(diags.all().size() == 2);
}

TEST_CASE("lst: a list that includes itself is an error and is skipped") {
    auto v = test::makeVfs({{"index.lst", "a.lst\n"}, {"a.lst", "x.tgi\nindex.lst\n"}, {"x.tgi", ""}});
    Diagnostics diags;
    CHECK(paths(buildLoadOrder(*v, diags)) == std::vector<std::string>{"x.tgi"});
    CHECK(diags.count(DiagCode::LstRecursion) == 1);
}

TEST_CASE("lst: a later depot replaces a list wholesale (how the PER/SAU mod works)") {
    auto base = std::make_unique<vfs::MemoryDepot>("Data.rwd");
    base->add("index_aa.lst", "templates.lst\n");
    base->add("templates.lst", "Templates/t.tgi\n");
    base->add("Templates/t.tgi", "");
    auto mod = std::make_unique<vfs::MemoryDepot>("data/");
    mod->add("templates.lst", "templates/t.tgi\ntemplates/mod.tgi\n");  // different case, extra line
    mod->add("templates/mod.tgi", "");
    vfs::Vfs v;
    v.mount(std::move(base));
    v.mount(std::move(mod));
    Diagnostics diags;
    const LoadOrder o = buildLoadOrder(v, diags);
    REQUIRE(o.files.size() == 2);
    CHECK(v.depot(o.files[0].file->depot).name() == "Data.rwd");
    CHECK(v.depot(o.files[1].file->depot).name() == "data/");
    CHECK(diags.empty());
}
