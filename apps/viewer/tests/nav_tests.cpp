#include <doctest/doctest.h>

#include "nav.hpp"
#include "scene.hpp"
#include "test_support.hpp"

#include <cmath>

using namespace viewer;
using namespace viewer::test;
using expanse::Vec3;
using expanse::units::au_m;

namespace {

const Scene& start_scene() {
    static const Scene scene = build_scene(*new_game_snapshot(focus_hints()));
    return scene;
}

void run_for(OrbitCamera& cam, const Scene& scene, double seconds) {
    for (double t = 0.0; t < seconds; t += 1.0 / 60.0) {
        cam.update(1.0 / 60.0, scene);
    }
}

void run_for(Navigator& nav, const Scene& scene, double seconds) { run_for(nav.orbit_camera(), scene, seconds); }

InputEvent key(Key k, bool shift = false) { return {.type = InputEvent::Type::key_down, .key = k, .shift = shift}; }
InputEvent digit(int d) { return {.type = InputEvent::Type::key_down, .key = Key::digit, .digit = d}; }

std::optional<ObjectRef> focus_of(const Navigator& nav) { return nav.orbit_camera().focus(); }

} // namespace

TEST_CASE("ease_in_out is a smooth monotonic ramp") {
    CHECK(ease_in_out(0.0) == 0.0);
    CHECK(ease_in_out(1.0) == 1.0);
    CHECK(ease_in_out(0.5) == doctest::Approx(0.5));
    CHECK(ease_in_out(-1.0) == 0.0);
    CHECK(ease_in_out(2.0) == 1.0);
    double previous = 0.0;
    for (int i = 1; i <= 100; ++i) {
        const double e = ease_in_out(i / 100.0);
        CHECK(e >= previous);
        previous = e;
    }
    // Starts and ends at rest.
    CHECK(ease_in_out(0.01) < 1e-4);
    CHECK(1.0 - ease_in_out(0.99) < 1e-4);
}

TEST_CASE("fly-to converges on the target and zooms out on a long jump") {
    const Scene& scene = start_scene();
    const SceneObject* earth = scene.find_key("earth");
    const SceneObject* jupiter = scene.find_key("jupiter");
    REQUIRE(earth != nullptr);
    REQUIRE(jupiter != nullptr);

    OrbitCamera cam;
    cam.focus_on(*earth, std::nullopt, false);
    cam.update(0.0, scene);
    CHECK(expanse::distance(cam.camera().target, earth->position) == doctest::Approx(0.0));
    CHECK(cam.camera().distance_m == doctest::Approx(earth->frame_distance_m));

    cam.focus_on(*jupiter, std::nullopt, true);
    CHECK(cam.flying());
    double peak = 0.0;
    for (int i = 0; i < 30; ++i) { // half of a 0.8 s flight at 60 Hz, and a bit more
        cam.update(1.0 / 60.0, scene);
        peak = std::max(peak, cam.camera().distance_m);
    }
    CHECK(cam.flying());
    const double separation = expanse::distance(earth->position, jupiter->position);
    CHECK(peak > separation); // both ends were in view mid-flight
    run_for(cam, scene, 1.0);
    CHECK_FALSE(cam.flying());
    CHECK(expanse::distance(cam.camera().target, jupiter->position) < 1.0);
    CHECK(cam.camera().distance_m == doctest::Approx(jupiter->frame_distance_m));
    CHECK(cam.focus() == jupiter->ref);
}

TEST_CASE("the camera tracks its focus as it moves") {
    const auto later = build_scene(*new_game_snapshot(focus_hints(), sim::Duration{86400 * 3}));
    const Scene& scene = start_scene();
    const SceneObject* luna_then = scene.find_key("luna");
    const SceneObject* luna_now = later.find_key("luna");
    REQUIRE(luna_then != nullptr);
    REQUIRE(luna_now != nullptr);
    REQUIRE(expanse::distance(luna_then->position, luna_now->position) > 1e8);

    OrbitCamera cam;
    cam.focus_on(*luna_then, std::nullopt, true);
    cam.update(0.4, scene);
    cam.update(0.2, later); // mid-flight the target moves on
    cam.update(1.0, later);
    CHECK(expanse::distance(cam.camera().target, luna_now->position) < 1.0);
}

