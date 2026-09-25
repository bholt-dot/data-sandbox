#pragma once

// Text command layer: tokenizer, typed arguments, command specs and a registry that turns a line
// of text into a validated, typed Invocation (and back into canonical text).
//
// Nothing here executes commands. Invocations are plain data so they can be queued, logged,
// snapshotted and written out as a replayable script; see command_bus.hpp for dispatch.
//
// Line syntax:
//   advance 30d                     positional arguments, whitespace separated
//   dock "Ceres Station"            double quotes group words; \" and \\ escape inside them
//   say 'single quotes are literal'
//   buy water --qty 40 --max-price=12.5 --dry-run
//                                   options: --name value, --name=value, bare --flag
//   rename -- --odd-name            a lone "--" ends option parsing
//   advance 1d  # comment           '#' at the start of a word starts a comment

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "simcore/time.hpp"

namespace sim {

// A user-facing error: bad syntax, bad argument, or a handler refusing a command.
class CommandError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// --- tokenizer ---------------------------------------------------------------------------------

struct Token {
    std::string text;       // unquoted, unescaped
    std::size_t column = 0; // byte offset of the token's first character in the line
    // True when the token begins with an unquoted "--": it is option syntax. `"--x"` is a value.
    bool option_syntax = false;
};

// Splits a line into tokens, dropping any comment. Throws CommandError on an unterminated quote.
std::vector<Token> tokenize(std::string_view line);

// Returns `text` as a single token that tokenize() reads back verbatim (quoted only if needed).
// Throws std::invalid_argument for text containing a line break (a script line cannot hold one).
std::string quote_token(std::string_view text);

// --- typed values ------------------------------------------------------------------------------

// Standard gravity (CGPM 1901, exact), for accelerations written in g.
inline constexpr double standard_gravity_mps2 = 9.80665;

struct Acceleration {
    double mps2 = 0.0;

    constexpr auto operator<=>(const Acceleration&) const = default;
    constexpr double gees() const { return mps2 / standard_gravity_mps2; }

    static constexpr auto fields(auto& self) { return std::tie(self.mps2); }
};

enum class ArgType : std::uint8_t {
    integer,      // std::int64_t: 42, -7
    number,       // double: 0.25, 1e6 (finite)
    string,       // std::string
    duration,     // sim::Duration: 30d, 6h, 90m, 45s, 2w, 1d12h, 1.5h (exact whole seconds)
    acceleration, // sim::Acceleration: 0.3g, 2.5m/s2 (non-negative)
    flag,         // bool: options only; present = true, or --name=true/false
};

// Alternative order matches ArgType; it is part of the snapshot encoding, so only append.
using ArgValue = std::variant<std::int64_t, double, std::string, Duration, Acceleration, bool>;

std::string_view type_name(ArgType type);

// Parses `text` as `type`. Throws CommandError with a message like
// "expected duration like 30d, 6h or 90m, got 'abc'".
ArgValue parse_value(ArgType type, std::string_view text);

// Canonical text of a value, already quoted as a token: parse_value(type, unquoted) == value.
std::string format_value(const ArgValue& value);

// --- specs -------------------------------------------------------------------------------------

using Completer = std::function<std::vector<std::string>()>;

struct ArgSpec {
    std::string name{};
    ArgType type = ArgType::string;
    std::string help{};
    // Positionals only: a required positional may not follow an optional one. Options are
    // always optional.
    bool required = true;
    // Parsed with `type` at registration; filled into invocations when the argument is omitted,
    // so logged commands are explicit and don't depend on defaults of a later build.
    std::optional<std::string> default_value{};
    // String arguments only: the accepted values (also used for tab completion).
    std::vector<std::string> choices{};
    // Tab-completion candidates computed on demand (e.g. station names). Not used for validation.
    Completer completer{};
    // Last positional only, string type: takes every remaining non-option word (e.g. a query
    // like `where rmass < 0.2`). The value is the words' canonical text (each quote_token()ed,
    // joined by single spaces); handlers split it again with tokenize().
    bool rest = false;
};

enum class CommandKind : std::uint8_t {
    query,  // read-only: runs immediately
    action, // mutates simulation state: goes through the bus queue and the command log
};

struct CommandSpec {
    std::string name{};
    std::vector<std::string> aliases{};
    std::string summary{}; // one line, shown by `help`
    std::string details{}; // optional extra paragraph(s), shown by `help <command>`
    std::vector<ArgSpec> positionals{};
    std::vector<ArgSpec> options{};
    CommandKind kind = CommandKind::query;
};

// --- invocations -------------------------------------------------------------------------------

struct NamedArg {
    std::string name;
    ArgValue value;

