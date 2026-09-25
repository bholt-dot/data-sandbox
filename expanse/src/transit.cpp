#include "expanse/transit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace expanse::transit {

namespace {

BurnProfile make_profile(double distance, double accel, double peak_speed) {
    BurnProfile p;
    p.distance = distance;
    p.accel = accel;
    if (distance <= 0.0 || peak_speed <= 0.0) {
        return p;
    }
    p.peak_speed = peak_speed;
    p.burn_time = peak_speed / accel;
    // Distance covered by both burns is v^2/a; the rest is coasted at peak speed.
    p.coast_time = std::max(0.0, (distance - peak_speed * p.burn_time) / peak_speed);
    return p;
}

} // namespace

BurnProfile brachistochrone(double distance, double accel) {
    return make_profile(distance, accel, std::sqrt(accel * distance));
}

BurnProfile capped_profile(double distance, double accel, double max_delta_v) {
    return make_profile(distance, accel, std::min(std::sqrt(accel * distance), 0.5 * max_delta_v));
}

std::optional<BurnProfile> profile_for_duration(double distance, double accel, double duration) {
    if (distance <= 0.0) {
        BurnProfile p;
        p.accel = accel;
        p.coast_time = std::max(0.0, duration); // parked
        return p;
    }
    const BurnProfile fastest = brachistochrone(distance, accel);
    if (duration < fastest.total_time()) {
        return std::nullopt;
    }
    // Total time T = v/a + d/v for peak speed v; solve v^2 - aT v + a d = 0 for the smaller (cheaper)
    // root, written in the form that avoids cancellation when aT >> sqrt(a d).
    const double at = accel * duration;
    const double disc = std::max(0.0, at * at - 4.0 * accel * distance);
    const double v = std::min(fastest.peak_speed, 2.0 * accel * distance / (at + std::sqrt(disc)));
    BurnProfile p;
    p.distance = distance;
    p.accel = accel;
    p.peak_speed = v;
    p.burn_time = v / accel;
    p.coast_time = std::max(0.0, duration - 2.0 * p.burn_time);
    return p;
}

PathState state_along(const BurnProfile& p, double elapsed) {
    const double total = p.total_time();
    if (elapsed <= 0.0) {
        return {0.0, 0.0};
    }
    if (elapsed >= total) {
        return {p.distance, 0.0};
    }
    if (elapsed < p.burn_time) {
        return {0.5 * p.accel * elapsed * elapsed, p.accel * elapsed};
    }
    if (elapsed < p.burn_time + p.coast_time) {
        const double burned = 0.5 * p.accel * p.burn_time * p.burn_time;
        return {burned + p.peak_speed * (elapsed - p.burn_time), p.peak_speed};
    }
    const double remaining = total - elapsed;
    return {p.distance - 0.5 * p.accel * remaining * remaining, p.accel * remaining};
}

double available_delta_v(const MassBudget& ship) {
    return ship.exhaust_velocity * std::log(ship.wet_mass() / ship.empty_mass());
}

double reaction_mass_from_initial(double delta_v, double initial_mass, double exhaust_velocity) {
    return -initial_mass * std::expm1(-delta_v / exhaust_velocity);
}

double reaction_mass_from_final(double delta_v, double final_mass, double exhaust_velocity) {
    return final_mass * std::expm1(delta_v / exhaust_velocity);
}

std::string_view describe(PlotStatus status) {
    switch (status) {
    case PlotStatus::ok: return "ok";
    case PlotStatus::invalid_request: return "invalid request";
    case PlotStatus::no_intercept: return "no intercept within the planning horizon";
    case PlotStatus::insufficient_reaction_mass: return "not enough reaction mass";
    case PlotStatus::path_obstructed: return "straight-line path passes too close to a hazard";
    }
    return "unknown";
}

