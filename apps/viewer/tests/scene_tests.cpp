#include <doctest/doctest.h>

#include "expanse/content.hpp"
#include "expanse/scenario.hpp"
#include "expanse/ships.hpp"
#include "pick.hpp"
#include "scene.hpp"
#include "test_support.hpp"

#include <cmath>
#include <memory>

using namespace viewer;
using namespace viewer::test;
using expanse::Vec3;
using expanse::units::au_m;

namespace {

double norm(const LineVertex& v) {
    return expanse::length(Vec3{static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)});
}

Vec3 to_vec(const LineVertex& v) {
    return Vec3{static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)} * au_m;
}

// Applies a column-major matrix to (x, y, z, 1).
std::array<double, 4> transform(const Mat4d& m, double x, double y, double z) {
    std::array<double, 4> r{};
    for (std::size_t row = 0; row < 4; ++row) {
        r[row] = m[row] * x + m[4 + row] * y + m[8 + row] * z + m[12 + row];
    }
    return r;
}

Camera looking_at(const SceneObject& o, double distance_m) {
    Camera c;
    c.target = o.position;
    c.distance_m = distance_m;
    return c;
}

} // namespace

TEST_CASE("build_scene has an object per body station and ship and an orbit per orbiting body") {
    const auto snap = new_game_snapshot(focus_hints());
    const expanse::Content& c = *snap->content;
    const Scene scene = build_scene(*snap);

    const std::size_t bodies = c.table<expanse::BodyDef>().size();
    const std::size_t stations = c.table<expanse::StationDef>().size();
    CHECK(scene.objects.size() == bodies + stations + snap->world->ships.size());
    CHECK(scene.orbits.size() >= bodies - 1); // every body but the Sun has an orbit
    // The Sun is drawn first (its glow sits under everything else) at the origin.
    CHECK(scene.objects.front().emissive);
    CHECK(scene.objects.front().key == "sun");
    CHECK(expanse::length(scene.objects.front().position) == doctest::Approx(0.0));

    const SceneObject* mars = scene.find_key("mars");
    REQUIRE(mars != nullptr);
    CHECK(mars->color.r == doctest::Approx(0xc1 / 255.0));
    CHECK(mars->color.g == doctest::Approx(0x44 / 255.0));
    CHECK(mars->radius_m == doctest::Approx(3389.5e3));
    CHECK(mars->min_distance_m == doctest::Approx(2.0 * 3389.5e3));
    CHECK(mars->frame_distance_m > 2.0 * 23460e3); // shows Deimos' orbit

    // The player's ship is docked at Ceres Station, on Ceres: both keep the camera off Ceres.
    REQUIRE(scene.focus_ship.has_value());
    const SceneObject* ship = scene.find_key("ship");
    const SceneObject* ceres = scene.find_key("ceres");
    REQUIRE(ship != nullptr);
    REQUIRE(ceres != nullptr);
    CHECK(ship->ref == *scene.focus_ship);
    CHECK(ship->min_distance_m >= 2.0 * ceres->radius_m);
    CHECK(scene.find(ship->ref) == ship);
}

TEST_CASE("orbit samples close and lie on the orbit") {
    const auto snap = new_game_snapshot();
    const expanse::Content& c = *snap->content;
    const auto earth = c.orbit_of(c.find<expanse::BodyDef>("earth"));
    const expanse::orbit::Orbit& orbit = c.orbits().orbit(earth);

    std::vector<LineVertex> v;
    const Vec3 eye{}; // at the Sun: camera-relative == heliocentric
    const std::uint32_t n = sample_orbit(orbit, Vec3{}, eye, 900.0, 0.3, v);
    REQUIRE(n == v.size());
    CHECK(n >= 97);
    CHECK(v.front().x == v.back().x);
    CHECK(v.front().y == v.back().y);
    CHECK(v.front().z == v.back().z);
    for (const LineVertex& p : v) {
        CHECK(norm(p) >= expanse::units::to_au(orbit.periapsis()) - 1e-6);
        CHECK(norm(p) <= expanse::units::to_au(orbit.apoapsis()) + 1e-6);
    }
}

