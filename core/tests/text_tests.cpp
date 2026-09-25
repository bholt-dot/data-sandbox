#include <doctest/doctest.h>

#include "simcore/text.hpp"

using sim::reflow;

TEST_CASE("reflow joins hard wrapped lines into paragraphs") {
    CHECK(reflow("You bought the ship\nfrom a broker.\n") == "You bought the ship from a broker.");
    CHECK(reflow("  one\n  two  \n\n\nthree\nfour") == "one two\n\nthree four");
    CHECK(reflow("") == "");
    CHECK(reflow("\n\n  \n") == "");
    CHECK(reflow("single") == "single");
}
