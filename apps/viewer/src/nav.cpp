#include "nav.hpp"

#include "pick.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace viewer {

namespace {

using expanse::Vec3;

constexpr double zoom_rate_per_s = 14.0; // exponential approach of wheel zoom to its target

Vec3 lerp(const Vec3& a, const Vec3& b, double t) { return a + (b - a) * t; }

const SceneObject* find_sun(const Scene& scene) {
    for (const SceneObject& o : scene.objects) {
        if (o.emissive) {
            return &o;
        }
    }
    return nullptr;
}

} // namespace

double ease_in_out(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

OrbitCamera::OrbitCamera()
    : log_distance_(std::log(camera_.distance_m)), log_distance_target_(log_distance_) {}

void OrbitCamera::set_angles(double yaw_rad, double pitch_rad) {
    camera_.yaw_rad = yaw_rad;
    camera_.pitch_rad = std::clamp(pitch_rad, min_pitch_rad, max_pitch_rad);
    flight_.animate_angles = false;
}

double OrbitCamera::target_distance_m() const { return std::exp(log_distance_target_); }

void OrbitCamera::clamp_target() {
    log_distance_target_ =
        std::clamp(log_distance_target_, std::log(std::max(min_distance_m_, 1.0)), std::log(max_distance_m));
}

void OrbitCamera::focus_on(const SceneObject& object, std::optional<double> distance_m, bool animate) {
    const Vec3 from = camera_.target;
    focus_ = object.ref;
    focus_position_ = object.position;
    pan_ = {};
    min_distance_m_ = object.min_distance_m;
    log_distance_target_ = std::log(std::max(distance_m.value_or(object.frame_distance_m), 1.0));
    clamp_target();
    if (!animate) {
        flight_.active = false;
        log_distance_ = log_distance_target_;
        camera_.target = object.position;
        camera_.distance_m = std::exp(log_distance_);
        return;
    }
    // Zoom out mid-flight far enough to see both ends at once (half the view spans ~0.41 d).
    const double separation = expanse::distance(from, object.position);
    const double mid = 0.5 * (log_distance_ + log_distance_target_);
    flight_.active = true;
    flight_.t = 0.0;
    flight_.from = from;
    flight_.from_log_distance = log_distance_;
    flight_.bump = separation > 0.0 ? std::max(0.0, std::log(1.3 * separation) - mid) : 0.0;
    flight_.animate_angles = false;
}

void OrbitCamera::reset(const Scene& scene, bool animate) {
    const Camera defaults;
    if (const SceneObject* sun = find_sun(scene)) {
        focus_on(*sun, defaults.distance_m, animate);
    } else {
        focus_.reset();
        focus_position_ = {};
        pan_ = {};
        log_distance_target_ = std::log(defaults.distance_m);
    }
    if (animate && flight_.active) {
        flight_.animate_angles = true;
        flight_.from_yaw = camera_.yaw_rad;
        flight_.from_pitch = camera_.pitch_rad;
        flight_.to_yaw = camera_.yaw_rad + std::remainder(defaults.yaw_rad - camera_.yaw_rad, expanse::units::two_pi);
        flight_.to_pitch = defaults.pitch_rad;
    } else {
        set_angles(defaults.yaw_rad, defaults.pitch_rad);
    }
}

void OrbitCamera::orbit(double dyaw_rad, double dpitch_rad) {
    flight_.animate_angles = false;
    camera_.yaw_rad = std::remainder(camera_.yaw_rad + dyaw_rad, expanse::units::two_pi);
    camera_.pitch_rad = std::clamp(camera_.pitch_rad + dpitch_rad, min_pitch_rad, max_pitch_rad);
}

void OrbitCamera::zoom(double steps) {
    log_distance_target_ -= steps * std::log(zoom_step);
    clamp_target();
}

void OrbitCamera::pan(double dx_px, double dy_px, double viewport_height_px) {
    const CameraBasis b = camera_basis(camera_);
    const double metres_per_px = camera_.distance_m / focal_px(camera_, viewport_height_px);
    pan_ -= b.right * (dx_px * metres_per_px);
    pan_ += b.up * (dy_px * metres_per_px);
}

void OrbitCamera::update(double dt_s, const Scene& scene) {
    if (focus_) {
        if (const SceneObject* o = scene.find(*focus_)) {
            focus_position_ = o->position;
            min_distance_m_ = o->min_distance_m;
        } else {
            focus_.reset(); // gone (a ship sold): stay where it was last seen
        }
    }
    clamp_target();
    const Vec3 look = focus_position_ + pan_;
    if (flight_.active) {
        flight_.t = std::min(flight_.t + dt_s / fly_seconds, 1.0);
        const double e = ease_in_out(flight_.t);
        camera_.target = lerp(flight_.from, look, e);
        log_distance_ = flight_.from_log_distance + (log_distance_target_ - flight_.from_log_distance) * e +
                        flight_.bump * 4.0 * e * (1.0 - e);
        if (flight_.animate_angles) {
            camera_.yaw_rad = flight_.from_yaw + (flight_.to_yaw - flight_.from_yaw) * e;
            camera_.pitch_rad = flight_.from_pitch + (flight_.to_pitch - flight_.from_pitch) * e;
        }
        if (flight_.t >= 1.0) {
            flight_.active = false;
            flight_.animate_angles = false;
        }
    } else {
        camera_.target = look;
        const double k = 1.0 - std::exp(-dt_s * zoom_rate_per_s);
        log_distance_ += (log_distance_target_ - log_distance_) * k;
        if (std::abs(log_distance_target_ - log_distance_) < 1e-6) {
            log_distance_ = log_distance_target_;
        }
    }
    camera_.distance_m = std::exp(log_distance_);
}

bool Navigator::focus_key(std::string_view key, const Scene& scene, std::optional<double> distance_m, bool animate) {
    const SceneObject* o = scene.find_key(key);
    if (o == nullptr) {
        return false;
    }
    camera_.focus_on(*o, distance_m, animate);
    return true;
}

void Navigator::set_viewport(double width_px, double height_px) {
    if (width_px > 0.0 && height_px > 0.0) {
        viewport_width_ = width_px;
        viewport_height_ = height_px;
    }
}

bool Navigator::in_ui_region(double x, double y) const {
    if (!ui_region_) {
        return false;
    }
    const auto& r = *ui_region_;
    return x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3];
}

