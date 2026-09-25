#include "viewer/viewer.hpp"

#include "expanse/calendar.hpp"
#include "renderer.hpp"
#include "scene.hpp"

#define SDL_MAIN_HANDLED // the host program owns main(); see SDL_SetMainReady below
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <iostream>
#include <string>

namespace viewer {

namespace {

int fail(std::string_view what) {
    std::cerr << std::format("belter viewer: {}: {}\n", what, SDL_GetError());
    return 1;
}

// Once a swapchain texture has been acquired the command buffer must be submitted; cancelling
// it is an error (SDL_CancelGPUCommandBuffer).
int submit_and_fail(SDL_GPUCommandBuffer* cmd, std::string_view what) {
    const std::string error = SDL_GetError();
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_SetError("%s", error.c_str());
    return fail(what);
}

// SDL init/quit, window and device, in construction order; torn down in reverse.
struct Platform {
    SDL_Window* window = nullptr;
    SDL_GPUDevice* device = nullptr;
    bool claimed = false;

    Platform() = default;
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    ~Platform() {
        if (device != nullptr) {
            SDL_WaitForGPUIdle(device);
            if (claimed) {
                SDL_ReleaseWindowFromGPUDevice(device, window);
            }
            SDL_DestroyGPUDevice(device);
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
    }
};

// Scene-sized offscreen colour target; recreated when the drawable size changes.
struct SceneTarget {
    GpuTexture texture;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool ensure(SDL_GPUDevice* device, std::uint32_t w, std::uint32_t h) {
        if (texture && w == width && h == height) {
            return true;
        }
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = Renderer::target_format;
        // SAMPLER: blit source.
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = w;
        info.height = h;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        texture = GpuTexture{device, SDL_CreateGPUTexture(device, &info)};
        width = w;
        height = h;
        return static_cast<bool>(texture);
    }
};

// Records a download of the scene target, submits, waits and writes the image.
bool submit_and_save(SDL_GPUDevice* device, SDL_GPUCommandBuffer* cmd, const SceneTarget& target,
                     const std::filesystem::path& path) {
    const std::uint32_t bytes = target.width * target.height * 4;
    const SDL_GPUTransferBufferCreateInfo tb_info{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = bytes, .props = 0};
    GpuTransferBuffer transfer{device, SDL_CreateGPUTransferBuffer(device, &tb_info)};
    if (!transfer) {
        const std::string error = SDL_GetError();
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_SetError("%s", error.c_str());
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region{};
    region.texture = target.texture.get();
    region.w = target.width;
    region.h = target.height;
    region.d = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = transfer.get();
    dst.pixels_per_row = target.width;
    dst.rows_per_layer = target.height;
    SDL_DownloadFromGPUTexture(copy, &region, &dst);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence == nullptr) {
        return false;
    }
    const bool waited = SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);
    if (!waited) {
        return false;
    }

    const void* pixels = SDL_MapGPUTransferBuffer(device, transfer.get(), false);
    if (pixels == nullptr) {
        return false;
    }
    // R8G8B8A8_UNORM is byte order R, G, B, A: SDL's RGBA32. Force opaque for the file.
    SDL_Surface* surface = SDL_CreateSurface(static_cast<int>(target.width), static_cast<int>(target.height),
                                             SDL_PIXELFORMAT_RGBA32);
    bool ok = surface != nullptr;
    if (ok) {
        const auto* src = static_cast<const std::uint8_t*>(pixels);
        for (std::uint32_t y = 0; y < target.height; ++y) {
            auto* row = static_cast<std::uint8_t*>(surface->pixels) + static_cast<std::size_t>(y) *
                                                                         static_cast<std::size_t>(surface->pitch);
            std::memcpy(row, src + static_cast<std::size_t>(y) * target.width * 4, std::size_t{target.width} * 4);
            for (std::uint32_t x = 0; x < target.width; ++x) {
                row[x * 4 + 3] = 255;
            }
        }
    }
    SDL_UnmapGPUTransferBuffer(device, transfer.get());
    if (ok) {
        const std::string file = path.string();
        ok = path.extension() == ".bmp" ? SDL_SaveBMP(surface, file.c_str()) : SDL_SavePNG(surface, file.c_str());
    }
    SDL_DestroySurface(surface);
    return ok;
}

struct Input {
    bool dragging = false;
    Camera home;
};

// Minimal controls until the camera bean: drag to orbit, wheel to zoom, R to reset, Esc/Q to close.
bool handle_event(const SDL_Event& e, Camera& camera, Input& input) {
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return false;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_Q) {
            return false;
        }
        if (e.key.key == SDLK_R) {
            camera = input.home;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_LEFT) {
            input.dragging = e.button.down;
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (input.dragging) {
            constexpr double radians_per_px = 0.005;
            camera.yaw_rad -= static_cast<double>(e.motion.xrel) * radians_per_px;
            camera.pitch_rad = std::clamp(camera.pitch_rad + static_cast<double>(e.motion.yrel) * radians_per_px,
                                          expanse::units::deg(-89.0), expanse::units::deg(89.0));
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        camera.distance_m = std::clamp(camera.distance_m * std::pow(0.85, static_cast<double>(e.wheel.y)),
                                       1.0e-4 * expanse::units::au_m, 200.0 * expanse::units::au_m);
        break;
    default:
        break;
    }
    return true;
}

int run(ViewerLink& link, const ViewerOptions& options) {
    SDL_SetMainReady(); // we provide main() ourselves (no SDL_main.h)
    SDL_SetAppMetadata("belter viewer", nullptr, "net.data-sandbox.belter");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return fail("SDL_Init");
    }
    Platform platform;

