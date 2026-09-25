#pragma once

// The system viewer window: a read-only 3D view of the snapshots published to a ViewerLink.
// SDL3 GPU API on Vulkan, SPIR-V shaders embedded in the binary.

#include "viewer/viewer_link.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace viewer {

struct ViewerOptions {
    int width = 1280; // window size in screen coordinates (pixels for a screenshot run)
    int height = 720;
    std::string title = "belter - system view";
    bool gpu_debug = false; // SDL GPU debug mode (Vulkan validation layers if installed)
    // Stop after this many rendered frames; 0 = run until closed.
    int max_frames = 0;
    // If set, the last frame rendered (see max_frames) is read back from the GPU and written
    // here: .png, or .bmp.
    std::filesystem::path screenshot;
    // Initial view (no fly-in): focus a body or station by content key, or "ship" for the
    // highlighted ship; distance and angles override the focus's framing and the defaults.
    std::string focus;
    std::optional<double> distance_au;
    std::optional<double> yaw_deg;
    std::optional<double> pitch_deg;
    // Open with the info panel of the focus showing (as after a click on it).
    bool show_info = false;
    // Print the controls to stderr when the window opens.
    bool print_controls = true;
};

// Opens the window and renders until the window is closed, link.request_close() is called or
// max_frames frames have been rendered; calls link.notify_viewer_closed() on every exit path.
// Must be called on the main thread. Returns 0 on success; on failure prints the reason to
// stderr and returns non-zero (a missing display or Vulkan driver is a failure, not a crash).
int run_viewer(ViewerLink& link, const ViewerOptions& options);

// One line listing the window's mouse and key controls.
std::string_view controls_help();

} // namespace viewer
