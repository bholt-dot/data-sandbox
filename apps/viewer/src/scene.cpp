#include "scene.hpp"

#include "expanse/orbit.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace viewer {

namespace {

using expanse::Vec3;
using expanse::units::au_m;

constexpr double point_min_distance_m = 2.0e6;   // closest approach to a station or ship
constexpr double sun_frame_distance_m = 9.0 * au_m; // the home view

Rgba from_hex(std::uint32_t rgb) {
    return {static_cast<float>((rgb >> 16) & 0xffU) / 255.0F, static_cast<float>((rgb >> 8) & 0xffU) / 255.0F,
            static_cast<float>(rgb & 0xffU) / 255.0F, 1.0F};
}

// Fallback palette for bodies without a `color` in the game data.
Rgba body_color(const expanse::BodyDef& body) {
    if (body.color) {
        if (const auto rgb = expanse::parse_hex_color(*body.color)) {
            return from_hex(*rgb);
        }
    }
    switch (body.kind) {
    case expanse::BodyKind::star:
        return {1.00F, 0.90F, 0.62F, 1.0F};
    case expanse::BodyKind::planet:
        return {0.85F, 0.80F, 0.72F, 1.0F};
    case expanse::BodyKind::moon:
        return {0.70F, 0.70F, 0.72F, 1.0F};
    case expanse::BodyKind::dwarf_planet:
    case expanse::BodyKind::asteroid:
        break;
    }
    return {0.72F, 0.64F, 0.56F, 1.0F};
}

Rgba faction_color(expanse::Faction faction) {
    switch (faction) {
    case expanse::Faction::earth:
        return {0.45F, 0.72F, 1.00F, 1.0F};
    case expanse::Faction::mars:
        return {1.00F, 0.45F, 0.35F, 1.0F};
    case expanse::Faction::belt:
        return {1.00F, 0.76F, 0.30F, 1.0F};
    case expanse::Faction::independent:
        break;
    }
    return {0.78F, 0.78F, 0.84F, 1.0F};
}

Rgba with_alpha(Rgba c, float a) {
    c.a = a;
    return c;
}

double smoothstep(double lo, double hi, double x) {
    const double t = std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

Mat4d multiply(const Mat4d& a, const Mat4d& b) {
    Mat4d r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a[static_cast<std::size_t>(k * 4 + row)] * b[static_cast<std::size_t>(col * 4 + k)];
            }
            r[static_cast<std::size_t>(col * 4 + row)] = sum;
        }
    }
    return r;
}

// Rotation only: positions are already relative to the eye.
Mat4d view_matrix(const CameraBasis& b) {
    return {b.right.x, b.up.x, -b.forward.x, 0.0, //
            b.right.y, b.up.y, -b.forward.y, 0.0, //
            b.right.z, b.up.z, -b.forward.z, 0.0, //
            0.0,       0.0,    0.0,          1.0};
}

std::array<float, 4> relative4(const Vec3& world, const Vec3& eye, double w) {
    const auto rel = camera_relative(world, eye);
    return {rel[0], rel[1], rel[2], static_cast<float>(w)};
}

// Where a station (or a ship docked at one) may be viewed from: out of reach of its host body.
struct PointLimits {
    double min_m = point_min_distance_m;
    double frame_m = 0.01 * au_m;
};

PointLimits point_limits(const Vec3& position, const Vec3& host_position, double host_radius_m) {
    const double offset = expanse::distance(position, host_position);
    if (offset > 20.0 * host_radius_m) {
        return {};
    }
    const double min_m = std::max(offset + 2.0 * host_radius_m, point_min_distance_m);
    return {min_m, 6.0 * min_m};
}

} // namespace

const SceneObject* Scene::find(ObjectRef ref) const {
    for (const SceneObject& o : objects) {
        if (o.ref == ref) {
            return &o;
        }
    }
    return nullptr;
}

