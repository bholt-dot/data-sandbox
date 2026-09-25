#include <doctest/doctest.h>

#include "expanse/orbit.hpp"
#include "expanse/units.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace expanse;
using namespace expanse::orbit;
namespace u = expanse::units;

namespace {

// Howard Hinnant's days_from_civil: days since 1970-01-01 in the proleptic Gregorian calendar.
std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Test scenarios put the scenario epoch at J2000.0 (2000-01-01 12:00 TT). The ~64-69 s TT-UTC
// offset is irrelevant at the tolerances used here.
sim::Time utc(std::int64_t y, std::int64_t m, std::int64_t d, std::int64_t hh, std::int64_t mm) {
    const std::int64_t j2000 = days_from_civil(2000, 1, 1) * 86400 + 12 * 3600;
    return sim::Time{days_from_civil(y, m, d) * 86400 + hh * 3600 + mm * 60 - j2000};
}

// JPL "Keplerian Elements for Approximate Positions of the Major Planets" (E. M. Standish),
// Table 1 (1800-2050 AD), J2000 mean ecliptic and equinox. Only the J2000 values are used; the
// per-century rates are used only in the mean-motion consistency test.
struct StandishRow {
    double a_au, e, i_deg, mean_lon_deg, lon_peri_deg, lon_node_deg;
    double mean_lon_rate_deg_per_cy, lon_peri_rate_deg_per_cy;
};
constexpr StandishRow em_bary{1.00000261, 0.01671123, -0.00001531, 100.46457166,
                              102.93768193, 0.0, 35999.37244981, 0.32327364};
constexpr StandishRow mars{1.52371034, 0.09339410, 1.84969142, -4.55343205,
                           -23.94362959, 49.55953891, 19140.30268499, 0.44441088};

// GM of the Earth-Moon system; the barycentre orbits the Sun with mu_sun + mu_emb.
constexpr double mu_emb = 4.0350323e14;
constexpr double mu_mars = 4.282837e13;
constexpr double mu_moon = 4.9028e12;

Elements from_standish(const StandishRow& row, double mu) {
    Elements el;
    el.semi_major_axis = u::au(row.a_au);
    el.eccentricity = row.e;
    el.inclination = u::deg(row.i_deg);
    el.lon_ascending_node = u::deg(row.lon_node_deg);
    el.arg_periapsis = u::deg(row.lon_peri_deg - row.lon_node_deg);  // omega = varpi - Omega
    el.mean_anomaly_at_epoch = u::deg(row.mean_lon_deg - row.lon_peri_deg); // M = L - varpi
    el.epoch = sim::Time{0};
    el.mu = mu;
    return el;
}

Elements simple(double a, double e, double mu, double m0 = 0.0) {
    Elements el;
    el.semi_major_axis = a;
    el.eccentricity = e;
    el.mean_anomaly_at_epoch = m0;
    el.mu = mu;
    return el;
}

double rel_err(double got, double want) { return std::abs(got - want) / std::abs(want); }

} // namespace

TEST_CASE("Kepler solver residual over a sweep of e and M") {
    const std::vector<double> eccentricities{0.0,  0.001, 0.0167, 0.1, 0.3,  0.5,
                                             0.7,  0.9,   0.95,   0.97, 0.99, 0.999};
    int worst_iterations = 0;
    double worst_residual = 0.0;
    for (double e : eccentricities) {
        for (int k = -2000; k <= 4000; ++k) {
            // M from -2pi to 4pi, plus the exact endpoints of each half-revolution.
            const double m = static_cast<double>(k) * (u::two_pi / 1000.0);
            const KeplerResult r = solve_kepler_elliptic_detailed(m, e);
            REQUIRE(r.converged);
            const double ecc = r.eccentric_anomaly;
            const double m_reduced = std::remainder(m, u::two_pi);
            const double residual = std::abs(ecc - e * std::sin(ecc) - m_reduced);
            worst_residual = std::max(worst_residual, residual);
            worst_iterations = std::max(worst_iterations, r.iterations);
            CHECK(ecc >= -u::pi);
            CHECK(ecc <= u::pi);
        }
    }
    CHECK(worst_residual < 1e-12);
    MESSAGE("worst residual " << worst_residual << ", worst iterations " << worst_iterations);
    // Typical is 4-5; the tail is e >= 0.99 near periapsis, far below the iteration cap.
    CHECK(worst_iterations <= 12);
}

