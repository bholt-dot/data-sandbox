#include "overlay.hpp"

#include "expanse/calendar.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace viewer {

namespace {

using expanse::Vec3;
using expanse::units::au_m;

// Palette: light text over a near-black sky; tones for the panel.
constexpr Rgba text_color{0.87F, 0.90F, 0.94F, 1.0F};
constexpr Rgba title_color{1.0F, 1.0F, 1.0F, 1.0F};
constexpr Rgba dim_color{0.58F, 0.63F, 0.71F, 1.0F};
constexpr Rgba good_color{0.52F, 0.92F, 0.58F, 1.0F};
constexpr Rgba warning_color{1.0F, 0.76F, 0.32F, 1.0F};
constexpr Rgba bad_color{1.0F, 0.45F, 0.40F, 1.0F};
constexpr Rgba section_color{0.96F, 0.82F, 0.50F, 1.0F};
constexpr Rgba panel_color{0.020F, 0.028F, 0.045F, 0.93F};
constexpr Rgba panel_border{0.45F, 0.52F, 0.62F, 0.45F};

Rgba tone_color(Tone tone) {
    switch (tone) {
    case Tone::dim:
        return dim_color;
    case Tone::good:
        return good_color;
    case Tone::warning:
        return warning_color;
    case Tone::bad:
        return bad_color;
    case Tone::normal:
        break;
    }
    return text_color;
}

Rgba with_alpha(Rgba c, float a) {
    c.a = a;
    return c;
}

// A label in the object's colour, lifted towards white so dark or saturated colours still read.
Rgba label_color(const Rgba& c, float alpha) {
    constexpr float lift = 0.55F;
    return {c.r + (1.0F - c.r) * lift, c.g + (1.0F - c.g) * lift, c.b + (1.0F - c.b) * lift, alpha};
}

double smoothstep(double lo, double hi, double x) {
    const double t = std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

OverlayQuad rect_quad(const Rect& r, Rgba color) {
    return {{r.x, r.y, r.right(), r.y, r.right(), r.bottom(), r.x, r.bottom()}, color};
}

// A line of `thickness` px from a to b, as a quad.
OverlayQuad line_quad(float ax, float ay, float bx, float by, float thickness, Rgba color) {
    const float dx = bx - ax;
    const float dy = by - ay;
    const float len = std::max(std::hypot(dx, dy), 1e-3F);
    const float nx = -dy / len * thickness * 0.5F;
    const float ny = dx / len * thickness * 0.5F;
    return {{ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny}, color};
}

float snap(float v) { return std::round(v); }

// Greedy word wrap on spaces; a single word longer than the width stays on its own line.
std::vector<std::string> wrap(const std::string& text, TextStyle style, float max_w, const MeasureText& measure) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t end = std::min(text.find(' ', pos), text.size());
        const std::string word = text.substr(pos, end - pos);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && measure(candidate, style).w > max_w) {
            lines.push_back(std::move(line));
            line = word;
        } else {
            line = candidate;
        }
        pos = end + 1;
    }
    if (!line.empty() || lines.empty()) {
        lines.push_back(std::move(line));
    }
    return lines;
}

struct Projected {
    float x = 0.0F;
    float y = 0.0F;
    double depth = 0.0;
};

std::optional<Projected> project(const CameraBasis& basis, const Vec3& eye, double focal, const Vec3& world,
                                 float width, float height) {
    const Vec3 rel = world - eye;
    const double depth = expanse::dot(rel, basis.forward);
    if (depth <= 0.0) {
        return std::nullopt;
    }
    return Projected{static_cast<float>(static_cast<double>(width) / 2.0 + expanse::dot(rel, basis.right) / depth * focal),
                     static_cast<float>(static_cast<double>(height) / 2.0 - expanse::dot(rel, basis.up) / depth * focal), depth};
}

struct Candidate {
    std::optional<ObjectRef> ref;
    std::string text;
    TextStyle style = TextStyle::label;
    Rgba color;
    int tier = 0;
    bool placed_before = false;
    double depth = 0.0;
    LabelBox box;
};

} // namespace

void OverlayDrawList::clear() {
    for (OverlayLayer& layer : layers) {
        layer.quads.clear();
        layer.texts.clear();
    }
}

