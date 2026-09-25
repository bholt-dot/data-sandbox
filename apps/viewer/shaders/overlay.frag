#version 450

// Glyph coverage is the atlas alpha (SDL_ttf's TTF_IMAGE_ALPHA glyphs are white). Output is
// premultiplied alpha, like the scene's other passes.

layout(set = 2, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

void main() {
    float coverage = texture(atlas, v_uv).a * v_color.a;
    out_color = vec4(v_color.rgb * coverage, coverage);
}
