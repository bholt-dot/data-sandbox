#pragma once

// Full-screen terminal UI for the belter shell (FTXUI):
//
//   status banner    date, cash, next loan instalment, ship, reaction mass, hull, crew
//   ─────────────────────────────────────────────┬───────────────────────
//   command output (scrollback: PgUp/PgDn/wheel)  │ journal: urgent entries
//                                                 │ pinned, then the latest
//   ─────────────────────────────────────────────┴───────────────────────
//   > input line (Tab completes, Up/Down history)
//   hint line (completion candidates, scroll position, keys)
//
// Terminals narrower than wide_min_width drop the journal panel; pinned urgent entries then show
// above the output instead.
//
// Threading: run() blocks the thread that calls it, which need not be the main thread (a future
// SDL viewer will own the main thread). The bus and session are read and mutated only from inside
// run() (event handlers and rendering), so the caller must not touch them until run() returns.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "expanse/shell.hpp"
#include "simcore/doc.hpp"
#include "simcore/time.hpp"

namespace belter {

struct TuiOptions {
    bool color = true;         // false under NO_COLOR: bold/dim/inverse only
    int wide_min_width = 100;  // below this the journal panel collapses
    sim::Duration pin_for = sim::days(7); // how long an unacknowledged urgent entry stays pinned
    std::size_t max_entries = 2000;       // scrollback, in commands
};

class Tui {
public:
    Tui(expanse::ShellBus& bus, expanse::Session& session, TuiOptions options = {});

    // Runs the full-screen loop until `quit`, Ctrl+C or Ctrl+D on an empty line, then restores
    // the terminal. Returns the process exit code.
    int run();

    // Runs a line as if typed and entered (also records it in the input history).
    void execute(std::string_view line);
    // Key and mouse handling on top of the input line; true if consumed.
    bool on_event(ftxui::Event event);
    // One frame at the given terminal size.
    ftxui::Element view(int width, int height);

    // The component tree (input line + event handling) that run() drives.
    ftxui::Component component() const { return root_; }
    const std::string& input() const { return input_; }
    bool quit_requested() const { return quit_; }

private:
    void push(sim::Doc doc);
    void relayout(std::size_t width);
    void complete();
    void history_step(int delta);
    void exit_loop();

    std::vector<std::size_t> pinned() const; // journal indices, newest first
    std::vector<sim::Line> banner(int width) const; // rows
    ftxui::Element journal(int width, int height) const;
    ftxui::Element output(int width, int height);
    ftxui::Element hint(int width) const;

    expanse::ShellBus& bus_;
    expanse::Session& session_;
    TuiOptions options_;

    std::vector<sim::Doc> entries_; // one per command
    std::vector<sim::Line> rows_; // entries_ laid out at rows_width_
    std::size_t rows_width_ = 0;
    std::size_t scroll_ = 0; // rows scrolled back from the bottom
    int page_ = 10;          // output rows on screen, for PgUp/PgDn

    std::string input_;
    int cursor_ = 0; // byte offset into input_
    std::vector<std::string> history_;
    std::size_t history_pos_ = 0; // == history_.size() when not browsing
    std::string draft_;           // the line being typed before browsing history
    std::vector<std::string> candidates_; // shown after an ambiguous Tab

    std::size_t acknowledged_ = 0; // journal entries before this index are no longer pinned
    bool quit_ = false;
    std::function<void()> exit_;

    ftxui::Component input_component_;
    ftxui::Component root_;
};

} // namespace belter
