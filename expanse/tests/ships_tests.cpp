#include <doctest/doctest.h>

#include "expanse/calendar.hpp"
#include "expanse/content.hpp"
#include "expanse/scenario.hpp"
#include "expanse/ships.hpp"
#include "expanse/simulation.hpp"
#include "expanse/units.hpp"
#include "expanse/world.hpp"

#include <cmath>
#include <limits>
#include <memory>
#include <string>

using namespace expanse;
namespace u = expanse::units;

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

StationId station(const char* key) {
    const StationId id = game_content().find<StationDef>(key);
    REQUIRE(id);
    return id;
}

ShipId player_ship(const World& w) { return w.ships.handle_at(0); }

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Runs until the ship docks, stopping on player events like the shell's `advance until event`.
sim::Time advance_until_docked(const Content& c, World& w, ShipId ship) {
    for (int i = 0; i < 100 && !std::holds_alternative<Docked>(w.ships.at(ship).location); ++i) {
        advance_to(c, w, w.now() + sim::days(365), true);
    }
    return w.now();
}

} // namespace

TEST_CASE("plot preview matches transit plot_transit") {
    const Content& c = game_content();
    const World w = new_game(c, "secondhand", 1);
    const ShipId id = player_ship(w);
    const Ship& ship = w.ships.at(id);
    const ShipClassDef& cls = c.table<ShipClassDef>()[ship.ship_class];
    const StationId ceres = station("ceres_station");
    const StationId vesta = station("vesta_dock");

    const CoursePreview p = plot_course(c, w, id, vesta, {0.3});
    REQUIRE(p.feasible());
    CHECK(p.reason.empty());
    CHECK(p.origin == ceres);
    CHECK(p.destination == vesta);
    CHECK_FALSE(p.accel_limited);

    transit::PlotRequest r;
    r.origin = transit::Origin::at_body(c.orbit_of(ceres));
    r.destination = c.orbit_of(vesta);
    r.departure = w.now();
    r.accel = u::gees(0.3);
    r.ship.dry_mass = u::tonnes(cls.dry_mass_t);
    r.ship.cargo_mass = 0.0;
    r.ship.reaction_mass = u::tonnes(ship.reaction_mass_t);
    r.ship.exhaust_velocity = u::km_per_s(cls.exhaust_velocity_km_s);
    const transit::Plot t = transit::plot_transit(c.orbits(), r);
    REQUIRE(t.feasible());

    CHECK(p.departure == w.now());
    CHECK(p.wait == sim::Duration{});
    CHECK(p.arrival == t.arrival);
    CHECK(p.duration == t.duration);
    CHECK(p.flip == t.flip);
    CHECK(p.distance_au == doctest::Approx(u::to_au(t.distance)));
    CHECK(p.delta_v_km_s == doctest::Approx(t.delta_v / 1000.0));
    CHECK(p.match_delta_v_km_s == doctest::Approx(t.match_delta_v / 1000.0));
    CHECK(p.peak_speed_km_s == doctest::Approx(t.peak_speed / 1000.0));
    CHECK(p.accel_g == doctest::Approx(0.3));
    CHECK(p.reaction_mass_needed_t == doctest::Approx(t.reaction_mass_used / 1000.0));
    CHECK(p.reaction_mass_aboard_t == doctest::Approx(ship.reaction_mass_t));
    CHECK(p.reaction_mass_after_t == doctest::Approx(t.reaction_mass_left / 1000.0));
    CHECK(p.tank_capacity_t == doctest::Approx(cls.reaction_mass_capacity_t));
    CHECK(p.tank_used_pct ==
          doctest::Approx(100.0 * p.reaction_mass_needed_t / cls.reaction_mass_capacity_t));
    CHECK(p.tank_after_pct ==
          doctest::Approx(100.0 * p.reaction_mass_after_t / cls.reaction_mass_capacity_t));
    CHECK(p.hull_wear_pct == doctest::Approx(100.0 * hull_wear(t.profile)));
    CHECK(p.hull_wear_pct > 0.0);
    CHECK(p.light_lag == light_lag(t.distance));
    CHECK(p.plot.start == t.start);
    CHECK(p.plot.end == t.end);

    // A dv cap trades time for reaction mass.
    const CoursePreview slow = plot_course(c, w, id, vesta, {0.3, 0.5 * p.delta_v_km_s});
    REQUIRE(slow.feasible());
    CHECK(slow.delta_v_km_s <= 0.5 * p.delta_v_km_s + 1e-6);
    CHECK(slow.arrival > p.arrival);
    CHECK(slow.reaction_mass_needed_t < p.reaction_mass_needed_t);

    // Plotting has no side effects.
    CHECK(w.messages.empty());
}

