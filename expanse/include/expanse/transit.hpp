#pragma once

#include "expanse/orbit.hpp"
#include "expanse/vec3.hpp"
#include "simcore/time.hpp"

#include <limits>
#include <optional>
#include <string_view>
#include <tuple>

// Torch-ship transits: constant-acceleration burn profiles, reaction mass, and the moving-target
// intercept that turns "go to Ceres" into a departure/arrival pair.
//
// Model (the standard simplification for high-thrust Epstein-drive ships):
// - The path is a straight line from the origin's position at departure to the destination's
//   position at arrival. Solar gravity is ignored during the transit: at 0.1-1 g the drive
//   out-pulls the Sun (~0.0006 g at 1 AU) by two to three orders of magnitude.
// - The ship accelerates at a constant `accel` (the drive throttles as reaction mass burns off),
//   optionally coasts, then flips and decelerates at the same rate. With no coast this is the
//   brachistochrone: t = 2 sqrt(d/a), dv = 2 sqrt(d a).
// - Matching the destination's velocity: the profile starts and ends at rest relative to the
//   straight line; the velocity difference between origin (at departure) and destination (at
//   arrival) is added to the dv budget as |v_dest - v_origin|. Blended into the main burn this
//   costs at most that much (vector sum <= sum of magnitudes), so it is a cheap conservative bound.
//   Orbital velocity differences (~5-40 km/s) are small next to torch peak speeds (hundreds of
//   km/s), so its effect on transit time is ignored.
namespace expanse::transit {

// Effective exhaust velocity of a civilian Epstein drive [m/s], ~3.3% c (Isp ~1.02e6 s). The
// Expanse wiki quotes ~1.9e7 m/s for the Rocinante; fan estimates of D-He3 limits run to ~9% c.
// We use about half the canon figure (worn, cheap belter drives) so that reaction mass is a real
// constraint: a hauler with tank = half its dry+cargo mass spends ~37% of its tank on a 1 AU hop
// at 0.3 g and ~22% at 0.1 g, while a coasting "slow boat" plot costs a few percent.
inline constexpr double epstein_exhaust_velocity = 1.0e7;

// ---- Burn profiles ------------------------------------------------------------------------------

// Accelerate for `burn_time`, coast for `coast_time`, decelerate for `burn_time`, along a straight
// line of length `distance`. All SI, relative to the line (starts and ends at rest).
struct BurnProfile {
    double distance = 0.0;   // [m]
    double accel = 0.0;      // [m/s^2]
    double peak_speed = 0.0; // [m/s]
    double burn_time = 0.0;  // each of the accel and decel burns [s]
    double coast_time = 0.0; // [s]

    double total_time() const { return 2.0 * burn_time + coast_time; }
    double delta_v() const { return 2.0 * peak_speed; }
    // Time from start at which the ship flips (middle of the coast; exactly mid-trip).
    double flip_time() const { return burn_time + 0.5 * coast_time; }

    static constexpr auto fields(auto& self) {
        return std::tie(self.distance, self.accel, self.peak_speed, self.burn_time, self.coast_time);
    }
};

// Pure flip-and-burn. Precondition: distance >= 0, accel > 0.
BurnProfile brachistochrone(double distance, double accel);

// Accel-coast-decel whose profile dv (both burns) does not exceed `max_delta_v`. Falls back to the
// brachistochrone when the cap is not binding. Precondition: distance >= 0, accel > 0,
// max_delta_v > 0.
BurnProfile capped_profile(double distance, double accel, double max_delta_v);

// Cheapest profile that covers `distance` in exactly `duration` seconds, or nullopt if even a
// brachistochrone is too slow (duration < 2 sqrt(d/a)).
std::optional<BurnProfile> profile_for_duration(double distance, double accel, double duration);

inline double transit_time(double distance, double accel,
                           double max_delta_v = std::numeric_limits<double>::infinity()) {
    return capped_profile(distance, accel, max_delta_v).total_time();
}

// Profile dv needed to arrive within `duration`; nullopt if not reachable at this acceleration.
inline std::optional<double> delta_v_for_duration(double distance, double accel, double duration) {
    const auto p = profile_for_duration(distance, accel, duration);
    return p ? std::optional<double>{p->delta_v()} : std::nullopt;
}

struct PathState {
    double distance = 0.0; // along the line from the start [m]
    double speed = 0.0;    // along the line [m/s]
};

// Progress `elapsed` seconds after the start; clamped to the endpoints outside [0, total_time].
PathState state_along(const BurnProfile& profile, double elapsed);

// ---- Reaction mass (Tsiolkovsky: dv = ve ln(m0 / m1)) ------------------------------------------

struct MassBudget {
    double dry_mass = 0.0;      // hull, drive, crew, consumables [kg]
    double cargo_mass = 0.0;    // [kg]
    double reaction_mass = 0.0; // on board [kg]
    double exhaust_velocity = epstein_exhaust_velocity; // [m/s]

