#version 450
#extension GL_GOOGLE_include_directive : require

// Line strips (orbits, transits, plotted courses). Vertices are stored relative to the strip's
// origin (a parent body, a transit's start point) so they stay small and static; the per-draw
// origin is the camera-relative position of that point, computed in double on the CPU.

#include "frame.glsl"

layout(std140, set = 1, binding = 1) uniform Strip {
    vec4 origin; // xyz: camera-relative origin [AU]
    vec4 color;  // straight RGBA
};

layout(location = 0) in vec3 in_position; // relative to the origin [AU]

layout(location = 0) out vec4 v_color;

void main() {
    gl_Position = view_proj * vec4(origin.xyz + in_position, 1.0);
    v_color = color;
}
