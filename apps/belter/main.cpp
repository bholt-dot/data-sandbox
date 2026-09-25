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

namespace {

constexpr std::string_view usage = R"(usage: belter [--data DIR] [--script FILE] [--echo] [--plain]

  --data DIR     game data directory (default: the source tree's data/)
  --script FILE  run commands from FILE and exit (stops at the first error)
  --echo         with --script: echo each command before its output
  --plain        line-by-line shell instead of the full-screen interface

On a terminal belter opens a full-screen interface; with --plain, or when input or output is
redirected, it reads commands line by line. Colour follows the NO_COLOR convention.
)";

struct Args {
    std::string data_dir = BELTER_DEFAULT_DATA_DIR;
    std::string script;
    bool echo = false;
    bool plain = false;
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
        } else {
            return false;
        }
    }
    return true;
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

    expanse::Session session{content, std::nullopt, 0};
    expanse::ShellBus bus;
    expanse::register_game_commands(bus, content);

    if (!args.script.empty()) {
        std::ifstream file(args.script);
        if (!file) {
            std::cerr << std::format("cannot open script '{}'\n", args.script);
            return 1;
        }
        sim::StreamLineReader reader(file, args.script, args.echo ? &std::cout : nullptr);
        return sim::run_repl(bus, session, reader, std::cout, {.prompt = "> ", .stop_on_error = true});
    }

#if SIM_HAVE_TUI
    // Full screen only for a person at a capable terminal.
    if (!args.plain && sim::is_interactive_terminal() && sim::ansi_for_terminal(true) != sim::Ansi::none) {
        belter::Tui tui(bus, session, {.color = !sim::no_color_requested()});
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
    const sim::Ansi ansi = terminal ? sim::ansi_for_terminal(true) : sim::Ansi::none;
    return sim::run_repl(bus, session, reader, std::cout, {.ansi = ansi});
}
