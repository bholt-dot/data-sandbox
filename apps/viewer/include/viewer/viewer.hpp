#pragma once

// The system viewer window: a read-only 3D view of the snapshots published to a ViewerLink.
// SDL3 GPU API on Vulkan, SPIR-V shaders embedded in the binary.

#include "viewer/viewer_link.hpp"

#include <filesystem>
#include <string>

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
};

// Opens the window and renders until the window is closed, link.request_close() is called or
// max_frames frames have been rendered; calls link.notify_viewer_closed() on every exit path.
// Must be called on the main thread. Returns 0 on success; on failure prints the reason to
// stderr and returns non-zero (a missing display or Vulkan driver is a failure, not a crash).
int run_viewer(ViewerLink& link, const ViewerOptions& options);

} // namespace viewer
