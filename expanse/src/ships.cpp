#include "expanse/ships.hpp"

#include "expanse/calendar.hpp"
#include "expanse/finance.hpp"
#include "expanse/scenario.hpp"
#include "expanse/simulation.hpp"
#include "expanse/stat_ids.hpp"
#include "expanse/units.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <variant>

namespace expanse {

namespace {

double kg_to_t(double kg) { return kg / units::tonne_kg; }
double to_km_s(double m_s) { return m_s / units::km_m; }

double pct(double part, double whole) { return whole > 0.0 ? 100.0 * part / whole : 0.0; }

const ShipClassDef& class_of(const Content& content, const Ship& ship) {
    return content.table<ShipClassDef>()[ship.ship_class];
}

const std::string& station_name(const Content& content, StationId id) {
    return content.table<StationDef>()[id].name;
}

orbit::BodyId sun_body(const Content& content) {
    for (auto [id, body] : content.table<BodyDef>()) {
        if (body.kind == BodyKind::star) {
            return content.orbit_of(id);
        }
    }
    return orbit::no_body;
}

// A validated request for the ship's course, departing now, or a preview carrying the reason it
// could not be built.
struct Prepared {
    CoursePreview preview;
    transit::PlotRequest request;
    bool ok = false;
};

CoursePreview fail(CoursePreview p, CourseStatus status, std::string reason) {
    p.status = status;
    p.reason = std::move(reason);
    return p;
}

Prepared prepare(const Content& content, const World& world, ShipId ship_id, StationId destination,
                 double accel_g, double max_delta_v_km_s) {
    Prepared out;
    CoursePreview& p = out.preview;
    p.ship = ship_id;
    p.destination = destination;
    p.departure = p.arrival = p.flip = world.now();

    const Ship* ship = world.ships.get(ship_id);
    if (ship == nullptr) {
        p = fail(p, CourseStatus::unknown_ship, "No such ship.");
        return out;
    }
    const ShipClassDef& cls = class_of(content, *ship);
    p.reaction_mass_aboard_t = ship->reaction_mass_t;
    p.reaction_mass_after_t = ship->reaction_mass_t;
    p.tank_capacity_t = cls.reaction_mass_capacity_t;
    p.tank_after_pct = pct(ship->reaction_mass_t, cls.reaction_mass_capacity_t);
    p.wet_mass_t = cls.dry_mass_t + cargo_mass_t(*ship) + ship->reaction_mass_t;

    if (!content.table<StationDef>().contains(destination)) {
        p = fail(p, CourseStatus::unknown_destination, "No such station.");
        return out;
    }
    if (const auto* u = std::get_if<Underway>(&ship->location)) {
        p.origin = u->origin;
        p = fail(p, CourseStatus::underway,
                 std::format("{} is already underway to {}.", ship->name,
                             station_name(content, u->destination)));
        return out;
    }
    p.origin = std::get<Docked>(ship->location).station;
    if (p.origin == destination) {
        p = fail(p, CourseStatus::already_there,
                 std::format("{} is already docked at {}.", ship->name,
                             station_name(content, destination)));
        return out;
    }
    if (!(std::isfinite(accel_g) && accel_g > 0.0)) {
        p = fail(p, CourseStatus::invalid_acceleration, "Acceleration must be a positive number of g.");
        return out;
    }
    if (!(max_delta_v_km_s > 0.0)) {
        p = fail(p, CourseStatus::invalid_delta_v_budget, "The delta-v budget must be positive.");
        return out;
    }
    const double cargo = cargo_mass_t(*ship);
    if (cargo > cls.cargo_capacity_t * (1.0 + 1e-9)) {
        p = fail(p, CourseStatus::overloaded,
                 std::format("{} is carrying {:.1f} t in a {:.0f} t hold.", ship->name, cargo,
                             cls.cargo_capacity_t));
        return out;
    }

    const double drive_g = effective_max_accel_g(content, *ship);
    p.accel_limited = accel_g > drive_g;
    p.accel_g = std::min(accel_g, drive_g);

    transit::PlotRequest& r = out.request;
    r.origin = transit::Origin::at_body(content.orbit_of(p.origin));
    r.destination = content.orbit_of(destination);
    r.departure = world.now();
    r.accel = units::gees(p.accel_g);
    r.max_delta_v = max_delta_v_km_s * units::km_m; // inf stays inf
    r.ship = mass_budget(content, *ship);
    r.avoid = sun_body(content);
    r.avoid_radius = r.avoid.valid() ? units::au(sun_keep_out_au) : 0.0;
    out.ok = true;
    return out;
}

// A plot whose numbers are meaningful (it found an intercept), feasible or not.
bool has_course(const transit::Plot& plot) {
    return plot.status != transit::PlotStatus::invalid_request &&
           plot.status != transit::PlotStatus::no_intercept;
}

CoursePreview finish(const Content& content, const World& world, CoursePreview p,
                     const transit::Plot& plot) {
    p.plot = plot;
    p.departure = plot.departure;
    p.wait = plot.departure - world.now();
    const std::string& dest = station_name(content, p.destination);

    if (plot.status == transit::PlotStatus::invalid_request) {
        return fail(p, CourseStatus::invalid_acceleration, "The course request is invalid.");
    }
    if (plot.status == transit::PlotStatus::no_intercept) {
        return fail(p, CourseStatus::no_intercept,
                    std::format("No course to {} within the planning horizon at {:.2f} g.", dest,
                                p.accel_g));
    }

    p.arrival = plot.arrival;
    p.flip = plot.flip;
    p.duration = plot.duration;
    p.distance_au = units::to_au(plot.distance);
    p.delta_v_km_s = to_km_s(plot.delta_v);
    p.match_delta_v_km_s = to_km_s(plot.match_delta_v);
    p.peak_speed_km_s = to_km_s(plot.peak_speed);
    p.light_lag = light_lag(plot.distance);
    p.reaction_mass_needed_t = kg_to_t(plot.reaction_mass_used);
    p.reaction_mass_after_t = p.reaction_mass_aboard_t - p.reaction_mass_needed_t;
    p.tank_used_pct = pct(p.reaction_mass_needed_t, p.tank_capacity_t);
    p.tank_after_pct = pct(p.reaction_mass_after_t, p.tank_capacity_t);
    p.hull_wear_pct = 100.0 * hull_wear(plot.profile);

    switch (plot.status) {
    case transit::PlotStatus::ok:
        p.status = CourseStatus::ok;
        p.reason.clear();
        return p;
    case transit::PlotStatus::path_obstructed:
        return fail(p, CourseStatus::path_obstructed,
                    std::format("The straight course to {} passes {:.2f} AU from the Sun "
                                "(keep-out {:.2f} AU). Try another departure time.",
                                dest, units::to_au(plot.closest_approach), sun_keep_out_au));
    case transit::PlotStatus::insufficient_reaction_mass:
        return fail(p, CourseStatus::insufficient_reaction_mass,
                    std::format("Needs {:.1f} t of reaction mass; {:.1f} t aboard. Burn softer, "
                                "cap the delta-v, or refuel.",
                                p.reaction_mass_needed_t, p.reaction_mass_aboard_t));
    default:
        return fail(p, CourseStatus::invalid_acceleration, "The course request is invalid.");
    }
}

} // namespace

std::string_view describe(CourseStatus status) {
    switch (status) {
    case CourseStatus::ok: return "ok";
    case CourseStatus::unknown_ship: return "unknown ship";
    case CourseStatus::unknown_destination: return "unknown destination";
    case CourseStatus::underway: return "already underway";
    case CourseStatus::already_there: return "already there";
    case CourseStatus::invalid_acceleration: return "invalid acceleration";
    case CourseStatus::invalid_delta_v_budget: return "invalid delta-v budget";
    case CourseStatus::overloaded: return "overloaded";
    case CourseStatus::no_intercept: return "no intercept";
    case CourseStatus::path_obstructed: return "path too close to the Sun";
    case CourseStatus::insufficient_reaction_mass: return "not enough reaction mass";
    case CourseStatus::too_slow: return "cannot arrive in time";
    case CourseStatus::dock_fees_owed: return "docking fees owed";
    }
    return "unknown";
}

double effective_max_accel_g(const Content& content, const Ship& ship) {
    const ShipClassDef& cls = class_of(content, ship);
    const double wet = cls.dry_mass_t + cargo_mass_t(ship) + ship.reaction_mass_t;
    return wet > 0.0 ? cls.max_accel_g * cls.dry_mass_t / wet : 0.0;
}

transit::MassBudget mass_budget(const Content& content, const Ship& ship) {
    const ShipClassDef& cls = class_of(content, ship);
    transit::MassBudget m;
    m.dry_mass = units::tonnes(cls.dry_mass_t);
    m.cargo_mass = units::tonnes(cargo_mass_t(ship));
    m.reaction_mass = units::tonnes(ship.reaction_mass_t);
    m.exhaust_velocity = units::km_per_s(cls.exhaust_velocity_km_s);
    return m;
}

double hull_wear(const transit::BurnProfile& profile) {
    return hull_wear_per_1000_km_s * profile.delta_v() / units::km_per_s(1000.0) +
           hull_wear_per_day * profile.total_time() / units::day_s;
}

CoursePreview plot_course(const Content& content, const World& world, ShipId ship,
                          StationId destination, const CourseOptions& options) {
    Prepared prep = prepare(content, world, ship, destination, options.accel_g, options.max_delta_v_km_s);
    if (!prep.ok) {
        return prep.preview;
    }
    return finish(content, world, std::move(prep.preview),
                  transit::plot_transit(content.orbits(), prep.request));
}

CoursePreview plot_cheapest_within(const Content& content, const World& world, ShipId ship,
                                   StationId destination, double accel_g, sim::Duration max_travel) {
    Prepared prep = prepare(content, world, ship, destination, accel_g,
                            std::numeric_limits<double>::infinity());
    if (!prep.ok) {
        return prep.preview;
    }
    transit::PlotRequest& r = prep.request;
    const transit::Plot fastest = transit::plot_transit(content.orbits(), r);
    if (!has_course(fastest)) {
        return finish(content, world, std::move(prep.preview), fastest);
    }
    if (fastest.duration > max_travel) {
        CoursePreview p = finish(content, world, std::move(prep.preview), fastest);
        return fail(std::move(p), CourseStatus::too_slow,
                    std::format("Even at full burn the trip to {} takes {} (limit {}).",
                                station_name(content, destination), format_trip(fastest.duration),
                                format_trip(max_travel)));
    }
    // Arrival time is non-increasing in the dv cap (a larger cap never slows any leg), so the
    // cheapest course that still makes the deadline is found by bisecting on the cap.
    transit::Plot best = fastest;
    double lo = 0.0;
    double hi = fastest.delta_v;
    for (int i = 0; i < 64 && hi - lo > 1.0; ++i) {
        const double mid = 0.5 * (lo + hi);
        r.max_delta_v = mid;
        const transit::Plot p = transit::plot_transit(content.orbits(), r);
        if (has_course(p) && p.duration <= max_travel) {
            hi = mid;
            best = p;
        } else {
            lo = mid;
        }
    }
    return finish(content, world, std::move(prep.preview), best);
}

CoursePreview plot_best_departure(const Content& content, const World& world, ShipId ship,
                                  StationId destination, const CourseOptions& options,
                                  sim::Duration window, sim::Duration step,
                                  transit::Objective objective) {
    Prepared prep = prepare(content, world, ship, destination, options.accel_g, options.max_delta_v_km_s);
    if (!prep.ok) {
        return prep.preview;
    }
    const transit::Plot plot = transit::best_departure(content.orbits(), prep.request,
                                                       world.now() + window, step, objective);
    return finish(content, world, std::move(prep.preview), plot);
}

std::vector<CoursePreview> plot_all_destinations(const Content& content, const World& world,
                                                 ShipId ship, const CourseOptions& options) {
    std::vector<CoursePreview> out;
    const Ship* s = world.ships.get(ship);
    const Docked* docked = s != nullptr ? std::get_if<Docked>(&s->location) : nullptr;
    for (auto [id, station] : content.table<StationDef>()) {
        (void)station;
        if (docked != nullptr && docked->station == id) {
            continue;
        }
        out.push_back(plot_course(content, world, ship, id, options));
    }
    return out;
}

DepartResult depart(const Content& content, World& world, ShipId ship_id, StationId destination,
                    const CourseOptions& options) {
    DepartResult result;
    result.preview = plot_course(content, world, ship_id, destination, options);
    const CoursePreview& p = result.preview;
    if (!p.feasible()) {
        result.status = p.status;
        result.reason = p.reason;
        return result;
    }
    // Someone must be aboard to fly: the captain always is (not yet a CrewMember row), so there is
    // no crew check until the crew system models the captain explicitly.

    if (const DepartureCheck dock = can_depart(content, world, ship_id); !dock.allowed) {
        result.status = CourseStatus::dock_fees_owed;
        result.reason = dock.reason;
        return result;
    }

    Ship& ship = world.ships.at(ship_id);
    ship.reaction_mass_t = std::max(0.0, ship.reaction_mass_t - p.reaction_mass_needed_t);

    Underway u;
    u.origin = p.origin;
    u.destination = destination;
    u.departure = p.plot.departure;
    u.arrival = p.plot.arrival;
    u.start = p.plot.start;
    u.end = p.plot.end;
    u.profile = p.plot.profile;
    const bool players = ship.owner == world.player;
    u.arrival_event = world.scheduler.schedule_at(
        u.arrival, ShipArrives{ship_id}, {priority_arrivals, players ? player_event : 0u});
    ship.location = u;

    if (players) {
        post(world, MessageKind::ship,
             std::format("{} departed {} for {} at {:.2f} g. ETA {} ({}). Burned {:.1f} t of "
                         "reaction mass; {:.1f} t left ({:.0f}%).",
                         ship.name, station_name(content, p.origin), station_name(content, destination),
                         p.accel_g, calendar::format_datetime(p.arrival), format_trip(p.duration),
                         p.reaction_mass_needed_t, ship.reaction_mass_t,
                         pct(ship.reaction_mass_t, p.tank_capacity_t)));
    }
    result.status = CourseStatus::ok;
    return result;
}

ShipStatus ship_status(const Content& content, const World& world, ShipId ship_id) {
    ShipStatus st;
    const Ship* ship = world.ships.get(ship_id);
    if (ship == nullptr) {
        return st;
    }
    const ShipClassDef& cls = class_of(content, *ship);
    st.valid = true;
    st.name = ship->name;
    st.class_name = cls.name;
    st.reaction_mass_t = ship->reaction_mass_t;
    st.reaction_mass_capacity_t = cls.reaction_mass_capacity_t;
    st.reaction_mass_pct = pct(ship->reaction_mass_t, cls.reaction_mass_capacity_t);
    st.delta_v_available_km_s = to_km_s(transit::available_delta_v(mass_budget(content, *ship)));
    st.cargo_t = cargo_mass_t(*ship);
    st.cargo_capacity_t = cls.cargo_capacity_t;
    st.hull_pct = 100.0 * ship->hull_condition;
    st.max_accel_g = effective_max_accel_g(content, *ship);

    if (const auto* docked = std::get_if<Docked>(&ship->location)) {
        st.docked = true;
        st.station = docked->station;
        st.location = "docked at " + station_name(content, docked->station);
        return st;
    }
    const auto& u = std::get<Underway>(ship->location);
    st.station = u.origin;
    st.destination = u.destination;
    st.eta = u.arrival;
    st.time_remaining = std::max(sim::Duration{}, u.arrival - world.now());
    const double elapsed = (world.now() - u.departure).to_seconds_f();
    const transit::PathState s = transit::state_along(u.profile, elapsed);
    st.progress = u.profile.distance > 0.0 ? s.distance / u.profile.distance : 1.0;
    st.speed_km_s = to_km_s(s.speed);
    st.distance_to_go_au = units::to_au(u.profile.distance - s.distance);
    const char* phase = elapsed < u.profile.burn_time                         ? "accelerating"
                        : elapsed < u.profile.burn_time + u.profile.coast_time ? "coasting"
                                                                               : "braking";
    st.location = std::format("en route {} -> {} ({}, {:.0f}%)", station_name(content, u.origin),
                              station_name(content, u.destination), phase, 100.0 * st.progress);
    return st;
}

sim::Duration light_lag(double distance_m) {
    return sim::seconds(static_cast<std::int64_t>(std::llround(distance_m / units::speed_of_light)));
}

Separation separation(const Content& content, StationId a, StationId b, sim::Time t) {
    const auto& orbits = content.orbits();
    const double d = distance(orbits.world_position(content.orbit_of(a), t),
                              orbits.world_position(content.orbit_of(b), t));
    return {d, units::to_au(d), light_lag(d)};
}

std::string format_trip(sim::Duration d) {
    const std::int64_t s = std::max<std::int64_t>(0, d.seconds);
    const std::int64_t days = s / 86400;
    const std::int64_t hours = (s % 86400) / 3600;
    const std::int64_t minutes = (s % 3600) / 60;
    if (days > 0) {
        return std::format("{}d {}h", days, hours);
    }
    if (hours > 0) {
        return std::format("{}h {}m", hours, minutes);
    }
    return std::format("{}m", minutes);
}

namespace systems {

void ship_arrives(const Content& content, World& world, sim::Scheduler<Event>& scheduler, ShipId ship_id) {
    Ship* ship = world.ships.get(ship_id);
    if (ship == nullptr) {
        return; // ship was scrapped/sold while underway
    }
    const auto* u = std::get_if<Underway>(&ship->location);
    if (u == nullptr || u->arrival != scheduler.now()) {
        return; // stale event (the transit it belonged to no longer exists)
    }
    const StationId destination = u->destination;
    const sim::Duration trip = u->arrival - u->departure;
    // A skilled engineer aboard reduces wear (crew stat multiplier, 1.0 without one).
    const double wear = hull_wear(u->profile) * world.stats.get(ship_key(ship_id), stat::hull_wear);

    ship->location = Docked{destination};
    const double before = ship->hull_condition;
    ship->hull_condition = std::max(0.0, ship->hull_condition - wear);

    if (ship->owner != world.player) {
        return; // NPC traffic is not journaled
    }
    post(world, MessageKind::ship,
         std::format("{} docked at {} after {}.", ship->name, station_name(content, destination),
                     format_trip(trip)));
    if (ship->hull_condition < hull_warning_level && before >= hull_warning_level) {
        post(world, MessageKind::warning,
             std::format("{}'s hull is down to {:.0f}%. Get it looked at before it gives out.",
                         ship->name, 100.0 * ship->hull_condition),
             true);
    }
}

} // namespace systems

} // namespace expanse
