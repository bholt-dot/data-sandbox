#include <doctest/doctest.h>

#include "expanse/calendar.hpp"
#include "expanse/contracts.hpp"
#include "expanse/economy.hpp"
#include "expanse/finance.hpp"
#include "expanse/scenario.hpp"
#include "expanse/ships.hpp"
#include "info.hpp"
#include "overlay.hpp"
#include "scene.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

using namespace viewer;
using namespace viewer::test;
using expanse::Vec3;
using expanse::units::au_m;

namespace {

// A monospace stand-in for SDL_ttf: 7 px per byte, 16 px lines (20 for titles).
TextSize fake_measure(std::string_view text, TextStyle style) {
    return {7.0F * static_cast<float>(text.size()), style == TextStyle::title ? 20.0F : 16.0F};
}

const InfoLine* find_pair(const InfoPanel& panel, std::string_view key) {
    for (const InfoLine& line : panel.lines) {
        if (line.kind == InfoLine::Kind::pair && line.cells[0] == key) {
            return &line;
        }
    }
    return nullptr;
}

bool has_line(const InfoPanel& panel, InfoLine::Kind kind, std::string_view text) {
    return std::any_of(panel.lines.begin(), panel.lines.end(), [&](const InfoLine& l) {
        return l.kind == kind && !l.cells.empty() && l.cells[0] == text;
    });
}

// A new game with the player's ship focused (as the shell hints it), optionally with a plot.
struct Game {
    std::shared_ptr<const expanse::Content> content = game_content();
    expanse::World world;
    expanse::ShipId ship;

    Game() : world(expanse::new_game(*content, content->table<expanse::ScenarioDef>().keys().front(), 1)) {
        ship = player_ship(world);
    }
    expanse::StationId station(std::string_view key) const { return content->find<expanse::StationDef>(key); }
    std::shared_ptr<const ViewSnapshot> snapshot(std::optional<expanse::CoursePreview> plot = std::nullopt) const {
        ViewHints hints;
        hints.focus_ship = ship;
        hints.plot_preview = std::move(plot);
        return make_snapshot(content, world, hints);
    }
};

ObjectRef ref_of(const Scene& scene, std::string_view key) {
    const SceneObject* o = scene.find_key(key);
    REQUIRE(o != nullptr);
    return o->ref;
}

} // namespace

TEST_CASE("declutter places labels by priority and tries the other sides") {
    const std::vector<LabelBox> boxes{
        {.anchor_x = 100.0F, .anchor_y = 100.0F, .marker_px = 4.0F, .w = 50.0F, .h = 16.0F},
        {.anchor_x = 100.0F, .anchor_y = 100.0F, .marker_px = 4.0F, .w = 50.0F, .h = 16.0F}, // same spot
        {.anchor_x = 100.0F, .anchor_y = 100.0F, .marker_px = 4.0F, .w = 50.0F, .h = 16.0F},
        {.anchor_x = 100.0F, .anchor_y = 100.0F, .marker_px = 4.0F, .w = 50.0F, .h = 16.0F},
        {.anchor_x = 100.0F, .anchor_y = 100.0F, .marker_px = 4.0F, .w = 50.0F, .h = 16.0F}, // no side left
        {.anchor_x = 395.0F, .anchor_y = 150.0F, .marker_px = 2.0F, .w = 40.0F, .h = 16.0F}, // at the right edge
    };
    const auto placed = declutter(boxes, {}, {}, 400.0F, 300.0F, 4.0F);
    REQUIRE(placed.size() == boxes.size());
    REQUIRE(placed[0]);
    CHECK(placed[0]->x == doctest::Approx(108.0)); // right of the marker, first choice
    CHECK(placed[0]->y == doctest::Approx(92.0));  // vertically centred
    REQUIRE(placed[1]);
    CHECK(placed[1]->right() == doctest::Approx(92.0)); // left
    REQUIRE(placed[2]);
    CHECK(placed[2]->bottom() == doctest::Approx(92.0)); // above
    REQUIRE(placed[3]);
    CHECK(placed[3]->y == doctest::Approx(108.0)); // below
    CHECK_FALSE(placed[4].has_value());
    REQUIRE(placed[5]);
    CHECK(placed[5]->right() <= 400.0F); // flipped to the left to stay on screen
    for (std::size_t i = 0; i < placed.size(); ++i) {
        for (std::size_t j = i + 1; j < placed.size(); ++j) {
            if (placed[i] && placed[j]) {
                CHECK_FALSE(placed[i]->overlaps(*placed[j]));
            }
        }
    }
}

