#include "scene.hpp"

#include "expanse/orbit.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace viewer {

namespace {

using expanse::Vec3;
using expanse::units::au_m;

LineVertex to_line_vertex(const Vec3& metres) {
    return {static_cast<float>(metres.x / au_m), static_cast<float>(metres.y / au_m),
            static_cast<float>(metres.z / au_m)};
}

// Placeholder palette until bodies carry their own look (repo-zvv9).
Rgba body_color(std::string_view key, expanse::BodyKind kind) {
    struct Named {
        std::string_view key;
        Rgba color;
    };
    static constexpr std::array named{
        Named{"sun", {1.00F, 0.90F, 0.62F, 1.0F}},     Named{"mercury", {0.66F, 0.62F, 0.58F, 1.0F}},
        Named{"venus", {0.93F, 0.84F, 0.60F, 1.0F}},   Named{"earth", {0.35F, 0.60F, 1.00F, 1.0F}},
        Named{"mars", {0.93F, 0.42F, 0.27F, 1.0F}},    Named{"jupiter", {0.86F, 0.72F, 0.54F, 1.0F}},
        Named{"saturn", {0.92F, 0.82F, 0.55F, 1.0F}},  Named{"uranus", {0.60F, 0.88F, 0.92F, 1.0F}},
        Named{"neptune", {0.38F, 0.52F, 0.98F, 1.0F}},
    };
    for (const auto& n : named) {
        if (n.key == key) {
            return n.color;
        }
    }
    switch (kind) {
    case expanse::BodyKind::moon:
        return {0.70F, 0.70F, 0.72F, 1.0F};
    case expanse::BodyKind::dwarf_planet:
    case expanse::BodyKind::asteroid:
        return {0.72F, 0.64F, 0.56F, 1.0F};
    default:
        return {0.85F, 0.85F, 0.85F, 1.0F};
    }
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

// One closed orbit as a line strip in the parent's frame, sampled uniformly in eccentric anomaly
// (denser near periapsis, where curvature is highest).
void add_orbit_strip(Scene& scene, const expanse::orbit::Orbit& orbit, const Vec3& parent_position,
                     Rgba color) {
    constexpr int segments = 256;
    const double a = orbit.semi_major_axis;
    const double b = a * orbit.semi_minor_ratio;
    SceneStrip strip{.origin = parent_position,
                     .first = static_cast<std::uint32_t>(scene.line_vertices.size()),
                     .count = segments + 1,
                     .color = color,
                     .extent_m = orbit.apoapsis()};
    for (int i = 0; i <= segments; ++i) {
        const double ecc_anomaly = expanse::units::two_pi * static_cast<double>(i % segments) / segments;
        const Vec3 local = orbit.p_hat * (a * (std::cos(ecc_anomaly) - orbit.eccentricity)) +
                           orbit.q_hat * (b * std::sin(ecc_anomaly));
        scene.line_vertices.push_back(to_line_vertex(local));
    }
    scene.strips.push_back(strip);
}

void add_segment_strip(Scene& scene, const Vec3& from, const Vec3& to, Rgba color) {
    const SceneStrip strip{.origin = from,
                           .first = static_cast<std::uint32_t>(scene.line_vertices.size()),
                           .count = 2,
                           .color = color,
                           .extent_m = expanse::distance(from, to)};
    scene.line_vertices.push_back({});
    scene.line_vertices.push_back(to_line_vertex(to - from));
    scene.strips.push_back(strip);
}

// ---- small double-precision matrix helpers (column-major, like Mat4) ----

using Mat4d = std::array<double, 16>;

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

struct CameraBasis {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
};

CameraBasis basis(const Camera& camera) {
    const Vec3 forward = expanse::normalized(camera.target - camera.eye());
    Vec3 right = expanse::cross(forward, Vec3{0.0, 0.0, 1.0});
    if (expanse::length_squared(right) < 1e-24) { // looking straight along the pole
        right = Vec3{1.0, 0.0, 0.0};
    }
    right = expanse::normalized(right);
    return {right, expanse::cross(right, forward), forward};
}

// Rotation only: positions are already relative to the eye.
Mat4d view_matrix(const CameraBasis& b) {
    return {b.right.x, b.up.x, -b.forward.x, 0.0, //
            b.right.y, b.up.y, -b.forward.y, 0.0, //
            b.right.z, b.up.z, -b.forward.z, 0.0, //
            0.0,       0.0,    0.0,          1.0};
}

// Right-handed, clip depth in [0, 1] (SDL GPU's convention on every backend), infinite far plane.
Mat4d projection_matrix(const Camera& camera, double aspect) {
    const double f = 1.0 / std::tan(camera.fov_y_rad / 2.0);
    const double near = camera.near_m / scene_unit_m;
    return {f / aspect, 0.0, 0.0,   0.0,  //
            0.0,        f,   0.0,   0.0,  //
            0.0,        0.0, -1.0,  -1.0, //
            0.0,        0.0, -near, 0.0};
}

double focal_px(const Camera& camera, double height) { return (height / 2.0) / std::tan(camera.fov_y_rad / 2.0); }

std::array<float, 4> relative_to(const Vec3& world, const Vec3& eye, double w) {
    const Vec3 rel = (world - eye) / scene_unit_m;
    return {static_cast<float>(rel.x), static_cast<float>(rel.y), static_cast<float>(rel.z),
            static_cast<float>(w)};
}

} // namespace

Scene build_scene(const ViewSnapshot& snapshot) {
    Scene scene;
    scene.time = snapshot.time;
    const expanse::Content& content = *snapshot.content;
    const expanse::orbit::OrbitSystem& orbits = content.orbits();
    const std::vector<expanse::orbit::StateVector> states = orbits.evaluate(snapshot.time);
    auto position_of = [&](expanse::orbit::BodyId id) { return states[id.value].position; };

    std::vector<SceneSprite> stars;
    std::vector<SceneSprite> planets;
    std::vector<SceneSprite> minor;
    for (auto [id, body] : content.table<expanse::BodyDef>()) {
        const expanse::orbit::BodyId oid = content.orbit_of(id);
        const Rgba color = body_color(content.table<expanse::BodyDef>().key(id), body.kind);
        SceneSprite sprite{.position = position_of(oid), .radius_m = body.radius_km * 1000.0, .color = color};
        float orbit_alpha = 0.3F;
        switch (body.kind) {
        case expanse::BodyKind::star:
            sprite.min_px = 7.0F;
            sprite.glow = 1.0F;
            stars.push_back(sprite);
            break;
        case expanse::BodyKind::planet:
            sprite.min_px = 3.5F;
            sprite.glow = 0.3F;
            orbit_alpha = 0.45F;
            planets.push_back(sprite);
            break;
        case expanse::BodyKind::moon:
            sprite.min_px = 1.5F;
            orbit_alpha = 0.25F;
            minor.push_back(sprite);
            break;
        case expanse::BodyKind::dwarf_planet:
        case expanse::BodyKind::asteroid:
            sprite.min_px = 2.0F;
            minor.push_back(sprite);
            break;
        }
        if (!orbits.is_fixed(oid)) {
            add_orbit_strip(scene, orbits.orbit(oid), position_of(orbits.parent(oid)),
                            with_alpha(color, orbit_alpha));
        }
    }

    std::vector<SceneSprite> stations;
    for (auto [id, station] : content.table<expanse::StationDef>()) {
        const expanse::orbit::BodyId oid = content.orbit_of(id);
        const Rgba color = faction_color(station.faction);
        stations.push_back({.position = position_of(oid),
                            .color = color,
                            .min_px = 3.0F,
                            .shape = SpriteShape::diamond});
        if (station.orbit && !orbits.is_fixed(oid)) {
            add_orbit_strip(scene, orbits.orbit(oid), position_of(orbits.parent(oid)), with_alpha(color, 0.3F));
        }
    }

    std::vector<SceneSprite> ships;
    if (snapshot.world) {
        const expanse::World& world = *snapshot.world;
        constexpr Rgba player_color{0.45F, 1.00F, 0.55F, 1.0F};
        constexpr Rgba other_color{0.80F, 0.80F, 0.80F, 1.0F};
        for (auto [id, ship] : world.ships) {
            const bool mine = ship.owner == world.player;
            const Rgba color = mine ? player_color : other_color;
            const Vec3 position = expanse::ship_position(content, world, ship);
            ships.push_back({.position = position, .color = color, .min_px = 2.5F});
            if (const auto* underway = std::get_if<expanse::Underway>(&ship.location)) {
                add_segment_strip(scene, underway->start, underway->end, with_alpha(color, 0.6F));
            }
            if (snapshot.hints.focus_ship == id) {
                ships.push_back({.position = position, .color = color, .min_px = 8.0F, .shape = SpriteShape::ring});
            }
        }
    }

    if (const auto& preview = snapshot.hints.plot_preview) {
        const Rgba color = preview->feasible() ? Rgba{1.0F, 0.80F, 0.25F, 0.85F} : Rgba{1.0F, 0.30F, 0.25F, 0.85F};
        add_segment_strip(scene, preview->plot.start, preview->plot.end, color);
        ships.push_back({.position = preview->plot.end, .color = color, .min_px = 6.0F, .shape = SpriteShape::ring});
    }

    // Painter's order: glow first so everything else stays readable on top of it.
    for (auto* group : {&stars, &planets, &minor, &stations, &ships}) {
        scene.sprites.insert(scene.sprites.end(), group->begin(), group->end());
    }
    return scene;
}

Vec3 Camera::eye() const {
    const double c = std::cos(pitch_rad);
    return target + Vec3{c * std::cos(yaw_rad), c * std::sin(yaw_rad), std::sin(pitch_rad)} * distance_m;
}

void prepare_frame(const Scene& scene, const Camera& camera, float width, float height, FrameData& out) {
    const Vec3 eye = camera.eye();
    const Mat4d vp = multiply(projection_matrix(camera, static_cast<double>(width) / static_cast<double>(height)),
                              view_matrix(basis(camera)));
    for (std::size_t i = 0; i < 16; ++i) {
        out.frame.view_proj.m[i] = static_cast<float>(vp[i]);
    }
    const double focal = focal_px(camera, static_cast<double>(height));
    out.frame.viewport = {width, height, static_cast<float>(focal), 0.0F};

    out.sprites.clear();
    for (const SceneSprite& s : scene.sprites) {
        out.sprites.push_back({.position_radius = relative_to(s.position, eye, s.radius_m / scene_unit_m),
                               .color = s.color,
                               .style = {s.min_px, static_cast<float>(s.shape), s.glow, 0.0F}});
    }

    out.strips.clear();
    constexpr double min_extent_px = 3.0; // smaller strips are noise around their parent
    for (const SceneStrip& strip : scene.strips) {
        const double dist = std::max(expanse::distance(strip.origin, eye), camera.near_m);
        if (strip.extent_m / dist * focal < min_extent_px) {
            continue;
        }
        out.strips.push_back({.first = strip.first,
                              .count = strip.count,
                              .uniforms = {.origin = relative_to(strip.origin, eye, 0.0), .color = strip.color}});
    }
}

std::optional<std::array<double, 2>> project_to_pixels(const Camera& camera, expanse::Vec3 world, double width,
                                                       double height) {
    const CameraBasis b = basis(camera);
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
