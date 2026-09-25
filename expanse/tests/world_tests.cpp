#include <doctest/doctest.h>

#include "expanse/calendar.hpp"
#include "expanse/content.hpp"
#include "expanse/scenario.hpp"
#include "expanse/simulation.hpp"
#include "expanse/units.hpp"
#include "expanse/world.hpp"

#include <memory>

using namespace expanse;

namespace {

const Content& game_content() {
    static const std::unique_ptr<Content> content = [] {
        sim::Diagnostics diags;
        auto c = Content::load(EXPANSE_DATA_DIR, diags);
        if (!c) {
            FAIL(diags.to_string());
        }
        return c;
    }();
    return *content;
}

std::unique_ptr<Content> load_inline(std::string text, sim::Diagnostics& diags) {
    const sim::DataSource src{"inline.toml", std::move(text)};
    return Content::load(std::span{&src, 1}, diags);
}

} // namespace

TEST_CASE("calendar round trips and known dates") {
    using namespace calendar;
    CHECK(format_date(sim::Time{0}) == "2350-01-01");
    CHECK(format_datetime(sim::Time{0} + sim::hours(6) + sim::minutes(30)) == "2350-01-01 06:30");
    CHECK(format_date(to_time({2350, 3, 14})) == "2350-03-14");
    CHECK(format_date(to_time({2352, 2, 29})) == "2352-02-29");
    CHECK(format_datetime(j2000) == "2000-01-01 12:00");
    CHECK(format_date(sim::Time{-1}) == "2349-12-31");
    CHECK(parse_date("2350-02-29") == std::nullopt); // 2350 is not a leap year
    CHECK(parse_date("2352-02-29").has_value());
    CHECK(parse_date("2350-3-14") == std::nullopt);
    for (std::int64_t d = -200000; d < 200000; d += 997) {
        const sim::Time t{d * 86400};
        CHECK(to_time(*parse_date(format_date(t))) == t);
    }
}

TEST_CASE("shipped game data loads and builds an orbit system") {
    const Content& c = game_content();
    CHECK(c.table<StationDef>().size() >= 3);
    const auto ceres_station = c.find<StationDef>("ceres_station");
    REQUIRE(ceres_station);
    const auto ceres = c.find<BodyDef>("ceres");
    const sim::Time t = calendar::to_time({2350, 3, 14});
    // A surface port sits at its body.
    const Vec3 station_pos = c.orbits().world_position(c.orbit_of(ceres_station), t);
    const Vec3 body_pos = c.orbits().world_position(c.orbit_of(ceres), t);
    CHECK(distance(station_pos, body_pos) == doctest::Approx(0.0));
    // Ceres stays in the main belt.
    const double r_au = units::to_au(length(body_pos));
    CHECK(r_au > 2.5);
    CHECK(r_au < 3.0);
    CHECK(c.fingerprint() != 0);
}

TEST_CASE("content validation reports cycles and missing gm") {
    sim::Diagnostics diags;
    auto c = load_inline(R"(
[body.sun]
name = "Sun"
kind = "star"
radius_km = 1
[body.rock]
name = "Rock"
kind = "asteroid"
parent = "sun"
radius_km = 1
orbit = { a_au = 1.0 }
)", diags);
    CHECK_FALSE(c);
    CHECK(diags.contains("no 'gm_km3_s2'"));

    sim::Diagnostics diags2;
    auto c2 = load_inline(R"(
[body.a]
name = "A"
kind = "moon"
parent = "b"
gm_km3_s2 = 1
radius_km = 1
orbit = { a_km = 10 }
[body.b]
name = "B"
kind = "moon"
parent = "a"
gm_km3_s2 = 1
radius_km = 1
orbit = { a_km = 10 }
)", diags2);
    CHECK_FALSE(c2);
    CHECK(diags2.contains("cycle"));

    sim::Diagnostics diags3;
    auto c3 = load_inline(R"(
[body.sun]
name = "Sun"
kind = "star"
radius_km = 1
gm_km3_s2 = 1
[body.rock]
name = "Rock"
kind = "asteroid"
parent = "sun"
radius_km = 1
orbit = { a_au = 1.0, a_km = 5.0 }
)", diags3);
    CHECK_FALSE(c3);
    CHECK(diags3.contains("exactly one of"));
}