TEST_CASE("declutter keeps labels off obstacles and other markers but not their own") {
    const std::vector<LabelBox> boxes{{.anchor_x = 100.0F, .anchor_y = 100.0F, .marker_px = 4.0F, .w = 50.0F, .h = 16.0F}};
    const std::vector<Rect> own_marker{{95.0F, 95.0F, 10.0F, 10.0F}};
    auto placed = declutter(boxes, {}, own_marker, 400.0F, 300.0F);
    REQUIRE(placed[0]);
    CHECK(placed[0]->x > 100.0F);

    // Another marker to the right and the HUD to the left: the label goes above.
    const std::vector<Rect> markers{{95.0F, 95.0F, 10.0F, 10.0F}, {130.0F, 95.0F, 10.0F, 10.0F}};
    const std::vector<Rect> hud{{0.0F, 94.0F, 94.0F, 20.0F}};
    placed = declutter(boxes, hud, markers, 400.0F, 300.0F);
    REQUIRE(placed[0]);
    CHECK(placed[0]->bottom() <= 96.0F);
    CHECK(placed[0]->x == doctest::Approx(75.0));
}

TEST_CASE("label tiers put focus and hover first then the player ship then planets") {
    const Game game;
    const Scene scene = build_scene(*game.snapshot());
    const SceneObject& earth = *scene.find_key("earth");
    const SceneObject& luna = *scene.find_key("luna");
    const SceneObject& ceres = *scene.find_key("ceres");
    const SceneObject& vesta = *scene.find_key("vesta");
    const SceneObject& station = *scene.find_key("ceres_station");
    const SceneObject& ship = *scene.find_key("ship");
    CHECK(label_tier(ship, std::nullopt, std::nullopt) == 1);
    CHECK(label_tier(earth, std::nullopt, std::nullopt) == 2);
    CHECK(label_tier(*scene.find_key("sun"), std::nullopt, std::nullopt) == 2);
    CHECK(label_tier(station, std::nullopt, std::nullopt) == 3);
    CHECK(label_tier(luna, std::nullopt, std::nullopt) == 3);
    CHECK(label_tier(ceres, std::nullopt, std::nullopt) == 3);
    CHECK(label_tier(vesta, std::nullopt, std::nullopt) == 4);
    CHECK(label_tier(vesta, vesta.ref, std::nullopt) == 0);
    CHECK(label_tier(luna, std::nullopt, luna.ref) == 0);
}

TEST_CASE("labels fade with scale") {
    const Game game;
    const Scene scene = build_scene(*game.snapshot());
    const Camera system_view; // the default: 9 AU from the Sun
    const double h = 720.0;
    CHECK(label_visibility(scene, *scene.find_key("jupiter"), system_view, h) == 1.0F);
    CHECK(label_visibility(scene, *scene.find_key("ship"), system_view, h) == 1.0F);
    CHECK(label_visibility(scene, *scene.find_key("ganymede"), system_view, h) == 0.0F);
    CHECK(label_visibility(scene, *scene.find_key("vesta"), system_view, h) == 0.0F);
    CHECK(label_visibility(scene, *scene.find_key("hollow_nail"), system_view, h) == 0.0F);

    Camera jupiter_view;
    jupiter_view.target = scene.find_key("jupiter")->position;
    jupiter_view.distance_m = scene.find_key("jupiter")->frame_distance_m;
    CHECK(label_visibility(scene, *scene.find_key("ganymede"), jupiter_view, h) == 1.0F);
    CHECK(label_visibility(scene, *scene.find_key("ganymede_agridomes"), jupiter_view, h) == 0.0F); // on Ganymede

    Camera belt_view;
    belt_view.target = scene.find_key("ceres")->position;
    belt_view.distance_m = 0.8 * au_m;
    CHECK(label_visibility(scene, *scene.find_key("hollow_nail"), belt_view, h) == 1.0F);
    CHECK(label_visibility(scene, *scene.find_key("ceres"), belt_view, h) == 1.0F);
    CHECK(label_visibility(scene, *scene.find_key("ceres_station"), belt_view, h) == 0.0F); // on Ceres
}

