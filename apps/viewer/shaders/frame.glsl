// Per-frame uniforms shared by every vertex shader; must match FrameUniforms in scene.hpp.
//
// SDL GPU's SPIR-V resource layout (SDL_CreateGPUShader): vertex shaders read sampled textures,
// then storage textures, then storage buffers from set 0 and uniform buffers from set 1;
// fragment shaders use sets 2 and 3 the same way. Within a set, bindings count from 0 in that
// order and match the slot indices passed to SDL_BindGPU* / SDL_PushGPU*UniformData.
//
// Positions reaching the GPU are camera-relative (the CPU subtracts the eye position in double
// precision), in scene units of 1 AU, so view_proj carries rotation and projection only.
layout(std140, set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 viewport; // x, y: target size [px]; z: focal length [px] (half height / tan(fov_y / 2))
};
