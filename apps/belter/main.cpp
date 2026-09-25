#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "expanse/content.hpp"
#include "expanse/shell.hpp"
#include "line_editor.hpp"
#if SIM_HAVE_TUI
#include "tui.hpp"
#endif
#include "simcore/command_bus.hpp"
#include "simcore/doc.hpp"
#include "simcore/repl.hpp"
#if SIM_HAVE_VIEWER
#include <thread>

#include "viewer/view_snapshot.hpp"
#include "viewer/viewer.hpp"
#include "viewer/viewer_link.hpp"
#endif

namespace {

constexpr std::string_view usage = R"(usage: belter [--data DIR] [--script FILE] [--echo] [--plain] [--view]
              [--color auto|always|never]

  --data DIR     game data directory (default: the source tree's data/)
  --script FILE  run commands from FILE and exit (stops at the first error)
  --echo         with --script: echo each command before its output
  --plain        line-by-line shell instead of the full-screen interface
  --color WHEN   auto (default: colour on a terminal unless NO_COLOR is set), always, never
  --view         also open the 3D system viewer window (builds with SIM_VIEWER=ON); the shell
                 keeps working as usual and the window follows each command. With --script the
                 window stays open after the script until you close it.
  --screenshot F with --view and --script: run the script, render the result, save it to F
                 (.png) and exit. Works headless with SDL_VIDEO_DRIVER=offscreen.

On a terminal belter opens a full-screen interface; with --plain, or when input or output is
redirected, it reads commands line by line. Colour follows the NO_COLOR convention.
)";

struct Args {
    std::string data_dir = BELTER_DEFAULT_DATA_DIR;
    std::string script;
    bool echo = false;
    bool plain = false;
    std::string color = "auto";
    bool view = false;
    std::string screenshot;
};

bool parse_args(std::span<char*> argv, Args& args) {
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string_view a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argv.size() ? argv[++i] : nullptr; };
        if (a == "--data") {
            const char* v = value();
            if (v == nullptr) return false;
            args.data_dir = v;
        } else if (a == "--script") {
            const char* v = value();
            if (v == nullptr) return false;
            args.script = v;
        } else if (a == "--echo") {
            args.echo = true;
        } else if (a == "--plain") {
            args.plain = true;
        } else if (a == "--color") {
            const char* v = value();
            if (v == nullptr) return false;
            args.color = v;
            if (args.color != "auto" && args.color != "always" && args.color != "never") return false;
        } else if (a == "--view") {
            args.view = true;
        } else if (a == "--screenshot") {
            const char* v = value();
            if (v == nullptr) return false;
            args.screenshot = v;
        } else {
            return false;
        }
    }
    return true;
}

// The escape codes to use for `is_terminal` output. --color overrides detection and NO_COLOR
// (per no-color.org, an explicit flag wins over the environment).
sim::Ansi ansi_mode(const Args& args, bool is_terminal) {
    if (args.color == "always") {
        return sim::Ansi::color;
    }
    if (args.color == "never") {
        return is_terminal ? sim::Ansi::mono : sim::Ansi::none;
    }
    return is_terminal ? sim::ansi_for_terminal(true) : sim::Ansi::none;
}

// Runs the shell in the mode the arguments and terminal call for; returns the exit code.
int run_shell(const Args& args, expanse::ShellBus& bus, expanse::Session& session) {
    if (!args.script.empty()) {
        std::ifstream file(args.script);
        if (!file) {
            std::cerr << std::format("cannot open script '{}'\n", args.script);
            return 1;
        }
        sim::StreamLineReader reader(file, args.script, args.echo ? &std::cout : nullptr);
        return sim::run_repl(bus, session, reader, std::cout,
                             {.prompt = "> ", .stop_on_error = true, .ansi = ansi_mode(args, false)});
    }

#if SIM_HAVE_TUI
    // Full screen only for a person at a capable terminal.
    if (!args.plain && sim::is_interactive_terminal() && sim::ansi_for_terminal(true) != sim::Ansi::none) {
        belter::Tui tui(bus, session, {.color = ansi_mode(args, true) == sim::Ansi::color});
        return tui.run();
    }
#endif

    std::unique_ptr<sim::LineReader> terminal = sim::make_terminal_reader(
        [&bus](std::string_view line) { return bus.registry().complete(line); });
    // Piped input: echo each line so the transcript reads like an interactive session.
    sim::StreamLineReader piped(std::cin, {}, &std::cout);
    sim::LineReader& reader = terminal ? *terminal : piped;

    if (terminal) {
        std::cout << "BELTER — a hard-SF trading sim. 'new' starts a game, 'help' lists commands.\n";
    }
    // Colour only when a person is at the terminal; piped sessions stay plain text.
    const sim::Ansi ansi = ansi_mode(args, terminal != nullptr);
    return sim::run_repl(bus, session, reader, std::cout, {.ansi = ansi});
}

#if SIM_HAVE_VIEWER
// The window takes the main thread (an SDL requirement on some platforms); the shell runs on a
// second thread and publishes a read-only snapshot after every line. Only the shell thread ever
// touches the session.
int run_with_viewer(const Args& args, expanse::ShellBus& bus, expanse::Session& session) {
    viewer::ViewerLink link;
    auto publish = [&link](const expanse::Session& s) {
        if (link.viewer_closed()) {
            return;
        }
        viewer::ViewHints hints;
        if (s.world) {
            for (auto [id, ship] : s.world->ships) {
                if (ship.owner == s.world->player) {
                    hints.focus_ship = id;
                    break;
                }
            }
        }
        hints.plot_preview = s.last_plot;
        link.publish(viewer::make_snapshot(s, std::move(hints)));
    };
    bus.on_after_line([&publish](const expanse::Session& s, const sim::LineResult&) { publish(s); });
    publish(session);

    if (!args.screenshot.empty()) {
        // Deterministic capture: finish the script first, then render its end state.
        const int rc = run_shell(args, bus, session);
        viewer::ViewerOptions opts;
        opts.max_frames = 30;
        opts.screenshot = args.screenshot;
        opts.print_controls = false;
        return rc != 0 ? rc : viewer::run_viewer(link, opts);
    }

    // Before the shell (and a full-screen interface) takes over the terminal.
    std::cerr << "belter: viewer controls: " << viewer::controls_help() << '\n';
    viewer::ViewerOptions opts;
    opts.print_controls = false;
    int shell_rc = 0;
    std::thread shell([&] {
        shell_rc = run_shell(args, bus, session);
        if (args.script.empty()) {
            link.request_close(); // leaving the shell closes the window
        }
    });
    if (viewer::run_viewer(link, opts) != 0) {
        std::cerr << "belter: the viewer could not start; the shell carries on without it\n";
    }
    shell.join();
    return shell_rc;
}
#endif

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

    expanse::Session session;
    session.content = content;
    expanse::ShellBus bus;
    expanse::register_game_commands(bus, content);

    if (!args.screenshot.empty() && (!args.view || args.script.empty())) {
        std::cerr << "belter: --screenshot needs --view and --script\n";
        return 2;
    }
    if (args.view) {
#if SIM_HAVE_VIEWER
        return run_with_viewer(args, bus, session);
#else
        std::cerr << "belter: built without the viewer; reconfigure with -DSIM_VIEWER=ON\n";
        return 2;
#endif
    }
    return run_shell(args, bus, session);
}
