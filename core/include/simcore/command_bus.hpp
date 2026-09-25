#pragma once

// Command dispatch for a text shell: queries run immediately, actions go through a queue and a
// command log.
//
// Reproducibility contract: every change to simulation state enters through an *action*. Actions
// are queued as plain-data Invocations and applied in submission order by apply_pending(), which
// the game calls at the start of a tick (a shell-played game advances time only through actions
// such as `advance 3d`, so the position in the log fully determines when a command applied).
// Applied actions are appended to the log, which can be written out as a script. Feeding that
// script (with the same seed and build) through execute_line() reproduces the session exactly,
// which is what batch/script mode and "save my session as a replay" rely on.
//
// Queries get a const Context and are never logged. Handlers write styled output to the Doc they
// are given (never std::cout); the caller renders it as plain text, ANSI or a full-screen UI.
//
// Actions that fail (CommandError or any other exception from the handler) are still logged, with
// the error: a handler may have changed state before failing, and replaying the failure keeps the
// replay faithful. Parse errors never reach the log.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <format>
#include <functional>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "simcore/command.hpp"
#include "simcore/doc.hpp"

namespace sim {

struct LoggedCommand {
    Invocation invocation;
    std::string error; // empty if the handler succeeded

    bool operator==(const LoggedCommand&) const = default;
    static auto fields(auto& self) { return std::tie(self.invocation, self.error); }
};

struct LineResult {
    enum class Status : std::uint8_t { empty, ok, error, quit };
    Status status = Status::empty;
    std::string error;

    bool ok() const { return status != Status::error; }
};

template <class Context>
class CommandBus {
public:
    using QueryHandler = std::function<void(const Context&, const Invocation&, Doc&)>;
    using ActionHandler = std::function<void(Context&, const Invocation&, Doc&)>;

    struct ApplyResult {
        std::size_t applied = 0;
        std::vector<std::string> errors;
    };

    CommandBus() { add_builtins(); }
    // Built-in handlers refer back to the bus.
    CommandBus(const CommandBus&) = delete;
    CommandBus& operator=(const CommandBus&) = delete;

    void add_query(CommandSpec spec, QueryHandler handler) {
        spec.kind = CommandKind::query;
        const CommandSpec& s = registry_.add(std::move(spec));
        queries_.emplace_back(s.name, std::move(handler));
    }

    void add_action(CommandSpec spec, ActionHandler handler) {
        spec.kind = CommandKind::action;
        const CommandSpec& s = registry_.add(std::move(spec));
        actions_.emplace_back(s.name, std::move(handler));
    }

    const CommandRegistry& registry() const { return registry_; }

    // Parses one line; runs it if it is a query, queues it if it is an action.
    LineResult submit_line(std::string_view line, const Context& ctx, Doc& out) {
        std::optional<Invocation> inv;
        try {
            inv = registry_.parse(line);
        } catch (const CommandError& e) {
            return {LineResult::Status::error, e.what()};
        }
        if (!inv) {
            return {};
        }
        if (registry_.at(inv->command).kind == CommandKind::action) {
            submit(std::move(*inv));
            return {LineResult::Status::ok, {}};
        }
        try {
            find_handler(queries_, inv->command)(ctx, *inv, out);
        } catch (const CommandError& e) {
            return {LineResult::Status::error, e.what()};
        } catch (const std::exception& e) {
            return {LineResult::Status::error, std::format("internal error: {}", e.what())};
        }
        if (quit_requested_) {
            return {LineResult::Status::quit, {}};
        }
        return {LineResult::Status::ok, {}};
    }

    // Queues an already-parsed action (e.g. from a replay, a test or an AI).
    void submit(Invocation inv) {
        const CommandSpec* spec = registry_.find(inv.command);
        if (spec == nullptr || spec->name != inv.command || spec->kind != CommandKind::action) {
            throw std::invalid_argument(
                std::format("CommandBus::submit: '{}' is not a registered action", inv.command));
        }
        pending_.push_back(std::move(inv));
    }

