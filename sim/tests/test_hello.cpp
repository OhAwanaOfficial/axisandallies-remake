#include <doctest/doctest.h>

#include "core/hello.hpp"

TEST_CASE("simHello returns a non-empty greeting") {
    CHECK(!corps::simHello().empty());
}
