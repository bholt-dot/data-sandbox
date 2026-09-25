#pragma once

// CPU side of the viewer, independent of SDL: turns a ViewSnapshot into drawable geometry
// (build_scene, once per snapshot) and a camera into per-frame GPU data (prepare_frame).
//
// Precision: world positions stay double (metres) until prepare_frame subtracts the camera eye
// position; only the camera-relative result is narrowed to float, in scene units of 1 AU.

#include "viewer/view_snapshot.hpp"

#include "expanse/units.hpp"
#include "expanse/vec3.hpp"

#include <array>
#include <cstdint>
#include <optional>
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
};
static_assert(sizeof(FrameUniforms) == 80);

struct StripUniforms { // line.vert: Strip (std140)
    std::array<float, 4> origin{};
    Rgba color;
};
static_assert(sizeof(StripUniforms) == 32);

enum class SpriteShape : std::uint8_t { disc = 0, diamond = 1, ring = 2 };

struct SpriteInstance { // sprite.vert: Sprite (std140 storage buffer: vec4 members only)
    std::array<float, 4> position_radius{};
    Rgba color;
    std::array<float, 4> style{}; // min radius [px], shape, glow, unused
};
static_assert(sizeof(SpriteInstance) == 48);

struct LineVertex { // line.vert: in_position
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};
static_assert(sizeof(LineVertex) == 12);

// ---- Scene: what to draw, in world coordinates -----------------------------------------------

struct SceneSprite {
    expanse::Vec3 position; // heliocentric [m]
    double radius_m = 0.0;  // physical radius; the sprite never shrinks below min_px
    Rgba color;
    float min_px = 2.0F;
    SpriteShape shape = SpriteShape::disc;
    float glow = 0.0F;
};

struct SceneStrip {
    expanse::Vec3 origin;    // heliocentric [m]; vertices are relative to this
    std::uint32_t first = 0; // into Scene::line_vertices
    std::uint32_t count = 0;
    Rgba color;
    double extent_m = 0.0; // rough size, to skip strips that would be a few pixels across
};

struct Scene {
    sim::Time time{};
    std::vector<SceneSprite> sprites;           // in draw order
    std::vector<LineVertex> line_vertices;      // relative to their strip's origin [AU]
    std::vector<SceneStrip> strips;             // in draw order (drawn before sprites)
};

Scene build_scene(const ViewSnapshot& snapshot);

// ---- Camera ------------------------------------------------------------------------------------

// Orbits `target` at `distance`; z is ecliptic north. Deliberately simple: the scene bean
// replaces it with an orbit/follow camera and reversed-Z.
struct Camera {
    expanse::Vec3 target{};
    double distance_m = 9.0 * expanse::units::au_m; // frames Jupiter's orbit
    double yaw_rad = expanse::units::deg(-90.0);  // eye direction around the ecliptic pole
    double pitch_rad = expanse::units::deg(60.0); // eye elevation above the ecliptic plane
    double fov_y_rad = expanse::units::deg(45.0);
    double near_m = 1.0e6; // infinite far plane

    expanse::Vec3 eye() const;
};

struct StripDraw {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    StripUniforms uniforms;
};

struct FrameData {
    FrameUniforms frame;
    std::vector<SpriteInstance> sprites;
    std::vector<StripDraw> strips;
};

// Per-frame, camera-relative GPU data for a viewport of width x height pixels. Reuses `out`'s
// storage.
void prepare_frame(const Scene& scene, const Camera& camera, float width, float height, FrameData& out);

// Pixel position (origin top-left) of a world point, or nullopt if it is behind the camera.
std::optional<std::array<double, 2>> project_to_pixels(const Camera& camera, expanse::Vec3 world,
                                                       double width, double height);

} // namespace viewer
