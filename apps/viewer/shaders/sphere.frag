#version 450
#extension GL_GOOGLE_include_directive : require

// Ray-traced sphere: exact silhouette (antialiased over one pixel), exact depth written to
// gl_FragDepth (reversed-Z), Lambert lighting from the Sun with a dark night side. Emissive
// spheres (the Sun) get limb darkening instead. Output is premultiplied alpha.

#define FRAME_SET 3 // fragment-stage uniforms
#include "frame.glsl"

layout(location = 0) in vec3 v_point;
layout(location = 1) flat in vec4 v_sphere;
layout(location = 2) flat in vec4 v_color;
layout(location = 3) flat in vec4 v_params;

layout(location = 0) out vec4 out_color;

const float night_ambient = 0.03;

void main() {
    vec3 dir = normalize(v_point); // the eye is at the origin
    vec3 c = v_sphere.xyz;
    float r = v_sphere.w;
    float along = dot(dir, c);
    // Closest approach of the ray to the centre, in the numerically stable geometric form.
    vec3 miss = c - along * dir;
    float miss_len = length(miss);
    float edge_px = (r - miss_len) * viewport.z / along; // > 0 inside the silhouette
    float coverage = clamp(edge_px + 0.5, 0.0, 1.0);
    if (coverage <= 0.0 || along <= 0.0) {
        discard;
    }
    float half_chord = sqrt(max(r * r - miss_len * miss_len, 0.0));
    vec3 hit = dir * (along - half_chord);
    vec3 normal = normalize(hit - c);

    vec3 color;
    if (v_params.x > 0.5) {
        float mu = max(dot(normal, -dir), 0.0);
        color = v_color.rgb * (0.55 + 0.45 * sqrt(mu)); // limb darkening
    } else {
        vec3 to_sun = normalize(sun.xyz - c);
        float lambert = max(dot(normal, to_sun), 0.0);
        color = v_color.rgb * (lambert + night_ambient);
    }

    vec4 clip = view_proj * vec4(hit, 1.0);
    gl_FragDepth = clip.z / clip.w;
    float alpha = coverage * v_color.a;
    out_color = vec4(color * alpha, alpha);
}
