#include <doctest/doctest.h>

#include "expanse/content.hpp"
#include "expanse/scenario.hpp"
#include "expanse/ships.hpp"
#include "scene.hpp"

#include <cmath>
#include <memory>

using namespace viewer;

namespace {

std::shared_ptr<const expanse::Content> game_content() {
    static const std::shared_ptr<const expanse::Content> content = [] {
        sim::Diagnostics diags;
        std::shared_ptr<const expanse::Content> c = expanse::Content::load(BELTER_DATA_DIR, diags);
        if (!c) {
            FAIL(diags.to_string());
        }
        return c;
    }();
    return content;
}

std::shared_ptr<const ViewSnapshot> new_game_snapshot(ViewHints hints = {}) {
    const auto content = game_content();
    const std::string scenario = content->table<expanse::ScenarioDef>().keys().front();
    return make_snapshot(content, expanse::new_game(*content, scenario, 1), std::move(hints));
}

double norm(float x, float y, float z) {
    const expanse::Vec3 v{static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)};
    return expanse::length(v);
}

expanse::ShipId player_ship(const expanse::World& world) {
    for (auto [id, ship] : world.ships) {
        if (ship.owner == world.player) {
            return id;
        }
    }
    return {};
}

} // namespace

TEST_CASE("build_scene has a sprite per body station and ship and an orbit per orbiting body") {
    const auto snap = new_game_snapshot();
    const expanse::Content& c = *snap->content;
    const Scene scene = build_scene(*snap);

    const std::size_t bodies = c.table<expanse::BodyDef>().size();
    const std::size_t stations = c.table<expanse::StationDef>().size();
    CHECK(scene.sprites.size() == bodies + stations + snap->world->ships.size());
    CHECK(scene.strips.size() >= bodies - 1); // every body but the Sun has an orbit
    for (const SceneStrip& s : scene.strips) {
        CHECK(s.first + s.count <= scene.line_vertices.size());
    }
    // The Sun is drawn first (its glow sits under everything else) at the origin.
    CHECK(scene.sprites.front().glow > 0.0F);
    CHECK(expanse::length(scene.sprites.front().position) == doctest::Approx(0.0));
}

TEST_CASE("orbit strips close and lie on the orbit") {
    const auto snap = new_game_snapshot();
    const expanse::Content& c = *snap->content;
    const Scene scene = build_scene(*snap);
    const auto earth = c.orbit_of(c.find<expanse::BodyDef>("earth"));
    const expanse::orbit::Orbit& orbit = c.orbits().orbit(earth);

    bool found = false;
    for (const SceneStrip& s : scene.strips) {
        if (std::abs(s.extent_m - orbit.apoapsis()) > 1.0) {
            continue;
        }
        found = true;
        const LineVertex& first = scene.line_vertices[s.first];
        const LineVertex& last = scene.line_vertices[s.first + s.count - 1];
        CHECK(first.x == last.x);
        CHECK(first.y == last.y);
        for (std::uint32_t i = s.first; i < s.first + s.count; ++i) {
            const LineVertex& v = scene.line_vertices[i];
            const double r_au = norm(v.x, v.y, v.z);
            CHECK(r_au >= expanse::units::to_au(orbit.periapsis()) - 1e-6);
            CHECK(r_au <= expanse::units::to_au(orbit.apoapsis()) + 1e-6);
        }
    }
    CHECK(found);
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

TEST_CASE("prepare_frame matches the projection and culls sub-pixel orbits") {
    const auto snap = new_game_snapshot();
    const Scene scene = build_scene(*snap);
    const Camera camera;
    FrameData frame;
    prepare_frame(scene, camera, 1280.0F, 720.0F, frame);

    REQUIRE(frame.sprites.size() == scene.sprites.size());
    CHECK(frame.strips.size() < scene.strips.size()); // moon and station orbits vanish at this range
    CHECK(frame.strips.size() >= 8);                  // planet orbits don't

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
    CHECK(z / w < 1.0F);
    // The eye is `distance` from the Sun.
    const double dist_au = norm(p[0], p[1], p[2]);
    CHECK(dist_au == doctest::Approx(camera.distance_m / expanse::units::au_m).epsilon(1e-6));
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
    CHECK(b.sprites.size() == a.sprites.size() + 2); // focus ring + destination marker
    CHECK(b.strips.size() == a.strips.size() + 1);   // the course line
    CHECK(b.sprites.back().shape == SpriteShape::ring);
}
