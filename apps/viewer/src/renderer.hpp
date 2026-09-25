#pragma once

// GPU side of the viewer: pipelines and buffers, and the recording of one frame into an
// offscreen colour + depth target. Knows nothing about windows or swapchains, so the same code
// renders for the screen (blitted to the swapchain) and for screenshots (downloaded).
//
// Pass order: lit spheres (depth test + write, reversed-Z GREATER), then lines (depth tested
// against the spheres, no write), then sprites (no depth test; the CPU culls hidden ones).

#include "gpu.hpp"
#include "scene.hpp"

#include <memory>

namespace viewer {

class Renderer {
public:
    // The scene target format. The HDR work (repo-zvv9) switches this to a float format and adds
    // a tonemapping pass; the swapchain format is independent because frames reach it by blit.
    static constexpr SDL_GPUTextureFormat target_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    // The best depth format the device offers for reversed-Z: D32_FLOAT (Vulkan requires it or
    // X8_D24 to be supported), else one with a float depth plus stencil, else 24-bit.
    static SDL_GPUTextureFormat choose_depth_format(SDL_GPUDevice* device);

    // Null on failure (SDL error set).
    static std::unique_ptr<Renderer> create(SDL_GPUDevice* device, SDL_GPUTextureFormat depth_format);

    // Records buffer uploads; call outside any render pass, before draw().
    bool upload(SDL_GPUCopyPass* pass, const FrameData& frame);
    // Records a render pass that clears `target` and `depth` and draws the frame.
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTexture* depth, const FrameData& frame) const;

private:
    explicit Renderer(SDL_GPUDevice* device) : device_(device) {}

    SDL_GPUDevice* device_;
    GpuPipeline sphere_pipeline_;
    GpuPipeline sprite_pipeline_;
    GpuPipeline line_pipeline_;
    DynamicBuffer spheres_{SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ};
    DynamicBuffer sprites_{SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ};
    DynamicBuffer lines_{SDL_GPU_BUFFERUSAGE_VERTEX};
};

} // namespace viewer