TEST_CASE("drive limits acceleration at current mass") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ShipId id = player_ship(w);
    const Ship& ship = w.ships.at(id);
    const ShipClassDef& cls = c.table<ShipClassDef>()[ship.ship_class];
    const double wet = cls.dry_mass_t + ship.reaction_mass_t;
    CHECK(effective_max_accel_g(c, ship) == doctest::Approx(cls.max_accel_g * cls.dry_mass_t / wet));

    const CoursePreview p = plot_course(c, w, id, station("vesta_dock"), {10.0});
    CHECK(p.accel_limited);
    CHECK(p.accel_g == doctest::Approx(effective_max_accel_g(c, ship)));
    CHECK(p.plot.profile.accel == doctest::Approx(u::gees(p.accel_g)));
}

TEST_CASE("depart deducts reaction mass and schedules arrival") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ShipId id = player_ship(w);
    const StationId vesta = station("vesta_dock");
    const double before = w.ships.at(id).reaction_mass_t;
    const std::size_t pending = w.scheduler.pending_event_count();

    const DepartResult r = depart(c, w, id, vesta, {0.3});
    REQUIRE(r.ok());
    const Ship& ship = w.ships.at(id);
    CHECK(ship.reaction_mass_t == doctest::Approx(before - r.preview.reaction_mass_needed_t));
    CHECK(ship.reaction_mass_t > 0.0);

    const auto* uw = std::get_if<Underway>(&ship.location);
    REQUIRE(uw != nullptr);
    CHECK(uw->origin == station("ceres_station"));
    CHECK(uw->destination == vesta);
    CHECK(uw->departure == w.now());
    CHECK(uw->arrival == r.preview.arrival);
    CHECK(uw->start == r.preview.plot.start);
    CHECK(uw->end == r.preview.plot.end);
    CHECK(uw->profile.total_time() == doctest::Approx((uw->arrival - uw->departure).to_seconds_f()));

    CHECK(w.scheduler.pending_event_count() == pending + 1);
    REQUIRE(w.scheduler.is_pending(uw->arrival_event));
    bool found = false;
    for (const auto& e : w.scheduler.pending_events()) {
        if (e.id == uw->arrival_event) {
            found = true;
            CHECK(e.time == uw->arrival);
            CHECK(e.priority == priority_arrivals);
            CHECK((e.flags & player_event) != 0);
            REQUIRE(std::holds_alternative<ShipArrives>(e.payload));
            CHECK(std::get<ShipArrives>(e.payload).ship == id);
        }
    }
    CHECK(found);

    REQUIRE(w.messages.size() == 1);
    CHECK(w.messages[0].kind == MessageKind::ship);
    CHECK(contains(w.messages[0].text, "Dustkicker departed Ceres Station for Vesta Dock"));

    // Mid-flight the ship is between the endpoints, and the status says so.
    advance_to(c, w, w.now() + sim::days(2), false);
    const ShipStatus st = ship_status(c, w, id);
    CHECK(st.valid);
    CHECK_FALSE(st.docked);
    REQUIRE(st.eta.has_value());
    CHECK(*st.eta == uw->arrival);
    CHECK(st.time_remaining == uw->arrival - w.now());
    CHECK(st.progress > 0.0);
    CHECK(st.progress < 1.0);
    CHECK(st.speed_km_s > 0.0);
    CHECK(contains(st.location, "en route Ceres Station -> Vesta Dock"));
    CHECK(st.reaction_mass_t == doctest::Approx(ship.reaction_mass_t));
}