TEST_CASE("orbit sampling is dense next to the camera and cheap far away") {
    const auto snap = new_game_snapshot();
    const Scene scene = build_scene(*snap);
    const expanse::Content& c = *snap->content;
    const auto ceres_orbit = c.orbits().orbit(c.orbit_of(c.find<expanse::BodyDef>("ceres")));
    const SceneObject* ceres = scene.find_key("ceres");
    REQUIRE(ceres != nullptr);

    // A camera 5000 km above Ceres, i.e. right next to Ceres' orbit.
    const Vec3 eye = ceres->position + Vec3{0.0, 0.0, 5.0e6};
    std::vector<LineVertex> v;
    const std::uint32_t n = sample_orbit(ceres_orbit, Vec3{}, eye, 900.0, 0.3, v);
    CHECK(n < 4000); // grows with log(size / distance), not size / distance

    double nearest = 1e300;
    for (std::size_t i = 0; i + 1 < v.size(); ++i) {
        const Vec3 a = to_vec(v[i]);
        const Vec3 b = to_vec(v[i + 1]);
        const double dist = std::min(expanse::length(a), expanse::length(b));
        nearest = std::min(nearest, dist);
        // Segments near the eye are short compared with their distance, so perspective can't
        // bend them visibly; the tolerance on float narrowing is a few metres.
        CHECK(expanse::distance(a, b) <= 0.5 * dist * 1.1 + 1.0e5);
    }
    CHECK(nearest < 1.0e7); // the strip really passes by the camera
}

TEST_CASE("the default camera centres the Sun and keeps the inner system in view") {
    const auto snap = new_game_snapshot();
    const expanse::Content& c = *snap->content;
    const Camera camera;
    const auto sun = project_to_pixels(camera, {}, 1280, 720);
    REQUIRE(sun.has_value());
    CHECK((*sun)[0] == doctest::Approx(640.0));
    CHECK((*sun)[1] == doctest::Approx(360.0));

    for (const char* key : {"mercury", "venus", "earth", "mars", "jupiter"}) {
        const auto body = c.orbit_of(c.find<expanse::BodyDef>(key));
        const auto px = project_to_pixels(camera, c.orbits().world_position(body, snap->time), 1280, 720);
        REQUIRE(px.has_value());
        CHECK((*px)[0] > 0.0);
        CHECK((*px)[0] < 1280.0);
        CHECK((*px)[1] > 0.0);
        CHECK((*px)[1] < 720.0);
    }
}

TEST_CASE("reversed-Z projection maps the near plane to 1 and infinity to 0") {
    const double near = 1000.0 / au_m;
    const Mat4d p = reversed_z_projection(expanse::units::deg(45.0), 16.0 / 9.0, near);
    auto depth = [&](double distance) {
        const auto clip = transform(p, 0.0, 0.0, -distance); // straight ahead is -z
        return clip[2] / clip[3];
    };
    CHECK(depth(near) == doctest::Approx(1.0));
    CHECK(depth(1e30) == doctest::Approx(0.0));
    CHECK(depth(1e30) >= 0.0);
    // Monotonic and still distinct in float from 1 km to 40 AU.
    double previous = 2.0;
    for (const double km : {1.0, 10.0, 1e3, 1e5, 1e7, 1e9, 1.5e8 * 40.0}) {
        const double d = depth(km * 1000.0 / au_m);
        CHECK(d < previous);
        CHECK(static_cast<float>(d) > 0.0F);
        previous = d;
    }
    // Two points 0.1% apart at 40 AU land on different float depths.
    const auto far_a = static_cast<float>(depth(40.0));
    const auto far_b = static_cast<float>(depth(40.04));
    CHECK(far_a != far_b);
    // A point behind the camera has negative w and is clipped.
    CHECK(transform(p, 0.0, 0.0, 1.0)[3] < 0.0);
}

