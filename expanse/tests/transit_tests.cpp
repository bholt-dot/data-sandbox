#include <doctest/doctest.h>

#include "expanse/orbit.hpp"
#include "expanse/transit.hpp"
#include "expanse/units.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

using namespace expanse;
using namespace expanse::transit;
using orbit::BodyId;
using orbit::Elements;
using orbit::OrbitSystem;
namespace u = expanse::units;

namespace {

double rel_err(double got, double want) { return std::abs(got - want) / std::abs(want); }

Elements elements(double a_au, double e, double i_deg, double node_deg, double peri_deg,
                  double m0_deg, double mu) {
    Elements el;
    el.semi_major_axis = u::au(a_au);
    el.eccentricity = e;
    el.inclination = u::deg(i_deg);
    el.lon_ascending_node = u::deg(node_deg);
    el.arg_periapsis = u::deg(peri_deg);
    el.mean_anomaly_at_epoch = u::deg(m0_deg);
    el.mu = mu;
    return el;
}

// Inner solar system at J2000 (Standish Table 1 for Earth-Moon barycentre and Mars; Ceres from
// the JPL SBDB osculating elements, rounded), plus a Tycho-like station in the inner belt.
struct Scenario {
    OrbitSystem sys;
    BodyId sun, earth, mars, ceres, tycho;

    Scenario() {
        sun = sys.add_fixed();
        earth = sys.add_orbiting(
            sun, elements(1.00000261, 0.01671123, -0.00001531, 0.0, 102.93768193,
                          100.46457166 - 102.93768193, u::mu_sun));
        mars = sys.add_orbiting(
            sun, elements(1.52371034, 0.09339410, 1.84969142, 49.55953891,
                          -23.94362959 - 49.55953891, -4.55343205 + 23.94362959, u::mu_sun));
        ceres = sys.add_orbiting(sun, elements(2.7675, 0.0758, 10.59, 80.31, 73.60, 6.0, u::mu_sun));
        tycho = sys.add_orbiting(sun, elements(2.30, 0.05, 4.0, 20.0, 40.0, 200.0, u::mu_sun));
    }
};

// A modest belter hauler: tank holds half the dry+cargo mass.
MassBudget hauler() {
    MassBudget m;
    m.dry_mass = u::tonnes(1500.0);
    m.cargo_mass = u::tonnes(2000.0);
    m.reaction_mass = u::tonnes(1750.0);
    return m;
}

PlotRequest request(BodyId from, BodyId to, sim::Time t0, double gees) {
    PlotRequest r;
    r.origin = Origin::at_body(from);
    r.destination = to;
    r.departure = t0;
    r.accel = u::gees(gees);
    r.ship = hauler();
    r.ship.reaction_mass = u::tonnes(1.0e6); // effectively unlimited unless a test says otherwise
    return r;
}

// The plotted arrival must be the earliest whole second at which the destination is reachable:
// the ship can make it by `arrival`, but not to where the destination is one second earlier.
void check_intercept(const OrbitSystem& sys, const PlotRequest& r, const Plot& p) {
    REQUIRE(p.status == PlotStatus::ok);
    CHECK(p.evaluations <= intercept_max_evaluations);
    const Vec3 dest = sys.world_position(r.destination, p.arrival);
    const Vec3 ship = ship_position(p, p.arrival);
    CHECK(distance(ship, dest) <= 1e-9 * p.distance + 1.0);
    CHECK(p.profile.total_time() <= p.duration.to_seconds_f() + 1e-6);
    CHECK(brachistochrone(p.distance, r.accel).total_time() <= p.duration.to_seconds_f() + 1e-6);
    const sim::Time before = p.arrival - sim::seconds(1);
    const double d_before = distance(sys.world_position(r.destination, before), p.start);
    CHECK(transit_time(d_before, r.accel) > (before - p.departure).to_seconds_f());
    CHECK(distance(ship_position(p, p.departure), p.start) == 0.0);
}

// Independent reference: plain fixed-point iteration T <- ceil(transit_time(d(T))). It converges
// for torch transits because the closing speed of the destination is far below the ship's
// average speed (contraction factor ~ v_dest / v_ship << 1).
std::int64_t fixed_point_arrival(const OrbitSystem& sys, const PlotRequest& r, Vec3 p0) {
    std::int64_t t = 0;
    for (int i = 0; i < 100; ++i) {
        const double d = distance(sys.world_position(r.destination, r.departure + sim::seconds(t)), p0);
        const auto next = static_cast<std::int64_t>(std::ceil(transit_time(d, r.accel)));
        if (next == t) {
            break;
        }
        t = next;
    }
    return t;
}

} // namespace