TEST_CASE("the system view labels planets without overlaps or clutter") {
    const Game game;
    const auto snap = game.snapshot();
    const Scene scene = build_scene(*snap);
    OverlayInput in{.snapshot = snap.get(), .snapshot_generation = 1, .scene = &scene, .width = 1280.0F, .height = 720.0F};
    in.focus = scene.objects.front().ref;
    Overlay overlay;
    OverlayDrawList list;
    overlay.build(in, fake_measure, list);

    const auto& labels = overlay.placed_labels();
    auto labelled = [&](std::string_view name) {
        return std::any_of(labels.begin(), labels.end(), [&](const auto& l) { return l.first == name; });
    };
    CHECK(labelled("Sol"));
    CHECK(labelled("Earth"));
    CHECK(labelled("Mars"));
    CHECK(labelled(scene.find_key("ship")->name));
    CHECK_FALSE(labelled("Luna"));
    CHECK_FALSE(labelled("Vesta"));
    CHECK_FALSE(labelled("Ceres Station"));
    CHECK(labels.size() < 12);
    for (std::size_t i = 0; i < labels.size(); ++i) {
        CHECK(labels[i].second.inside(1280.0F, 720.0F));
        for (std::size_t j = i + 1; j < labels.size(); ++j) {
            CHECK_FALSE(labels[i].second.overlaps(labels[j].second));
        }
    }
    // The HUD: the date in the corner, the focus, a scale bar and the hint line.
    const auto& hud = list.layers[1].texts;
    auto shows = [&](std::string_view text) {
        return std::any_of(hud.begin(), hud.end(), [&](const OverlayText& t) { return t.text == text; });
    };
    CHECK(shows("2350-03-14 00:00"));
    CHECK(shows("Sol"));
    CHECK(std::any_of(hud.begin(), hud.end(), [](const OverlayText& t) { return t.text.ends_with(" AU"); }));
    CHECK(std::any_of(hud.begin(), hud.end(), [](const OverlayText& t) { return t.text.starts_with("drag orbit"); }));
    CHECK_FALSE(overlay.info_rect().has_value());

    in.show_hints = false;
    overlay.build(in, fake_measure, list);
    CHECK(shows("H help"));
}

TEST_CASE("the info panel keeps labels off and a tooltip follows the cursor") {
    const Game game;
    const auto snap = game.snapshot();
    const Scene scene = build_scene(*snap);
    OverlayInput in{.snapshot = snap.get(), .snapshot_generation = 1, .scene = &scene, .width = 1280.0F, .height = 720.0F};
    in.camera.target = scene.find_key("ceres")->position;
    in.camera.distance_m = 0.8 * au_m;
    in.focus = ref_of(scene, "ceres_station");
    in.show_info = true;
    in.hovered = ref_of(scene, "hollow_nail");
    in.cursor_x = 600.0;
    in.cursor_y = 300.0;
    Overlay overlay;
    OverlayDrawList list;
    overlay.build(in, fake_measure, list);

    REQUIRE(overlay.info_rect().has_value());
    const Rect panel = *overlay.info_rect();
    CHECK(panel.inside(1280.0F, 720.0F));
    CHECK(panel.right() == doctest::Approx(1280.0 - 12.0));
    for (const auto& [text, rect] : overlay.placed_labels()) {
        CHECK_FALSE(rect.overlaps(panel));
    }
    const auto& panel_texts = list.layers[1].texts;
    CHECK(std::any_of(panel_texts.begin(), panel_texts.end(), [](const OverlayText& t) { return t.text == "Market"; }));
    const auto& tip = list.layers[2].texts;
    REQUIRE(tip.size() == 2);
    CHECK(tip[0].text == "Hollow Nail");
    CHECK(tip[0].x > 600.0F);
    CHECK(tip[1].text.starts_with("Belt station in solar orbit"));
}