TEST_CASE("camera-relative conversion keeps sub-metre precision at 30 AU") {
    const Vec3 eye{30.0 * au_m, 1.0 * au_m, 0.1 * au_m};
    const Vec3 offset{0.25, -0.5, 0.125}; // metres
    const auto rel = camera_relative(eye + offset, eye);
    CHECK(static_cast<double>(rel[0]) * au_m == doctest::Approx(0.25).epsilon(1e-3));
    CHECK(static_cast<double>(rel[1]) * au_m == doctest::Approx(-0.5).epsilon(1e-3));
    CHECK(static_cast<double>(rel[2]) * au_m == doctest::Approx(0.125).epsilon(1e-3));
    // Narrowing first and subtracting in float quantises to ~285 km (float's spacing near 30):
    // a 1 km offset is lost entirely.
    const float naive = static_cast<float>((eye.x + 1000.0) / au_m) - static_cast<float>(eye.x / au_m);
    CHECK(std::abs(static_cast<double>(naive) * au_m - 1000.0) > 900.0);
}

TEST_CASE("prepare_frame matches the projection and fades small orbits") {
    const auto snap = new_game_snapshot();
    const Scene scene = build_scene(*snap);
    const Camera camera;
    FrameData frame;
    prepare_frame(scene, camera, 1280.0F, 720.0F, frame);

    CHECK(frame.spheres.empty()); // everything is a dot from 9 AU
    CHECK(frame.sprites.size() >= scene.objects.size() - 2); // a few may hide behind the Sun
    CHECK(frame.strips.size() < scene.orbits.size() + scene.segments.size()); // moon orbits fade out
    CHECK(frame.strips.size() >= 8);                                          // planet orbits don't

    // Apply view_proj to the Sun's camera-relative position: it must land at the centre.
    const auto& p = frame.sprites.front().position_radius;
    const auto& m = frame.frame.view_proj.m;
    const float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    const float y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    const float z = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
    const float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
    REQUIRE(w > 0.0F);
    CHECK(x / w == doctest::Approx(0.0).epsilon(1e-5));
    CHECK(y / w == doctest::Approx(0.0).epsilon(1e-5));
    CHECK(z / w > 0.0F);
    CHECK(z / w < 1e-6F); // reversed-Z: 9 AU is almost at infinity
    // The eye is `distance` from the Sun, and the frame's Sun direction says so too.
    const double dist_au = expanse::length(Vec3{p[0], p[1], p[2]});
    CHECK(dist_au == doctest::Approx(camera.distance_m / au_m).epsilon(1e-6));
    CHECK(frame.frame.sun[0] == doctest::Approx(p[0]));

    // Reuse: a second frame of the same view allocates nothing new.
    const auto* sprites = frame.sprites.data();
    const auto* lines = frame.line_vertices.data();
    prepare_frame(scene, camera, 1280.0F, 720.0F, frame);
    CHECK(frame.sprites.data() == sprites);
    CHECK(frame.line_vertices.data() == lines);
}

TEST_CASE("bodies turn from dots into lit spheres up close") {
    const auto snap = new_game_snapshot();
    const Scene scene = build_scene(*snap);
    const SceneObject* earth = scene.find_key("earth");
    REQUIRE(earth != nullptr);

    FrameData frame;
    prepare_frame(scene, looking_at(*earth, 5.0 * earth->radius_m), 1280.0F, 720.0F, frame);
    REQUIRE(frame.spheres.size() >= 1);
    bool earth_sphere = false;
    for (const SphereInstance& s : frame.spheres) {
        if (std::abs(static_cast<double>(s.position_radius[3]) * au_m - earth->radius_m) < 1.0) {
            earth_sphere = true;
            CHECK(s.color.a == doctest::Approx(1.0));
            CHECK(s.params[0] == 0.0F); // lit, not emissive
        }
    }
    CHECK(earth_sphere);

    CHECK(sphere_weight(*earth, 0.5) == 0.0F);
    CHECK(sphere_weight(*earth, 1000.0) == 1.0F);
    const float mid = sphere_weight(*earth, 1.5 * static_cast<double>(earth->min_px));
    CHECK(mid > 0.0F);
    CHECK(mid < 1.0F);
}