TEST_CASE("brachistochrone closed form: 1 AU at 1 g takes about 2.9 days") {
    const BurnProfile p = brachistochrone(u::au(1.0), u::g0);
    const double t = 2.0 * std::sqrt(u::au(1.0) / u::g0);
    CHECK(rel_err(p.total_time(), t) < 1e-12);
    CHECK(p.total_time() / u::day_s == doctest::Approx(2.859).epsilon(1e-3));
    CHECK(rel_err(p.delta_v(), 2.0 * std::sqrt(u::au(1.0) * u::g0)) < 1e-12);
    CHECK(rel_err(p.delta_v(), u::g0 * p.total_time()) < 1e-12);
    CHECK(rel_err(p.peak_speed, 0.5 * u::g0 * p.total_time()) < 1e-12);
    CHECK(p.coast_time == 0.0);
    CHECK(rel_err(p.flip_time(), 0.5 * p.total_time()) < 1e-12);

    const BurnProfile zero = brachistochrone(0.0, u::g0);
    CHECK(zero.total_time() == 0.0);
    CHECK(zero.delta_v() == 0.0);
}

TEST_CASE("coast profile: lower dv cap monotonically increases time and conserves distance") {
    const double d = u::au(1.5);
    const double a = u::gees(0.3);
    const BurnProfile fast = brachistochrone(d, a);
    const BurnProfile uncapped = capped_profile(d, a, 10.0 * fast.delta_v());
    CHECK(uncapped.total_time() == fast.total_time());
    CHECK(uncapped.delta_v() == fast.delta_v());

    double prev_time = fast.total_time();
    double prev_dv = fast.delta_v();
    for (double frac = 0.95; frac > 0.01; frac -= 0.05) {
        const BurnProfile p = capped_profile(d, a, frac * fast.delta_v());
        CHECK(p.total_time() > prev_time);
        CHECK(p.delta_v() < prev_dv);
        CHECK(rel_err(p.delta_v(), frac * fast.delta_v()) < 1e-12);
        CHECK(p.coast_time > 0.0);
        // Path length from the kinematics matches the requested distance.
        CHECK(rel_err(state_along(p, p.total_time() * (1.0 - 1e-15)).distance, d) < 1e-9);
        CHECK(rel_err(p.peak_speed * p.burn_time + p.peak_speed * p.coast_time, d) < 1e-12);
        prev_time = p.total_time();
        prev_dv = p.delta_v();
    }
}

TEST_CASE("profile_for_duration inverts the capped profile") {
    const double d = u::au(0.8);
    const double a = u::gees(0.2);
    const double t_min = brachistochrone(d, a).total_time();
    CHECK_FALSE(profile_for_duration(d, a, 0.99 * t_min).has_value());
    CHECK_FALSE(delta_v_for_duration(d, a, 0.5 * t_min).has_value());

    const auto at_min = profile_for_duration(d, a, t_min);
    REQUIRE(at_min.has_value());
    CHECK(rel_err(at_min->delta_v(), brachistochrone(d, a).delta_v()) < 1e-9);

    double prev_dv = at_min->delta_v();
    for (double k : {1.01, 1.2, 1.5, 2.0, 5.0, 20.0, 100.0}) {
        const double t = k * t_min;
        const auto dv = delta_v_for_duration(d, a, t);
        REQUIRE(dv.has_value());
        CHECK(*dv < prev_dv);
        CHECK(rel_err(transit_time(d, a, *dv), t) < 1e-9);
        const auto p = profile_for_duration(d, a, t);
        REQUIRE(p.has_value());
        CHECK(rel_err(p->total_time(), t) < 1e-12);
        CHECK(rel_err(state_along(*p, 0.5 * t).distance, 0.5 * d) < 1e-9);
        prev_dv = *dv;
    }
}

