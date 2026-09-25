#pragma once

// CPU side of the viewer, independent of SDL: turns a ViewSnapshot into a Scene of objects in
// world coordinates (build_scene, once per snapshot) and a Scene plus a Camera into per-frame GPU
// data (prepare_frame).
//
// Precision rule: world positions stay double (heliocentric metres) on the CPU. Every frame,
// prepare_frame subtracts the camera eye position in double and only then narrows to float, in
// scene units of 1 AU, so what the GPU sees is always small near the camera (floating origin).
// That holds for every draw: spheres, sprites and line vertices alike.
//
// Depth: reversed-Z with an infinite far plane and a float depth buffer (near -> 1, infinity -> 0,
// compare GREATER); float's exponent then cancels the 1/z distribution, so 1 km to 40 AU resolve
// without z-fighting.

#include "viewer/view_snapshot.hpp"

#include "expanse/orbit.hpp"
#include "expanse/units.hpp"
#include "expanse/vec3.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

inline constexpr double scene_unit_m = expanse::units::au_m;

struct Rgba {
    float r = 1.0F;
    float g = 1.0F;
    float b = 1.0F;
    float a = 1.0F;
};

// ---- GPU-facing layouts (must match shaders/*.glsl) --------------------------------------------

// Column-major, as GLSL expects.
struct Mat4 {
    std::array<float, 16> m{};
};

struct FrameUniforms { // frame.glsl: Frame (std140)
    Mat4 view_proj;
    std::array<float, 4> viewport{}; // width, height, focal length [px], unused
    std::array<float, 4> sun{};      // camera-relative Sun position [AU], unused
};
static_assert(sizeof(FrameUniforms) == 96);

struct StripUniforms { // line.vert: Strip (std140)
    Rgba color;
};
static_assert(sizeof(StripUniforms) == 16);

enum class SpriteShape : std::uint8_t { disc = 0, diamond = 1, ring = 2 };

struct SpriteInstance { // sprite.vert: Sprite (std140 storage buffer: vec4 members only)
    std::array<float, 4> position_radius{}; // camera-relative [AU], physical radius [AU]
    Rgba color;
    std::array<float, 4> style{}; // min radius [px], shape, glow, core alpha
};
static_assert(sizeof(SpriteInstance) == 48);

struct SphereInstance { // sphere.vert: Sphere (std140 storage buffer)
    std::array<float, 4> position_radius{}; // camera-relative centre [AU], radius [AU]
    Rgba color;                             // albedo (or emission), straight alpha
    std::array<float, 4> params{};          // emissive (0/1), unused x3
};
static_assert(sizeof(SphereInstance) == 48);

struct LineVertex { // line.vert: in_position, camera-relative [AU]
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};
static_assert(sizeof(LineVertex) == 12);

// ---- Scene: what to draw, in world coordinates -----------------------------------------------

enum class ObjectKind : std::uint8_t { body, station, ship };

// Identifies a scene object across snapshots: a content DefId index for bodies and stations, a
// World handle (index + generation) for ships.
struct ObjectRef {
    ObjectKind kind = ObjectKind::body;
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    constexpr bool operator==(const ObjectRef&) const = default;
};

struct SceneObject {
    ObjectRef ref;
    std::string key;  // content key for bodies and stations; empty for ships
    std::string name; // display name
    expanse::Vec3 position; // heliocentric [m]
    double radius_m = 0.0;  // physical radius of a body; 0 for stations and ships
    Rgba color;
    float min_px = 2.0F; // never drawn smaller than this
    SpriteShape shape = SpriteShape::disc;
    float glow = 0.0F;
    bool emissive = false; // the Sun: lit from within, not by the Sun
    // Camera limits when this object is the focus: never closer than min_distance_m (outside a
    // body, or the planet a station sits at); frame_distance_m is the distance a jump to it uses.
    double min_distance_m = 0.0;
    double frame_distance_m = 0.0;
};

// A closed Kepler orbit around a parent, sampled per frame (see sample_orbit).
struct SceneOrbit {
    expanse::orbit::Orbit orbit;
    expanse::Vec3 parent_position; // heliocentric [m]
    Rgba color;
};

// A straight line (transits, plotted courses).
struct SceneSegment {
    expanse::Vec3 from;
    expanse::Vec3 to;
    Rgba color;
};