    bool operator==(const NamedArg&) const = default;
    static auto fields(auto& self) { return std::tie(self.name, self.value); }
};

// One parsed command: the canonical command name (never an alias) and its arguments in spec
// order (positionals, then options). Omitted arguments with a default carry the default; flags
// are always present; other omitted arguments are absent.
struct Invocation {
    std::string command;
    std::vector<NamedArg> args;

    bool operator==(const Invocation&) const = default;
    static auto fields(auto& self) { return std::tie(self.command, self.args); }

    const ArgValue* find(std::string_view name) const {
        for (const NamedArg& a : args) {
            if (a.name == name) {
                return &a.value;
            }
        }
        return nullptr;
    }
    bool has(std::string_view name) const { return find(name) != nullptr; }

    // Throws std::logic_error if the argument is absent or of another type: that is a mismatch
    // between a handler and its own spec, not a user error.
    template <class T>
    const T& get(std::string_view name) const {
        const ArgValue* v = find(name);
        if (v == nullptr) {
            throw std::logic_error("Invocation::get: '" + std::string(name) + "' is absent");
        }
        const T* p = std::get_if<T>(v);
        if (p == nullptr) {
            throw std::logic_error("Invocation::get: '" + std::string(name) + "' has another type");
        }
        return *p;
    }

    template <class T>
    T get_or(std::string_view name, T fallback) const {
        return has(name) ? get<T>(name) : std::move(fallback);
    }

    bool flag(std::string_view name) const { return get_or<bool>(name, false); }
};

// Candidates for completing the word that ends at the end of `line_before_cursor`.
struct Completion {
    std::size_t replace_from = 0;        // byte offset where the word being completed starts
    std::vector<std::string> candidates; // full replacement words, quoted where needed
};

// --- registry ----------------------------------------------------------------------------------

class CommandRegistry {
public:
    // Validates the spec (unique names/aliases, well-formed arguments, parseable defaults).
    // Throws std::invalid_argument for a malformed spec: that is a programming error.
    const CommandSpec& add(CommandSpec spec);

    // By name or alias.
    const CommandSpec* find(std::string_view name) const;
    const CommandSpec& at(std::string_view name) const; // throws std::out_of_range

    // All specs, sorted by name.
    std::vector<const CommandSpec*> commands() const;

    // Parses a line. Returns nullopt for a blank or comment-only line; throws CommandError for
    // anything unparseable, including unknown commands (with a "did you mean" suggestion).
    std::optional<Invocation> parse(std::string_view line) const;
    Invocation bind(std::span<const Token> tokens) const;

    // Canonical script line for an invocation; parse(format(inv)) == inv.
    std::string format(const Invocation& inv) const;

    // Known command names close to `word` (prefix matches first, then small edit distance).
    std::vector<std::string> suggest(std::string_view word) const;

    std::string usage(const CommandSpec& spec) const;
    void write_help(std::ostream& out) const;                          // command overview
    void write_help(std::ostream& out, const CommandSpec& spec) const; // one command in detail

    Completion complete(std::string_view line_before_cursor) const;

private:
    struct Entry {
        CommandSpec spec;
        std::vector<std::optional<ArgValue>> positional_defaults;
        std::vector<std::optional<ArgValue>> option_defaults;
    };

    const Entry* find_entry(std::string_view name) const;

    std::vector<Entry> entries_; // registration order; small, so linear lookup is fine
};

// Parses a whole script (one command per line); errors carry "line N: ". Blank and comment lines
// are skipped.
std::vector<Invocation> parse_script(const CommandRegistry& registry, std::string_view text);

// Damerau-Levenshtein (optimal string alignment) distance, ASCII case-insensitive.
std::size_t edit_distance(std::string_view a, std::string_view b);

} // namespace sim