std::optional<ObjectRef> Navigator::hovered(const Scene& scene) const {
    if (cursor_x_ < 0.0 || cursor_y_ < 0.0 || drag_ != Drag::none || in_ui_region(cursor_x_, cursor_y_)) {
        return std::nullopt;
    }
    const auto hit = pick(scene, camera(), cursor_x_, cursor_y_, viewport_width_, viewport_height_);
    return hit ? std::optional(hit->ref) : std::nullopt;
}

void Navigator::cycle(ObjectKind kind, int direction, const Scene& scene) {
    std::vector<const SceneObject*> list; // a key press, not a frame: allocating is fine
    std::optional<std::size_t> current;
    for (const SceneObject& o : scene.objects) {
        if (o.ref.kind == kind) {
            if (camera_.focus() == o.ref) {
                current = list.size();
            }
            list.push_back(&o);
        }
    }
    if (list.empty()) {
        return;
    }
    const auto n = static_cast<std::ptrdiff_t>(list.size());
    std::ptrdiff_t next = direction > 0 ? 0 : n - 1;
    if (current) {
        next = ((static_cast<std::ptrdiff_t>(*current) + direction) % n + n) % n;
    }
    camera_.focus_on(*list[static_cast<std::size_t>(next)], std::nullopt, true);
}

bool Navigator::handle(const InputEvent& event, const Scene& scene) {
    using Type = InputEvent::Type;
    switch (event.type) {
    case Type::quit:
        return false;
    case Type::key_down:
        switch (event.key) {
        case Key::escape:
            if (show_info_) {
                show_info_ = false;
                break;
            }
            return false;
        case Key::q:
            return false;
        case Key::h:
            show_hints_ = !show_hints_;
            break;
        case Key::i:
            show_info_ = !show_info_;
            break;
        case Key::f:
            if (scene.focus_ship) {
                if (const SceneObject* ship = scene.find(*scene.focus_ship)) {
                    camera_.focus_on(*ship, std::nullopt, true);
                }
            }
            break;
        case Key::home:
            if (const SceneObject* sun = find_sun(scene)) {
                camera_.focus_on(*sun, std::nullopt, true);
            }
            break;
        case Key::digit:
            if (event.digit >= 1 && event.digit <= 9) {
                const Bookmark& b = bookmarks[static_cast<std::size_t>(event.digit - 1)];
                focus_key(b.key, scene,
                          b.distance_au > 0.0 ? std::optional(b.distance_au * expanse::units::au_m) : std::nullopt,
                          true);
            }
            break;
        case Key::tab:
            cycle(ObjectKind::station, event.shift ? -1 : 1, scene);
            break;
        case Key::left_bracket:
            cycle(ObjectKind::ship, -1, scene);
            break;
        case Key::right_bracket:
            cycle(ObjectKind::ship, 1, scene);
            break;
        case Key::r:
            camera_.reset(scene, true);
            break;
        case Key::plus:
            camera_.zoom(1.0);
            break;
        case Key::minus:
            camera_.zoom(-1.0);
            break;
        case Key::other:
            break;
        }
        break;
    case Type::button_down:
        drag_ = event.button == MouseButton::left && !event.shift ? Drag::orbit : Drag::pan;
        dragged_px_ = 0.0;
        cursor_x_ = event.x;
        cursor_y_ = event.y;
        break;
    case Type::button_up:
        if (drag_ == Drag::orbit && event.button == MouseButton::left && dragged_px_ < click_slop_px &&
            !in_ui_region(event.x, event.y)) {
            // A click, not a drag: fly to what was clicked.
            if (const auto hit = pick(scene, camera(), event.x, event.y, viewport_width_, viewport_height_)) {
                if (const SceneObject* o = scene.find(hit->ref)) {
                    camera_.focus_on(*o, std::nullopt, true);
                    show_info_ = true;
                }
            }
        }
        drag_ = Drag::none;
        break;
    case Type::motion:
        cursor_x_ = event.x;
        cursor_y_ = event.y;
        dragged_px_ += std::hypot(event.dx, event.dy);
        if (drag_ == Drag::orbit && dragged_px_ < click_slop_px) {
            break; // not a drag yet
        }
        if (drag_ == Drag::orbit) {
            camera_.orbit(-event.dx * radians_per_px, event.dy * radians_per_px);
        } else if (drag_ == Drag::pan) {
            camera_.pan(event.dx, event.dy, viewport_height_);
        }
        break;
    case Type::wheel:
        camera_.zoom(event.dy);
        break;
    }
    return true;
}

} // namespace viewer
