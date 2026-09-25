#include "expanse/orbit.hpp"

#include "expanse/units.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>

namespace expanse::orbit {

KeplerResult solve_kepler_elliptic_detailed(double mean_anomaly, double eccentricity) {
    assert(eccentricity >= 0.0 && eccentricity < 1.0);
    const double e = eccentricity;

    // Kepler's equation is odd in (E, M) and 2pi-periodic, so solve for M in [0, pi] only.
    const double m_reduced = std::remainder(mean_anomaly, units::two_pi);
    const double sign = m_reduced < 0.0 ? -1.0 : 1.0;
    const double m = std::abs(m_reduced);

    KeplerResult result;
    if (e == 0.0 || m == 0.0 || m == units::pi) {
        result.eccentric_anomaly = m_reduced;
        result.converged = true;
        return result;
    }

    // f(E) = E - e sin E - m is increasing and convex on [0, pi] with f(m) <= 0 and
    // f(min(m + e, pi)) >= 0, so the root is bracketed there.
    double lo = m;
    double hi = std::min(m + e, units::pi);
    // Danby's starting guess; kept inside the bracket.
    double ecc_anomaly = std::min(m + 0.85 * e, hi);

    constexpr double eps = std::numeric_limits<double>::epsilon();
    for (int it = 1; it <= kepler_max_iterations; ++it) {
        result.iterations = it;
        const double f = ecc_anomaly - e * std::sin(ecc_anomaly) - m;
        if (f == 0.0) {
            result.converged = true;
            break;
        }
        if (f < 0.0) {
            lo = ecc_anomaly;
        } else {
            hi = ecc_anomaly;
        }
        const double fp = 1.0 - e * std::cos(ecc_anomaly);
        // f is convex on [0, pi], so a Newton step from the left overshoots the root and one
        // from the right approaches it monotonically. Clamping an overshoot to hi (where
        // f >= 0) therefore keeps every later iterate on the monotone side.
        const double next = std::clamp(ecc_anomaly - f / fp, lo, hi);
        const double step = next - ecc_anomaly;
        ecc_anomaly = next;
        // Rounding in f is ~eps * max(1, E), which limits E to ~that / f'. Demanding more makes
        // iterates cycle between neighbouring doubles when f' is small (e -> 1, M -> 0).
        const double resolvable = 4.0 * eps * std::max(1.0, ecc_anomaly) / fp;
        if (std::abs(step) <= resolvable || hi - lo <= resolvable) {
            result.converged = true;
            break;
        }
    }

    result.eccentric_anomaly = sign * ecc_anomaly;
    return result;
}

double Orbit::period() const { return units::two_pi / mean_motion; }

double Orbit::periapsis() const { return semi_major_axis * (1.0 - eccentricity); }

double Orbit::apoapsis() const { return semi_major_axis * (1.0 + eccentricity); }

double Orbit::mean_anomaly_at(sim::Time t) const {
    // Subtract in integer seconds first so dt is exact regardless of how far t is from the epoch.
    const double dt = (t - epoch).to_seconds_f();
    return mean_anomaly_at_epoch + mean_motion * dt;
}

Orbit make_orbit(const Elements& el) {
    if (!(el.mu > 0.0)) {
        throw std::invalid_argument("orbit: mu must be positive");
    }
    if (!(el.eccentricity >= 0.0 && el.eccentricity < 1.0)) {
        throw std::invalid_argument("orbit: only elliptic orbits (0 <= e < 1) are supported");
    }
    if (!(el.semi_major_axis > 0.0)) {
        throw std::invalid_argument("orbit: semi-major axis must be positive for elliptic orbits");
    }

    Orbit o;
    o.semi_major_axis = el.semi_major_axis;
    o.eccentricity = el.eccentricity;
    o.semi_minor_ratio = std::sqrt((1.0 - el.eccentricity) * (1.0 + el.eccentricity));
    const double a = el.semi_major_axis;
    o.mean_motion = std::sqrt(el.mu / (a * a * a));
    o.mean_anomaly_at_epoch = el.mean_anomaly_at_epoch;
    o.mu = el.mu;
    o.epoch = el.epoch;

    // Columns 1 and 2 of R3(-Omega) R1(-i) R3(-omega): perifocal -> parent frame.
    const double co = std::cos(el.lon_ascending_node);
    const double so = std::sin(el.lon_ascending_node);
    const double cw = std::cos(el.arg_periapsis);
    const double sw = std::sin(el.arg_periapsis);
    const double ci = std::cos(el.inclination);
    const double si = std::sin(el.inclination);
    o.p_hat = {co * cw - so * sw * ci, so * cw + co * sw * ci, sw * si};
    o.q_hat = {-co * sw - so * cw * ci, -so * sw + co * cw * ci, cw * si};
    return o;
}

StateVector state_at(const Orbit& orbit, sim::Time t) {
    const double e = orbit.eccentricity;
    const double a = orbit.semi_major_axis;
    const double ecc_anomaly = solve_kepler_elliptic(orbit.mean_anomaly_at(t), e);
    const double c = std::cos(ecc_anomaly);
    const double s = std::sin(ecc_anomaly);

    // Perifocal coordinates straight from E; avoids computing the true anomaly.
    const double x = a * (c - e);
    const double y = a * orbit.semi_minor_ratio * s;
    const double r = a * (1.0 - e * c);
    const double k = std::sqrt(orbit.mu * a) / r;
    const double vx = -k * s;
    const double vy = k * orbit.semi_minor_ratio * c;

    return {orbit.p_hat * x + orbit.q_hat * y, orbit.p_hat * vx + orbit.q_hat * vy};
}

Vec3 position_at(const Orbit& orbit, sim::Time t) {
    const double e = orbit.eccentricity;
    const double a = orbit.semi_major_axis;
    const double ecc_anomaly = solve_kepler_elliptic(orbit.mean_anomaly_at(t), e);
    const double x = a * (std::cos(ecc_anomaly) - e);
    const double y = a * orbit.semi_minor_ratio * std::sin(ecc_anomaly);
    return orbit.p_hat * x + orbit.q_hat * y;
}

std::uint32_t OrbitSystem::push(BodyId parent) {
    if (parent.valid() && parent.value >= parent_.size()) {
        throw std::invalid_argument("OrbitSystem: parent must be added before its children");
    }
    const auto index = static_cast<std::uint32_t>(parent_.size());
    parent_.push_back(parent);
    fixed_.push_back(0);
    fixed_offset_.push_back({});
    orbit_.push_back({});
    return index;
}

BodyId OrbitSystem::add_fixed(Vec3 offset, BodyId parent) {
    const std::uint32_t i = push(parent);
    fixed_[i] = 1;
    fixed_offset_[i] = offset;
    return BodyId{i};
}

BodyId OrbitSystem::add_orbiting(BodyId parent, const Elements& el) {
    if (!parent.valid()) {
        throw std::invalid_argument("OrbitSystem: an orbiting body needs a parent");
    }
    Orbit o = make_orbit(el); // validate before mutating the table
    const std::uint32_t i = push(parent);
    orbit_[i] = o;
    return BodyId{i};
}

BodyId OrbitSystem::parent(BodyId body) const { return parent_.at(body.value); }

bool OrbitSystem::is_fixed(BodyId body) const { return fixed_.at(body.value) != 0; }

const Orbit& OrbitSystem::orbit(BodyId body) const {
    assert(!is_fixed(body));
    return orbit_.at(body.value);
}

StateVector OrbitSystem::local_state(BodyId body, sim::Time t) const {
    const std::size_t i = body.value;
    if (fixed_.at(i) != 0) {
        return {fixed_offset_[i], {}};
    }
    return state_at(orbit_[i], t);
}

StateVector OrbitSystem::world_state(BodyId body, sim::Time t) const {
    StateVector s = local_state(body, t);
    for (BodyId p = parent_.at(body.value); p.valid(); p = parent_[p.value]) {
        const StateVector ps = local_state(p, t);
        s.position += ps.position;
        s.velocity += ps.velocity;
    }
    return s;
}

Vec3 OrbitSystem::world_position(BodyId body, sim::Time t) const {
    auto local_position = [&](std::size_t i) {
        return fixed_[i] != 0 ? fixed_offset_[i] : position_at(orbit_[i], t);
    };
    Vec3 pos = local_position(body.value);
    for (BodyId p = parent_.at(body.value); p.valid(); p = parent_[p.value]) {
        pos += local_position(p.value);
    }
    return pos;
}

void OrbitSystem::evaluate(sim::Time t, std::span<StateVector> out) const {
    if (out.size() != size()) {
        throw std::invalid_argument("OrbitSystem::evaluate: output size mismatch");
    }
    for (std::size_t i = 0; i < size(); ++i) {
        StateVector s = fixed_[i] != 0 ? StateVector{fixed_offset_[i], {}} : state_at(orbit_[i], t);
        if (const BodyId p = parent_[i]; p.valid()) {
            s.position += out[p.value].position;
            s.velocity += out[p.value].velocity;
        }
        out[i] = s;
    }
}

std::vector<StateVector> OrbitSystem::evaluate(sim::Time t) const {
    std::vector<StateVector> out(size());
    evaluate(t, out);
    return out;
}

} // namespace expanse::orbit
