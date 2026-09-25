#version 450

// The 2D overlay (labels, HUD, panels): pixel-space triangles, origin top-left, y down. Text
// comes from SDL_ttf's glyph atlas; solid quads sample a 1x1 white texture with the same
// pipeline. Drawn after the scene in display space (with HDR, after tonemapping), no depth.

layout(std140, set = 1, binding = 0) uniform Overlay {
    vec4 viewport; // xy: target size [px]
};

layout(location = 0) in vec2 in_position; // [px]
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;    // straight alpha

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    vec2 ndc = in_position / viewport.xy * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    v_uv = in_uv;
    v_color = in_color;
}