TEST_CASE("markers behind a body are hidden but a port at its centre is not") {
    const auto snap = new_game_snapshot();
    const Scene scene = build_scene(*snap);
    const SceneObject* earth = scene.find_key("earth");
    REQUIRE(earth != nullptr);
    const Vec3 eye = earth->position + Vec3{0.0, 0.0, 3.0 * earth->radius_m};
    CHECK(occluded(scene, eye, earth->position - Vec3{0.0, 0.0, 3.0 * earth->radius_m}));
    CHECK_FALSE(occluded(scene, eye, earth->position)); // inside: a surface port
    CHECK_FALSE(occluded(scene, eye, earth->position + Vec3{3.0 * earth->radius_m, 0.0, 0.0}));
    CHECK_FALSE(occluded(scene, eye, earth->position, earth)); // never by itself
}

TEST_CASE("view hints add a focus ring and a plotted course") {
    const auto plain = new_game_snapshot();
    const expanse::World& world = *plain->world;
    const expanse::ShipId ship = player_ship(world);
    REQUIRE_FALSE(ship.is_null());

    const auto& stations = plain->content->table<expanse::StationDef>();
    const auto destination = plain->content->find<expanse::StationDef>(stations.keys().back());
    ViewHints hints{.focus_ship = ship,
                    .plot_preview = expanse::plot_course(*plain->content, world, ship, destination, {})};
    const auto hinted = new_game_snapshot(hints);

    const Scene a = build_scene(*plain);
    const Scene b = build_scene(*hinted);
    CHECK(b.markers.size() == a.markers.size() + 2); // focus ring + destination marker
    CHECK(b.segments.size() == a.segments.size() + 1); // the course line
    CHECK(b.markers.back().shape == SpriteShape::ring);
    CHECK_FALSE(a.focus_ship.has_value());
    CHECK(b.focus_ship.has_value());
}

TEST_CASE("picking finds the nearest object under the cursor") {
    const auto snap = new_game_snapshot(focus_hints());
    const Scene scene = build_scene(*snap);
    const SceneObject* earth = scene.find_key("earth");
    const SceneObject* luna = scene.find_key("luna");
    REQUIRE(earth != nullptr);
    REQUIRE(luna != nullptr);

    const Camera camera = looking_at(*earth, 1.5 * expanse::distance(earth->position, luna->position));
    const auto centre = pick(scene, camera, 640.0, 360.0, 1280.0, 720.0);
    REQUIRE(centre.has_value());
    CHECK(centre->ref == earth->ref);
    CHECK(centre->distance_px == 0.0);

    const auto luna_px = project_to_pixels(camera, luna->position, 1280.0, 720.0);
    REQUIRE(luna_px.has_value());
    const auto near_luna = pick(scene, camera, (*luna_px)[0] + 5.0, (*luna_px)[1], 1280.0, 720.0);
    REQUIRE(near_luna.has_value());
    CHECK(near_luna->ref == luna->ref);
    CHECK(near_luna->distance_px < 5.0);

    // Empty sky: nothing within reach.
    CHECK_FALSE(pick(scene, camera, 5.0, 5.0, 1280.0, 720.0, 4.0).has_value());

    // Ceres, Ceres Station and the ship docked there share one spot: the body wins.
    const SceneObject* ceres = scene.find_key("ceres");
    REQUIRE(ceres != nullptr);
    const auto docked = pick(scene, looking_at(*ceres, 0.001 * au_m), 640.0, 360.0, 1280.0, 720.0);
    REQUIRE(docked.has_value());
    CHECK(docked->ref == ceres->ref);

    // A marker on a planet's disc wins over the planet: it is centred nearer the cursor.
    const SceneObject* earth_orbital = scene.find_key("earth_orbital");
    REQUIRE(earth_orbital != nullptr);
    Camera close = looking_at(*earth, 6.0 * earth->radius_m);
    close.target = earth_orbital->position; // aim at the station, with Earth filling the view
    const auto on_disc = pick(scene, close, 640.0, 360.0, 1280.0, 720.0);
    REQUIRE(on_disc.has_value());
    CHECK(on_disc->ref == earth_orbital->ref);
}
