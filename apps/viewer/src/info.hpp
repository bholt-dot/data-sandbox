#pragma once

// What the overlay says, independent of SDL and of layout: the info panel for an object, hover
// tooltips, the HUD strings and the scale bar. Everything is read from an immutable ViewSnapshot
// (its World copy and Content), never from the live session.

#include "scene.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace viewer {

enum class Tone : std::uint8_t { normal, dim, good, warning, bad };

struct InfoLine {
    enum class Kind : std::uint8_t {
        title,     // cells[0]: the object's name
        subtitle,  // cells[0]: what it is ("Belt station at Ceres")
        pair,      // cells[0]: key, cells[1]: value
        section,   // cells[0]: a heading within the panel
        table,     // cells: one row of a table; the first row after a section is its header
        text,      // cells[0]: a free line (list entries, notes)
    };
    Kind kind = Kind::text;
    std::vector<std::string> cells;
    Tone tone = Tone::normal;
};

struct InfoPanel {
    std::vector<InfoLine> lines;
};

// Market rows a station's panel lists at most.
inline constexpr std::size_t info_market_rows = 6;

// The panel for `ref` in `scene` (built from `snapshot`); empty if there is no such object.
//   body     kind (and parent), distance and light-lag from the player's ship, distance from the
//            Sun, radius, stations there
//   station  faction and host, distance and light-lag, population, docking fee (and any tab),
//            job offers on the board, the player's contracts to or from it, and a market table
//            (commodity, stock, ask, bid) of the entries furthest from their base price
//   ship     class and owner, where it is or is going, ETA, speed, reaction mass, hull, cargo,
//            consignments and passengers; the player's cash for the player's ship
InfoPanel object_info(const ViewSnapshot& snapshot, const Scene& scene, ObjectRef ref);

// Two short lines for a hover tooltip: the name, then what it is and how far from the ship.
struct Tooltip {
    std::string title;
    std::string detail;
};
Tooltip tooltip(const ViewSnapshot& snapshot, const Scene& scene, ObjectRef ref);

// "Planet", "Moon of Jupiter", "Belt station at Ceres", "Your ship", ...
std::string describe_object(const ViewSnapshot& snapshot, const Scene& scene, const SceneObject& object);

// The ship distances are measured from: the hinted focus ship, else the player's first ship.
const SceneObject* reference_ship(const Scene& scene);

// ---- Formatting --------------------------------------------------------------------------------

// "1,234,567"
std::string group_digits(std::int64_t value);
// Metres as a captain reads them: "850 km", "12,400 km", "0.084 AU", "5.20 AU".
std::string format_distance(double metres);
// One-way light delay: "< 1 s", "12 s", "8m 19s", "1h 12m".
std::string format_light_lag(sim::Duration lag);
// "Mars Highport · arr 2350-03-14 06:00 · Δv 1,234 km/s".
std::string course_label(const SceneCourse& course);

// ---- Scale bar ---------------------------------------------------------------------------------

struct ScaleBar {
    double length_m = 0.0;
    double length_px = 0.0;
    std::string label; // "0.1 AU", "20,000 km"
};

// The longest bar of at most max_px whose length is 1, 2 or 5 times a power of ten, in AU from
// 0.01 AU up and in km below. metres_per_px must be positive.
ScaleBar choose_scale_bar(double metres_per_px, double max_px);

} // namespace viewer