TEST_CASE("arrival docks the ship at its destination and wears the hull") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ShipId id = player_ship(w);
    const StationId vesta = station("vesta_dock");
    const double hull = w.ships.at(id).hull_condition;
    const DepartResult r = depart(c, w, id, vesta, {0.3});
    REQUIRE(r.ok());
    const transit::BurnProfile profile = std::get<Underway>(w.ships.at(id).location).profile;

    // Half-open advance: at exactly the arrival instant the event has not fired yet.
    advance_to(c, w, r.preview.arrival, false);
    CHECK(std::holds_alternative<Underway>(w.ships.at(id).location));
    advance_to(c, w, r.preview.arrival + sim::seconds(1), false);

    const Ship& ship = w.ships.at(id);
    REQUIRE(std::holds_alternative<Docked>(ship.location));
    CHECK(std::get<Docked>(ship.location).station == vesta);
    CHECK(ship_position(c, w, ship) == c.orbits().world_position(c.orbit_of(vesta), w.now()));
    // The docked position at arrival is where the transit ended.
    CHECK(distance(ship_position(c, ship, r.preview.arrival), r.preview.plot.end) < 1e-3);
    CHECK(ship.hull_condition == doctest::Approx(hull - hull_wear(profile)));
    CHECK(ship.hull_condition < hull);

    bool docked_msg = false;
    for (const Message& m : w.messages) {
        docked_msg = docked_msg || contains(m.text, "Dustkicker docked at Vesta Dock after " +
                                                        format_trip(r.preview.duration) + ".");
    }
    CHECK(docked_msg);
    const ShipStatus st = ship_status(c, w, id);
    CHECK(st.docked);
    CHECK(st.location == "docked at Vesta Dock");
    CHECK(st.hull_pct == doctest::Approx(100.0 * ship.hull_condition));
}

TEST_CASE("advance until event stops at the arrival") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ShipId id = player_ship(w);
    const DepartResult r = depart(c, w, id, station("pallas_refinery"), {0.1});
    REQUIRE(r.ok());
    REQUIRE(r.preview.duration > sim::days(7)); // the loan payment interrupts first
    const AdvanceReport first = advance_to(c, w, w.now() + sim::days(365), true);
    CHECK(first.stopped_early);
    CHECK(w.now() < r.preview.arrival);
    CHECK(advance_until_docked(c, w, id) == r.preview.arrival);
}

TEST_CASE("hull warning when wear crosses the warning level") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ShipId id = player_ship(w);
    w.ships.at(id).hull_condition = hull_warning_level + 0.001;
    REQUIRE(depart(c, w, id, station("vesta_dock"), {0.3}).ok());
    advance_until_docked(c, w, id);
    CHECK(w.ships.at(id).hull_condition < hull_warning_level);
    bool warned = false;
    for (const Message& m : w.messages) {
        warned = warned || (m.kind == MessageKind::warning && m.urgent && contains(m.text, "hull"));
    }
    CHECK(warned);
}