    std::size_t pending() const { return pending_.size(); }

    // Applies queued actions in submission order and logs them. Call at the start of a tick.
    ApplyResult apply_pending(Context& ctx, Doc& out) {
        ApplyResult result;
        while (!pending_.empty()) {
            Invocation inv = std::move(pending_.front());
            pending_.pop_front();
            std::string error;
            try {
                find_handler(actions_, inv.command)(ctx, inv, out);
            } catch (const CommandError& e) {
                error = e.what();
            } catch (const std::exception& e) {
                error = std::format("internal error: {}", e.what());
            }
            ++result.applied;
            if (!error.empty()) {
                result.errors.push_back(error);
            }
            log_.push_back({std::move(inv), std::move(error)});
        }
        return result;
    }

    // submit_line + apply_pending: the unit of work for a REPL line or a script line.
    LineResult execute_line(std::string_view line, Context& ctx, Doc& out) {
        LineResult r = submit_line(line, std::as_const(ctx), out);
        if (r.status == LineResult::Status::error) {
            return r;
        }
        ApplyResult applied = apply_pending(ctx, out);
        if (!applied.errors.empty()) {
            return {LineResult::Status::error, std::move(applied.errors.front())};
        }
        return r;
    }

    const std::vector<LoggedCommand>& log() const { return log_; }
    void clear_log() { log_.clear(); }

    // The log as a replayable script: one command per line; failed commands carry a comment.
    void write_script(std::ostream& out) const {
        for (const LoggedCommand& c : log_) {
            out << registry_.format(c.invocation);
            if (!c.error.empty()) {
                std::string note = c.error;
                std::replace(note.begin(), note.end(), '\n', ' ');
                out << "  # failed: " << note;
            }
            out << '\n';
        }
    }

    bool quit_requested() const { return quit_requested_; }

private:
    template <class H>
    static const H& find_handler(const std::vector<std::pair<std::string, H>>& handlers,
                                 const std::string& name) {
        for (const auto& [n, h] : handlers) {
            if (n == name) {
                return h;
            }
        }
        throw std::logic_error("CommandBus: no handler for '" + name + "'");
    }

    void add_builtins() {
        add_query({.name = "help",
                   .summary = "List commands, or show how to use one",
                   .positionals = {{.name = "command",
                                    .help = "command to describe",
                                    .required = false,
                                    .completer = [this] { return command_names(); }}}},
                  [this](const Context&, const Invocation& inv, Doc& out) {
                      if (!inv.has("command")) {
                          registry_.write_help(out);
                          return;
                      }
                      const std::string& name = inv.get<std::string>("command");
                      const CommandSpec* spec = registry_.find(name);
                      if (spec == nullptr) {
                          const auto s = registry_.suggest(name);
                          throw CommandError(std::format(
                              "unknown command '{}'{}", name,
                              s.empty() ? std::string() : std::format("; did you mean '{}'?", s.front())));
                      }
                      registry_.write_help(out, *spec);
                  });
        add_query({.name = "quit", .aliases = {"exit"}, .summary = "Leave the shell"},
                  [this](const Context&, const Invocation&, Doc&) { quit_requested_ = true; });
        add_query({.name = "history",
                   .summary = "Show this session's state-changing commands as a replayable script"},
                  [this](const Context&, const Invocation&, Doc& out) {
                      std::ostringstream script;
                      write_script(script);
                      out << script.str();
                  });
    }

    std::vector<std::string> command_names() const {
        std::vector<std::string> names;
        for (const CommandSpec* s : registry_.commands()) {
            names.push_back(s->name);
        }
        return names;
    }

    CommandRegistry registry_;
    std::vector<std::pair<std::string, QueryHandler>> queries_;
    std::vector<std::pair<std::string, ActionHandler>> actions_;
    std::deque<Invocation> pending_;
    std::vector<LoggedCommand> log_;
    bool quit_requested_ = false;
};

} // namespace sim
