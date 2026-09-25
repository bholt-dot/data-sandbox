#include "pick.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace viewer {

namespace {

int kind_rank(ObjectKind kind) {
    switch (kind) {
    case ObjectKind::body:
        return 0;
    case ObjectKind::station:
        return 1;
    case ObjectKind::ship:
        break;
    }
    return 2;
}

} // namespace

std::optional<PickHit> pick(const Scene& scene, const Camera& camera, double x, double y, double width,
                            double height, double max_px) {
    const expanse::Vec3 eye = camera.eye();
    const CameraBasis basis = camera_basis(camera);
    const double focal = focal_px(camera, height);

    std::optional<PickHit> best;
    std::tuple<double, double, int> best_key{};
    constexpr double same_spot_px = 1.0; // centres closer than this are one spot on screen
    for (const SceneObject& o : scene.objects) {
        const expanse::Vec3 rel = o.position - eye;
        const double depth = expanse::dot(rel, basis.forward);
        if (depth <= 0.0) {
            continue;
        }
        const double px = width / 2.0 + expanse::dot(rel, basis.right) / depth * focal;
        const double py = height / 2.0 - expanse::dot(rel, basis.up) / depth * focal;
        const double radius_px = std::max(static_cast<double>(o.min_px), o.radius_m / depth * focal);
        const double gap = std::max(0.0, std::hypot(px - x, py - y) - radius_px);
        if (gap > max_px || occluded(scene, eye, o.position, &o)) {
            continue;
        }
        const double centre_px = std::floor(std::hypot(px - x, py - y) / same_spot_px);
        const std::tuple key{gap, centre_px, kind_rank(o.ref.kind)};
        if (!best || key < best_key) {
            best = PickHit{o.ref, gap};
            best_key = key;
        }
    }
    return best;
}

} // namespace viewer
