#include "renderer.hpp"

#include <cstddef>
#include <span>

namespace viewer {

namespace {

// Premultiplied alpha: shaders output rgb * a.
SDL_GPUColorTargetDescription premultiplied_target() {
    SDL_GPUColorTargetDescription desc{};
    desc.format = Renderer::target_format;
    desc.blend_state.enable_blend = true;
    desc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    desc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    desc.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    desc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    desc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    desc.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    return desc;
}

enum class DepthMode : std::uint8_t { none, test, test_write };

// Every pipeline of the pass declares the same colour + depth attachments.
SDL_GPUGraphicsPipelineCreateInfo pipeline_info(const SDL_GPUColorTargetDescription& target,
                                                SDL_GPUTextureFormat depth_format, DepthMode depth) {
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    // Clip at the near plane (SDL defaults to depth clamping instead): lines to points behind the
    // eye must be cut at w > 0, not clamped.
    info.rasterizer_state.enable_depth_clip = true;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = depth_format;
    info.depth_stencil_state.enable_depth_test = depth != DepthMode::none;
    info.depth_stencil_state.enable_depth_write = depth == DepthMode::test_write;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER; // reversed-Z: nearer is larger
    return info;
}

template <typename T>
std::span<const std::byte> bytes_of(const std::vector<T>& v) {
    return std::as_bytes(std::span(v));
}

} // namespace

SDL_GPUTextureFormat Renderer::choose_depth_format(SDL_GPUDevice* device) {
    for (const SDL_GPUTextureFormat f : {SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT}) {
        if (SDL_GPUTextureSupportsFormat(device, f, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            return f;
        }
    }
    return SDL_GPU_TEXTUREFORMAT_D24_UNORM;
}

std::unique_ptr<Renderer> Renderer::create(SDL_GPUDevice* device, SDL_GPUTextureFormat depth_format) {
    std::unique_ptr<Renderer> r(new Renderer(device));
    const SDL_GPUColorTargetDescription target = premultiplied_target();

    {
        // sphere.vert: storage buffer Spheres (set 0), uniform Frame (set 1, slot 0);
        // sphere.frag: uniform Frame (set 3, slot 0).
        GpuShader vs = load_shader(device, "sphere.vert", {.storage_buffers = 1, .uniform_buffers = 1});
        GpuShader fs = load_shader(device, "sphere.frag", {.uniform_buffers = 1});
        if (!vs || !fs) {
            return nullptr;
        }
        SDL_GPUGraphicsPipelineCreateInfo info = pipeline_info(target, depth_format, DepthMode::test_write);
        info.vertex_shader = vs.get();
        info.fragment_shader = fs.get();
        r->sphere_pipeline_ = GpuPipeline{device, SDL_CreateGPUGraphicsPipeline(device, &info)};
        if (!r->sphere_pipeline_) {
            return nullptr;
        }
    }
    {
        // sprite.vert: storage buffer Sprites (set 0), uniform Frame (set 1, slot 0).
        GpuShader vs = load_shader(device, "sprite.vert", {.storage_buffers = 1, .uniform_buffers = 1});
        GpuShader fs = load_shader(device, "sprite.frag", {});
        if (!vs || !fs) {
            return nullptr;
        }
        SDL_GPUGraphicsPipelineCreateInfo info = pipeline_info(target, depth_format, DepthMode::none);
        info.vertex_shader = vs.get();
        info.fragment_shader = fs.get();
        r->sprite_pipeline_ = GpuPipeline{device, SDL_CreateGPUGraphicsPipeline(device, &info)};
        if (!r->sprite_pipeline_) {
            return nullptr;
        }
    }
    {
        // line.vert: uniforms Frame (set 1, slot 0) and Strip (set 1, slot 1); one vertex buffer.
        GpuShader vs = load_shader(device, "line.vert", {.uniform_buffers = 2});
        GpuShader fs = load_shader(device, "line.frag", {});
        if (!vs || !fs) {
            return nullptr;
        }
        SDL_GPUVertexBufferDescription vb{};
        vb.slot = 0;
        vb.pitch = sizeof(LineVertex);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute position{};
        position.location = 0;
        position.buffer_slot = 0;
        position.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        position.offset = 0;

        SDL_GPUGraphicsPipelineCreateInfo info = pipeline_info(target, depth_format, DepthMode::test);
        info.vertex_shader = vs.get();
        info.fragment_shader = fs.get();
        info.vertex_input_state.vertex_buffer_descriptions = &vb;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = &position;
        info.vertex_input_state.num_vertex_attributes = 1;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_LINESTRIP;
        r->line_pipeline_ = GpuPipeline{device, SDL_CreateGPUGraphicsPipeline(device, &info)};
        if (!r->line_pipeline_) {
            return nullptr;
        }
    }
    return r;
}

bool Renderer::upload(SDL_GPUCopyPass* pass, const FrameData& frame) {
    return spheres_.upload(device_, pass, bytes_of(frame.spheres)) &&
           sprites_.upload(device_, pass, bytes_of(frame.sprites)) &&
           lines_.upload(device_, pass, bytes_of(frame.line_vertices));
}

void Renderer::draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTexture* depth,
                    const FrameData& frame) const {
    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.clear_color = {0.008F, 0.010F, 0.018F, 1.0F};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth_target{};
    depth_target.texture = depth;
    depth_target.clear_depth = 0.0F; // reversed-Z: 0 is infinitely far
    depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
    depth_target.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth_target.cycle = true;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth_target);

    // Uniform pushes persist for the command buffer, per stage and slot.
    SDL_PushGPUVertexUniformData(cmd, 0, &frame.frame, sizeof(frame.frame));
    SDL_PushGPUFragmentUniformData(cmd, 0, &frame.frame, sizeof(frame.frame));

    if (!frame.spheres.empty() && spheres_.get() != nullptr) {
        SDL_BindGPUGraphicsPipeline(pass, sphere_pipeline_.get());
        SDL_GPUBuffer* storage = spheres_.get();
        SDL_BindGPUVertexStorageBuffers(pass, 0, &storage, 1);
        SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(frame.spheres.size()), 0, 0);
    }

    if (!frame.strips.empty() && lines_.get() != nullptr) {
        SDL_BindGPUGraphicsPipeline(pass, line_pipeline_.get());
        const SDL_GPUBufferBinding binding{.buffer = lines_.get(), .offset = 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        for (const StripDraw& strip : frame.strips) {
            SDL_PushGPUVertexUniformData(cmd, 1, &strip.uniforms, sizeof(strip.uniforms));
            SDL_DrawGPUPrimitives(pass, strip.count, 1, strip.first, 0);
        }
    }

    if (!frame.sprites.empty() && sprites_.get() != nullptr) {
        SDL_BindGPUGraphicsPipeline(pass, sprite_pipeline_.get());
        SDL_GPUBuffer* storage = sprites_.get();
        SDL_BindGPUVertexStorageBuffers(pass, 0, &storage, 1);
        SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(frame.sprites.size()), 0, 0);
    }

    SDL_EndGPURenderPass(pass);
}

} // namespace viewer