const SceneObject* Scene::find_key(std::string_view key) const {
    if (key == "ship") {
        return focus_ship ? find(*focus_ship) : nullptr;
    }
    for (const SceneObject& o : objects) {
        if (o.ref.kind != ObjectKind::ship && o.key == key) {
            return &o;
        }
    }
    return nullptr;
}

Scene build_scene(const ViewSnapshot& snapshot) {
    Scene scene;
    scene.time = snapshot.time;
    const expanse::Content& content = *snapshot.content;
    const expanse::orbit::OrbitSystem& orbits = content.orbits();
    const std::vector<expanse::orbit::StateVector> states = orbits.evaluate(snapshot.time);
    auto position_of = [&](expanse::orbit::BodyId id) { return states[id.value].position; };
    const auto& bodies = content.table<expanse::BodyDef>();

    // A planet is framed with its regular moons; irregular ones (Phoebe) would shrink it to a dot.
    std::vector<double> moon_system_m(bodies.size(), 0.0);
    for (auto [id, body] : bodies) {
        if (body.kind == expanse::BodyKind::moon && body.parent) {
            const double apo = orbits.orbit(content.orbit_of(id)).apoapsis();
            double& system = moon_system_m[body.parent->index];
            if (apo < 100.0 * bodies[*body.parent].radius_km * 1000.0) {
                system = std::max(system, apo);
            }
        }
    }

    for (auto [id, body] : bodies) {
        const expanse::orbit::BodyId oid = content.orbit_of(id);
        const double radius_m = body.radius_km * 1000.0;
        SceneObject object{.ref = {ObjectKind::body, id.index, 0},
                           .key = std::string(bodies.key(id)),
                           .name = body.name,
                           .position = position_of(oid),
                           .radius_m = radius_m,
                           .color = body_color(body),
                           .min_distance_m = std::max(2.0 * radius_m, 1000.0)};
        float orbit_alpha = 0.3F;
        if (body.parent && bodies[*body.parent].kind != expanse::BodyKind::star) {
            object.host = ObjectRef{ObjectKind::body, body.parent->index, 0};
        }
        switch (body.kind) {
        case expanse::BodyKind::star:
            object.cls = ObjectClass::star;
            object.min_px = 7.0F;
            object.glow = 1.0F;
            object.emissive = true;
            object.frame_distance_m = sun_frame_distance_m;
            break;
        case expanse::BodyKind::planet:
            object.cls = ObjectClass::planet;
            object.min_px = 3.5F;
            object.glow = 0.3F;
            object.frame_distance_m = std::max(20.0 * radius_m, 2.2 * moon_system_m[id.index]);
            orbit_alpha = 0.45F;
            break;
        case expanse::BodyKind::moon:
            object.cls = ObjectClass::moon;
            object.min_px = 1.5F;
            object.frame_distance_m = 8.0 * radius_m;
            orbit_alpha = 0.25F;
            break;
        case expanse::BodyKind::dwarf_planet:
        case expanse::BodyKind::asteroid:
            object.cls = body.kind == expanse::BodyKind::dwarf_planet ? ObjectClass::dwarf_planet : ObjectClass::asteroid;
            object.min_px = 2.0F;
            object.frame_distance_m = 8.0 * radius_m;
            break;
        }
        object.frame_distance_m = std::max(object.frame_distance_m, 1.5 * object.min_distance_m);
        if (!orbits.is_fixed(oid)) {
            scene.orbits.push_back({.orbit = orbits.orbit(oid),
                                    .parent_position = position_of(orbits.parent(oid)),
                                    .color = with_alpha(object.color, orbit_alpha)});
        }
        scene.objects.push_back(std::move(object));
    }
    // Painter's order for the sprites: the Sun's glow under everything, then planets, then the rest.
    std::stable_partition(scene.objects.begin(), scene.objects.end(), [](const SceneObject& o) { return o.emissive; });
    std::stable_partition(scene.objects.begin(), scene.objects.end(),
                          [](const SceneObject& o) { return o.emissive || o.glow > 0.0F; });

    const auto& stations = content.table<expanse::StationDef>();
    std::vector<PointLimits> station_limits(stations.size());
    for (auto [id, station] : stations) {
        const expanse::orbit::BodyId oid = content.orbit_of(id);
        const Vec3 position = position_of(oid);
        const PointLimits limits = point_limits(position, position_of(content.orbit_of(station.body)),
                                                bodies[station.body].radius_km * 1000.0);
        station_limits[id.index] = limits;
        const Rgba color = faction_color(station.faction);
        std::optional<ObjectRef> host;
        if (bodies[station.body].kind != expanse::BodyKind::star) {
            host = ObjectRef{ObjectKind::body, station.body.index, 0};
        }
        scene.objects.push_back({.ref = {ObjectKind::station, id.index, 0},
                                 .key = std::string(stations.key(id)),
                                 .name = station.name,
                                 .position = position,
                                 .color = color,
                                 .min_px = 3.0F,
                                 .shape = SpriteShape::diamond,
                                 .cls = ObjectClass::station,
                                 .host = host,
                                 .min_distance_m = limits.min_m,
                                 .frame_distance_m = limits.frame_m});
        if (station.orbit && !orbits.is_fixed(oid)) {
            scene.orbits.push_back({.orbit = orbits.orbit(oid),
                                    .parent_position = position_of(orbits.parent(oid)),
                                    .color = with_alpha(color, 0.3F)});
        }
    }

    if (snapshot.world) {
        const expanse::World& world = *snapshot.world;
        constexpr Rgba player_color{0.45F, 1.00F, 0.55F, 1.0F};
        constexpr Rgba other_color{0.80F, 0.80F, 0.80F, 1.0F};
        for (auto [id, ship] : world.ships) {
            const bool mine = ship.owner == world.player;
            const Rgba color = mine ? player_color : other_color;
            const Vec3 position = expanse::ship_position(content, world, ship);
            PointLimits limits;
            std::optional<ObjectRef> host;
            if (const auto* docked = std::get_if<expanse::Docked>(&ship.location)) {
                limits = station_limits[docked->station.index];
                host = ObjectRef{ObjectKind::station, docked->station.index, 0};
            } else if (const auto* underway = std::get_if<expanse::Underway>(&ship.location)) {
                scene.segments.push_back({underway->start, underway->end, with_alpha(color, 0.6F)});
                const double dv = underway->profile.delta_v() / expanse::units::km_m;
                scene.courses.push_back({.from = underway->start,
                                         .to = underway->end,
                                         .flip = flip_point(underway->start, underway->end, underway->profile),
                                         .departure = underway->departure,
                                         .arrival = underway->arrival,
                                         .delta_v_km_s = dv,
                                         .destination = stations[underway->destination].name,
                                         .preview = false,
                                         .feasible = true,
                                         .color = color});
            }
            const ObjectRef ref{ObjectKind::ship, id.index, id.generation};
            scene.objects.push_back({.ref = ref,
                                     .key = {},
                                     .name = ship.name,
                                     .position = position,
                                     .color = color,
                                     .min_px = 2.5F,
                                     .cls = ObjectClass::ship,
                                     .player = mine,
                                     .host = host,
                                     .min_distance_m = limits.min_m,
                                     .frame_distance_m = limits.frame_m});
            if (snapshot.hints.focus_ship == id) {
                scene.focus_ship = ref;
                scene.markers.push_back({.position = position, .color = color, .min_px = 8.0F});
            }
        }
    }

    if (const auto& preview = snapshot.hints.plot_preview) {
        const Rgba color = preview->feasible() ? Rgba{1.0F, 0.80F, 0.25F, 0.85F} : Rgba{1.0F, 0.30F, 0.25F, 0.85F};
        scene.segments.push_back({preview->plot.start, preview->plot.end, color, true});
        scene.markers.push_back({.position = preview->plot.end, .color = color, .min_px = 6.0F});
        std::string destination;
        if (!preview->destination.is_null() && preview->destination.index < stations.size()) {
            destination = stations[preview->destination].name;
        }
        scene.courses.push_back({.from = preview->plot.start,
                                 .to = preview->plot.end,
                                 .flip = flip_point(preview->plot.start, preview->plot.end, preview->plot.profile),
                                 .departure = preview->departure,
                                 .arrival = preview->arrival,
                                 .delta_v_km_s = preview->delta_v_km_s,
                                 .destination = std::move(destination),
                                 .preview = true,
                                 .feasible = preview->feasible(),
                                 .color = color});
    }
    return scene;
}