TEST_CASE("log zoom steps and distance limits") {
    const Scene& scene = start_scene();
    const SceneObject* mars = scene.find_key("mars");
    REQUIRE(mars != nullptr);

    OrbitCamera cam;
    cam.focus_on(*mars, 0.01 * au_m, false);
    cam.zoom(1.0);
    run_for(cam, scene, 2.0);
    CHECK(cam.camera().distance_m == doctest::Approx(0.01 * au_m / OrbitCamera::zoom_step));
    cam.zoom(-2.0);
    run_for(cam, scene, 2.0);
    CHECK(cam.camera().distance_m == doctest::Approx(0.01 * au_m * OrbitCamera::zoom_step));

    // Never inside the planet: at least twice its radius from its centre.
    cam.zoom(1000.0);
    run_for(cam, scene, 2.0);
    CHECK(cam.camera().distance_m == doctest::Approx(2.0 * mars->radius_m));
    CHECK(expanse::distance(cam.camera().eye(), mars->position) >= 2.0 * mars->radius_m * (1.0 - 1e-9));
    cam.zoom(-1000.0);
    run_for(cam, scene, 2.0);
    CHECK(cam.camera().distance_m == doctest::Approx(OrbitCamera::max_distance_m));

    // A too-close requested distance is clamped as well.
    cam.focus_on(*mars, 1.0, false);
    CHECK(cam.camera().distance_m == doctest::Approx(2.0 * mars->radius_m));
}

TEST_CASE("orbit and pan move the eye but keep the distance") {
    const Scene& scene = start_scene();
    Navigator nav;
    nav.orbit_camera().reset(scene, false);
    nav.update(0.0, scene);
    const Camera before = nav.camera();

    using Type = InputEvent::Type;
    CHECK(nav.handle({.type = Type::button_down, .button = MouseButton::left}, scene));
    nav.handle({.type = Type::motion, .dx = 100.0, .dy = 1e6}, scene);
    nav.handle({.type = Type::button_up, .button = MouseButton::left}, scene);
    nav.update(0.0, scene);
    CHECK(nav.camera().yaw_rad == doctest::Approx(before.yaw_rad - 100.0 * Navigator::radians_per_px));
    CHECK(nav.camera().pitch_rad == doctest::Approx(OrbitCamera::max_pitch_rad));
    CHECK(nav.camera().distance_m == doctest::Approx(before.distance_m));

    nav.set_viewport(1280.0, 720.0);
    nav.handle({.type = Type::button_down, .shift = true, .button = MouseButton::left}, scene);
    nav.handle({.type = Type::motion, .dx = 50.0}, scene);
    nav.handle({.type = Type::button_up}, scene);
    nav.update(0.0, scene);
    const double moved = expanse::distance(nav.camera().target, before.target);
    CHECK(moved == doctest::Approx(50.0 * nav.camera().distance_m / focal_px(nav.camera(), 720.0)));
    // Focusing again clears the pan.
    nav.handle(key(Key::home), scene);
    run_for(nav, scene, 1.0);
    CHECK(expanse::length(nav.camera().target) < 1.0);

    // Mouse motion without a button does nothing.
    const Camera still = nav.camera();
    nav.handle({.type = Type::motion, .dx = 30.0, .dy = 30.0}, scene);
    nav.update(0.0, scene);
    CHECK(nav.camera().yaw_rad == still.yaw_rad);
}

TEST_CASE("hotkeys focus bookmarks the Sun and the player ship") {
    const Scene& scene = start_scene();
    Navigator nav;
    nav.orbit_camera().reset(scene, false);

    for (int d = 1; d <= 9; ++d) {
        const SceneObject* expected = scene.find_key(bookmarks[static_cast<std::size_t>(d - 1)].key);
        REQUIRE_MESSAGE(expected != nullptr, bookmarks[static_cast<std::size_t>(d - 1)].key);
        nav.handle(digit(d), scene);
        CHECK(focus_of(nav) == expected->ref);
    }
    nav.handle(digit(4), scene);
    run_for(nav, scene, 1.0);
    CHECK(nav.camera().distance_m == doctest::Approx(bookmarks[3].distance_au * au_m));

    nav.handle(key(Key::f), scene);
    CHECK(focus_of(nav) == scene.focus_ship);
    nav.handle(key(Key::home), scene);
    CHECK(focus_of(nav) == scene.objects.front().ref);

    nav.handle(digit(6), scene);
    nav.orbit_camera().orbit(1.0, 0.3);
    nav.handle(key(Key::r), scene);
    run_for(nav, scene, 1.0);
    const Camera defaults;
    CHECK(focus_of(nav) == scene.objects.front().ref);
    CHECK(nav.camera().distance_m == doctest::Approx(defaults.distance_m));
    CHECK(nav.camera().yaw_rad == doctest::Approx(defaults.yaw_rad));
    CHECK(nav.camera().pitch_rad == doctest::Approx(defaults.pitch_rad));

    const double d0 = nav.orbit_camera().target_distance_m();
    nav.handle(key(Key::plus), scene);
    CHECK(nav.orbit_camera().target_distance_m() == doctest::Approx(d0 / OrbitCamera::zoom_step));
    nav.handle({.type = InputEvent::Type::wheel, .dy = -2.0}, scene);
    CHECK(nav.orbit_camera().target_distance_m() == doctest::Approx(d0 * OrbitCamera::zoom_step));
    nav.handle(key(Key::minus), scene);
    CHECK(nav.orbit_camera().target_distance_m() ==
          doctest::Approx(d0 * OrbitCamera::zoom_step * OrbitCamera::zoom_step));

    CHECK(nav.handle(key(Key::other), scene));
    CHECK_FALSE(nav.handle(key(Key::escape), scene));
    CHECK_FALSE(nav.handle(key(Key::q), scene));
    CHECK_FALSE(nav.handle({.type = InputEvent::Type::quit}, scene));
}

