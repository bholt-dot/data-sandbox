#include <doctest/doctest.h>

#include "simcore/time.hpp"

using namespace sim;

TEST_CASE("time arithmetic") {
    Time t{100};
    t += days(1);
    CHECK(t.seconds == 100 + 86400);
    CHECK((t - Time{100}) == days(1));
    CHECK(Time{5} < Time{6});
    CHECK(hours(2) + minutes(30) == seconds(9000));
}

TEST_CASE("format_duration") {
    CHECK(format_duration(seconds(42)) == "42s");
    CHECK(format_duration(minutes(3) + seconds(4)) == "3m 4s");
    CHECK(format_duration(hours(5) + minutes(1)) == "5h 1m");
    CHECK(format_duration(days(11) + hours(4) + minutes(3)) == "11d 4h 3m");
    CHECK(format_duration(seconds(-90)) == "-1m 30s");
}
