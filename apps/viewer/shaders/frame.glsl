// Per-frame uniforms shared by every shader stage; must match FrameUniforms in scene.hpp.
//
// SDL GPU's SPIR-V resource layout (SDL_CreateGPUShader): vertex shaders read sampled textures,
// then storage textures, then storage buffers from set 0 and uniform buffers from set 1;
// fragment shaders use sets 2 and 3 the same way. Within a set, bindings count from 0 in that
// order and match the slot indices passed to SDL_BindGPU* / SDL_PushGPU*UniformData.
//
// Positions reaching the GPU are camera-relative (the CPU subtracts the eye position in double
// precision), in scene units of 1 AU, so view_proj carries rotation and projection only. The
// projection is reversed-Z with an infinite far plane: depth = near / distance, 1 at the near
// plane and 0 at infinity (clear to 0, compare GREATER).
//
// Vertex stages read it from set 1; a fragment stage defines FRAME_SET 3 before the #include.
#ifndef FRAME_SET
#define FRAME_SET 1
#endif
layout(std140, set = FRAME_SET, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 viewport; // x, y: target size [px]; z: focal length [px] (half height / tan(fov_y / 2))
    vec4 sun;      // xyz: camera-relative position of the Sun [AU]
};