int label_tier(const SceneObject& object, std::optional<ObjectRef> focus, std::optional<ObjectRef> hovered) {
    if (focus == object.ref || hovered == object.ref) {
        return 0;
    }
    switch (object.cls) {
    case ObjectClass::star:
    case ObjectClass::planet:
        return 2;
    case ObjectClass::dwarf_planet:
    case ObjectClass::moon:
    case ObjectClass::station:
        return 3;
    case ObjectClass::asteroid:
        return 4;
    case ObjectClass::ship:
        break;
    }
    return object.player ? 1 : 5;
}

float label_visibility(const Scene& scene, const SceneObject& object, const Camera& camera, double height_px) {
    if (object.cls == ObjectClass::star || object.cls == ObjectClass::planet ||
        (object.cls == ObjectClass::ship && object.player)) {
        return 1.0F;
    }
    const Vec3 eye = camera.eye();
    const double distance = std::max(expanse::distance(eye, object.position), camera.near_m);
    if (object.host) {
        if (const SceneObject* host = scene.find(*object.host)) {
            // A port on the surface counts as a body-width off it: labelled once the body is a
            // disc rather than a dot.
            const double separation = std::max(expanse::distance(object.position, host->position), 2.0 * host->radius_m);
            const double separation_px = separation / distance * focal_px(camera, height_px);
            return static_cast<float>(smoothstep(14.0, 30.0, separation_px));
        }
    }
    double near_au = 4.0; // fully visible nearer than this
    double far_au = 7.0;  // gone beyond
    if (object.cls == ObjectClass::asteroid) {
        near_au = 2.5;
        far_au = 4.5;
    } else if (object.cls == ObjectClass::ship) {
        near_au = 0.3;
        far_au = 0.6;
    }
    return static_cast<float>(1.0 - smoothstep(near_au * au_m, far_au * au_m, distance));
}

Rect label_rect(const LabelBox& box, LabelSide side, float gap_px) {
    const float r = box.marker_px + gap_px;
    switch (side) {
    case LabelSide::right:
        return {box.anchor_x + r, box.anchor_y - box.h / 2.0F, box.w, box.h};
    case LabelSide::left:
        return {box.anchor_x - r - box.w, box.anchor_y - box.h / 2.0F, box.w, box.h};
    case LabelSide::above:
        return {box.anchor_x - box.w / 2.0F, box.anchor_y - r - box.h, box.w, box.h};
    case LabelSide::below:
        break;
    }
    return {box.anchor_x - box.w / 2.0F, box.anchor_y + r, box.w, box.h};
}

std::vector<std::optional<Rect>> declutter(std::span<const LabelBox> boxes, std::span<const Rect> obstacles,
                                           std::span<const Rect> markers, float width, float height, float gap_px) {
    std::vector<std::optional<Rect>> result(boxes.size());
    std::vector<Rect> taken(obstacles.begin(), obstacles.end());
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const LabelBox& box = boxes[i];
        for (const LabelSide side : label_sides) {
            const Rect r = label_rect(box, side, gap_px);
            if (!r.inside(width, height)) {
                continue;
            }
            const auto hits = [&](const Rect& t) { return t.overlaps(r); };
            const bool on_marker = std::any_of(markers.begin(), markers.end(), [&](const Rect& m) {
                const bool own = std::abs(m.x + m.w / 2.0F - box.anchor_x) < 0.5F &&
                                 std::abs(m.y + m.h / 2.0F - box.anchor_y) < 0.5F;
                return !own && hits(m);
            });
            if (!on_marker && std::none_of(taken.begin(), taken.end(), hits)) {
                result[i] = r;
                taken.push_back(r);
                break;
            }
        }
    }
    return result;
}