TEST_CASE("tab and brackets cycle stations and ships") {
    const Scene& scene = start_scene();
    std::vector<ObjectRef> stations;
    std::vector<ObjectRef> ships;
    for (const SceneObject& o : scene.objects) {
        (o.ref.kind == ObjectKind::station ? stations : ships).push_back(o.ref);
    }
    std::erase_if(ships, [](const ObjectRef& r) { return r.kind != ObjectKind::ship; });
    REQUIRE(stations.size() >= 3);
    REQUIRE_FALSE(ships.empty());

    Navigator nav;
    nav.handle(key(Key::tab), scene);
    CHECK(focus_of(nav) == stations[0]);
    nav.handle(key(Key::tab), scene);
    CHECK(focus_of(nav) == stations[1]);
    nav.handle(key(Key::tab, true), scene);
    nav.handle(key(Key::tab, true), scene);
    CHECK(focus_of(nav) == stations.back()); // wraps around

    nav.handle(key(Key::right_bracket), scene);
    CHECK(focus_of(nav) == ships.front());
    nav.handle(key(Key::left_bracket), scene);
    CHECK(focus_of(nav) == ships[ships.size() > 1 ? ships.size() - 1 : 0]);
}

TEST_CASE("focus by key and a vanished focus") {
    const Scene& scene = start_scene();
    Navigator nav;
    CHECK(nav.focus_key("titan", scene, std::nullopt, false));
    CHECK_FALSE(nav.focus_key("no_such_place", scene, std::nullopt, false));
    CHECK(nav.focus_key("ship", scene, std::nullopt, false));
    const Vec3 last = nav.camera().target;

    const Scene empty{};
    nav.update(0.1, empty); // the ship is gone: stay where it was
    CHECK(nav.camera().target == last);
    CHECK_FALSE(focus_of(nav).has_value());
}

TEST_CASE("a click focuses the object under the cursor and a drag does not") {
    const Scene& scene = start_scene();
    const SceneObject* mars = scene.find_key("mars");
    REQUIRE(mars != nullptr);
    Navigator nav;
    nav.set_viewport(1280.0, 720.0);
    nav.orbit_camera().reset(scene, false);
    nav.update(0.0, scene);
    const auto px = project_to_pixels(nav.camera(), mars->position, 1280.0, 720.0);
    REQUIRE(px.has_value());
    const double x = (*px)[0];
    const double y = (*px)[1];

    using Type = InputEvent::Type;
    nav.handle({.type = Type::motion, .x = x, .y = y}, scene);
    CHECK(nav.hovered(scene) == mars->ref);

    // Dragged: an orbit, not a click.
    nav.handle({.type = Type::button_down, .x = x, .y = y}, scene);
    nav.handle({.type = Type::motion, .dx = 20.0, .x = x + 20.0, .y = y}, scene);
    nav.handle({.type = Type::button_up, .x = x + 20.0, .y = y}, scene);
    CHECK(focus_of(nav) != mars->ref);

    // Clicked (a small wobble is still a click).
    nav.update(0.0, scene);
    const auto again = project_to_pixels(nav.camera(), mars->position, 1280.0, 720.0);
    REQUIRE(again.has_value());
    nav.handle({.type = Type::button_down, .x = (*again)[0], .y = (*again)[1]}, scene);
    nav.handle({.type = Type::motion, .dx = 1.0, .x = (*again)[0] + 1.0, .y = (*again)[1]}, scene);
    nav.handle({.type = Type::button_up, .x = (*again)[0] + 1.0, .y = (*again)[1]}, scene);
    CHECK(focus_of(nav) == mars->ref);

    // Empty sky: the focus stays.
    nav.handle({.type = Type::button_down, .x = 3.0, .y = 3.0}, scene);
    nav.handle({.type = Type::button_up, .x = 3.0, .y = 3.0}, scene);
    CHECK(focus_of(nav) == mars->ref);
}
