#version 450

layout(location = 0) in vec4 v_color;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(v_color.rgb * v_color.a, v_color.a); // premultiplied
}