Vec3 flip_point(const Vec3& start, const Vec3& end, const expanse::transit::BurnProfile& profile) {
    const double length = expanse::distance(start, end);
    if (length <= 0.0) {
        return start;
    }
    const double along = expanse::transit::state_along(profile, profile.flip_time()).distance;
    return start + (end - start) * std::clamp(along / length, 0.0, 1.0);
}

Vec3 Camera::eye() const {
    const double c = std::cos(pitch_rad);
    return target + Vec3{c * std::cos(yaw_rad), c * std::sin(yaw_rad), std::sin(pitch_rad)} * distance_m;
}

CameraBasis camera_basis(const Camera& camera) {
    const double c = std::cos(camera.pitch_rad);
    const Vec3 forward = -Vec3{c * std::cos(camera.yaw_rad), c * std::sin(camera.yaw_rad), std::sin(camera.pitch_rad)};
    Vec3 right = expanse::cross(forward, Vec3{0.0, 0.0, 1.0});
    if (expanse::length_squared(right) < 1e-24) { // looking straight along the pole
        right = Vec3{1.0, 0.0, 0.0};
    }
    right = expanse::normalized(right);
    return {right, expanse::cross(right, forward), forward};
}

Mat4d reversed_z_projection(double fov_y_rad, double aspect, double near_units) {
    // clip.z = near, clip.w = -z_view: depth = near / distance, 1 at the near plane, 0 at infinity.
    const double f = 1.0 / std::tan(fov_y_rad / 2.0);
    return {f / aspect, 0.0, 0.0,        0.0,  //
            0.0,        f,   0.0,        0.0,  //
            0.0,        0.0, 0.0,        -1.0, //
            0.0,        0.0, near_units, 0.0};
}