// A decoration without an identity of its own (focus ring, plotted destination).
struct SceneMarker {
    expanse::Vec3 position;
    Rgba color;
    float min_px = 6.0F;
    SpriteShape shape = SpriteShape::ring;
};

struct Scene {
    sim::Time time{};
    std::vector<SceneObject> objects; // bodies (star first), then stations, then ships
    std::vector<SceneOrbit> orbits;
    std::vector<SceneSegment> segments;
    std::vector<SceneMarker> markers;
    std::optional<ObjectRef> focus_ship; // ViewHints::focus_ship, if that ship exists

    const SceneObject* find(ObjectRef ref) const;
    // A body or station by content key ("earth", "ceres_station"); "ship" is the focus ship.
    const SceneObject* find_key(std::string_view key) const;
};

Scene build_scene(const ViewSnapshot& snapshot);

// ---- Camera ------------------------------------------------------------------------------------

// A render camera: looks at `target` from `distance_m` away; z is ecliptic north. The
// navigation controller (nav.hpp) drives it.
struct Camera {
    expanse::Vec3 target{};
    double distance_m = 9.0 * expanse::units::au_m; // frames Jupiter's orbit
    double yaw_rad = expanse::units::deg(-90.0);  // eye direction around the ecliptic pole
    double pitch_rad = expanse::units::deg(60.0); // eye elevation above the ecliptic plane
    double fov_y_rad = expanse::units::deg(45.0);
    double near_m = 1000.0; // reversed-Z, infinite far plane: a small near costs nothing

    expanse::Vec3 eye() const;
};

struct CameraBasis {
    expanse::Vec3 right;
    expanse::Vec3 up;
    expanse::Vec3 forward;
};
CameraBasis camera_basis(const Camera& camera);

using Mat4d = std::array<double, 16>; // column-major, like Mat4

// Right-handed, looking down -z, clip depth in [0, 1] (SDL GPU's convention on every backend),
// reversed (near plane -> 1) with an infinite far plane (-> 0), in scene units.
Mat4d reversed_z_projection(double fov_y_rad, double aspect, double near_units);
// Projection x view rotation for camera-relative positions in scene units.
Mat4d view_projection(const Camera& camera, double aspect);
// Pixels per scene unit of lateral size at unit distance: half height / tan(fov_y / 2).
double focal_px(const Camera& camera, double height);

// Narrows a world position to camera-relative float scene units (the precision rule above).
std::array<float, 3> camera_relative(const expanse::Vec3& world, const expanse::Vec3& eye);

// Appends one closed orbit as a camera-relative line strip, sampled in eccentric anomaly with a
// step that keeps the chord's deviation from the true ellipse under `tolerance_px` on screen and
// segments shorter than half their distance to the eye, so an orbit passing next to the camera
// stays smooth while distant parts stay cheap (vertex count grows ~log(size / distance)).
// Returns the number of vertices appended.
std::uint32_t sample_orbit(const expanse::orbit::Orbit& orbit, const expanse::Vec3& parent_position,
                           const expanse::Vec3& eye, double focal, double tolerance_px,
                           std::vector<LineVertex>& out);

// True if the segment from `eye` to `point` passes through a body of the scene other than
// `self` (a point inside a body, like a surface port at its centre, is not occluded by it).
bool occluded(const Scene& scene, const expanse::Vec3& eye, const expanse::Vec3& point,
              const SceneObject* self = nullptr);

// How much of a body is drawn as a lit sphere (1) rather than as a dot (0): a crossfade over
// projected radii between min_px and twice that.
float sphere_weight(const SceneObject& object, double projected_px);

struct StripDraw {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    StripUniforms uniforms;
};

struct FrameData {
    FrameUniforms frame;
    std::vector<SphereInstance> spheres;
    std::vector<SpriteInstance> sprites;
    std::vector<LineVertex> line_vertices;
    std::vector<StripDraw> strips;
};

// Per-frame, camera-relative GPU data for a viewport of width x height pixels. Reuses `out`'s
// storage, so a steady scene allocates nothing per frame.
void prepare_frame(const Scene& scene, const Camera& camera, float width, float height, FrameData& out);

// Pixel position (origin top-left) of a world point, or nullopt if it is behind the camera.
std::optional<std::array<double, 2>> project_to_pixels(const Camera& camera, expanse::Vec3 world,
                                                       double width, double height);

} // namespace viewer