    double empty_mass() const { return dry_mass + cargo_mass; }
    double wet_mass() const { return empty_mass() + reaction_mass; }
};

// dv from burning the whole tank.
double available_delta_v(const MassBudget& ship);
// Reaction mass burned to gain `delta_v` starting from `initial_mass`.
double reaction_mass_from_initial(double delta_v, double initial_mass, double exhaust_velocity);
// Reaction mass needed so that `final_mass` remains after gaining `delta_v`.
double reaction_mass_from_final(double delta_v, double final_mass, double exhaust_velocity);
// Reaction mass `ship` burns for `delta_v`, starting from its current wet mass.
inline double reaction_mass_for(const MassBudget& ship, double delta_v) {
    return reaction_mass_from_initial(delta_v, ship.wet_mass(), ship.exhaust_velocity);
}

// ---- Plotting (moving-target intercept) ---------------------------------------------------------

// Where a transit starts: a body in the OrbitSystem (station, moon, ...) or a free ship state.
struct Origin {
    orbit::BodyId body = orbit::no_body;
    orbit::StateVector state{}; // world frame; used when !body.valid()

    static Origin at_body(orbit::BodyId b) { return Origin{b, {}}; }
    static Origin at_state(const orbit::StateVector& s) { return Origin{orbit::no_body, s}; }
};

struct PlotRequest {
    Origin origin;
    orbit::BodyId destination = orbit::no_body;
    sim::Time departure{};
    double accel = 0.0; // sustained acceleration (crew g-tolerance) [m/s^2]
    // Total dv the captain is willing to spend, velocity match included. Infinite = fastest.
    double max_delta_v = std::numeric_limits<double>::infinity();
    MassBudget ship;
    // Optional keep-out sphere (normally the Sun) that the straight path must clear.
    orbit::BodyId avoid = orbit::no_body;
    double avoid_radius = 0.0; // [m]
    // Give up if no intercept arrives within this long after departure.
    sim::Duration horizon = sim::days(3650);
};

enum class PlotStatus {
    ok,
    invalid_request,             // bad accel/dv/masses/bodies
    no_intercept,                // destination not catchable within the horizon
    insufficient_reaction_mass,  // numbers are filled in; the tank is too small
    path_obstructed,             // straight line passes inside the keep-out sphere
};

std::string_view describe(PlotStatus status);

struct Plot {
    PlotStatus status = PlotStatus::invalid_request;
    sim::Time departure{};
    sim::Time arrival{};
    sim::Duration duration{};
    Vec3 start{}; // origin position at departure [m]
    Vec3 end{};   // destination position at arrival [m]
    BurnProfile profile{};
    double distance = 0.0;
    double match_delta_v = 0.0; // |v_dest(arrival) - v_origin(departure)|
    double delta_v = 0.0;       // profile + match
    double reaction_mass_used = 0.0;
    double reaction_mass_left = 0.0; // negative when infeasible
    double peak_speed = 0.0;
    sim::Time flip{};
    double closest_approach = std::numeric_limits<double>::infinity(); // to `avoid`, if set
    int evaluations = 0; // intercept function evaluations spent

    bool feasible() const { return status == PlotStatus::ok; }
};

// Bound on intercept evaluations (scan + refinement); exceeding it reports no_intercept.
inline constexpr int intercept_max_evaluations = 256;

// Solves for the earliest arrival t1 >= departure such that the ship, leaving the origin at
// `departure`, reaches the destination's position at t1. Times are whole seconds (sim::Time), so
// the root is bracketed on the integer grid: f(T) = transit_time(|p_dest(t0+T) - p0|) - T is
// scanned outward from T = 0 (where f > 0) to the first sign change, then refined by safeguarded
// regula falsi (Illinois) with a bisection fallback. Deterministic and bounded.
Plot plot_transit(const orbit::OrbitSystem& system, const PlotRequest& request);

enum class Objective { earliest_arrival, least_delta_v };

// Tries departures request.departure, +step, ... <= window_end and returns the best feasible plot
// (ties: earliest departure). If none is feasible, returns the plot for request.departure.
Plot best_departure(const orbit::OrbitSystem& system, const PlotRequest& request,
                    sim::Time window_end, sim::Duration step, Objective objective);

// Ship state along a plotted transit at time t (clamped to start before departure and end after).
// Velocity is along the path only; the blended velocity-match component is not modelled.
orbit::StateVector ship_state(const Plot& plot, sim::Time t);
inline Vec3 ship_position(const Plot& plot, sim::Time t) { return ship_state(plot, t).position; }

} // namespace expanse::transit
