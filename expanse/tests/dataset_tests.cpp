// Sanity checks on the shipped solar-system data (data/*.toml) against independently known
// astronomical values, so a typo in an element set shows up as a failing test.

#include <doctest/doctest.h>

#include "expanse/calendar.hpp"
#include "expanse/content.hpp"
#include "expanse/units.hpp"

#include <cmath>
#include <memory>
#include <span>
#include <string_view>

using namespace expanse;

namespace {

const Content& dataset() {
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

const sim::Time game_start = calendar::to_time({2350, 1, 1});

bool is_finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

sim::DefId<BodyDef> require_body(const Content& c, std::string_view key) {
    const auto id = c.find<BodyDef>(key);
    INFO("body ", key);
    REQUIRE(id);
    return id;
}

// Hill-sphere radius [m] of a body, taken at its periapsis (the conservative end).
double hill_radius(const Content& c, sim::DefId<BodyDef> id) {
    const auto& bodies = c.table<BodyDef>();
    const BodyDef& b = bodies[id];
    const BodyDef& parent = bodies[*b.parent];
    const double q = c.orbits().orbit(c.orbit_of(id)).periapsis();
    return q * std::cbrt(b.gm_km3_s2 / (3.0 * parent.gm_km3_s2));
}

double period_days(const Content& c, sim::DefId<BodyDef> id) {
    return c.orbits().orbit(c.orbit_of(id)).period() / units::day_s;
}

struct Range {
    std::string_view key;
    double min_au;
    double max_au;
};

// Perihelion/aphelion (rounded outward) from the NASA planetary fact sheets and JPL SBDB.
constexpr Range heliocentric_ranges[] = {
    {"mercury", 0.307, 0.467}, {"venus", 0.718, 0.729},  {"earth", 0.983, 1.017},
    {"mars", 1.381, 1.667},    {"jupiter", 4.95, 5.46},  {"saturn", 9.02, 10.06},
    {"uranus", 18.28, 20.10},  {"neptune", 29.81, 30.33}, {"ceres", 2.54, 2.99},
    {"vesta", 2.15, 2.58},     {"pallas", 2.13, 3.42},   {"juno", 1.97, 3.36},
    {"hygiea", 2.80, 3.49},    {"psyche", 2.52, 3.32},   {"eros", 1.13, 1.79},
};

struct KnownPeriod {
    std::string_view key;
    double days;
};

// Sidereal orbital periods, NASA planetary / satellite fact sheets.
constexpr KnownPeriod planet_periods[] = {
    {"mercury", 87.969}, {"venus", 224.701},    {"earth", 365.256},   {"mars", 686.980},
    {"jupiter", 4332.589}, {"saturn", 10759.22}, {"uranus", 30685.4}, {"neptune", 60189.0},
};

constexpr KnownPeriod galilean_periods[] = {
    {"io", 1.769138}, {"europa", 3.551181}, {"ganymede", 7.154553}, {"callisto", 16.689018},
};

constexpr KnownPeriod other_moon_periods[] = {
    {"luna", 27.321661},    {"phobos", 0.318910},  {"deimos", 1.262441}, {"enceladus", 1.370218},
    {"rhea", 4.518212},     {"titan", 15.945421},  {"miranda", 1.413479}, {"titania", 8.705872},
    {"oberon", 13.463239},
};

void check_periods(const Content& c, std::span<const KnownPeriod> known, double tolerance) {
    for (const KnownPeriod& k : known) {
        const double p = period_days(c, require_body(c, k.key));
        INFO(k.key, ": ", p, " d vs known ", k.days, " d");
        CHECK(std::abs(p / k.days - 1.0) < tolerance);
    }
}

} // namespace

TEST_CASE("dataset major bodies sit in their known heliocentric ranges at game start") {
    const Content& c = dataset();
    for (const Range& r : heliocentric_ranges) {
        const auto id = require_body(c, r.key);
        const double d_au = units::to_au(length(c.orbits().world_position(c.orbit_of(id), game_start)));
        INFO(r.key, " at ", d_au, " AU");
        CHECK(d_au >= r.min_au);
        CHECK(d_au <= r.max_au);
    }
}

TEST_CASE("dataset every body is between its periapsis and apoapsis") {
    const Content& c = dataset();
    for (auto [id, body] : c.table<BodyDef>()) {
        if (!body.orbit) {
            continue;
        }
        const orbit::Orbit& o = c.orbits().orbit(c.orbit_of(id));
        for (int k = 0; k < 4; ++k) {
            const sim::Time t = game_start + sim::days(1000 * k);
            const Vec3 p = c.orbits().local_state(c.orbit_of(id), t).position;
            INFO(c.table<BodyDef>().key(id), " day ", 1000 * k);
            REQUIRE(is_finite(p));
            CHECK(length(p) >= o.periapsis() * (1.0 - 1e-9));
            CHECK(length(p) <= o.apoapsis() * (1.0 + 1e-9));
        }
        // Planets: close to a, within e.
        if (body.kind == BodyKind::planet) {
            const double r = length(c.orbits().world_position(c.orbit_of(id), game_start));
            CHECK(std::abs(r / o.semi_major_axis - 1.0) <= body.orbit->e + 1e-9);
        }
    }
}

TEST_CASE("dataset moons stay well inside their parent Hill sphere and clear of its surface") {
    const Content& c = dataset();
    const auto& bodies = c.table<BodyDef>();
    int moons = 0;
    for (auto [id, body] : bodies) {
        if (!body.parent || bodies[*body.parent].kind == BodyKind::star) {
            continue;
        }
        ++moons;
        const BodyDef& parent = bodies[*body.parent];
        const orbit::Orbit& o = c.orbits().orbit(c.orbit_of(id));
        INFO(bodies.key(id), " around ", bodies.key(*body.parent));
        // Prograde satellites are stable out to ~0.49 R_H, retrograde ones to ~0.93 R_H, with R_H
        // taken at the planet's pericentre (Domingos et al. 2006, after Hamilton & Burns 1992).
        CHECK(o.apoapsis() < 0.5 * hill_radius(c, *body.parent));
        CHECK(o.periapsis() > units::km(parent.radius_km + body.radius_km));
    }
    CHECK(moons >= 14);
}

TEST_CASE("dataset planet periods match known sidereal periods") {
    check_periods(dataset(), planet_periods, 0.005);
}

TEST_CASE("dataset Galilean moon periods match known values") {
    check_periods(dataset(), galilean_periods, 0.01);
}

TEST_CASE("dataset regular moon periods match known values") {
    // Luna is ~0.5% slow: its orbit uses Earth's GM alone, not Earth + Moon.
    check_periods(dataset(), other_moon_periods, 0.01);
}

TEST_CASE("dataset every station has a finite position near its host body") {
    const Content& c = dataset();
    const auto& bodies = c.table<BodyDef>();
    const auto& stations = c.table<StationDef>();
    CHECK(stations.size() >= 12);
    CHECK(stations.size() <= 20);
    for (auto [id, st] : stations) {
        const BodyDef& host = bodies[st.body];
        INFO("station ", stations.key(id));
        for (int k = 0; k < 4; ++k) {
            const sim::Time t = game_start + sim::days(250 * k);
            const Vec3 p = c.orbits().world_position(c.orbit_of(id), t);
            const Vec3 h = c.orbits().world_position(c.orbit_of(st.body), t);
            REQUIRE(is_finite(p));
            CHECK(units::to_au(length(p)) < 40.0);
            if (host.kind == BodyKind::star) {
                continue;
            }
            const double reach = st.orbit ? hill_radius(c, st.body) * 0.5 : 0.0;
            CHECK(distance(p, h) <= reach + 1.0);
        }
        if (st.orbit) {
            const orbit::Orbit& o = c.orbits().orbit(c.orbit_of(id));
            CHECK(o.periapsis() > units::km(host.radius_km));
        }
    }
}

TEST_CASE("dataset station markets are in a sane range") {
    const Content& c = dataset();
    const auto& stations = c.table<StationDef>();
    for (auto [id, st] : stations) {
        INFO("station ", stations.key(id));
        CHECK(st.population > 0);
        CHECK(st.docking_fee > 0);
        CHECK(st.docking_fee < 5000);
        REQUIRE_FALSE(st.market.empty());
        for (const MarketEntryDef& m : st.market) {
            INFO("commodity ", c.table<CommodityDef>().key(m.commodity));
            CHECK(m.production + m.consumption > 0.0);
            CHECK(m.production <= 1000.0);
            CHECK(m.consumption <= 1000.0);
            CHECK(m.stock <= 100000.0);
        }
    }
}

TEST_CASE("dataset keeps the starter stations and the Secondhand start") {
    const Content& c = dataset();
    for (std::string_view key : {"ceres_station", "tycho_station", "vesta_dock", "pallas_refinery", "mars_highport"}) {
        INFO("station ", key);
        CHECK(c.find<StationDef>(key));
    }
    const auto sc = c.find<ScenarioDef>("secondhand");
    REQUIRE(sc);
    const auto start = c.table<ScenarioDef>()[sc].start_station;
    CHECK(c.table<StationDef>().key(start) == "ceres_station");
    CHECK(c.table<StationDef>()[start].body == require_body(c, "ceres"));
}