TEST_CASE("body display colours are optional hex strings") {
    CHECK(parse_hex_color("#c1440e") == 0xc1440eU);
    CHECK(parse_hex_color("#FFe7B0") == 0xffe7b0U);
    CHECK_FALSE(parse_hex_color("c1440e"));
    CHECK_FALSE(parse_hex_color("#c1440"));
    CHECK_FALSE(parse_hex_color("#c1440g"));

    const Content& c = game_content();
    const auto& bodies = c.table<BodyDef>();
    CHECK(bodies[c.find<BodyDef>("mars")].color == "#c1440e");
    CHECK_FALSE(bodies[c.find<BodyDef>("eros")].color); // optional: the viewer falls back

    sim::Diagnostics diags;
    auto bad = load_inline(R"(
[body.sun]
name = "Sun"
kind = "star"
radius_km = 1
color = "yellow"
)", diags);
    CHECK_FALSE(bad);
    CHECK(diags.contains("must be a colour"));
}

TEST_CASE("secondhand scenario starts dirt poor") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 42);
    CHECK(calendar::format_date(w.now()) == "2350-03-14");
    const Company& player = w.companies.at(w.player);
    CHECK(player.is_player);
    CHECK(player.cash < 5000);
    REQUIRE(w.ships.size() == 1);
    auto [ship_id, ship] = *w.ships.begin();
    (void)ship_id;
    CHECK(ship.owner == w.player);
    CHECK(std::holds_alternative<Docked>(ship.location));
    const ShipClassDef& cls = c.table<ShipClassDef>()[ship.ship_class];
    CHECK(ship.reaction_mass_t < cls.reaction_mass_capacity_t);
    CHECK(ship.hull_condition < 1.0);
    REQUIRE(w.loans.size() == 1);
    CHECK(w.scheduler.pending_event_count() >= 1); // first loan payment
    CHECK(w.stations.size() == c.table<StationDef>().size());
    CHECK_THROWS_AS(new_game(c, "no_such_scenario", 1), std::invalid_argument);
}

TEST_CASE("world save load hash round trip") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 7);
    (void)w.rng("crew").next_u64();
    const std::uint64_t h = world_hash(w);
    const auto bytes = save_world(w);
    World loaded = load_world(bytes, c);
    CHECK(world_hash(loaded) == h);
    CHECK(save_world(loaded) == bytes);
    // Streams continue identically after load.
    CHECK(loaded.rng("crew").next_u64() == w.rng("crew").next_u64());
}

TEST_CASE("new game is reproducible by seed") {
    const Content& c = game_content();
    CHECK(world_hash(new_game(c, "secondhand", 1)) == world_hash(new_game(c, "secondhand", 1)));
    CHECK(world_hash(new_game(c, "secondhand", 1)) != world_hash(new_game(c, "secondhand", 2)));
}

TEST_CASE("load rejects saves made with other content") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 7);
    w.content_fingerprint ^= 1;
    const auto bytes = save_world(w);
    CHECK_THROWS_AS(load_world(bytes, c), sim::SerializeError);
}

TEST_CASE("docked ship position follows its station") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 7);
    auto [id, ship] = *w.ships.begin();
    (void)id;
    const auto station = std::get<Docked>(ship.location).station;
    const Vec3 p = ship_position(c, w, ship);
    CHECK(distance(p, c.orbits().world_position(c.orbit_of(station), w.now())) == doctest::Approx(0.0));
}

TEST_CASE("advance runs a year of ticks and stops on player events") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 3);
    const sim::Time start = w.now();
    // The first loan payment is a player event: an interruptible advance stops there.
    const AdvanceReport r = advance_to(c, w, start + sim::days(30), true);
    CHECK(r.stopped_early);
    CHECK(w.now() == start + sim::days(7));
    // An uninterruptible advance runs through.
    const AdvanceReport r2 = advance_to(c, w, start + sim::days(365), false);
    CHECK_FALSE(r2.stopped_early);
    CHECK(w.now() == start + sim::days(365));
    CHECK(r2.occurrences >= 358); // daily ticks alone
}

TEST_CASE("advancing is reproducible") {
    const Content& c = game_content();
    World a = new_game(c, "secondhand", 11);
    World b = new_game(c, "secondhand", 11);
    advance_to(c, a, a.now() + sim::days(200), false);
    advance_to(c, b, b.now() + sim::days(50), false);
    World b2 = load_world(save_world(b), c);
    advance_to(c, b2, b2.now() + sim::days(150), false);
    CHECK(world_hash(a) == world_hash(b2));
}