TEST_CASE("state along the path at start and midpoint and end") {
    const double d = u::au(1.0);
    const double a = u::gees(0.5);
    for (const BurnProfile& p : {brachistochrone(d, a), capped_profile(d, a, u::km_per_s(800.0))}) {
        CHECK(state_along(p, -10.0).distance == 0.0);
        CHECK(state_along(p, 0.0).speed == 0.0);
        const PathState mid = state_along(p, p.flip_time());
        CHECK(rel_err(mid.distance, 0.5 * d) < 1e-12);
        CHECK(rel_err(mid.speed, p.peak_speed) < 1e-12);
        CHECK(state_along(p, p.total_time()).distance == d);
        CHECK(state_along(p, p.total_time() + 1e6).speed == 0.0);
        // Continuous across phase boundaries.
        for (double tb : {p.burn_time, p.burn_time + p.coast_time}) {
            const PathState lo = state_along(p, tb * (1.0 - 1e-12));
            const PathState hi = state_along(p, tb * (1.0 + 1e-12));
            CHECK(std::abs(lo.distance - hi.distance) < 1e-6 * d);
            CHECK(std::abs(lo.speed - hi.speed) < 1e-6 * p.peak_speed);
        }
    }
}

TEST_CASE("rocket equation round trip") {
    const MassBudget m = hauler();
    const double dv = available_delta_v(m);
    CHECK(rel_err(dv, m.exhaust_velocity * std::log(1.5)) < 1e-12);
    CHECK(rel_err(reaction_mass_for(m, dv), m.reaction_mass) < 1e-12);
    CHECK(rel_err(reaction_mass_from_final(dv, m.empty_mass(), m.exhaust_velocity), m.reaction_mass) <
          1e-12);
    for (double v : {1.0, 1e3, 1e5, 1e6, 5e6}) {
        const double burned = reaction_mass_from_initial(v, m.wet_mass(), m.exhaust_velocity);
        CHECK(rel_err(m.exhaust_velocity * std::log(m.wet_mass() / (m.wet_mass() - burned)), v) <
              1e-9);
    }
    CHECK(reaction_mass_for(m, 0.0) == 0.0);
}

TEST_CASE("Epstein tuning: a 1 AU hop costs a hauler a meaningful but not absurd share of its tank") {
    const MassBudget m = hauler();
    const auto tank_share = [&](double distance, double gees) {
        return reaction_mass_for(m, brachistochrone(distance, u::gees(gees)).delta_v()) /
               m.reaction_mass;
    };
    CHECK(tank_share(u::au(1.0), 0.3) > 0.10);
    CHECK(tank_share(u::au(1.0), 0.3) < 0.40);
    CHECK(tank_share(u::au(1.0), 0.1) > 0.10);
    CHECK(tank_share(u::au(0.5), 0.3) < 0.40);
    // A slow boat (coast at 150 km/s) is cheap.
    const double slow = reaction_mass_for(m, capped_profile(u::au(1.0), u::gees(0.3),
                                                           u::km_per_s(300.0)).delta_v()) /
                        m.reaction_mass;
    CHECK(slow < 0.10);
}

TEST_CASE("intercept Earth to Mars over many departure dates") {
    const Scenario s;
    int count = 0;
    for (std::int64_t day = 0; day < 800; day += 23) {
        const PlotRequest r = request(s.earth, s.mars, sim::Time{day * 86400}, 1.0);
        const Plot p = plot_transit(s.sys, r);
        check_intercept(s.sys, r, p);
        // 1 g Earth-Mars takes days, not months.
        CHECK(p.duration.seconds > 86400);
        CHECK(p.duration.seconds < 8 * 86400);
        const std::int64_t fp = fixed_point_arrival(s.sys, r, p.start);
        CHECK(std::abs(fp - p.duration.seconds) <= 2);
        ++count;
    }
    CHECK(count > 30);
}