TEST_CASE("station info panel shows the market jobs and docking from the snapshot") {
    const Game game;
    const auto snap = game.snapshot();
    const Scene scene = build_scene(*snap);
    const InfoPanel panel = object_info(*snap, scene, ref_of(scene, "ceres_station"));

    REQUIRE(panel.lines.size() > 8);
    CHECK(panel.lines[0].kind == InfoLine::Kind::title);
    CHECK(panel.lines[0].cells[0] == "Ceres Station");
    CHECK(panel.lines[1].cells[0] == "Belt station at Ceres");
    const InfoLine* here = find_pair(panel, "From your ship");
    REQUIRE(here != nullptr);
    CHECK(here->cells[1] == "here"); // docked there
    const expanse::StationDef& def = game.content->table<expanse::StationDef>()[game.station("ceres_station")];
    const InfoLine* docking = find_pair(panel, "Docking");
    REQUIRE(docking != nullptr);
    CHECK(docking->cells[1] == expanse::format_credits(def.docking_fee) + " / day");
    const InfoLine* jobs = find_pair(panel, "Job board");
    REQUIRE(jobs != nullptr);
    const std::size_t offers = expanse::contracts::board_at(game.world, game.station("ceres_station")).size();
    CHECK(jobs->cells[1] == (offers == 1 ? std::string("1 offer") : std::format("{} offers", offers)));
    REQUIRE(find_pair(panel, "Your contracts") != nullptr);

    // The market table: a header, then rows whose ask is above the bid, as quoted.
    CHECK(has_line(panel, InfoLine::Kind::section, "Market"));
    std::vector<const InfoLine*> rows;
    for (const InfoLine& l : panel.lines) {
        if (l.kind == InfoLine::Kind::table) {
            rows.push_back(&l);
        }
    }
    REQUIRE(rows.size() >= 2);
    CHECK(rows[0]->cells == std::vector<std::string>{"Commodity", "Stock", "Ask", "Bid"});
    CHECK(rows.size() - 1 <= info_market_rows);
    const auto& commodities = game.content->table<expanse::CommodityDef>();
    for (std::size_t i = 1; i < rows.size(); ++i) {
        REQUIRE(rows[i]->cells.size() == 4);
        bool matched = false;
        for (auto [cid, commodity] : commodities) {
            if (commodity.name != rows[i]->cells[0]) {
                continue;
            }
            const auto q = expanse::economy::quote(*game.content, game.world, game.station("ceres_station"), cid);
            REQUIRE(q.has_value());
            CHECK(rows[i]->cells[2] == group_digits(std::llround(q->ask)));
            CHECK(rows[i]->cells[3] == group_digits(std::llround(q->bid)));
            CHECK(q->ask > q->bid);
            matched = true;
        }
        CHECK(matched);
    }

    // A station elsewhere: distance and light lag from the ship.
    const InfoPanel mars = object_info(*snap, scene, ref_of(scene, "mars_highport"));
    const InfoLine* far = find_pair(mars, "From your ship");
    REQUIRE(far != nullptr);
    CHECK(far->cells[1].find(" AU · ") != std::string::npos);
    CHECK(far->cells[1].ends_with(" light"));
    CHECK(mars.lines[1].cells[0] == "Mars station at Mars");
}

TEST_CASE("ship info panel shows status fuel hull cargo and the course when underway") {
    Game game;
    const Scene docked_scene = build_scene(*game.snapshot());
    const InfoPanel docked = object_info(*game.snapshot(), docked_scene, *docked_scene.focus_ship);
    CHECK(docked.lines[1].cells[0].starts_with("Your ship · "));
    REQUIRE(find_pair(docked, "Status") != nullptr);
    CHECK(find_pair(docked, "Status")->cells[1] == "docked at Ceres Station");
    const expanse::ShipStatus st = expanse::ship_status(*game.content, game.world, game.ship);
    REQUIRE(find_pair(docked, "Reaction mass") != nullptr);
    CHECK(find_pair(docked, "Reaction mass")->cells[1].starts_with(std::format("{:.0f}%", st.reaction_mass_pct)));
    REQUIRE(find_pair(docked, "Hull") != nullptr);
    CHECK(find_pair(docked, "Hull")->cells[1] == std::format("{:.0f}%", st.hull_pct));
    REQUIRE(find_pair(docked, "Cargo") != nullptr);
    REQUIRE(find_pair(docked, "Cash") != nullptr);
    CHECK(find_pair(docked, "ETA") == nullptr);

    const expanse::DepartResult r = expanse::depart(*game.content, game.world, game.ship, game.station("hollow_nail"),
                                                    {.accel_g = 0.1});
    REQUIRE(r.ok());
    const auto snap = game.snapshot();
    const Scene scene = build_scene(*snap);
    const InfoPanel underway = object_info(*snap, scene, *scene.focus_ship);
    REQUIRE(find_pair(underway, "ETA") != nullptr);
    CHECK(find_pair(underway, "ETA")->cells[1].starts_with(expanse::calendar::format_datetime(r.preview.arrival)));
    REQUIRE(find_pair(underway, "Speed") != nullptr);
    CHECK(find_pair(underway, "Status")->cells[1].find("Hollow Nail") != std::string::npos);

    // The flown course is in the scene with its flip point and arrival label.
    REQUIRE(scene.courses.size() == 1);
    CHECK_FALSE(scene.courses[0].preview);
    CHECK(course_label(scene.courses[0])
              .starts_with("Hollow Nail · arr " + expanse::calendar::format_datetime(r.preview.arrival)));

    const Tooltip tip = tooltip(*snap, scene, ref_of(scene, "mars"));
    CHECK(tip.title == "Mars");
    CHECK(tip.detail.starts_with("Planet · "));
}

