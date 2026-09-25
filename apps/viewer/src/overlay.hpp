#pragma once

// The 2D overlay, independent of SDL: labels next to the markers (with decluttering), the HUD
// corner readout, the hotkey hint, the hover tooltip, the info panel and course markers, laid
// out in pixels as a draw list of text runs and quads. The SDL side (text.hpp) only measures and
// draws text; everything here is unit-testable with a fake measure function.
//
// Label placement is greedy by priority (after Christensen, Marks & Shieber, "An empirical study
// of algorithms for point-feature label placement", 1995, and the priority-ordered greedy
// labellers of interactive maps): candidates are sorted by tier, each tries four positions around
// its marker (right, left, above, below) and takes the first that stays on screen and overlaps
// neither a label already placed, another marker, nor the HUD and panels. A feature is unlabelled
// only if every position is taken by something of higher priority. Visibility fades with scale so
// a zoomed-out view shows planets, not a wall of text.

#include "info.hpp"
#include "scene.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

enum class TextStyle : std::uint8_t { label, small, body, heading, title };
inline constexpr std::size_t text_style_count = 5;

// Nominal pixel size of each style at ui_scale 1.
inline constexpr std::array<float, text_style_count> text_style_px{13.0F, 12.0F, 14.0F, 14.0F, 20.0F};

struct TextSize {
    float w = 0.0F;
    float h = 0.0F; // line height
};
// Size of a single line of text in pixels at the overlay's ui_scale.
using MeasureText = std::function<TextSize(std::string_view, TextStyle)>;

struct Rect {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool overlaps(const Rect& o) const { return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom(); }
    bool inside(float width, float height) const { return x >= 0.0F && y >= 0.0F && right() <= width && bottom() <= height; }
};

struct OverlayText {
    std::string text;
    TextStyle style = TextStyle::label;
    float x = 0.0F; // top-left of the line box [px]
    float y = 0.0F;
    Rgba color;
    bool outline = true; // a dark halo: legible over bright orbits and planets
};

struct OverlayQuad {
    std::array<float, 8> xy{}; // four corners, x/y pairs, in order around the quad [px]
    Rgba color;                // straight alpha
};

struct OverlayLayer {
    std::vector<OverlayQuad> quads; // drawn first
    std::vector<OverlayText> texts;
};

// Layer 0: world-anchored (labels, course ticks). Layer 1: screen furniture (HUD, hints, info
// panel). Layer 2: the tooltip, over everything.
struct OverlayDrawList {
    std::array<OverlayLayer, 3> layers;
    void clear();
};

// ---- Labels ------------------------------------------------------------------------------------

// Lower draws first and wins space: 0 focused or hovered, 1 player's ship and its courses,
// 2 the Sun and planets, 3 stations, moons and dwarf planets, 4 asteroids, 5 other ships.
int label_tier(const SceneObject& object, std::optional<ObjectRef> focus, std::optional<ObjectRef> hovered);

// 0..1 by scale: planets and the player's ship always; a moon, a station at a body or a docked
// ship once it stands clear of its host on screen; free-flying stations, dwarf planets and
// asteroids once the camera is near enough the belt.
float label_visibility(const Scene& scene, const SceneObject& object, const Camera& camera, double height_px);

enum class LabelSide : std::uint8_t { right, left, above, below };
inline constexpr std::array<LabelSide, 4> label_sides{LabelSide::right, LabelSide::left, LabelSide::above,
                                                      LabelSide::below};

struct LabelBox {
    float anchor_x = 0.0F; // marker centre [px]
    float anchor_y = 0.0F;
    float marker_px = 0.0F; // marker radius
    float w = 0.0F;         // text size
    float h = 0.0F;
};

// Where the label's text box goes on `side` of its marker (gap included).
Rect label_rect(const LabelBox& box, LabelSide side, float gap_px);

// Greedy placement of `boxes`, which must be sorted by priority (most important first). A label
// takes the first side whose rectangle lies inside width x height and overlaps no obstacle, no
// marker other than its own (a marker centred on its anchor) and no label placed before it.
// Returns the rectangle of each box, or nullopt where none fits.
std::vector<std::optional<Rect>> declutter(std::span<const LabelBox> boxes, std::span<const Rect> obstacles,
                                           std::span<const Rect> markers, float width, float height,
                                           float gap_px = 4.0F);

// ---- The overlay -------------------------------------------------------------------------------

struct OverlayInput {
    const ViewSnapshot* snapshot = nullptr; // may be null before the first publish
    std::uint64_t snapshot_generation = 0;  // changes whenever `snapshot` does
    const Scene* scene = nullptr;
    Camera camera{};
    float width = 0.0F; // viewport [px]
    float height = 0.0F;
    float ui_scale = 1.0F; // HiDPI: layout constants and text sizes scale by this
    std::optional<ObjectRef> focus{};
    std::optional<ObjectRef> hovered{};
    double cursor_x = -1.0;
    double cursor_y = -1.0;
    bool show_hints = true;
    bool show_info = false; // the panel for `focus`
};

// Keeps what must persist between frames: the labels placed last frame (they keep their place
// among equals, so labels don't flicker) and the info panel of the current snapshot and focus.
class Overlay {
public:
    void build(const OverlayInput& input, const MeasureText& measure, OverlayDrawList& out);

    // The panel rectangle of the last build (empty if none); tests and hit-testing use it.
    const std::optional<Rect>& info_rect() const { return info_rect_; }
    // Labels placed in the last build, in placement order.
    const std::vector<std::pair<std::string, Rect>>& placed_labels() const { return placed_labels_; }

private:
    std::vector<ObjectRef> placed_last_;
    std::vector<std::pair<std::string, Rect>> placed_labels_;
    std::uint64_t info_generation_ = 0;
    std::optional<ObjectRef> info_ref_;
    InfoPanel info_;
    std::optional<Rect> info_rect_;
};

// The hotkey hint line (toggled with H).
inline constexpr std::string_view hints_text =
    "drag orbit · shift/right-drag pan · wheel zoom · click focus + info · I info · F ship · Home Sun · "
    "1-9 bookmarks · Tab stations · [ ] ships · R reset · H hide help · Esc close";

} // namespace viewer
