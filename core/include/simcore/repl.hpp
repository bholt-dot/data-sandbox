#pragma once

// Read-eval-print loop over a CommandBus, independent of where lines come from: an interactive
// line editor (provided by the app, so core stays dependency-free), a pipe, or a script file.

#include <cstddef>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "simcore/command_bus.hpp"

namespace sim {

class LineReader {
public:
    virtual ~LineReader() = default;
    // The next line without its terminator, or nullopt at end of input.
    virtual std::optional<std::string> read_line(std::string_view prompt) = 0;
    // Where the last line came from, e.g. "script.txt:12"; empty if not meaningful.
    virtual std::string location() const { return {}; }
};

// Plain std::getline reader for pipes and script files: no prompt, no escape codes.
class StreamLineReader final : public LineReader {
public:
    // `source_name` enables location(); `echo` (if set) receives "<prompt><line>" for each line,
    // so a transcript of piped input reads like an interactive session.
    explicit StreamLineReader(std::istream& in, std::string source_name = {},
                              std::ostream* echo = nullptr)
        : in_(in), source_name_(std::move(source_name)), echo_(echo) {}

    std::optional<std::string> read_line(std::string_view prompt) override {
        std::string line;
        if (!std::getline(in_, line)) {
            return std::nullopt;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        ++line_no_;
        if (echo_ != nullptr) {
            *echo_ << prompt << line << '\n';
        }
        return line;
    }

    std::string location() const override {
        return source_name_.empty() ? std::string() : source_name_ + ":" + std::to_string(line_no_);
    }

private:
    std::istream& in_;
    std::string source_name_;
    std::ostream* echo_;
    std::size_t line_no_ = 0;
};

struct ReplOptions {
    std::string prompt = "> ";
    bool stop_on_error = false; // batch mode: the first error ends the run
};

// Runs until end of input, `quit`, or (with stop_on_error) the first error. Returns 0 on success,
// 1 if stopped by an error. Errors are written to `out` as "error: ..." (prefixed by the reader's
// location when it has one).
template <class Context>
int run_repl(CommandBus<Context>& bus, Context& ctx, LineReader& in, std::ostream& out,
             const ReplOptions& options = {}) {
    while (auto line = in.read_line(options.prompt)) {
        const LineResult r = bus.execute_line(*line, ctx, out);
        if (r.status == LineResult::Status::quit) {
            break;
        }
        if (r.status == LineResult::Status::error) {
            const std::string where = in.location();
            out << (where.empty() ? "" : where + ": ") << "error: " << r.error << '\n';
            if (options.stop_on_error) {
                out.flush();
                return 1;
            }
        }
        out.flush();
    }
    return 0;
}

} // namespace sim
