#version 450
#extension GL_GOOGLE_include_directive : require

// Screen-facing quads for bodies, stations and ships, one instance per sprite, pulled from a
// storage buffer (no vertex buffers): 6 vertices per instance, corners from gl_VertexIndex.
// Drawn without a depth test: the CPU drops markers hidden behind a body (scene.cpp, occluded).

#include "frame.glsl"

struct Sprite {
    vec4 position_radius; // xyz: camera-relative position [AU]; w: physical radius [AU]
    vec4 color;           // straight (non-premultiplied) RGBA
    vec4 style;           // x: minimum core radius [px]; y: shape; z: glow strength; w: core alpha
};

// SDL asks for std140 in storage buffers too (portable to its other backends); vec4-only
// members keep it identical to std430.
layout(std140, set = 0, binding = 0) readonly buffer Sprites {
    Sprite sprites[];
};

layout(location = 0) out vec2 v_uv;    // quad position in units of the core radius
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec4 v_style; // x: quad half-size in core radii; y: shape; z: glow; w: core alpha

const vec2 corners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                               vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main() {
    Sprite s = sprites[gl_InstanceIndex];
    vec2 corner = corners[gl_VertexIndex];
    vec4 clip = view_proj * vec4(s.position_radius.xyz, 1.0);

    // Clip w is the view-space depth, so radius * focal / w is the projected radius in pixels.
    float projected_px = s.position_radius.w * viewport.z / max(clip.w, 1e-12);
    float core_px = max(s.style.x, projected_px);
    float extent = 1.0 + 3.0 * s.style.z; // room for the glow halo
    clip.xy += corner * (core_px * extent) * (2.0 / viewport.xy) * clip.w;

    gl_Position = clip;
    v_uv = corner * extent;
    v_color = s.color;
    v_style = vec4(extent, s.style.y, s.style.z, s.style.w);
}