void Overlay::build(const OverlayInput& in, const MeasureText& measure, OverlayDrawList& out) {
    out.clear();
    placed_labels_.clear();
    info_rect_.reset();
    if (in.scene == nullptr || in.width <= 0.0F || in.height <= 0.0F) {
        return;
    }
    const Scene& scene = *in.scene;
    const float s = in.ui_scale;
    const float margin = 12.0F * s;
    OverlayLayer& world_layer = out.layers[0];
    OverlayLayer& screen_layer = out.layers[1];
    std::vector<Rect> obstacles; // screen furniture and markers: labels keep off them

    auto text_at = [&](OverlayLayer& layer, std::string text, TextStyle style, float x, float y, Rgba color,
                       bool outline = true) {
        const TextSize size = measure(text, style);
        layer.texts.push_back({std::move(text), style, snap(x), snap(y), color, outline});
        return size;
    };
    auto line_height = [&](TextStyle style) { return measure("Ag", style).h; };

    // ---- HUD corner: date, focus, scale bar ---------------------------------------------------
    {
        float y = margin;
        float right = margin;
        if (in.snapshot != nullptr) {
            const TextSize t = text_at(screen_layer, expanse::calendar::format_datetime(in.snapshot->time),
                                       TextStyle::title, margin, y, title_color);
            right = std::max(right, margin + t.w);
            y += t.h;
        }
        if (const SceneObject* focus = in.focus ? scene.find(*in.focus) : nullptr) {
            const TextSize k = text_at(screen_layer, "Focus", TextStyle::body, margin, y, dim_color);
            const TextSize v = text_at(screen_layer, focus->name, TextStyle::body, margin + k.w + 8.0F * s, y, text_color);
            right = std::max(right, margin + k.w + 8.0F * s + v.w);
            y += k.h;
        }
        const double metres_per_px = in.camera.distance_m / focal_px(in.camera, static_cast<double>(in.height));
        const ScaleBar bar = choose_scale_bar(metres_per_px, 150.0 * static_cast<double>(s));
        const float bar_px = static_cast<float>(bar.length_px);
        const float bar_y = snap(y + 9.0F * s);
        const Rgba bar_color{0.80F, 0.84F, 0.90F, 0.9F};
        const Rgba shadow{0.0F, 0.0F, 0.0F, 0.7F};
        const float t = std::max(1.0F, std::round(s));
        for (const auto& [off, color] : {std::pair{t, shadow}, std::pair{0.0F, bar_color}}) {
            const float x0 = margin + off;
            const float y0 = bar_y + off;
            screen_layer.quads.push_back(rect_quad({x0, y0, bar_px, t}, color));
            screen_layer.quads.push_back(rect_quad({x0, y0 - 4.0F * s, t, 4.0F * s + t}, color));
            screen_layer.quads.push_back(rect_quad({x0 + bar_px - t, y0 - 4.0F * s, t, 4.0F * s + t}, color));
        }
        const TextSize l = measure(bar.label, TextStyle::small);
        text_at(screen_layer, bar.label, TextStyle::small, margin + bar_px + 8.0F * s, bar_y - l.h / 2.0F - 1.0F * s,
                text_color);
        right = std::max(right, margin + bar_px + 8.0F * s + l.w);
        obstacles.push_back({0.0F, 0.0F, right + margin, bar_y + 8.0F * s});
    }

    // ---- Hotkey hints, bottom left ------------------------------------------------------------
    {
        const std::string text = in.show_hints ? std::string(hints_text) : std::string("H help");
        const float max_w = in.width - 2.0F * margin;
        // Break at the separators so each line stays a list of whole hints.
        std::vector<std::string> lines;
        std::string line;
        std::size_t pos = 0;
        while (pos < text.size()) {
            const std::size_t end = std::min(text.find(" · ", pos), text.size());
            const std::string item = text.substr(pos, end - pos);
            const std::string candidate = line.empty() ? item : line + " · " + item;
            if (!line.empty() && measure(candidate, TextStyle::small).w > max_w) {
                lines.push_back(std::move(line));
                line = item;
            } else {
                line = candidate;
            }
            pos = end == text.size() ? end : end + 3;
        }
        lines.push_back(std::move(line));
        const float lh = line_height(TextStyle::small);
        float y = in.height - margin - lh * static_cast<float>(lines.size());
        obstacles.push_back({0.0F, y - 4.0F * s, in.width, in.height - y + 4.0F * s});
        for (std::string& l : lines) {
            text_at(screen_layer, std::move(l), TextStyle::small, margin, y, dim_color);
            y += lh;
        }
    }

    // ---- Info panel for the focus, top right --------------------------------------------------
    if (in.show_info && in.focus && in.snapshot != nullptr) {
        if (info_generation_ != in.snapshot_generation || info_ref_ != in.focus) {
            info_ = object_info(*in.snapshot, scene, *in.focus);
            info_generation_ = in.snapshot_generation;
            info_ref_ = in.focus;
        }
        if (!info_.lines.empty()) {
            using Kind = InfoLine::Kind;
            const float pad = 12.0F * s;
            const float col_gap = 14.0F * s;
            const float max_w = std::min(430.0F * s, in.width * 0.5F) - 2.0F * pad;
            // Column widths: pairs share a key column; each table (section) its own columns.
            float key_w = 0.0F;
            float content_w = 220.0F * s;
            std::vector<std::vector<float>> table_cols; // one per table run
            bool in_table = false;
            for (const InfoLine& line : info_.lines) {
                if (line.kind == Kind::table) {
                    if (!in_table) {
                        table_cols.emplace_back();
                    }
                    in_table = true;
                    auto& cols = table_cols.back();
                    cols.resize(std::max(cols.size(), line.cells.size()), 0.0F);
                    for (std::size_t c = 0; c < line.cells.size(); ++c) {
                        cols[c] = std::max(cols[c], measure(line.cells[c], TextStyle::body).w);
                    }
                    continue;
                }
                in_table = false;
                if (line.kind == Kind::pair) {
                    key_w = std::max(key_w, measure(line.cells[0], TextStyle::body).w);
                }
            }
            for (const InfoLine& line : info_.lines) {
                if (line.kind == Kind::pair) {
                    content_w = std::max(content_w, key_w + col_gap + measure(line.cells[1], TextStyle::body).w);
                } else if (line.kind == Kind::title) {
                    content_w = std::max(content_w, measure(line.cells[0], TextStyle::title).w);
                }
            }
            for (const auto& cols : table_cols) {
                float w = 0.0F;
                for (const float c : cols) {
                    w += c;
                }
                content_w = std::max(content_w, w + col_gap * static_cast<float>(cols.size() - 1));
            }
            content_w = std::min(content_w, max_w);
            const float panel_w = content_w + 2.0F * pad;
            const float x0 = in.width - margin - panel_w;
            const float left = x0 + pad;

            // Lay out into a temporary list first: the background goes under it at the final height.
            std::vector<OverlayText> texts;
            float y = margin + pad;
            std::size_t table_index = 0;
            in_table = false;
            bool first_table_row = false;
            for (const InfoLine& line : info_.lines) {
                const Rgba color = tone_color(line.tone);
                if (line.kind != Kind::table) {
                    in_table = false;
                }
                switch (line.kind) {
                case Kind::title: {
                    const float lh = line_height(TextStyle::title);
                    texts.push_back({line.cells[0], TextStyle::title, left, y, title_color, false});
                    y += lh;
                    break;
                }
                case Kind::subtitle:
                case Kind::text:
                    for (std::string& l : wrap(line.cells[0], TextStyle::body, content_w, measure)) {
                        texts.push_back({std::move(l), TextStyle::body, left, y, color, false});
                        y += line_height(TextStyle::body);
                    }
                    if (line.kind == Kind::subtitle) {
                        y += 6.0F * s;
                    }
                    break;
                case Kind::section:
                    y += 8.0F * s;
                    texts.push_back({line.cells[0], TextStyle::heading, left, y, section_color, false});
                    y += line_height(TextStyle::heading) + 2.0F * s;
                    break;
                case Kind::pair: {
                    texts.push_back({line.cells[0], TextStyle::body, left, y, dim_color, false});
                    const float vx = left + key_w + col_gap;
                    for (std::string& l : wrap(line.cells[1], TextStyle::body, left + content_w - vx, measure)) {
                        texts.push_back({std::move(l), TextStyle::body, vx, y, color, false});
                        y += line_height(TextStyle::body);
                    }
                    break;
                }
                case Kind::table: {
                    if (!in_table) {
                        first_table_row = true;
                        in_table = true;
                        ++table_index;
                    }
                    const auto& cols = table_cols[table_index - 1];
                    // First column left-aligned, numbers right-aligned in theirs.
                    float cx = left;
                    const float extra = content_w - [&] {
                        float w = col_gap * static_cast<float>(cols.size() - 1);
                        for (const float c : cols) {
                            w += c;
                        }
                        return w;
                    }();
                    for (std::size_t c = 0; c < line.cells.size(); ++c) {
                        const float col_w = cols[c] + (c == 0 ? std::max(extra, 0.0F) : 0.0F);
                        const float tw = measure(line.cells[c], TextStyle::body).w;
                        const float tx = c == 0 ? cx : cx + col_w - tw;
                        texts.push_back({line.cells[c], TextStyle::body, tx, y, color, false});
                        cx += col_w + col_gap;
                    }
                    y += line_height(TextStyle::body);
                    if (first_table_row && line.tone == Tone::dim) {
                        // A rule under the header row.
                        screen_layer.quads.push_back(
                            rect_quad({left, snap(y), content_w, std::max(1.0F, std::round(s))}, panel_border));
                        y += 3.0F * s;
                    }
                    first_table_row = false;
                    break;
                }
                }
            }
            const Rect panel{snap(x0), margin, snap(panel_w), snap(y + pad - margin)};
            const float b = std::max(1.0F, std::round(s));
            // Background and border go before the rule quads already added (they draw first).
            std::vector<OverlayQuad> frame{rect_quad(panel, panel_color),
                                           rect_quad({panel.x, panel.y, panel.w, b}, panel_border),
                                           rect_quad({panel.x, panel.bottom() - b, panel.w, b}, panel_border),
                                           rect_quad({panel.x, panel.y, b, panel.h}, panel_border),
                                           rect_quad({panel.right() - b, panel.y, b, panel.h}, panel_border)};
            screen_layer.quads.insert(screen_layer.quads.begin(), frame.begin(), frame.end());
            for (OverlayText& t : texts) {
                t.x = snap(t.x);
                t.y = snap(t.y);
                screen_layer.texts.push_back(std::move(t));
            }
            info_rect_ = panel;
            obstacles.push_back({panel.x - 4.0F * s, panel.y - 4.0F * s, panel.w + 8.0F * s, panel.h + 8.0F * s});
        }
    }

    // ---- Tooltip at the cursor ----------------------------------------------------------------
    if (in.hovered && in.snapshot != nullptr && in.cursor_x >= 0.0 && in.cursor_y >= 0.0) {
        const Tooltip tip = tooltip(*in.snapshot, scene, *in.hovered);
        if (!tip.title.empty()) {
            const float pad = 7.0F * s;
            const TextSize a = measure(tip.title, TextStyle::body);
            const TextSize d = measure(tip.detail, TextStyle::small);
            const float w = std::max(a.w, d.w) + 2.0F * pad;
            const float h = a.h + d.h + 2.0F * pad;
            float x = static_cast<float>(in.cursor_x) + 16.0F * s;
            float y = static_cast<float>(in.cursor_y) + 18.0F * s;
            if (x + w > in.width - 4.0F * s) {
                x = static_cast<float>(in.cursor_x) - 12.0F * s - w;
            }
            if (y + h > in.height - 4.0F * s) {
                y = static_cast<float>(in.cursor_y) - 12.0F * s - h;
            }
            const Rect box{snap(std::max(x, 0.0F)), snap(std::max(y, 0.0F)), snap(w), snap(h)};
            out.layers[2].quads.push_back(rect_quad(box, panel_color));
            out.layers[2].quads.push_back(rect_quad({box.x, box.y, std::max(2.0F, std::round(2.0F * s)), box.h},
                                                   section_color));
            text_at(out.layers[2], tip.title, TextStyle::body, box.x + pad, box.y + pad, title_color, false);
            text_at(out.layers[2], tip.detail, TextStyle::small, box.x + pad, box.y + pad + a.h, dim_color, false);
            obstacles.push_back(box);
        }
    }

    // ---- Labels and course markers -------------------------------------------------------------
    const Vec3 eye = in.camera.eye();
    const CameraBasis basis = camera_basis(in.camera);
    const double focal = focal_px(in.camera, static_cast<double>(in.height));
    std::vector<Candidate> candidates;
    struct Disc {
        float x, y, r;
    };
    std::vector<Disc> discs; // drawn markers [px]
    for (const SceneMarker& m : scene.markers) {
        if (const auto p = project(basis, eye, focal, m.position, in.width, in.height)) {
            discs.push_back({p->x, p->y, m.min_px});
        }
    }
    for (const SceneObject& o : scene.objects) {
        const auto p = project(basis, eye, focal, o.position, in.width, in.height);
        if (!p || p->x < 0.0F || p->y < 0.0F || p->x > in.width || p->y > in.height) {
            continue;
        }
        if (occluded(scene, eye, o.position, &o)) {
            continue;
        }
        const int tier = label_tier(o, in.focus, in.hovered);
        const float visibility = tier == 0 ? 1.0F : label_visibility(scene, o, in.camera, static_cast<double>(in.height));
        const auto radius_px = static_cast<float>(o.radius_m / p->depth * focal);
        float marker = std::max(o.min_px, radius_px);
        if (radius_px < 0.5F * std::min(in.width, in.height)) {
            discs.push_back({p->x, p->y, marker}); // a disc filling the view blocks nothing
        }
        if (visibility < 0.05F) {
            continue;
        }
        Candidate c;
        c.ref = o.ref;
        c.text = o.name;
        c.color = tier == 0 ? title_color : label_color(o.color, std::min(1.0F, visibility));
        c.tier = tier;
        c.placed_before = std::find(placed_last_.begin(), placed_last_.end(), o.ref) != placed_last_.end();
        c.depth = p->depth;
        c.box = {p->x, p->y, marker, 0.0F, 0.0F};
        candidates.push_back(std::move(c));
    }

    for (const SceneCourse& course : scene.courses) {
        const auto a = project(basis, eye, focal, course.from, in.width, in.height);
        const auto b = project(basis, eye, focal, course.to, in.width, in.height);
        const auto f = project(basis, eye, focal, course.flip, in.width, in.height);
        if (!a || !b) {
            continue;
        }
        const float length_px = std::hypot(b->x - a->x, b->y - a->y);
        const Rgba color = with_alpha(course.color, 0.95F);
        if (f && length_px > 60.0F * s && !occluded(scene, eye, course.flip)) {
            // The flip tick: across the course line, with a dark underlay for contrast.
            const float dx = (b->x - a->x) / length_px;
            const float dy = (b->y - a->y) / length_px;
            const float half = 7.0F * s;
            const float nx = -dy * half;
            const float ny = dx * half;
            world_layer.quads.push_back(
                line_quad(f->x - nx, f->y - ny, f->x + nx, f->y + ny, 4.0F * s, {0.0F, 0.0F, 0.0F, 0.6F}));
            world_layer.quads.push_back(line_quad(f->x - nx, f->y - ny, f->x + nx, f->y + ny, 2.0F * s, color));
            if (length_px > 140.0F * s && f->x >= 0.0F && f->y >= 0.0F && f->x <= in.width && f->y <= in.height) {
                Candidate c;
                c.text = "flip";
                c.style = TextStyle::small;
                c.color = label_color(course.color, 1.0F);
                c.tier = 2;
                c.depth = f->depth;
                c.box = {f->x, f->y, half, 0.0F, 0.0F};
                candidates.push_back(std::move(c));
            }
        }
        if ((course.preview || length_px > 60.0F * s) && b->x >= 0.0F && b->y >= 0.0F && b->x <= in.width &&
            b->y <= in.height) {
            Candidate c;
            c.text = course_label(course);
            c.style = TextStyle::small;
            c.color = label_color(course.color, 1.0F);
            c.tier = 1;
            c.depth = b->depth;
            c.box = {b->x, b->y, 8.0F * s, 0.0F, 0.0F};
            candidates.push_back(std::move(c));
        }
    }

    // Priority order: tier, then what held a place last frame (no flicker between equals), then
    // nearer first; the index keeps the order total.
    std::vector<std::size_t> order(candidates.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
        const Candidate& p = candidates[x];
        const Candidate& q = candidates[y];
        if (p.tier != q.tier) {
            return p.tier < q.tier;
        }
        if (p.placed_before != q.placed_before) {
            return p.placed_before;
        }
        if (p.depth != q.depth) {
            return p.depth < q.depth;
        }
        return x < y;
    });

    // Other labels keep off every marker; a label clears the largest marker on its own spot (a
    // ship's focus ring around the station it is docked at).
    std::vector<Rect> markers;
    for (const Disc& d : discs) {
        const float m = std::max(d.r, 3.0F * s) + 1.0F * s;
        markers.push_back({d.x - m, d.y - m, 2.0F * m, 2.0F * m});
    }
    std::vector<LabelBox> boxes;
    boxes.reserve(order.size());
    for (const std::size_t i : order) {
        Candidate& c = candidates[i];
        for (const Disc& d : discs) {
            if (std::abs(d.x - c.box.anchor_x) < 1.5F && std::abs(d.y - c.box.anchor_y) < 1.5F) {
                c.box.marker_px = std::max(c.box.marker_px, d.r);
            }
        }
        const TextSize size = measure(c.text, c.style);
        c.box.w = size.w;
        c.box.h = size.h;
        boxes.push_back(c.box);
    }
    const std::vector<std::optional<Rect>> placed =
        declutter(boxes, obstacles, markers, in.width, in.height, 4.0F * s);

    placed_last_.clear();
    for (std::size_t k = 0; k < order.size(); ++k) {
        if (!placed[k]) {
            continue;
        }
        Candidate& c = candidates[order[k]];
        if (c.ref) {
            placed_last_.push_back(*c.ref);
        }
        placed_labels_.emplace_back(c.text, *placed[k]);
        world_layer.texts.push_back({std::move(c.text), c.style, snap(placed[k]->x), snap(placed[k]->y), c.color, true});
    }
}

} // namespace viewer