TEST_CASE("the flip point is mid-course for flip-and-burn and coasting profiles") {
    const Vec3 a{1.0 * au_m, 0.0, 0.0};
    const Vec3 b{1.0 * au_m, 2.0 * au_m, 0.0};
    const double d = expanse::distance(a, b);
    for (const auto& profile : {expanse::transit::brachistochrone(d, 3.0),
                                expanse::transit::capped_profile(d, 3.0, 400'000.0)}) {
        const Vec3 f = flip_point(a, b, profile);
        CHECK(f.x == doctest::Approx(a.x));
        CHECK(f.y == doctest::Approx(1.0 * au_m).epsilon(1e-9));
    }
    CHECK(expanse::transit::capped_profile(d, 3.0, 400'000.0).coast_time > 0.0);
    CHECK(flip_point(a, a, expanse::transit::BurnProfile{}) == a);

    // A plotted course: the flip marker is where the ship is at the flip time.
    const Game game;
    const expanse::CoursePreview plot =
        expanse::plot_course(*game.content, game.world, game.ship, game.station("mars_highport"));
    REQUIRE(plot.feasible());
    const Scene scene = build_scene(*game.snapshot(plot));
    REQUIRE(scene.courses.size() == 1);
    const SceneCourse& course = scene.courses[0];
    CHECK(course.preview);
    const Vec3 at_flip = expanse::transit::ship_position(plot.plot, plot.flip);
    // The flip time is whole seconds: the ship covers up to one second at peak speed of it.
    CHECK(expanse::distance(course.flip, at_flip) < plot.peak_speed_km_s * 1000.0 + 1000.0);
    CHECK(course_label(course) ==
          std::format("Mars Highport · arr {} · Δv {} km/s", expanse::calendar::format_datetime(plot.arrival),
                      group_digits(std::llround(plot.delta_v_km_s))));
    CHECK(std::any_of(scene.segments.begin(), scene.segments.end(), [](const SceneSegment& s) { return s.dashed; }));
}

TEST_CASE("scale bars come in 1 2 5 steps in AU or km") {
    auto bar = [](double max_m) { return choose_scale_bar(max_m / 150.0, 150.0); };
    CHECK(bar(0.37 * au_m).label == "0.2 AU");
    CHECK(bar(1.0 * au_m).label == "1 AU");
    CHECK(bar(7.3 * au_m).label == "5 AU");
    CHECK(bar(12.0 * au_m).label == "10 AU");
    CHECK(bar(0.06 * au_m).label == "0.05 AU");
    CHECK(bar(0.011 * au_m).label == "0.01 AU");
    CHECK(bar(23'000'000.0).label == "20,000 km");
    CHECK(bar(1'900'000.0).label == "1,000 km");
    CHECK(bar(4'500.0).label == "2 km");
    const ScaleBar b = choose_scale_bar(0.37 * au_m / 150.0, 150.0);
    CHECK(b.length_m == doctest::Approx(0.2 * au_m));
    CHECK(b.length_px == doctest::Approx(150.0 * 0.2 / 0.37));
    CHECK(b.length_px <= 150.0);
}

TEST_CASE("distances light lag and digits read like a captain's log") {
    CHECK(group_digits(0) == "0");
    CHECK(group_digits(999) == "999");
    CHECK(group_digits(1000) == "1,000");
    CHECK(group_digits(-1234567) == "-1,234,567");
    CHECK(format_distance(850'000.0) == "850 km");
    CHECK(format_distance(12'430'000.0) == "12,400 km");
    CHECK(format_distance(0.084 * au_m) == "0.084 AU");
    CHECK(format_distance(5.2 * au_m) == "5.20 AU");
    CHECK(format_distance(30.06 * au_m) == "30.1 AU");
    CHECK(format_light_lag({0}) == "< 1 s");
    CHECK(format_light_lag({12}) == "12 s");
    CHECK(format_light_lag({499}) == "8m 19s");
    CHECK(format_light_lag({4320}) == "1h 12m");
}
