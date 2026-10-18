#include <doctest/doctest.h>

#include "tgi_fixture.hpp"
#include "vfs/path.hpp"
#include "vfs/rwd_archive.hpp"

using namespace corps;

TEST_CASE("vfs paths: separators unified, keys lower-cased") {
    CHECK(vfs::normalizePath("Templates\\Foo.TGI") == "Templates/Foo.TGI");
    CHECK(vfs::normalizePath("/a//b/") == "a/b");
    CHECK(vfs::normalizePath("./x.lst") == "x.lst");
    CHECK(vfs::pathKey("Templates\\Foo.TGI") == "templates/foo.tgi");
    CHECK(vfs::parentDir("a/b/c.tgi") == "a/b");
    CHECK(vfs::fileName("a/b/c.tgi") == "c.tgi");
}

TEST_CASE("vfs glob: '*' in the last component, case-insensitive") {
    CHECK(vfs::globMatch("*.tgi", "a.TGI"));
    CHECK(vfs::globMatch("index*.lst", "index_aa.lst"));
    CHECK(vfs::globMatch("SVars*.tgi", "svars.tgi"));
    CHECK_FALSE(vfs::globMatch("*.tgi", "a.lst"));
    CHECK(vfs::globMatch("a*b*c", "aXXbYYc"));
}

TEST_CASE("wildcard order is ordinal on the upper-cased path (NTFS), not lower-cased") {
    // '_' (0x5F) sorts after letters when upper-cased, before them when lower-cased.
    CHECK(vfs::upperOrdinalLess("x/per_carrier.tgi", "x/per_car_armored_co.tgi"));
    CHECK_FALSE(vfs::upperOrdinalLess("x/per_car_armored_co.tgi", "x/per_carrier.tgi"));
    CHECK(vfs::upperOrdinalLess("A.tgi", "b.tgi"));
}

TEST_CASE("vfs: later depot replaces the whole file; listings are the union") {
    auto d1 = std::make_unique<vfs::MemoryDepot>("first");
    d1->add("Dir/a.tgi", "one");
    d1->add("Dir/b.tgi", "b");
    d1->add("Dir/Sub/c.tgi", "c");
    auto d2 = std::make_unique<vfs::MemoryDepot>("second");
    d2->add("dir\\A.TGI", "two");
    d2->add("dir/d.tgi", "d");
    vfs::Vfs v;
    v.mount(std::move(d1));
    v.mount(std::move(d2));

    std::string out;
    REQUIRE(v.read("DIR/a.tgi", out));
    CHECK(out == "two");
    CHECK(v.find("dir/a.tgi")->depot == 1);
    CHECK(v.find("dir/a.tgi")->shadowed == 1);

    const auto matches = v.glob("Dir/*.tgi");
    REQUIRE(matches.size() == 3);  // a (once), b, d; not recursive into Sub/
    CHECK(matches[0]->key == "dir/a.tgi");
    CHECK(matches[1]->key == "dir/b.tgi");
    CHECK(matches[2]->key == "dir/d.tgi");
    CHECK(v.glob("Nothing/*.tgi").empty());
}

TEST_CASE("RwdArchive reads a synthetic TGCK image") {
    const std::string image = vfs::buildRwdImage({{"Templates\\t.tgi", "[Template T]"},
                                                  {"Units/u.tgi", "[Thing]"},
                                                  {"empty.txt", ""}});
    std::string error;
    auto archive = vfs::RwdArchive::openMemory(image, "test.rwd", error);
    REQUIRE(archive != nullptr);
    CHECK(archive->toc().size() == 3);
    CHECK(archive->toc()[0].name == "Templates\\t.tgi");
    REQUIRE(archive->entries().size() == 3);
    CHECK(archive->entries()[1].key == "templates/t.tgi");  // sorted by key

    vfs::Vfs v;
    v.mount(std::move(archive));
    std::string out;
    REQUIRE(v.read("templates/T.tgi", out));
    CHECK(out == "[Template T]");
    REQUIRE(v.read("Units/u.tgi", out));
    CHECK(out == "[Thing]");
    REQUIRE(v.read("empty.txt", out));
    CHECK(out.empty());
}

TEST_CASE("RwdArchive: fileType 0 does not end the TOC; an invalid name does") {
    std::string image = vfs::buildRwdImage({{"a.tgi", "A"}, {"b.tgi", "B"}, {"c.tgi", "C"}});
    // TOC entries start right after the 30-byte header and the 3 data bytes.
    const std::size_t toc = 30 + 3;
    image[toc] = image[toc + 1] = image[toc + 2] = image[toc + 3] = '\0';  // first entry's fileType = 0
    const std::size_t entry = 4 + 2 + 2 * 5 + 8 + 8 + 4;
    image[toc + 2 * entry + 6] = '*';  // third entry's name now has an invalid character
    std::string error;
    auto archive = vfs::RwdArchive::openMemory(image, "t.rwd", error);
    REQUIRE(archive != nullptr);
    CHECK(archive->toc().size() == 2);
    CHECK(archive->toc()[0].fileType == 0);
}

TEST_CASE("RwdArchive keeps a short last entry when the TOC runs right up to the trailer") {
    std::string image = vfs::buildRwdImage({{"long_name.tgi", "A"}, {"z", "B"}});
    // Drop the builder's 64 zero bytes of padding so the 28-byte entry for "z" ends exactly at EOF-288.
    image.erase(image.size() - 288 - 64, 64);
    std::string error;
    auto archive = vfs::RwdArchive::openMemory(image, "t.rwd", error);
    REQUIRE(archive != nullptr);
    REQUIRE(archive->toc().size() == 2);
    CHECK(archive->toc()[1].name == "z");
}

TEST_CASE("RwdArchive rejects a file without the TGCK signature") {
    std::string error;
    CHECK(vfs::RwdArchive::openMemory(std::string(400, 'x'), "bad.rwd", error) == nullptr);
    CHECK_FALSE(error.empty());
}
