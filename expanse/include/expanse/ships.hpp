#pragma once

// Ship operations: course previews ("plot"), departures, arrivals, and the status queries the
// shell shows. Built on transit::plot_transit; this layer adds the ship's actual masses and tank,
// validation with player-facing reasons, and the World bookkeeping (Underway, ShipArrives).
//
// Units: player-facing numbers are in the units a captain reads (t, km/s, g, AU, %); the raw
// SI transit::Plot is kept alongside for callers that need it. Player mistakes never throw: every
// operation returns a status plus a sentence the shell can print as-is.
//
// Not implemented yet (documented gaps):
//   * abort / redirect while underway: a transit is committed until arrival.
//   * refuel and cargo trading belong to the economy system (economy.cpp).
//   * a captain is not yet a CrewMember row, so "someone aboard" is always satisfied.

#include "expanse/content.hpp"
#include "expanse/transit.hpp"
#include "expanse/world.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace expanse {

// ---- Tuning -----------------------------------------------------------------------------------

// Straight-line courses must clear the Sun by this much (heat load on a cheap hull) [AU].
inline constexpr double sun_keep_out_au = 0.2;

// Hull wear per transit = dv term (drive and frame stress) + time term (micrometeoroids, thermal
// cycling). A 0.3 g hop across the belt costs roughly 1% of the hull.
inline constexpr double hull_wear_per_1000_km_s = 0.005;
inline constexpr double hull_wear_per_day = 0.0005;
// Below this a message warns the captain after each arrival.
inline constexpr double hull_warning_level = 0.25;

// ---- Course preview ---------------------------------------------------------------------------

enum class CourseStatus : std::uint8_t {
    ok,
    unknown_ship,               // null or stale ShipId
    unknown_destination,        // null or invalid StationId
    underway,                   // ship is already in transit
    already_there,              // destination is the station the ship is docked at
    invalid_acceleration,       // accel <= 0 or not finite
    invalid_delta_v_budget,     // max dv <= 0 or NaN
    overloaded,                 // cargo exceeds the hold
    no_intercept,               // destination not reachable within the planning horizon
    path_obstructed,            // straight line passes too close to the Sun
    insufficient_reaction_mass, // numbers are filled in; the tank is too small
    too_slow,                   // cannot arrive within the requested time, even at full burn
    dock_fees_owed,             // reserved for finance::can_depart (finance bean)
};

std::string_view describe(CourseStatus status);

// How to fly. The ship's drive may cap the acceleration (see effective_max_accel_g).
struct CourseOptions {
    double accel_g = 0.3;
    // Total dv the captain is willing to spend, velocity match included [km/s]. Infinite = the
    // fastest course (flip-and-burn); lower values coast in the middle and arrive later.
    double max_delta_v_km_s = std::numeric_limits<double>::infinity();
};

// Everything a captain wants to see before committing. Modelled on what trading/strategy games
// show for a route (Elite's economical vs fastest plot and fuel check, Terra Invicta's launch
// date / arrival date / dv trade-off, Aurora's reachable-destination list): when you leave, when
// you arrive, what it costs, what is left, and a clear go/no-go with the reason.
struct CoursePreview {
    CourseStatus status = CourseStatus::unknown_ship;
    std::string reason; // player-facing; "" when ok

    ShipId ship;
    StationId origin;
    StationId destination;

    // Timing. `wait` > 0 only for plot_best_departure (leave later for a better course).
    sim::Time departure{};
    sim::Time arrival{};
    sim::Time flip{};          // turnover: start of the braking burn (mid-course)
    sim::Duration duration{};  // departure -> arrival
    sim::Duration wait{};      // now -> departure

    // Course.
    double distance_au = 0.0;       // straight-line length (to where the target will be)
    double accel_g = 0.0;           // acceleration actually flown
    bool accel_limited = false;     // requested accel exceeded the drive at current mass
    double delta_v_km_s = 0.0;      // total, velocity match included
    double match_delta_v_km_s = 0.0;
    double peak_speed_km_s = 0.0;
    sim::Duration light_lag{};      // one-way origin -> destination comms delay at departure

    // Reaction mass [t] and tank [% of capacity].
    double reaction_mass_needed_t = 0.0;
    double reaction_mass_aboard_t = 0.0;
    double reaction_mass_after_t = 0.0; // negative when short
    double tank_capacity_t = 0.0;
    double tank_used_pct = 0.0;
    double tank_after_pct = 0.0;