TEST_CASE("Kepler solver near the M = 0, e -> 1 corner") {
    for (double e : {0.95, 0.97, 0.99, 0.9999}) {
        for (double m : {1e-12, 1e-9, 1e-6, 1e-3, 1e-2}) {
            for (double sign : {1.0, -1.0}) {
                const KeplerResult r = solve_kepler_elliptic_detailed(sign * m, e);
                REQUIRE(r.converged);
                const double ecc = r.eccentric_anomaly;
                CHECK(std::abs(ecc - e * std::sin(ecc) - sign * m) < 1e-12);
                CHECK(ecc * sign > 0.0);
            }
        }
    }
}

TEST_CASE("Kepler solver special cases") {
    CHECK(solve_kepler_elliptic(0.0, 0.5) == 0.0);
    CHECK(solve_kepler_elliptic(u::pi, 0.5) == doctest::Approx(u::pi));
    CHECK(solve_kepler_elliptic(1.234, 0.0) == doctest::Approx(1.234));
}

TEST_CASE("make_orbit rejects unsupported elements") {
    CHECK_THROWS_AS(make_orbit(simple(1e7, 1.0, u::mu_earth)), std::invalid_argument);
    CHECK_THROWS_AS(make_orbit(simple(-1e7, 1.5, u::mu_earth)), std::invalid_argument);
    CHECK_THROWS_AS(make_orbit(simple(1e7, 0.1, 0.0)), std::invalid_argument);
    CHECK_THROWS_AS(make_orbit(simple(0.0, 0.1, u::mu_earth)), std::invalid_argument);
}

TEST_CASE("orbit returns to its starting point after one period") {
    // Choose a so the period is an exact number of seconds.
    const double period = 6000.0;
    const double a = std::cbrt(u::mu_earth * period * period / (u::two_pi * u::two_pi));
    for (double e : {0.0, 0.5, 0.95}) {
        Elements el = simple(a, e, u::mu_earth, 0.7);
        el.inclination = u::deg(28.5);
        el.lon_ascending_node = u::deg(40.0);
        el.arg_periapsis = u::deg(-75.0);
        const Orbit o = make_orbit(el);
        CHECK(o.period() == doctest::Approx(period).epsilon(1e-12));
        const sim::Time t0{12345};
        const StateVector s0 = state_at(o, t0);
        const StateVector s1 = state_at(o, t0 + sim::seconds(6000));
        const StateVector s10 = state_at(o, t0 + sim::seconds(60000));
        CHECK(distance(s0.position, s1.position) < 1e-3);
        CHECK(distance(s0.position, s10.position) < 1e-3);
        CHECK(distance(s0.velocity, s1.velocity) < 1e-6);
        if (e == 0.0) {
            CHECK(length(s0.position) == doctest::Approx(a).epsilon(1e-14));
        }
    }
}