TEST_CASE("intercept Tycho-like station to Ceres-like asteroid over many departure dates") {
    const Scenario s;
    for (std::int64_t day = 0; day < 3000; day += 61) {
        for (double g : {0.05, 0.3, 2.0}) {
            const PlotRequest r = request(s.tycho, s.ceres, sim::Time{day * 86400}, g);
            const Plot p = plot_transit(s.sys, r);
            check_intercept(s.sys, r, p);
            // The destination moved during the transit, so aiming at its departure-time position
            // would miss; the intercept aims at where it will be.
            const double naive = distance(s.sys.world_position(s.ceres, r.departure), p.end);
            CHECK(naive > 0.0);
            CHECK(p.match_delta_v < u::km_per_s(40.0));
            CHECK(p.delta_v > p.profile.delta_v());
        }
    }
}

TEST_CASE("intercept evaluations stay bounded for slow capped transits") {
    const Scenario s;
    for (std::int64_t day = 0; day < 2000; day += 97) {
        PlotRequest r = request(s.tycho, s.ceres, sim::Time{day * 86400}, 0.3);
        r.max_delta_v = u::km_per_s(120.0); // ~60 km/s coast: weeks to months
        const Plot p = plot_transit(s.sys, r);
        REQUIRE(p.status == PlotStatus::ok);
        CHECK(p.evaluations <= intercept_max_evaluations);
        CHECK(p.delta_v <= r.max_delta_v * (1.0 + 1e-12));
        CHECK(p.profile.coast_time > 0.0);
        CHECK(distance(ship_position(p, p.arrival), s.sys.world_position(s.ceres, p.arrival)) <
              1.0);
    }
}

TEST_CASE("slow boat is cheaper and slower than the fast plot") {
    const Scenario s;
    PlotRequest r = request(s.tycho, s.ceres, sim::Time{400 * 86400}, 0.3);
    r.ship = hauler();
    const Plot fast = plot_transit(s.sys, r);
    r.max_delta_v = 0.25 * fast.delta_v;
    const Plot slow = plot_transit(s.sys, r);
    REQUIRE(slow.status == PlotStatus::ok);
    CHECK(slow.arrival > fast.arrival);
    CHECK(slow.reaction_mass_used < fast.reaction_mass_used);
    CHECK(slow.peak_speed < fast.peak_speed);
    CHECK(slow.flip > slow.departure);
    CHECK(slow.flip < slow.arrival);
}

TEST_CASE("infeasible when reaction mass is insufficient") {
    const Scenario s;
    PlotRequest r = request(s.earth, s.ceres, sim::Time{0}, 1.0);
    r.ship = hauler();
    r.ship.reaction_mass = u::tonnes(50.0);
    const Plot p = plot_transit(s.sys, r);
    CHECK(p.status == PlotStatus::insufficient_reaction_mass);
    CHECK_FALSE(p.feasible());
    CHECK(p.reaction_mass_used > r.ship.reaction_mass);
    CHECK(p.reaction_mass_left < 0.0);
    CHECK(p.arrival > p.departure);
    CHECK_FALSE(describe(p.status).empty());

    // Carrying more reaction mass makes the ship heavier, so size the tank from the dry end.
    r.ship.reaction_mass =
        1.001 * reaction_mass_from_final(p.delta_v, r.ship.empty_mass(), r.ship.exhaust_velocity);
    const Plot fueled = plot_transit(s.sys, r);
    CHECK(fueled.status == PlotStatus::ok);
    CHECK(fueled.arrival == p.arrival);
}

TEST_CASE("no intercept when the dv cap cannot catch the destination within the horizon") {
    const Scenario s;
    PlotRequest r = request(s.earth, s.ceres, sim::Time{0}, 0.3);
    r.max_delta_v = u::km_per_s(20.0); // barely above the velocity match
    r.horizon = sim::days(60);
    const Plot p = plot_transit(s.sys, r);
    CHECK(p.status == PlotStatus::no_intercept);
    CHECK(p.evaluations <= intercept_max_evaluations);
}

