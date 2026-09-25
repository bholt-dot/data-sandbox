#version 450
#extension GL_GOOGLE_include_directive : require

// Bodies up close: a ray-traced sphere impostor. Each instance is one camera-facing quad in the
// plane through the sphere's centre, just large enough to cover its silhouette under perspective
// (the tangent cone), and the fragment shader intersects the view ray with the exact sphere.

#include "frame.glsl"

struct Sphere {
    vec4 position_radius; // xyz: camera-relative centre [AU]; w: radius [AU]
    vec4 color;           // albedo (emission if emissive), straight alpha
    vec4 params;          // x: emissive
};

layout(std140, set = 0, binding = 0) readonly buffer Spheres {
    Sphere spheres[];
};

layout(location = 0) out vec3 v_point;        // camera-relative point on the quad [AU]
layout(location = 1) flat out vec4 v_sphere;  // centre, radius
layout(location = 2) flat out vec4 v_color;
layout(location = 3) flat out vec4 v_params;

const vec2 corners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                               vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main() {
    Sphere s = spheres[gl_InstanceIndex];
    vec3 c = s.position_radius.xyz;
    float r = s.position_radius.w;
    float d = length(c);
    vec3 w = c / d;
    vec3 u = normalize(cross(w, abs(w.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
    vec3 v = cross(w, u);
    // Silhouette radius in the centre plane: d * tan(asin(r / d)); plus ~2 px for antialiasing.
    float half_size = r * d / sqrt(max(d * d - r * r, 1e-3 * d * d)) + 2.0 * d / viewport.z;
    vec3 p = c + (u * corners[gl_VertexIndex].x + v * corners[gl_VertexIndex].y) * half_size;

    gl_Position = view_proj * vec4(p, 1.0);
    v_point = p;
    v_sphere = s.position_radius;
    v_color = s.color;
    v_params = s.params;
}
