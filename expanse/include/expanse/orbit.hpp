#pragma once

#include "expanse/vec3.hpp"
#include "simcore/time.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

// Analytic two-body (Keplerian) motion. Position is a pure function of time: nothing here holds
// per-tick state, so the same (orbit, time) always yields the same state vector.
namespace expanse::orbit {

// Classical elements, SI units. Angles are in the parent's reference frame (for the planets: the
// J2000 mean ecliptic and equinox, heliocentric).
struct Elements {
    double semi_major_axis = 0.0;       // a [m]
    double eccentricity = 0.0;          // e, 0 <= e < 1 (hyperbolic not yet supported)
    double inclination = 0.0;           // i [rad]
    double lon_ascending_node = 0.0;    // Omega [rad]
    double arg_periapsis = 0.0;         // omega [rad]
    double mean_anomaly_at_epoch = 0.0; // M0 [rad]
    sim::Time epoch{};                  // time at which M = M0
    double mu = 0.0;                    // parent gravitational parameter G*M [m^3/s^2]
};

struct StateVector {
    Vec3 position; // [m]
    Vec3 velocity; // [m/s]
};

struct KeplerResult {
    double eccentric_anomaly = 0.0; // E in [-pi, pi]
    int iterations = 0;
    bool converged = false;
};

inline constexpr int kepler_max_iterations = 50;

// Solves M = E - e sin E for 0 <= e < 1. M may be any finite value; it is reduced to [-pi, pi]
// and the returned E lies in the same range (E and M share sign). Safeguarded Newton: the root is
// bracketed in [M, min(M + e, pi)] for M in [0, pi] and steps are clamped to the bracket; since
// f(E) = E - e sin E - M is convex there, this converges monotonically even for e -> 1.
KeplerResult solve_kepler_elliptic_detailed(double mean_anomaly, double eccentricity);

inline double solve_kepler_elliptic(double mean_anomaly, double eccentricity) {
    return solve_kepler_elliptic_detailed(mean_anomaly, eccentricity).eccentric_anomaly;
}

// Elements preprocessed for fast evaluation: the orientation is reduced to the perifocal basis
// (P toward periapsis, Q 90 degrees ahead in the direction of motion), so evaluation costs one
// Kepler solve plus a few multiply-adds.
struct Orbit {
    double semi_major_axis = 0.0;
    double eccentricity = 0.0;
    double semi_minor_ratio = 1.0; // sqrt(1 - e^2)
    double mean_motion = 0.0;      // n [rad/s]
    double mean_anomaly_at_epoch = 0.0;
    double mu = 0.0;
    sim::Time epoch{};
    Vec3 p_hat{1.0, 0.0, 0.0};
    Vec3 q_hat{0.0, 1.0, 0.0};

    double period() const;          // [s]
    double periapsis() const;       // [m]
    double apoapsis() const;        // [m]
    double mean_anomaly_at(sim::Time t) const;
};

// Throws std::invalid_argument for elements outside the supported (elliptic) domain.
Orbit make_orbit(const Elements& el);

// State relative to the parent body, in the parent's (non-rotating) frame.
StateVector state_at(const Orbit& orbit, sim::Time t);
Vec3 position_at(const Orbit& orbit, sim::Time t);

// Index into an OrbitSystem. Bodies are static scenario data (added once, never removed), so a
// dense index is sufficient; gameplay entities referring to bodies store this.
struct BodyId {
    std::uint32_t value = std::numeric_limits<std::uint32_t>::max();

    constexpr auto operator<=>(const BodyId&) const = default;
    constexpr bool valid() const { return value != std::numeric_limits<std::uint32_t>::max(); }
};

inline constexpr BodyId no_body{};

// Hierarchical table of body motions (Sun -> planets -> moons -> stations). Each body's motion is
// relative to its parent; world state = parent world state + local state. A parent must be added
// before its children, so index order is a valid parent-before-child evaluation order and the
// whole system evaluates in a single forward pass.
class OrbitSystem {
public:
    // A body fixed at `offset` relative to `parent` (or the world origin if no parent), e.g. the Sun.
    BodyId add_fixed(Vec3 offset = {}, BodyId parent = no_body);
    // A body on a Keplerian orbit around `parent`. `el.mu` is the parent's gravitational parameter.
    BodyId add_orbiting(BodyId parent, const Elements& el);

    std::size_t size() const { return parent_.size(); }
    BodyId parent(BodyId body) const;
    bool is_fixed(BodyId body) const;
    // Precondition: !is_fixed(body).
    const Orbit& orbit(BodyId body) const;

    StateVector local_state(BodyId body, sim::Time t) const;
    // Walks the parent chain (depth is small: Sun/planet/moon/station), so single-body queries
    // such as an intercept solver's inner loop don't pay for the whole system.
    StateVector world_state(BodyId body, sim::Time t) const;
    Vec3 world_position(BodyId body, sim::Time t) const;

    // Fills `out[i]` with the world state of body i. Precondition: out.size() == size().
    void evaluate(sim::Time t, std::span<StateVector> out) const;
    std::vector<StateVector> evaluate(sim::Time t) const;

private:
    std::uint32_t push(BodyId parent);

    // Parallel arrays indexed by BodyId::value.
    std::vector<BodyId> parent_;
    std::vector<std::uint8_t> fixed_;
    std::vector<Vec3> fixed_offset_;
    std::vector<Orbit> orbit_;
};

} // namespace expanse::orbit