TEST_CASE("invalid requests are rejected") {
    const Scenario s;
    PlotRequest r = request(s.earth, s.mars, sim::Time{0}, 1.0);
    PlotRequest bad = r;
    bad.accel = 0.0;
    CHECK(plot_transit(s.sys, bad).status == PlotStatus::invalid_request);
    bad = r;
    bad.destination = BodyId{99};
    CHECK(plot_transit(s.sys, bad).status == PlotStatus::invalid_request);
    bad = r;
    bad.ship.dry_mass = 0.0;
    CHECK(plot_transit(s.sys, bad).status == PlotStatus::invalid_request);
    bad = r;
    bad.max_delta_v = 0.0;
    CHECK(plot_transit(s.sys, bad).status == PlotStatus::invalid_request);
}

TEST_CASE("ship at an arbitrary state as origin") {
    const Scenario s;
    orbit::StateVector ship;
    ship.position = {u::au(1.8), u::au(-0.4), u::au(0.02)};
    ship.velocity = {u::km_per_s(3.0), u::km_per_s(15.0), 0.0};
    PlotRequest r = request(s.earth, s.ceres, sim::Time{123'456}, 0.5);
    r.origin = Origin::at_state(ship);
    const Plot p = plot_transit(s.sys, r);
    check_intercept(s.sys, r, p);
    CHECK(p.start == ship.position);
    const orbit::StateVector c = s.sys.world_state(s.ceres, p.arrival);
    CHECK(rel_err(p.match_delta_v, distance(c.velocity, ship.velocity)) < 1e-12);
}

TEST_CASE("ship position along a plotted transit") {
    const Scenario s;
    const PlotRequest r = request(s.earth, s.mars, sim::Time{86400 * 100}, 1.0);
    const Plot p = plot_transit(s.sys, r);
    REQUIRE(p.feasible());
    CHECK(ship_position(p, p.departure - sim::days(1)) == p.start);
    CHECK(ship_position(p, p.departure) == p.start);
    const Vec3 mid = ship_position(p, p.flip);
    CHECK(std::abs(distance(mid, p.start) - 0.5 * p.distance) < 1e-6 * p.distance + 1e3);
    CHECK(distance(ship_position(p, p.arrival), p.end) < 1e-6);
    CHECK(ship_position(p, p.arrival + sim::days(3)) == p.end);
    const orbit::StateVector at_flip = ship_state(p, p.flip);
    CHECK(rel_err(length(at_flip.velocity), p.peak_speed) < 1e-3);
}

TEST_CASE("straight path through the Sun is flagged") {
    OrbitSystem sys;
    const BodyId sun = sys.add_fixed();
    const BodyId a = sys.add_fixed({u::au(-1.0), 0.0, 0.0}, sun);
    const BodyId b = sys.add_fixed({u::au(1.0), u::au(0.01), 0.0}, sun);
    PlotRequest r;
    r.origin = Origin::at_body(a);
    r.destination = b;
    r.accel = u::gees(0.3);
    r.ship = hauler();
    r.ship.reaction_mass = u::tonnes(1e6);
    r.avoid = sun;
    r.avoid_radius = u::au(0.1);
    const Plot p = plot_transit(sys, r);
    CHECK(p.status == PlotStatus::path_obstructed);
    CHECK(p.closest_approach < u::au(0.02));
    r.avoid_radius = u::au(0.001);
    CHECK(plot_transit(sys, r).status == PlotStatus::ok);
}

TEST_CASE("best departure over a window and determinism") {
    const Scenario s;
    PlotRequest r = request(s.earth, s.mars, sim::Time{0}, 0.3);
    const sim::Time end = sim::Time{0} + sim::days(400);
    const Plot now = plot_transit(s.sys, r);
    const Plot early = best_departure(s.sys, r, end, sim::days(10), Objective::earliest_arrival);
    const Plot cheap = best_departure(s.sys, r, end, sim::days(10), Objective::least_delta_v);
    REQUIRE(early.feasible());
    REQUIRE(cheap.feasible());
    CHECK(early.arrival <= now.arrival);
    CHECK(cheap.delta_v <= now.delta_v);
    CHECK(cheap.delta_v <= early.delta_v);

    const Plot again = plot_transit(s.sys, r);
    CHECK(again.arrival == now.arrival);
    CHECK(again.delta_v == now.delta_v);
    CHECK(again.evaluations == now.evaluations);
}