TEST_CASE("depart failures leave the world unchanged") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ShipId id = player_ship(w);
    const StationId vesta = station("vesta_dock");
    const std::uint64_t h = world_hash(w);

    auto expect = [&](ShipId ship, StationId dest, CourseOptions opts, CourseStatus status) {
        CAPTURE(describe(status));
        const DepartResult r = depart(c, w, ship, dest, opts);
        CHECK_FALSE(r.ok());
        CHECK(r.status == status);
        CHECK(r.preview.status == status);
        CHECK_FALSE(r.reason.empty());
        CHECK(world_hash(w) == h);
    };

    expect(ShipId{}, vesta, {0.3}, CourseStatus::unknown_ship);
    expect(ShipId{id.index, id.generation + 1}, vesta, {0.3}, CourseStatus::unknown_ship);
    expect(id, StationId{}, {0.3}, CourseStatus::unknown_destination);
    expect(id, station("ceres_station"), {0.3}, CourseStatus::already_there);
    expect(id, vesta, {0.0}, CourseStatus::invalid_acceleration);
    expect(id, vesta, {-1.0}, CourseStatus::invalid_acceleration);
    expect(id, vesta, {std::numeric_limits<double>::quiet_NaN()}, CourseStatus::invalid_acceleration);
    expect(id, vesta, {0.3, 0.0}, CourseStatus::invalid_delta_v_budget);
    expect(id, vesta, {0.3, std::numeric_limits<double>::quiet_NaN()},
           CourseStatus::invalid_delta_v_budget);
    // A budget below the velocity match can never arrive.
    expect(id, vesta, {0.3, 0.001}, CourseStatus::no_intercept);

    SUBCASE("insufficient reaction mass") {
        w.ships.at(id).reaction_mass_t = 2.0;
        const std::uint64_t h2 = world_hash(w);
        const DepartResult r = depart(c, w, id, vesta, {0.3});
        CHECK(r.status == CourseStatus::insufficient_reaction_mass);
        CHECK(contains(r.reason, "reaction mass"));
        // The numbers are still there so the shell can show the shortfall.
        CHECK(r.preview.reaction_mass_needed_t > 2.0);
        CHECK(r.preview.reaction_mass_after_t < 0.0);
        CHECK(r.preview.arrival > w.now());
        CHECK(world_hash(w) == h2);
    }
    SUBCASE("overloaded") {
        w.ships.at(id).cargo.push_back(CargoLot{c.find<CommodityDef>("ore"), 700.0, 0});
        const DepartResult r = depart(c, w, id, vesta, {0.3});
        CHECK(r.status == CourseStatus::overloaded);
        CHECK(contains(r.reason, "700.0 t"));
    }
    SUBCASE("already underway") {
        REQUIRE(depart(c, w, id, vesta, {0.3}).ok());
        const std::uint64_t h2 = world_hash(w);
        const DepartResult r = depart(c, w, id, station("tycho_station"), {0.3});
        CHECK(r.status == CourseStatus::underway);
        CHECK(contains(r.reason, "already underway to Vesta Dock"));
        CHECK(world_hash(w) == h2);
        CHECK(plot_course(c, w, id, vesta).status == CourseStatus::underway);
    }
}

TEST_CASE("stale arrival events are ignored") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ShipId id = player_ship(w);
    // Unknown ship.
    dispatch(c, w, w.scheduler, Event{ShipArrives{ShipId{}}});
    // Docked ship: nothing to arrive from.
    const std::uint64_t h = world_hash(w);
    dispatch(c, w, w.scheduler, Event{ShipArrives{id}});
    CHECK(world_hash(w) == h);
    // Underway ship, but not its arrival time.
    REQUIRE(depart(c, w, id, station("vesta_dock"), {0.3}).ok());
    const std::uint64_t h2 = world_hash(w);
    dispatch(c, w, w.scheduler, Event{ShipArrives{id}});
    CHECK(world_hash(w) == h2);
    CHECK(std::holds_alternative<Underway>(w.ships.at(id).location));
    // Ship scrapped while underway: its arrival fires harmlessly.
    w.ships.erase(id);
    advance_to(c, w, w.now() + sim::days(30), false);
    CHECK(w.ships.size() == 0);
}