namespace {

bool body_ok(const orbit::OrbitSystem& sys, orbit::BodyId b) {
    return b.valid() && b.value < sys.size();
}

bool valid_request(const orbit::OrbitSystem& sys, const PlotRequest& r) {
    const MassBudget& s = r.ship;
    if (!(std::isfinite(r.accel) && r.accel > 0.0) || !(r.max_delta_v > 0.0)) {
        return false;
    }
    if (!(s.dry_mass > 0.0) || !(s.cargo_mass >= 0.0) || !(s.reaction_mass >= 0.0) ||
        !(std::isfinite(s.exhaust_velocity) && s.exhaust_velocity > 0.0)) {
        return false;
    }
    if (!body_ok(sys, r.destination) || r.horizon.seconds <= 0) {
        return false;
    }
    if (r.origin.body.valid() && !body_ok(sys, r.origin.body)) {
        return false;
    }
    if (r.avoid.valid() && !body_ok(sys, r.avoid)) {
        return false;
    }
    return true;
}

double segment_point_distance(const Vec3& a, const Vec3& b, const Vec3& p) {
    const Vec3 ab = b - a;
    const double len2 = length_squared(ab);
    const double k = len2 > 0.0 ? std::clamp(dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
    return distance(a + ab * k, p);
}

// f(T) = (time the ship needs to reach where the destination is at t0 + T) - T. Positive: the
// destination gets there before the ship can.
struct InterceptFn {
    const orbit::OrbitSystem& sys;
    const PlotRequest& req;
    Vec3 p0;
    Vec3 v0;
    int evaluations = 0;

    struct Sample {
        double f;
        double distance;
        double match_dv;
    };

    Sample operator()(std::int64_t t) {
        ++evaluations;
        const orbit::StateVector dest = sys.world_state(req.destination, req.departure + sim::seconds(t));
        const double d = distance(dest.position, p0);
        const double match = distance(dest.velocity, v0);
        const double cap = req.max_delta_v - match;
        if (!(cap > 0.0)) {
            return {std::numeric_limits<double>::infinity(), d, match};
        }
        return {transit_time(d, req.accel, cap) - static_cast<double>(t), d, match};
    }
};

struct Root {
    bool found = false;
    std::int64_t t = 0; // smallest whole second with f(t) <= 0 (to the scan's resolution)
};

// Integer-grid bracket refinement. Invariant: f(lo) > 0 >= f(hi). Regula falsi with the Illinois
// modification, falling back to bisection whenever a step fails to halve the bracket, so the loop
// needs at most ~2 log2(hi - lo) evaluations.
Root refine(InterceptFn& f, std::int64_t lo, double flo, std::int64_t hi, double fhi) {
    int retained = 0; // +1: lo kept last step, -1: hi kept last step
    bool bisect = false;
    while (hi - lo > 1) {
        if (f.evaluations >= intercept_max_evaluations) {
            return {};
        }
        const std::int64_t width = hi - lo;
        std::int64_t x = lo + width / 2;
        if (!bisect && std::isfinite(flo) && flo - fhi > 0.0) {
            const double frac = flo / (flo - fhi);
            x = lo + static_cast<std::int64_t>(std::llround(frac * static_cast<double>(width)));
            x = std::clamp(x, lo + 1, hi - 1);
        }
        const double fx = f(x).f;
        if (fx > 0.0) {
            lo = x;
            flo = fx;
            if (retained == -1) {
                fhi *= 0.5;
            }
            retained = -1;
        } else {
            hi = x;
            fhi = fx;
            if (retained == +1) {
                flo *= 0.5;
            }
            retained = +1;
        }
        bisect = (hi - lo) * 2 > width;
    }
    return {true, hi};
}

Root solve_intercept(InterceptFn& f, std::int64_t horizon) {
    const auto first = f(0);
    if (first.f <= 0.0) {
        return {true, 0};
    }
    // Scan outward for the first sign change so the earliest intercept is found (f can have several
    // roots when the destination swings towards and away from the origin). Fine linear steps over
    // ~2 brachistochrone times, then geometric growth out to the horizon.
    const double fast = brachistochrone(first.distance, f.req.accel).total_time();
    double step = std::max(60.0, fast / 32.0);
    std::int64_t lo = 0;
    double flo = first.f;
    for (int i = 0; lo < horizon; ++i) {
        if (f.evaluations >= intercept_max_evaluations) {
            return {};
        }
        if (i >= 64) {
            step *= 1.5;
        }
        const std::int64_t hi =
            std::min(horizon, lo + std::max<std::int64_t>(1, static_cast<std::int64_t>(step)));
        const double fhi = f(hi).f;
        if (fhi <= 0.0) {
            return refine(f, lo, flo, hi, fhi);
        }
        lo = hi;
        flo = fhi;
    }
    return {};
}

} // namespace

Plot plot_transit(const orbit::OrbitSystem& system, const PlotRequest& request) {
    Plot plot;
    plot.departure = request.departure;
    plot.arrival = request.departure;
    plot.flip = request.departure;
    if (!valid_request(system, request)) {
        plot.status = PlotStatus::invalid_request;
        return plot;
    }

    const orbit::StateVector from = request.origin.body.valid()
                                        ? system.world_state(request.origin.body, request.departure)
                                        : request.origin.state;
    plot.start = from.position;
    plot.end = from.position;

    InterceptFn f{system, request, from.position, from.velocity};
    const Root root = solve_intercept(f, request.horizon.seconds);
    plot.evaluations = f.evaluations;
    if (!root.found) {
        plot.status = PlotStatus::no_intercept;
        return plot;
    }

    plot.arrival = request.departure + sim::seconds(root.t);
    plot.duration = sim::seconds(root.t);
    const orbit::StateVector dest = system.world_state(request.destination, plot.arrival);
    plot.end = dest.position;
    plot.distance = distance(plot.end, plot.start);
    plot.match_delta_v = distance(dest.velocity, from.velocity);
    // The root guarantees transit_time <= duration; fly the cheapest profile that takes exactly the
    // (whole-second) duration so the ship and destination coincide at arrival.
    const auto profile =
        profile_for_duration(plot.distance, request.accel, plot.duration.to_seconds_f());
    plot.profile = profile ? *profile : brachistochrone(plot.distance, request.accel);
    plot.peak_speed = plot.profile.peak_speed;
    plot.delta_v = plot.profile.delta_v() + plot.match_delta_v;
    plot.flip = request.departure +
                sim::seconds(static_cast<std::int64_t>(std::llround(plot.profile.flip_time())));
    plot.reaction_mass_used = reaction_mass_for(request.ship, plot.delta_v);
    plot.reaction_mass_left = request.ship.reaction_mass - plot.reaction_mass_used;

    if (request.avoid.valid()) {
        const Vec3 hazard = system.world_position(request.avoid, plot.flip);
        plot.closest_approach = segment_point_distance(plot.start, plot.end, hazard);
    }

    if (plot.closest_approach < request.avoid_radius) {
        plot.status = PlotStatus::path_obstructed;
    } else if (plot.reaction_mass_left < 0.0) {
        plot.status = PlotStatus::insufficient_reaction_mass;
    } else {
        plot.status = PlotStatus::ok;
    }
    return plot;
}

Plot best_departure(const orbit::OrbitSystem& system, const PlotRequest& request,
                    sim::Time window_end, sim::Duration step, Objective objective) {
    const Plot first = plot_transit(system, request);
    if (step.seconds <= 0) {
        return first;
    }
    std::optional<Plot> best;
    if (first.feasible()) {
        best = first;
    }
    PlotRequest r = request;
    for (r.departure = request.departure + step; r.departure <= window_end; r.departure += step) {
        const Plot p = plot_transit(system, r);
        if (!p.feasible()) {
            continue;
        }
        const bool better = !best || (objective == Objective::earliest_arrival
                                          ? p.arrival < best->arrival
                                          : p.delta_v < best->delta_v);
        if (better) {
            best = p;
        }
    }
    return best ? *best : first;
}

orbit::StateVector ship_state(const Plot& plot, sim::Time t) {
    const double elapsed = (t - plot.departure).to_seconds_f();
    if (!(plot.distance > 0.0) || elapsed <= 0.0) {
        return {plot.start, {}};
    }
    if (elapsed >= plot.profile.total_time()) {
        return {plot.end, {}}; // exact, so arrival coincides with the destination bit-for-bit
    }
    const PathState s = state_along(plot.profile, elapsed);
    const Vec3 dir = (plot.end - plot.start) / plot.distance;
    return {plot.start + dir * s.distance, dir * s.speed};
}

} // namespace expanse::transit
