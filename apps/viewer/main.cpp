// belter-view: the system viewer on its own. Loads content, starts a new game (or loads a save),
// publishes a snapshot and opens the window. With --animate a second thread advances the clock
// and publishes after every step, the way the shell thread will after every command.

#include "expanse/content.hpp"
#include "expanse/scenario.hpp"
#include "expanse/ships.hpp"
#include "expanse/simulation.hpp"
#include "expanse/world.hpp"
#include "viewer/viewer.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view usage = R"(usage: belter-view [options]

  --data DIR          game data directory (default: the source tree's data/)
  --scenario KEY      scenario for a new game (default: the first one)
  --seed N            seed for a new game (default: 1)
  --load FILE         show a saved game instead of starting a new one
  --plot STATION      preview a course from the player's ship to STATION (a view hint)
  --animate DAYS      advance the game DAYS per second on a background thread
  --size WxH          window size (default: 1280x720)
  --frames N          exit after N frames
  --screenshot FILE   write the last frame to FILE (.png or .bmp); implies --frames 3 unless given
  --gpu-debug         enable SDL GPU debug mode / Vulkan validation layers

Controls: drag to orbit, mouse wheel to zoom, R to reset the camera, Esc or Q to close.
Headless: SDL_VIDEO_DRIVER=offscreen belter-view --screenshot out.png
)";

struct Args {
    std::string data_dir = BELTER_DEFAULT_DATA_DIR;
    std::string scenario;
    std::uint64_t seed = 1;
    std::string load;
    std::string plot;
    double animate_days_per_s = 0.0;
    viewer::ViewerOptions view;
};

template <typename T>
bool parse_number(std::string_view s, T& out) {
    const auto* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc{} && ptr == end;
}

bool parse_args(std::span<char*> argv, Args& args) {
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string_view a = argv[i];
        auto value = [&]() -> std::string_view { return i + 1 < argv.size() ? argv[++i] : std::string_view{}; };
        if (a == "--gpu-debug") {
            args.view.gpu_debug = true;
            continue;
        }
        const std::string_view v = value();
        if (v.empty()) {
            return false;
        }
        if (a == "--data") {
            args.data_dir = v;
        } else if (a == "--scenario") {
            args.scenario = v;
        } else if (a == "--seed") {
            if (!parse_number(v, args.seed)) return false;
        } else if (a == "--load") {
            args.load = v;
        } else if (a == "--plot") {
            args.plot = v;
        } else if (a == "--animate") {
            if (!parse_number(v, args.animate_days_per_s) || args.animate_days_per_s < 0.0) return false;
        } else if (a == "--size") {
            const std::size_t x = v.find('x');
            if (x == std::string_view::npos || !parse_number(v.substr(0, x), args.view.width) ||
                !parse_number(v.substr(x + 1), args.view.height) || args.view.width <= 0 || args.view.height <= 0) {
                return false;
            }
        } else if (a == "--frames") {
            if (!parse_number(v, args.view.max_frames) || args.view.max_frames <= 0) return false;
        } else if (a == "--screenshot") {
            args.view.screenshot = v;
        } else {
            return false;
        }
    }
    if (!args.view.screenshot.empty() && args.view.max_frames == 0) {
        args.view.max_frames = 3; // let the swapchain settle before capturing
    }
    return true;
}

std::optional<expanse::World> start_world(const Args& args, const expanse::Content& content) {
    if (!args.load.empty()) {
        std::ifstream file(args.load, std::ios::binary);
        if (!file) {
            std::cerr << std::format("cannot open '{}'\n", args.load);
            return std::nullopt;
        }
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        return expanse::load_world(bytes, content);
    }
    const auto& scenarios = content.table<expanse::ScenarioDef>();
    if (scenarios.empty()) {
        std::cerr << "no scenarios in the game data\n";
        return std::nullopt;
    }
    const std::string key = args.scenario.empty() ? scenarios.keys().front() : args.scenario;
    return expanse::new_game(content, key, args.seed);
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(std::span(argv, static_cast<std::size_t>(argc)), args)) {
        std::cerr << usage;
        return 2;
    }

    sim::Diagnostics diags;
    std::shared_ptr<const expanse::Content> content = expanse::Content::load(args.data_dir, diags);
    if (!content) {
        std::cerr << diags.to_string();
        return 1;
    }

    std::optional<expanse::World> world;
    try {
        world = start_world(args, *content);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    if (!world) {
        return 1;
    }

    // Highlight the player's ship, as the shell will for the ship being commanded.
    viewer::ViewHints hints;
    for (auto [id, ship] : world->ships) {
        if (ship.owner == world->player) {
            hints.focus_ship = id;
            break;
        }
    }
    if (!args.plot.empty()) {
        const expanse::StationId dest = content->find<expanse::StationDef>(args.plot);
        if (dest.is_null() || !hints.focus_ship) {
            std::cerr << std::format("cannot plot to '{}'\n", args.plot);
            return 1;
        }
        hints.plot_preview = expanse::plot_course(*content, *world, *hints.focus_ship, dest);
    }

    viewer::ViewerLink link;
    link.publish(viewer::make_snapshot(content, *world, hints));

    // Stand-in for the shell thread: owns the World, publishes after every change.
    std::jthread sim_thread;
    if (args.animate_days_per_s > 0.0 && args.view.screenshot.empty()) {
        sim_thread = std::jthread([&link, &content, hints, w = std::move(*world), days = args.animate_days_per_s](
                                      const std::stop_token& stop) mutable {
            constexpr auto step = std::chrono::milliseconds(50);
            hints.plot_preview.reset(); // a "depart now" course goes stale as soon as time moves
            const auto seconds_per_step = static_cast<std::int64_t>(days * 86400.0 * 0.05);
            while (!stop.stop_requested() && !link.viewer_closed()) {
                expanse::advance_to(*content, w, w.now() + sim::Duration{std::max<std::int64_t>(seconds_per_step, 1)},
                                    false);
                link.publish(viewer::make_snapshot(content, w, hints));
                std::this_thread::sleep_for(step);
            }
        });
    }

    const int status = viewer::run_viewer(link, args.view);
    // run_viewer has set viewer_closed(); the jthread joins on destruction.
    return status;
}
