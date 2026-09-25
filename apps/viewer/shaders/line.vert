#version 450
#extension GL_GOOGLE_include_directive : require

// Line strips (orbits, transits, plotted courses). The CPU samples them every frame directly in
// camera-relative coordinates (double subtraction, then float), densest near the camera.

#include "frame.glsl"

layout(std140, set = 1, binding = 1) uniform Strip {
    vec4 color; // straight RGBA
};

layout(location = 0) in vec3 in_position; // camera-relative [AU]

layout(location = 0) out vec4 v_color;

void main() {
    gl_Position = view_proj * vec4(in_position, 1.0);
    v_color = color;
}