TEST_CASE("cheapest course within a deadline") {
    const Content& c = game_content();
    const World w = new_game(c, "secondhand", 1);
    const ShipId id = player_ship(w);
    const StationId tycho = station("tycho_station");
    const CoursePreview fastest = plot_course(c, w, id, tycho, {0.3});
    REQUIRE(fastest.feasible());

    const CoursePreview p = plot_cheapest_within(c, w, id, tycho, 0.3, sim::days(30));
    REQUIRE(p.feasible());
    CHECK(p.duration <= sim::days(30));
    CHECK(p.duration > sim::days(29)); // uses the time it is given
    CHECK(p.delta_v_km_s < 0.5 * fastest.delta_v_km_s);
    CHECK(p.reaction_mass_needed_t < fastest.reaction_mass_needed_t);
    // A slightly tighter budget would miss the deadline.
    const CoursePreview tighter = plot_course(c, w, id, tycho, {0.3, 0.99 * p.delta_v_km_s});
    CHECK(tighter.duration > sim::days(30));

    const CoursePreview longer = plot_cheapest_within(c, w, id, tycho, 0.3, sim::days(60));
    REQUIRE(longer.feasible());
    CHECK(longer.delta_v_km_s < p.delta_v_km_s);

    const CoursePreview impossible = plot_cheapest_within(c, w, id, tycho, 0.3, sim::days(1));
    CHECK(impossible.status == CourseStatus::too_slow);
    CHECK(contains(impossible.reason, "Even at full burn"));
    CHECK(impossible.duration == fastest.duration);
}

TEST_CASE("best departure within a window") {
    const Content& c = game_content();
    const World w = new_game(c, "secondhand", 1);
    const ShipId id = player_ship(w);
    const StationId pallas = station("pallas_refinery");
    const CoursePreview now = plot_course(c, w, id, pallas, {0.3});
    REQUIRE(now.feasible());
    const CoursePreview cheap = plot_best_departure(c, w, id, pallas, {0.3}, sim::days(60),
                                                    sim::days(1), transit::Objective::least_delta_v);
    REQUIRE(cheap.feasible());
    CHECK(cheap.delta_v_km_s <= now.delta_v_km_s);
    CHECK(cheap.wait == cheap.departure - w.now());
    CHECK(cheap.wait >= sim::Duration{});
    CHECK(cheap.wait <= sim::days(60));
    CHECK(cheap.duration == cheap.arrival - cheap.departure);
}

TEST_CASE("all destinations from the start") {
    const Content& c = game_content();
    const World w = new_game(c, "secondhand", 1);
    const auto list = plot_all_destinations(c, w, player_ship(w), {0.1});
    CHECK(list.size() == c.table<StationDef>().size() - 1);
    for (const CoursePreview& p : list) {
        CHECK(p.destination != station("ceres_station"));
        CHECK(p.feasible()); // the Secondhand start can reach every station at 0.1 g
    }
}

TEST_CASE("separation and light lag") {
    const Content& c = game_content();
    const World w = new_game(c, "secondhand", 1);
    CHECK(light_lag(u::au_m) == sim::seconds(499));
    CHECK(light_lag(0.0) == sim::Duration{});
    const StationId ceres = station("ceres_station");
    const StationId vesta = station("vesta_dock");
    const Separation self = separation(c, ceres, ceres, w.now());
    CHECK(self.distance_m == 0.0);
    const Separation s = separation(c, ceres, vesta, w.now());
    CHECK(s.distance_m == doctest::Approx(distance(c.orbits().world_position(c.orbit_of(ceres), w.now()),
                                                   c.orbits().world_position(c.orbit_of(vesta), w.now()))));
    CHECK(s.distance_au == doctest::Approx(u::to_au(s.distance_m)));
    CHECK(s.light_lag == light_lag(s.distance_m));
    CHECK(s.light_lag > sim::minutes(1));
}

TEST_CASE("ship status docked and unknown") {
    const Content& c = game_content();
    const World w = new_game(c, "secondhand", 1);
    const ShipId id = player_ship(w);
    const ShipStatus st = ship_status(c, w, id);
    CHECK(st.valid);
    CHECK(st.docked);
    CHECK(st.name == "Dustkicker");
    CHECK(st.location == "docked at Ceres Station");
    CHECK_FALSE(st.eta.has_value());
    CHECK(st.reaction_mass_pct == doctest::Approx(35.0));
    CHECK(st.cargo_t == 0.0);
    CHECK(st.cargo_capacity_t == doctest::Approx(600.0));
    CHECK(st.hull_pct == doctest::Approx(62.0));
    CHECK(st.delta_v_available_km_s > 0.0);
    CHECK_FALSE(ship_status(c, w, ShipId{}).valid);
}