Mat4d view_projection(const Camera& camera, double aspect) {
    return multiply(reversed_z_projection(camera.fov_y_rad, aspect, camera.near_m / scene_unit_m),
                    view_matrix(camera_basis(camera)));
}

double focal_px(const Camera& camera, double height) { return (height / 2.0) / std::tan(camera.fov_y_rad / 2.0); }

std::array<float, 3> camera_relative(const Vec3& world, const Vec3& eye) {
    const Vec3 rel = (world - eye) / scene_unit_m; // subtract in double, then narrow
    return {static_cast<float>(rel.x), static_cast<float>(rel.y), static_cast<float>(rel.z)};
}

std::uint32_t sample_orbit(const expanse::orbit::Orbit& orbit, const Vec3& parent_position, const Vec3& eye,
                           double focal, double tolerance_px, std::vector<LineVertex>& out) {
    constexpr double two_pi = expanse::units::two_pi;
    constexpr double max_step = two_pi / 96.0;
    constexpr double min_step = 1e-7;
    constexpr std::uint32_t max_vertices = 16384;
    const double a = orbit.semi_major_axis;
    const double b = a * orbit.semi_minor_ratio;
    std::uint32_t count = 0;
    double ecc_anomaly = 0.0;
    for (;;) {
        const double e_eval = ecc_anomaly >= two_pi ? 0.0 : ecc_anomaly; // close the loop exactly
        const Vec3 local = orbit.p_hat * (a * (std::cos(e_eval) - orbit.eccentricity)) +
                           orbit.q_hat * (b * std::sin(e_eval));
        const Vec3 rel = parent_position + local - eye;
        const auto v = camera_relative(parent_position + local, eye);
        out.push_back({v[0], v[1], v[2]});
        ++count;
        if (ecc_anomaly >= two_pi) {
            break;
        }
        // |dr/dE| <= a and |d2r/dE2| <= a, so a chord of dE deviates by at most a dE^2 / 8. The
        // step suits the nearer end of the segment: shrink it while the far end comes closer.
        auto step_at = [&](double dist) {
            const double sag_m = tolerance_px * dist / focal;
            return std::clamp(std::min(std::sqrt(8.0 * sag_m / a), 0.5 * dist / a), min_step, max_step);
        };
        double dist = expanse::length(rel);
        double step = step_at(dist);
        for (int refine = 0; refine < 4; ++refine) {
            const double next = ecc_anomaly + step;
            const Vec3 ahead = parent_position - eye + orbit.p_hat * (a * (std::cos(next) - orbit.eccentricity)) +
                               orbit.q_hat * (b * std::sin(next));
            const double ahead_dist = expanse::length(ahead);
            if (ahead_dist >= dist) {
                break;
            }
            dist = ahead_dist;
            step = std::min(step, step_at(dist));
        }
        if (count + 1 >= max_vertices) {
            step = two_pi;
        }
        ecc_anomaly = std::min(ecc_anomaly + step, two_pi);
    }
    return count;
}

