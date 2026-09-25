#pragma once

// Viewer navigation, independent of SDL: an orbit camera around a focus object with smooth
// fly-to transitions and log-space zoom (OrbitCamera), and the mapping from key and mouse events
// to camera actions (Navigator). viewer.cpp translates SDL events into InputEvents.

#include "scene.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace viewer {

// ---- Orbit camera ------------------------------------------------------------------------------

class OrbitCamera {
public:
    static constexpr double fly_seconds = 0.8;
    static constexpr double max_distance_m = 200.0 * expanse::units::au_m;

    OrbitCamera();

    // Look at `object` (tracking it as it moves) from `distance_m` (default: its frame distance).
    // Animated: the look-at point and log(distance) ease over fly_seconds, zooming out on the way
    // if the jump is long, so both ends stay in view (after van Wijk & Nuij).
    void focus_on(const SceneObject& object, std::optional<double> distance_m, bool animate);
    // Back to the Sun from the default angles and distance.
    void reset(const Scene& scene, bool animate);

    void orbit(double dyaw_rad, double dpitch_rad);
    // Positive steps zoom in; one step is a factor of zoom_step.
    void zoom(double steps);
    // Moves the look-at point off the focus by a screen drag of (dx, dy) pixels.
    void pan(double dx_px, double dy_px, double viewport_height_px);

    // Advances animations by dt seconds and follows the focus to its position in `scene`.
    void update(double dt_s, const Scene& scene);

    const Camera& camera() const { return camera_; }
    std::optional<ObjectRef> focus() const { return focus_; }
    double target_distance_m() const;
    bool flying() const { return flight_.active; }
    void set_angles(double yaw_rad, double pitch_rad);

    static constexpr double zoom_step = 1.2;
    static constexpr double min_pitch_rad = -1.55; // just short of the poles
    static constexpr double max_pitch_rad = 1.55;

private:
    struct Flight {
        bool active = false;
        double t = 0.0; // 0..1
        expanse::Vec3 from;
        double from_log_distance = 0.0;
        double bump = 0.0; // extra log-distance at mid-flight
        bool animate_angles = false; // reset: also swing back to the default angles
        double from_yaw = 0.0;
        double from_pitch = 0.0;
        double to_yaw = 0.0;
        double to_pitch = 0.0;
    };

    void clamp_target();

    Camera camera_;
    std::optional<ObjectRef> focus_;
    expanse::Vec3 focus_position_;   // last known position of the focus [m]
    expanse::Vec3 pan_{};            // look-at offset from the focus [m]
    double log_distance_ = 0.0;      // current
    double log_distance_target_ = 0.0;
    double min_distance_m_ = 1000.0; // of the current focus
    Flight flight_;
};

// Smootherstep: C2-continuous ease-in-out on [0, 1].
double ease_in_out(double t);

// ---- Input -------------------------------------------------------------------------------------

enum class Key : std::uint8_t {
    other,
    f,
    home,
    digit, // with InputEvent::digit 0-9
    tab,
    left_bracket,
    right_bracket,
    r,
    plus,
    minus,
    escape,
    q,
};

enum class MouseButton : std::uint8_t { left, middle, right };

struct InputEvent {
    enum class Type : std::uint8_t { key_down, button_down, button_up, motion, wheel, quit };
    Type type = Type::key_down;
    Key key = Key::other;
    int digit = 0;
    bool shift = false; // key_down, button_down
    MouseButton button = MouseButton::left;
    double dx = 0.0; // motion [px]; wheel: dy in notches, positive away from the user
    double dy = 0.0;
    double x = 0.0; // cursor position [px, origin top-left]: buttons and motion
    double y = 0.0;
};

// Number-key bookmarks: a body or station content key and an optional distance override.
struct Bookmark {
    std::string_view key;
    double distance_au = 0.0; // 0: the object's frame distance
};
inline constexpr std::array<Bookmark, 9> bookmarks{{
    {"earth"}, {"luna"}, {"mars"}, {"ceres_station", 0.8}, {"vesta"},
    {"jupiter"}, {"ganymede"}, {"saturn"}, {"titan"},
}};

inline constexpr std::string_view controls_help_text =
    "left-drag orbit | click focus | shift/right-drag pan | wheel or +/- zoom | F player ship | Home Sun | "
    "1-9 Earth Luna Mars Ceres Vesta Jupiter Ganymede Saturn Titan | Tab/Shift+Tab stations | "
    "[ ] ships | R reset | Esc/Q close";

// Maps input events to camera actions against the current scene.
class Navigator {
public:
    // Returns false when the event asks to close the window.
    bool handle(const InputEvent& event, const Scene& scene);
    void update(double dt_s, const Scene& scene) { camera_.update(dt_s, scene); }
    void set_viewport(double width_px, double height_px);

    // Focus by key (see Scene::find_key); false if there is no such object.
    bool focus_key(std::string_view key, const Scene& scene, std::optional<double> distance_m, bool animate);

    // The object under the cursor (see pick.hpp), for hover feedback.
    std::optional<ObjectRef> hovered(const Scene& scene) const;

    OrbitCamera& orbit_camera() { return camera_; }
    const OrbitCamera& orbit_camera() const { return camera_; }
    const Camera& camera() const { return camera_.camera(); }

    static constexpr double radians_per_px = 0.005;
    static constexpr double click_slop_px = 4.0; // a press and release closer than this is a click

private:
    enum class Drag : std::uint8_t { none, orbit, pan };

    void cycle(ObjectKind kind, int direction, const Scene& scene);

    OrbitCamera camera_;
    Drag drag_ = Drag::none;
    double dragged_px_ = 0.0; // since the button went down
    double cursor_x_ = -1.0;
    double cursor_y_ = -1.0;
    double viewport_width_ = 1280.0;
    double viewport_height_ = 720.0;
};

} // namespace viewer