    // Mass and hull.
    double wet_mass_t = 0.0;
    double hull_wear_pct = 0.0; // expected, percentage points

    transit::Plot plot; // raw SI plot (start/end points, burn profile)

    bool feasible() const { return status == CourseStatus::ok; }
};

// Maximum acceleration the drive can give `ship` at its current mass: the class's max_accel_g is
// quoted at dry mass, so thrust = max_accel_g * dry and accel = thrust / wet. The profile is flown
// at constant acceleration (the drive throttles as mass burns off), so departure mass is the
// binding case.
double effective_max_accel_g(const Content& content, const Ship& ship);

// Current mass budget of `ship` in SI (tonnes -> kg, km/s -> m/s).
transit::MassBudget mass_budget(const Content& content, const Ship& ship);

// Expected hull wear (0..1 fraction) for a burn profile.
double hull_wear(const transit::BurnProfile& profile);

// Course from the ship's current dock to `destination`, departing now.
CoursePreview plot_course(const Content& content, const World& world, ShipId ship,
                          StationId destination, const CourseOptions& options = {});

// Cheapest course (least dv) that departs now and arrives within `max_travel`, at the given
// acceleration. options.max_delta_v_km_s is ignored. Status too_slow if even the fastest course
// takes longer.
CoursePreview plot_cheapest_within(const Content& content, const World& world, ShipId ship,
                                   StationId destination, double accel_g, sim::Duration max_travel);

// Best course departing within [now, now + window] on a `step` grid (see transit::best_departure).
// The preview's `wait` says how long to stay docked; `depart` always leaves now.
CoursePreview plot_best_departure(const Content& content, const World& world, ShipId ship,
                                  StationId destination, const CourseOptions& options,
                                  sim::Duration window, sim::Duration step,
                                  transit::Objective objective);

// Previews to every other station, in station key order (a "where can I go" list).
std::vector<CoursePreview> plot_all_destinations(const Content& content, const World& world,
                                                 ShipId ship, const CourseOptions& options = {});

// ---- Departure --------------------------------------------------------------------------------

struct DepartResult {
    CourseStatus status = CourseStatus::unknown_ship;
    std::string reason;     // player-facing; "" when ok
    CoursePreview preview;  // the course flown (or why not)

    bool ok() const { return status == CourseStatus::ok; }
};

// Commits the ship to plot_course(...): deducts reaction mass, sets it Underway and schedules
// ShipArrives (flagged player_event for the player's ships). On failure nothing changes.
DepartResult depart(const Content& content, World& world, ShipId ship, StationId destination,
                    const CourseOptions& options = {});

// ---- Status queries ---------------------------------------------------------------------------

struct ShipStatus {
    bool valid = false; // false for a null/stale ShipId; other fields then default
    std::string name;
    std::string class_name;
    bool docked = false;
    StationId station;     // docked at, else origin
    StationId destination; // when underway
    std::string location;  // "docked at Ceres Station" / "en route Ceres Station -> ..."

    // Underway only.
    std::optional<sim::Time> eta;
    sim::Duration time_remaining{};
    double progress = 0.0;           // 0..1 of the distance
    double speed_km_s = 0.0;         // relative to the course line
    double distance_to_go_au = 0.0;

    double reaction_mass_t = 0.0;
    double reaction_mass_capacity_t = 0.0;
    double reaction_mass_pct = 0.0;
    double delta_v_available_km_s = 0.0; // whole tank at current mass
    double cargo_t = 0.0;
    double cargo_capacity_t = 0.0;
    double hull_pct = 0.0;
    double max_accel_g = 0.0; // drive limit at current mass
};

ShipStatus ship_status(const Content& content, const World& world, ShipId ship);

struct Separation {
    double distance_m = 0.0;
    double distance_au = 0.0;
    sim::Duration light_lag{}; // one-way, rounded to the nearest second
};

// Straight-line distance between two stations at time t, and the one-way light delay.
Separation separation(const Content& content, StationId a, StationId b, sim::Time t);
// One-way light delay over `distance_m`, rounded to the nearest second.
sim::Duration light_lag(double distance_m);

// "6d 4h", "5h 12m", "40m": compact trip lengths for messages.
std::string format_trip(sim::Duration d);

} // namespace expanse