TEST_CASE("trip durations format compactly") {
    CHECK(format_trip(sim::days(6) + sim::hours(4) + sim::minutes(59)) == "6d 4h");
    CHECK(format_trip(sim::hours(5) + sim::minutes(12)) == "5h 12m");
    CHECK(format_trip(sim::minutes(40)) == "40m");
    CHECK(format_trip(sim::seconds(-5)) == "0m");
}

TEST_CASE("transits are deterministic and survive save load mid flight") {
    const Content& c = game_content();
    auto run_to = [&](World& w, sim::Time t) { advance_to(c, w, t, false); };

    World a = new_game(c, "secondhand", 5);
    World b = new_game(c, "secondhand", 5);
    const ShipId id = player_ship(a);
    const DepartResult ra = depart(c, a, id, station("tycho_station"), {0.1});
    const DepartResult rb = depart(c, b, id, station("tycho_station"), {0.1});
    REQUIRE(ra.ok());
    REQUIRE(rb.ok());
    CHECK(world_hash(a) == world_hash(b));

    const sim::Time mid = ra.preview.departure + sim::seconds(ra.preview.duration.seconds / 2 + 12345);
    run_to(a, mid);
    run_to(b, mid);
    World loaded = load_world(save_world(b), c);
    CHECK(world_hash(loaded) == world_hash(a));
    const Vec3 pa = ship_position(c, a, a.ships.at(id));
    CHECK(ship_position(c, loaded, loaded.ships.at(id)) == pa);
    // Mid-flight the ship is off both endpoints.
    CHECK(distance(pa, ra.preview.plot.start) > 1e9);
    CHECK(distance(pa, ra.preview.plot.end) > 1e9);

    const sim::Time later = ra.preview.arrival + sim::days(10);
    run_to(a, later);
    run_to(loaded, later);
    CHECK(world_hash(loaded) == world_hash(a));
    CHECK(std::holds_alternative<Docked>(loaded.ships.at(id).location));
}

TEST_CASE("courses through the Sun are refused") {
    // Two stations on opposite sides of the Sun: the straight course would pass through it.
    sim::Diagnostics diags;
    const sim::DataSource src{"inline.toml", R"(
[body.sun]
name = "Sol"
kind = "star"
gm_km3_s2 = 1.32712440018e11
radius_km = 695700

[station.east]
name = "East"
body = "sun"
orbit = { a_au = 1.0, mean_anomaly_deg = 0.0 }
faction = "belt"

[station.west]
name = "West"
body = "sun"
orbit = { a_au = 1.0, mean_anomaly_deg = 180.0 }
faction = "belt"

[ship_class.tug]
name = "Tug"
dry_mass_t = 100
cargo_capacity_t = 10
reaction_mass_capacity_t = 100
exhaust_velocity_km_s = 10000
max_accel_g = 2
crew_berths = 1
price = 1

[scenario.test]
name = "Test"
description = "test"
start_date = "2350-01-01"
start_station = "east"
ship_class = "tug"
ship_name = "Tug"
company_name = "Tug Co"
cash = 0
)"};
    const auto content = Content::load(std::span{&src, 1}, diags);
    REQUIRE_MESSAGE(content, diags.to_string());
    const Content& c = *content;
    World w = new_game(c, "test", 1);
    const ShipId id = player_ship(w);
    const StationId west = c.find<StationDef>("west");
    const DepartResult r = depart(c, w, id, west, {1.0});
    CHECK(r.status == CourseStatus::path_obstructed);
    CHECK(contains(r.reason, "from the Sun"));
    CHECK(r.preview.plot.closest_approach < u::au(sun_keep_out_au));
    CHECK(std::holds_alternative<Docked>(w.ships.at(id).location));
}
