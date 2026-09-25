#include "viewer/viewer.hpp"

#include "expanse/calendar.hpp"
#include "module_pins.hpp"
#include "nav.hpp"
#include "overlay.hpp"
#include "renderer.hpp"
#include "scene.hpp"
#include "text.hpp"

#define SDL_MAIN_HANDLED // the host program owns main(); see SDL_SetMainReady below
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

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
    bool ttf = false;

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
        if (ttf) {
            TTF_Quit();
        }
        SDL_Quit();
    }
};

// Scene-sized offscreen colour and depth targets; recreated when the drawable size changes.
struct SceneTarget {
    SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    GpuTexture texture;
    GpuTexture depth;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool ensure(SDL_GPUDevice* device, std::uint32_t w, std::uint32_t h) {
        if (texture && depth && w == width && h == height) {
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
        info.format = depth_format;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        depth = GpuTexture{device, SDL_CreateGPUTexture(device, &info)};
        width = w;
        height = h;
        return texture && depth;
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

Key to_key(SDL_Keycode key) {
    if (key >= SDLK_0 && key <= SDLK_9) {
        return Key::digit;
    }
    switch (key) {
    case SDLK_F:
        return Key::f;
    case SDLK_HOME:
        return Key::home;
    case SDLK_TAB:
        return Key::tab;
    case SDLK_LEFTBRACKET:
        return Key::left_bracket;
    case SDLK_RIGHTBRACKET:
        return Key::right_bracket;
    case SDLK_R:
        return Key::r;
    case SDLK_PLUS:
    case SDLK_EQUALS: // + without shift on US layouts
    case SDLK_KP_PLUS:
        return Key::plus;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
        return Key::minus;
    case SDLK_ESCAPE:
        return Key::escape;
    case SDLK_Q:
        return Key::q;
    case SDLK_H:
        return Key::h;
    case SDLK_I:
        return Key::i;
    default:
        return Key::other;
    }
}

// SDL event -> the SDL-free InputEvent the Navigator understands (nullopt: not one of ours).
// Mouse coordinates arrive in window points; `density` converts them to drawable pixels.
std::optional<InputEvent> to_input(const SDL_Event& e, double density) {
    using Type = InputEvent::Type;
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return InputEvent{.type = Type::quit};
    case SDL_EVENT_KEY_DOWN: {
        const Key key = to_key(e.key.key);
        return InputEvent{.type = Type::key_down,
                          .key = key,
                          .digit = key == Key::digit ? static_cast<int>(e.key.key - SDLK_0) : 0,
                          .shift = (e.key.mod & SDL_KMOD_SHIFT) != 0};
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        MouseButton button = MouseButton::left;
        if (e.button.button == SDL_BUTTON_MIDDLE) {
            button = MouseButton::middle;
        } else if (e.button.button == SDL_BUTTON_RIGHT) {
            button = MouseButton::right;
        } else if (e.button.button != SDL_BUTTON_LEFT) {
            return std::nullopt;
        }
        return InputEvent{.type = e.button.down ? Type::button_down : Type::button_up,
                          .shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0,
                          .button = button,
                          .x = static_cast<double>(e.button.x) * density,
                          .y = static_cast<double>(e.button.y) * density};
    }
    case SDL_EVENT_MOUSE_MOTION:
        return InputEvent{.type = Type::motion,
                          .dx = static_cast<double>(e.motion.xrel) * density,
                          .dy = static_cast<double>(e.motion.yrel) * density,
                          .x = static_cast<double>(e.motion.x) * density,
                          .y = static_cast<double>(e.motion.y) * density};
    case SDL_EVENT_MOUSE_WHEEL:
        return InputEvent{.type = Type::wheel, .dy = static_cast<double>(e.wheel.y)};
    default:
        return std::nullopt;
    }
}

// The initial view from ViewerOptions, applied without a fly-in once the first scene exists.
void apply_initial_view(Navigator& nav, const Scene& scene, const ViewerOptions& options) {
    OrbitCamera& cam = nav.orbit_camera();
    const std::optional<double> distance =
        options.distance_au ? std::optional(*options.distance_au * expanse::units::au_m) : std::nullopt;
    if (options.focus.empty() || !nav.focus_key(options.focus, scene, distance, false)) {
        if (!options.focus.empty()) {
            std::cerr << std::format("belter viewer: nothing called '{}' to focus on\n", options.focus);
        }
        cam.reset(scene, false);
        if (distance) {
            cam.zoom(std::log(cam.target_distance_m() / *distance) / std::log(OrbitCamera::zoom_step));
        }
    }
    const Camera defaults;
    cam.set_angles(options.yaw_deg ? expanse::units::deg(*options.yaw_deg) : defaults.yaw_rad,
                   options.pitch_deg ? expanse::units::deg(*options.pitch_deg) : defaults.pitch_rad);
    cam.update(60.0, scene); // settle any zoom easing
    nav.set_show_info(options.show_info);
}

int run(ViewerLink& link, const ViewerOptions& options) {
    SDL_SetMainReady(); // we provide main() ourselves (no SDL_main.h)
    SDL_SetAppMetadata("belter viewer", nullptr, "net.data-sandbox.belter");
    ModulePins pins; // sanitizer builds: keep the GPU driver mapped until exit (module_pins.hpp)
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
    pins.pin_new();
    // VSYNC is the default and always supported; set it explicitly as the frame pacing contract.
    if (!SDL_SetGPUSwapchainParameters(device, platform.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                       SDL_GPU_PRESENTMODE_VSYNC)) {
        return fail("SDL_SetGPUSwapchainParameters");
    }

    SceneTarget target;
    target.depth_format = Renderer::choose_depth_format(device);
    if (target.depth_format == SDL_GPU_TEXTUREFORMAT_D24_UNORM) {
        std::cerr << "belter viewer: no float depth buffer on this GPU; distant objects may z-fight\n";
    }
    std::unique_ptr<Renderer> renderer = Renderer::create(device, target.depth_format);
    if (!renderer) {
        return fail("creating pipelines");
    }
    if (!TTF_Init()) {
        return fail("TTF_Init");
    }
    platform.ttf = true;
    std::unique_ptr<OverlayRenderer> text =
        OverlayRenderer::create(device, Renderer::target_format, SDL_GetWindowDisplayScale(platform.window));
    if (!text) {
        return fail("creating the text overlay");
    }
    const MeasureText measure = [&text](std::string_view s, TextStyle style) { return text->measure(s, style); };
    if (options.print_controls) {
        std::cerr << std::format("belter viewer: {}\n", controls_help_text);
    }

    Navigator nav;
    std::optional<ObjectRef> hovered;
    bool initial_view_applied = false;
    std::shared_ptr<const ViewSnapshot> shown;
    std::uint64_t shown_generation = 0;
    Scene scene;
    FrameData frame;
    Overlay overlay;
    OverlayDrawList overlay_list;
    Uint64 last_ns = SDL_GetTicksNS();

    for (int frame_no = 1;; ++frame_no) {
        bool open = true;
        SDL_Event event;
        const double density = std::max(static_cast<double>(SDL_GetWindowPixelDensity(platform.window)), 1e-3);
        while (SDL_PollEvent(&event)) {
            if (const std::optional<InputEvent> input = to_input(event, density)) {
                open = nav.handle(*input, scene) && open;
            }
        }
        if (!open || link.close_requested()) {
            break;
        }

        bool retitle = false;
        if (std::shared_ptr<const ViewSnapshot> latest = link.latest(); latest != shown) {
            shown = std::move(latest);
            ++shown_generation;
            scene = shown ? build_scene(*shown) : Scene{};
            retitle = true;
            if (!initial_view_applied && !scene.objects.empty()) {
                apply_initial_view(nav, scene, options);
                initial_view_applied = true;
            }
        }
        hovered = nav.hovered(scene);
        if (retitle) {
            std::string title = options.title;
            if (shown) {
                title += std::format(" - {}", expanse::calendar::format_datetime(shown->time));
            }
            SDL_SetWindowTitle(platform.window, title.c_str());
        }

        const Uint64 now_ns = SDL_GetTicksNS();
        const double dt = std::min(static_cast<double>(now_ns - last_ns) * 1e-9, 0.1); // no leaps after a stall
        last_ns = now_ns;
        nav.update(dt, scene);

        // The overlay is laid out (and its glyphs rasterised, through SDL_ttf's own uploads)
        // before this frame's command buffer exists, at the window's current pixel size.
        int pixel_w = 0;
        int pixel_h = 0;
        SDL_GetWindowSizeInPixels(platform.window, &pixel_w, &pixel_h);
        if (pixel_w > 0 && pixel_h > 0) {
            if (!text->set_ui_scale(SDL_GetWindowDisplayScale(platform.window))) {
                return fail("reopening fonts");
            }
            nav.set_viewport(static_cast<double>(pixel_w), static_cast<double>(pixel_h));
            const OverlayInput overlay_input{.snapshot = shown.get(),
                                             .snapshot_generation = shown_generation,
                                             .scene = &scene,
                                             .camera = nav.camera(),
                                             .width = static_cast<float>(pixel_w),
                                             .height = static_cast<float>(pixel_h),
                                             .ui_scale = text->ui_scale(),
                                             .focus = nav.orbit_camera().focus(),
                                             .hovered = hovered,
                                             .cursor_x = nav.cursor_x(),
                                             .cursor_y = nav.cursor_y(),
                                             .show_hints = nav.show_hints(),
                                             .show_info = nav.show_info()};
            overlay.build(overlay_input, measure, overlay_list);
            text->prepare(overlay_list);
            if (const auto& r = overlay.info_rect()) {
                nav.set_ui_region(std::array{static_cast<double>(r->x), static_cast<double>(r->y),
                                             static_cast<double>(r->w), static_cast<double>(r->h)});
            } else {
                nav.set_ui_region(std::nullopt);
            }
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

        nav.set_viewport(static_cast<double>(w), static_cast<double>(h));
        prepare_frame(scene, nav.camera(), static_cast<float>(w), static_cast<float>(h), frame);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        const bool uploaded = renderer->upload(copy, frame) && text->upload(copy);
        SDL_EndGPUCopyPass(copy);
        if (!uploaded) {
            return submit_and_fail(cmd, "uploading frame data");
        }
        renderer->draw(cmd, target.texture.get(), target.depth.get(), frame);
        text->draw(cmd, target.texture.get(), static_cast<float>(w), static_cast<float>(h));

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
        if (frame_no == 1) {
            pins.pin_new(); // drivers may load their shader compiler lazily, at the first pipeline
        }
        if (last) {
            break;
        }
    }
    return 0; // GPU objects are released before `platform` tears down the device
}

} // namespace

std::string_view controls_help() { return controls_help_text; }

int run_viewer(ViewerLink& link, const ViewerOptions& options) {
    const int status = run(link, options);
    link.notify_viewer_closed();
    return status;
}

} // namespace viewer