bool occluded(const Scene& scene, const Vec3& eye, const Vec3& point, const SceneObject* self) {
    const Vec3 ray = point - eye;
    const double length = expanse::length(ray);
    if (length <= 0.0) {
        return false;
    }
    const Vec3 dir = ray / length;
    for (const SceneObject& o : scene.objects) {
        if (&o == self || o.radius_m <= 0.0) {
            continue;
        }
        const double r2 = o.radius_m * o.radius_m;
        if (expanse::length_squared(point - o.position) < r2 || expanse::length_squared(eye - o.position) < r2) {
            continue;
        }
        const Vec3 to_centre = o.position - eye;
        const double along = expanse::dot(to_centre, dir);
        const double miss2 = expanse::length_squared(to_centre - dir * along);
        if (miss2 >= r2) {
            continue;
        }
        const double entry = along - std::sqrt(r2 - miss2);
        if (entry > 0.0 && entry < length) {
            return true;
        }
    }
    return false;
}

float sphere_weight(const SceneObject& object, double projected_px) {
    if (object.radius_m <= 0.0) {
        return 0.0F;
    }
    const double lo = static_cast<double>(object.min_px);
    return static_cast<float>(smoothstep(lo, 2.0 * lo, projected_px));
}

void prepare_frame(const Scene& scene, const Camera& camera, float width, float height, FrameData& out) {
    const Vec3 eye = camera.eye();
    const Mat4d vp = view_projection(camera, static_cast<double>(width) / static_cast<double>(height));
    for (std::size_t i = 0; i < 16; ++i) {
        out.frame.view_proj.m[i] = static_cast<float>(vp[i]);
    }
    const double focal = focal_px(camera, static_cast<double>(height));
    out.frame.viewport = {width, height, static_cast<float>(focal), 0.0F};
    out.frame.sun = relative4(Vec3{}, eye, 0.0); // heliocentric coordinates: the Sun is the origin

    out.spheres.clear();
    out.sprites.clear();
    for (const SceneObject& o : scene.objects) {
        const double dist = expanse::distance(o.position, eye);
        if (dist <= 0.0) {
            continue;
        }
        const float sphere = sphere_weight(o, o.radius_m / dist * focal);
        if (sphere > 0.0F) {
            out.spheres.push_back({.position_radius = relative4(o.position, eye, o.radius_m / scene_unit_m),
                                   .color = with_alpha(o.color, o.color.a * sphere),
                                   .params = {o.emissive ? 1.0F : 0.0F, 0.0F, 0.0F, 0.0F}});
        }
        const float core = 1.0F - sphere;
        const float glow = o.emissive ? o.glow : o.glow * core; // a lit sphere has no halo
        if ((core > 0.0F || glow > 0.0F) && !occluded(scene, eye, o.position, &o)) {
            out.sprites.push_back({.position_radius = relative4(o.position, eye, o.radius_m / scene_unit_m),
                                   .color = o.color,
                                   .style = {o.min_px, static_cast<float>(o.shape), glow, core}});
        }
    }
    for (const SceneMarker& m : scene.markers) {
        if (!occluded(scene, eye, m.position)) {
            out.sprites.push_back({.position_radius = relative4(m.position, eye, 0.0),
                                   .color = m.color,
                                   .style = {m.min_px, static_cast<float>(m.shape), 0.0F, 1.0F}});
        }
    }

    out.line_vertices.clear();
    out.strips.clear();
    constexpr double tolerance_px = 0.3;
    for (const SceneOrbit& o : scene.orbits) {
        // Fade orbits far below the current scale (moons seen from across the system).
        const double dist = std::max(expanse::distance(o.parent_position, eye), camera.near_m);
        const double extent_px = o.orbit.apoapsis() / dist * focal;
        const auto fade = static_cast<float>(smoothstep(3.0, 30.0, extent_px));
        if (fade < 0.02F) {
            continue;
        }
        const auto first = static_cast<std::uint32_t>(out.line_vertices.size());
        const std::uint32_t count = sample_orbit(o.orbit, o.parent_position, eye, focal, tolerance_px, out.line_vertices);
        out.strips.push_back({.first = first, .count = count, .uniforms = {with_alpha(o.color, o.color.a * fade)}});
    }
    out.dashes.clear();
    for (const SceneSegment& s : scene.segments) {
        const auto first = static_cast<std::uint32_t>(out.line_vertices.size());
        if (!s.dashed) {
            for (const Vec3& p : {s.from, s.to}) {
                const auto v = camera_relative(p, eye);
                out.line_vertices.push_back({v[0], v[1], v[2]});
            }
            out.strips.push_back({.first = first, .count = 2, .uniforms = {s.color}});
            continue;
        }
        // Dashes of equal length along the line, about dash_px on screen at the line's nearer
        // end (perspective shortens the far ones, like a painted road).
        constexpr double dash_px = 14.0;
        constexpr std::uint32_t max_dashes = 400;
        const double length = expanse::distance(s.from, s.to);
        const double nearest =
            std::max(std::min(expanse::distance(s.from, eye), expanse::distance(s.to, eye)), camera.near_m);
        const double screen_px = length / nearest * focal;
        const auto dashes = static_cast<std::uint32_t>(std::clamp(screen_px / dash_px, 1.0, double{max_dashes}));
        for (std::uint32_t i = 0; i < dashes; ++i) {
            const double t0 = static_cast<double>(i) / static_cast<double>(dashes);
            const double t1 = (static_cast<double>(i) + 0.6) / static_cast<double>(dashes);
            for (const double t : {t0, t1}) {
                const auto v = camera_relative(s.from + (s.to - s.from) * t, eye);
                out.line_vertices.push_back({v[0], v[1], v[2]});
            }
        }
        out.dashes.push_back({.first = first, .count = 2 * dashes, .uniforms = {s.color}});
    }
}

std::optional<std::array<double, 2>> project_to_pixels(const Camera& camera, expanse::Vec3 world, double width,
                                                       double height) {
    const CameraBasis b = camera_basis(camera);
    const Vec3 rel = world - camera.eye();
    const double depth = expanse::dot(rel, b.forward);
    if (depth <= 0.0) {
        return std::nullopt;
    }
    const double focal = focal_px(camera, height);
    return std::array{width / 2.0 + expanse::dot(rel, b.right) / depth * focal,
                      height / 2.0 - expanse::dot(rel, b.up) / depth * focal};
}

} // namespace viewer