    platform.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, options.gpu_debug, nullptr);
    if (platform.device == nullptr) {
        return fail("no SPIR-V capable GPU device (Vulkan)");
    }
    SDL_GPUDevice* device = platform.device;

    platform.window = SDL_CreateWindow(options.title.c_str(), options.width, options.height,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (platform.window == nullptr) {
        return fail("SDL_CreateWindow");
    }
    if (!SDL_ClaimWindowForGPUDevice(device, platform.window)) {
        return fail("SDL_ClaimWindowForGPUDevice");
    }
    platform.claimed = true;
    // VSYNC is the default and always supported; set it explicitly as the frame pacing contract.
    if (!SDL_SetGPUSwapchainParameters(device, platform.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                       SDL_GPU_PRESENTMODE_VSYNC)) {
        return fail("SDL_SetGPUSwapchainParameters");
    }

    std::unique_ptr<Renderer> renderer = Renderer::create(device);
    if (!renderer) {
        return fail("creating pipelines");
    }

    Camera camera;
    Input input{.dragging = false, .home = camera};
    SceneTarget target;
    std::shared_ptr<const ViewSnapshot> shown;
    Scene scene;
    FrameData frame;
    bool scene_dirty = true;

    for (int frame_no = 1;; ++frame_no) {
        bool open = true;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            open = handle_event(event, camera, input) && open;
        }
        if (!open || link.close_requested()) {
            break;
        }

        if (std::shared_ptr<const ViewSnapshot> latest = link.latest(); latest != shown) {
            shown = std::move(latest);
            scene = shown ? build_scene(*shown) : Scene{};
            scene_dirty = true;
            const std::string title =
                shown ? std::format("{} - {}", options.title, expanse::calendar::format_datetime(shown->time))
                      : options.title;
            SDL_SetWindowTitle(platform.window, title.c_str());
        }

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
        if (cmd == nullptr) {
            return fail("SDL_AcquireGPUCommandBuffer");
        }
        SDL_GPUTexture* swapchain = nullptr;
        Uint32 w = 0;
        Uint32 h = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, platform.window, &swapchain, &w, &h)) {
            SDL_CancelGPUCommandBuffer(cmd);
            return fail("SDL_WaitAndAcquireGPUSwapchainTexture");
        }
        if (swapchain == nullptr || w == 0 || h == 0) {
            // Minimised or occluded: nothing to present this time round.
            SDL_CancelGPUCommandBuffer(cmd);
            SDL_Delay(16);
            continue;
        }
        if (!target.ensure(device, w, h)) {
            return submit_and_fail(cmd, "creating the scene target");
        }

        prepare_frame(scene, camera, static_cast<float>(w), static_cast<float>(h), frame);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        const bool uploaded = renderer->upload(copy, scene, scene_dirty, frame);
        SDL_EndGPUCopyPass(copy);
        if (!uploaded) {
            return submit_and_fail(cmd, "uploading frame data");
        }
        scene_dirty = false;
        renderer->draw(cmd, target.texture.get(), frame);

        SDL_GPUBlitInfo blit{};
        blit.source.texture = target.texture.get();
        blit.source.w = w;
        blit.source.h = h;
        blit.destination.texture = swapchain;
        blit.destination.w = w;
        blit.destination.h = h;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(cmd, &blit);

        const bool last = options.max_frames > 0 && frame_no >= options.max_frames;
        if (last && !options.screenshot.empty()) {
            if (!submit_and_save(device, cmd, target, options.screenshot)) {
                return fail(std::format("writing screenshot {}", options.screenshot.string()));
            }
            std::cerr << std::format("belter viewer: wrote {} ({}x{})\n", options.screenshot.string(), w, h);
        } else if (!SDL_SubmitGPUCommandBuffer(cmd)) {
            return fail("SDL_SubmitGPUCommandBuffer");
        }
        if (last) {
            break;
        }
    }
    return 0; // GPU objects are released before `platform` tears down the device
}

} // namespace

int run_viewer(ViewerLink& link, const ViewerOptions& options) {
    const int status = run(link, options);
    link.notify_viewer_closed();
    return status;
}

} // namespace viewer
