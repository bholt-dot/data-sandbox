#pragma once

// Interactive line editing (history, tab completion) for sim shells. Kept out of core so the
// framework has no third-party runtime dependency; built on isocline when SIM_LINE_EDITING is on.

#include <functional>
#include <memory>
#include <string_view>

#include "simcore/command.hpp"
#include "simcore/repl.hpp"

namespace sim {

using CompleteFn = std::function<Completion(std::string_view line_before_cursor)>;

// True when both stdin and stdout are terminals.
bool is_interactive_terminal();

// A line editor reading from the terminal, or nullptr when stdin/stdout are not a terminal or
// line editing was compiled out; callers then fall back to StreamLineReader.
std::unique_ptr<LineReader> make_terminal_reader(CompleteFn complete);

} // namespace sim
