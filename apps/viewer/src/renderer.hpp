#pragma once

// GPU side of the viewer: pipelines and buffers, and the recording of one frame into an
// offscreen colour target. Knows nothing about windows or swapchains, so the same code renders
// for the screen (blitted to the swapchain) and for screenshots (downloaded).

#include "gpu.hpp"
#include "scene.hpp"

#include <memory>

namespace viewer {

class Renderer {
public:
    // The scene target format. The HDR work (repo-zvv9) switches this to a float format and adds
    // a tonemapping pass; the swapchain format is independent because frames reach it by blit.
    static constexpr SDL_GPUTextureFormat target_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    // Null on failure (SDL error set).
    static std::unique_ptr<Renderer> create(SDL_GPUDevice* device);

    // Records buffer uploads; call outside any render pass, before draw(). Line geometry is only
    // re-sent when `scene_changed`.
    bool upload(SDL_GPUCopyPass* pass, const Scene& scene, bool scene_changed, const FrameData& frame);
    // Records a render pass that clears `target` and draws the frame.
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, const FrameData& frame) const;

private:
    explicit Renderer(SDL_GPUDevice* device) : device_(device) {}

    SDL_GPUDevice* device_;
    GpuPipeline sprite_pipeline_;
    GpuPipeline line_pipeline_;
    DynamicBuffer sprites_{SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ};
    DynamicBuffer lines_{SDL_GPU_BUFFERUSAGE_VERTEX};
};

} // namespace viewer
