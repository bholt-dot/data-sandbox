#pragma once

// Picking: which scene object is under (or near) the cursor. Pure CPU, on the double-precision
// scene; the labels and info panel use it for hover and click.

#include "scene.hpp"

#include <optional>

namespace viewer {

struct PickHit {
    ObjectRef ref;
    double distance_px = 0.0; // from the cursor to the object's drawn disc (0 = on it)
};

// The object nearest to the cursor at (x, y) pixels (origin top-left) within max_px of its drawn
// disc (at least its min_px marker), ignoring objects behind the camera or hidden behind a body.
// When several are under the cursor the one centred nearest to it wins (a station marker on a
// planet's disc), and among objects on the same spot, bodies before stations before ships (a
// planet over its orbital station at system scale; the rest are reachable with Tab, [ ] and F).
std::optional<PickHit> pick(const Scene& scene, const Camera& camera, double x, double y, double width,
                            double height, double max_px = 8.0);

} // namespace viewer
