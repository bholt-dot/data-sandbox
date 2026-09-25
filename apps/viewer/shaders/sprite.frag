#version 450

// Sprite shapes: 0 = disc, 1 = diamond, 2 = ring; plus an optional exponential glow halo.
// The core fades with v_style.w (dot -> sphere crossfade); the halo does not.
// Output is premultiplied alpha (blend: ONE, ONE_MINUS_SRC_ALPHA).

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec4 v_style;

layout(location = 0) out vec4 out_color;

void main() {
    int shape = int(v_style.y + 0.5);
    float d = shape == 1 ? abs(v_uv.x) + abs(v_uv.y) : length(v_uv);
    float aa = max(fwidth(d), 1e-4);

    float core = 1.0 - smoothstep(1.0 - aa, 1.0 + aa, d);
    if (shape == 2) {
        core *= smoothstep(0.6 - aa, 0.6 + aa, d);
    }
    core *= v_style.w; // fades out as the body's lit sphere fades in
    float extent = v_style.x;
    float halo = v_style.z * exp(-3.0 * max(d - 1.0, 0.0)) * (1.0 - smoothstep(0.7 * extent, extent, d));
    halo *= mix(v_style.w, 1.0, smoothstep(1.0 - aa, 1.0 + aa, d)); // not over a drawn sphere
    float coverage = clamp(core + halo * (1.0 - core), 0.0, 1.0) * v_color.a;
    if (coverage <= 0.0) {
        discard;
    }
    out_color = vec4(v_color.rgb * coverage, coverage);
}