TEST_CASE("vis-viva energy and angular momentum are consistent with the elements") {
    for (double e : {0.0, 0.2, 0.6, 0.95}) {
        const double a = u::km(26'560.0);
        Elements el = simple(a, e, u::mu_earth, 0.3);
        el.inclination = u::deg(55.0);
        el.lon_ascending_node = u::deg(120.0);
        el.arg_periapsis = u::deg(33.0);
        const Orbit o = make_orbit(el);
        const double h_expected = std::sqrt(u::mu_earth * a * (1.0 - e * e));
        for (std::int64_t t = 0; t < 43'200; t += 97) {
            const StateVector s = state_at(o, sim::Time{t});
            const double r = length(s.position);
            const double v2 = length_squared(s.velocity);
            CHECK(rel_err(v2, u::mu_earth * (2.0 / r - 1.0 / a)) < 1e-12);
            CHECK(rel_err(length(cross(s.position, s.velocity)), h_expected) < 1e-12);
            CHECK(r >= o.periapsis() * (1.0 - 1e-12));
            CHECK(r <= o.apoapsis() * (1.0 + 1e-12));
            // position_at is the cheap path; it must agree with state_at.
            CHECK(distance(position_at(o, sim::Time{t}), s.position) < 1e-6);
        }
    }
}

TEST_CASE("velocity matches the time derivative of position") {
    Elements el = simple(u::km(10'000.0), 0.7, u::mu_earth, 1.0);
    el.inclination = u::deg(63.4);
    el.arg_periapsis = u::deg(270.0);
    const Orbit o = make_orbit(el);
    for (std::int64_t t = 100; t < 10'000; t += 331) {
        const Vec3 fd =
            (position_at(o, sim::Time{t + 1}) - position_at(o, sim::Time{t - 1})) / 2.0;
        const Vec3 v = state_at(o, sim::Time{t}).velocity;
        CHECK(distance(fd, v) < 1e-3 * length(v));
    }
}

TEST_CASE("periapsis and apoapsis distances and directions") {
    const double a = u::au(2.5);
    const double e = 0.4;
    Elements el = simple(a, e, u::mu_sun, 0.0);
    // Node on +y, polar orbit, periapsis at the node: periapsis on +y, moving toward +z.
    el.inclination = u::deg(90.0);
    el.lon_ascending_node = u::deg(90.0);
    el.arg_periapsis = 0.0;
    const Orbit peri = make_orbit(el);
    const StateVector sp = state_at(peri, el.epoch);
    CHECK(length(sp.position) == doctest::Approx(a * (1.0 - e)).epsilon(1e-14));
    CHECK(sp.position.y == doctest::Approx(a * (1.0 - e)).epsilon(1e-14));
    CHECK(std::abs(sp.position.x) < 1e-3);
    CHECK(std::abs(sp.position.z) < 1e-3);
    CHECK(sp.velocity.z > 0.0);
    CHECK(length(sp.velocity) ==
          doctest::Approx(std::sqrt(u::mu_sun / a * (1.0 + e) / (1.0 - e))).epsilon(1e-12));
    CHECK(std::abs(dot(sp.position, sp.velocity)) < 1e-6 * length(sp.position));

    el.mean_anomaly_at_epoch = u::pi;
    const StateVector sa = state_at(make_orbit(el), el.epoch);
    CHECK(length(sa.position) == doctest::Approx(a * (1.0 + e)).epsilon(1e-14));
    CHECK(sa.position.y == doctest::Approx(-a * (1.0 + e)).epsilon(1e-14));
}

TEST_CASE("J2000 elements: mean motion from mu matches the published mean-longitude rate") {
    for (const auto& [row, mu] : {std::pair{em_bary, u::mu_sun + mu_emb},
                                  std::pair{mars, u::mu_sun + mu_mars}}) {
        const Orbit o = make_orbit(from_standish(row, mu));
        const double rate_deg_per_cy = u::to_deg(o.mean_motion) * u::julian_century_s;
        // dM/dt = dL/dt - dvarpi/dt; the remainder is perturbations the two-body model lacks.
        const double mean_anomaly_rate = row.mean_lon_rate_deg_per_cy - row.lon_peri_rate_deg_per_cy;
        CHECK(rel_err(rate_deg_per_cy, mean_anomaly_rate) < 1e-5);
    }
}

TEST_CASE("Earth's heliocentric distance and longitude at known 2024 dates") {
    const Orbit earth = make_orbit(from_standish(em_bary, u::mu_sun + mu_emb));

    // Perihelion 2024-01-03 00:38 UTC, 147,100,632 km; aphelion 2024-07-05 05:06 UTC,
    // 152,099,968 km (USNO / timeanddate). These are for Earth's centre; the EM barycentre is
    // within ~4,700 km of it, well inside the tolerance.
    const double tol_au = 2e-4;
    const double r_peri = u::to_au(length(position_at(earth, utc(2024, 1, 3, 0, 38))));
    const double r_aph = u::to_au(length(position_at(earth, utc(2024, 7, 5, 5, 6))));
    CHECK(r_peri == doctest::Approx(u::to_au(u::km(147'100'632.0))).epsilon(tol_au));
    CHECK(r_aph == doctest::Approx(u::to_au(u::km(152'099'968.0))).epsilon(tol_au));

    // It is a minimum/maximum: a week either side is farther/closer.
    const auto r_at = [&](sim::Time t) { return length(position_at(earth, t)); };
    CHECK(r_at(utc(2024, 1, 3, 0, 38)) < r_at(utc(2023, 12, 27, 0, 38)));
    CHECK(r_at(utc(2024, 1, 3, 0, 38)) < r_at(utc(2024, 1, 10, 0, 38)));
    CHECK(r_at(utc(2024, 7, 5, 5, 6)) > r_at(utc(2024, 6, 28, 5, 6)));
    CHECK(r_at(utc(2024, 7, 5, 5, 6)) > r_at(utc(2024, 7, 12, 5, 6)));

    // March equinox 2024-03-20 03:06 UTC: the Sun is at ecliptic longitude 0 of date, so Earth is
    // at 180 deg of date, i.e. 180 deg minus ~24.2 yr of precession (50.29"/yr) in J2000 frame.
    const Vec3 p = position_at(earth, utc(2024, 3, 20, 3, 6));
    double lon = u::to_deg(std::atan2(p.y, p.x));
    if (lon < 0.0) {
        lon += 360.0;
    }
    const double precession_deg = 24.22 * 50.29 / 3600.0;
    CHECK(lon == doctest::Approx(180.0 - precession_deg).epsilon(0.05 / 180.0));
    CHECK(std::abs(p.z) < u::km(10'000.0));
}

TEST_CASE("OrbitSystem: hierarchical world positions") {
    OrbitSystem sys;
    const BodyId sun = sys.add_fixed();
    const BodyId earth = sys.add_orbiting(sun, from_standish(em_bary, u::mu_sun + mu_emb));
    const BodyId marsb = sys.add_orbiting(sun, from_standish(mars, u::mu_sun + mu_mars));

    Elements moon_el = simple(u::km(384'400.0), 0.0549, u::mu_earth + mu_moon, 2.0);
    moon_el.inclination = u::deg(5.145);
    moon_el.lon_ascending_node = u::deg(125.08);
    moon_el.arg_periapsis = u::deg(318.15);
    const BodyId moon = sys.add_orbiting(earth, moon_el);

    const BodyId station = sys.add_orbiting(moon, simple(u::km(2'000.0), 0.01, mu_moon));
    const BodyId lagrange_marker = sys.add_fixed({u::km(1.0), u::km(2.0), u::km(3.0)}, marsb);

    REQUIRE(sys.size() == 6);
    CHECK(sys.is_fixed(sun));
    CHECK_FALSE(sys.is_fixed(moon));
    CHECK(sys.parent(moon) == earth);
    CHECK_FALSE(sys.parent(sun).valid());

    std::vector<StateVector> world(sys.size());
    for (std::int64_t day = -400; day <= 400; day += 37) {
        const sim::Time t = sim::Time{0} + sim::days(day) + sim::seconds(day * 17);
        sys.evaluate(t, world);

        CHECK(world[sun.value].position == Vec3{});
        CHECK(world[sun.value].velocity == Vec3{});

        // world = parent world + local, at every level.
        for (const BodyId b : {earth, marsb, moon, station, lagrange_marker}) {
            const BodyId p = sys.parent(b);
            const StateVector local = sys.local_state(b, t);
            CHECK(distance(world[b.value].position, world[p.value].position + local.position) <
                  1e-3);
            CHECK(distance(world[b.value].velocity, world[p.value].velocity + local.velocity) <
                  1e-9);
            // Single-body queries agree with the bulk pass.
            CHECK(distance(sys.world_position(b, t), world[b.value].position) < 1e-3);
            CHECK(distance(sys.world_state(b, t).velocity, world[b.value].velocity) < 1e-9);
        }

        const double moon_r = distance(world[moon.value].position, world[earth.value].position);
        CHECK(moon_r >= u::km(384'400.0 * (1.0 - 0.0549)) * (1.0 - 1e-12));
        CHECK(moon_r <= u::km(384'400.0 * (1.0 + 0.0549)) * (1.0 + 1e-12));
        CHECK(distance(world[lagrange_marker.value].position, world[marsb.value].position) ==
              doctest::Approx(length(Vec3{u::km(1.0), u::km(2.0), u::km(3.0)})));
    }
}

TEST_CASE("OrbitSystem: parents must precede children") {
    OrbitSystem sys;
    CHECK_THROWS_AS(sys.add_fixed({}, BodyId{0}), std::invalid_argument);
    const BodyId sun = sys.add_fixed();
    CHECK_THROWS_AS(sys.add_orbiting(BodyId{5}, simple(1e11, 0.1, u::mu_sun)),
                    std::invalid_argument);
    CHECK_THROWS_AS(sys.add_orbiting(no_body, simple(1e11, 0.1, u::mu_sun)),
                    std::invalid_argument);
    CHECK_THROWS_AS(sys.add_orbiting(sun, simple(1e11, 1.1, u::mu_sun)), std::invalid_argument);
    CHECK(sys.size() == 1);
}
