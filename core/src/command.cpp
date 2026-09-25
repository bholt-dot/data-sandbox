#include "simcore/command.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <sstream>

namespace sim {

namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool istarts_with(std::string_view s, std::string_view prefix) {
    if (prefix.size() > s.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (lower(s[i]) != lower(prefix[i])) {
            return false;
        }
    }
    return true;
}

// --- lexer ----------------------------------------------------------------------------------

struct Lexed {
    std::vector<Token> tokens;
    bool open_quote = false; // lenient mode only: the last token's quote was never closed
    bool comment = false;    // a comment started; nothing after it was tokenized
};

// Lenient mode (for completion) accepts an unterminated quote as running to the end of line.
Lexed lex(std::string_view line, bool lenient) {
    Lexed out;
    std::size_t i = 0;
    const std::size_t n = line.size();
    while (true) {
        while (i < n && is_space(line[i])) {
            ++i;
        }
        if (i >= n) {
            break;
        }
        if (line[i] == '#') {
            out.comment = true;
            break;
        }
        Token tok;
        tok.column = i;
        std::size_t unquoted_prefix = 0;
        bool quoted = false;
        while (i < n && !is_space(line[i])) {
            const char c = line[i];
            if (c == '"' || c == '\'') {
                const std::size_t open = i;
                quoted = true;
                ++i;
                bool closed = false;
                while (i < n) {
                    if (line[i] == c) {
                        closed = true;
                        ++i;
                        break;
                    }
                    if (c == '"' && line[i] == '\\' && i + 1 < n) {
                        tok.text += line[i + 1];
                        i += 2;
                        continue;
                    }
                    tok.text += line[i];
                    ++i;
                }
                if (!closed) {
                    if (!lenient) {
                        throw CommandError(
                            std::format("unterminated quote starting at column {}", open + 1));
                    }
                    out.open_quote = true;
                }
                continue;
            }
            tok.text += c;
            if (!quoted) {
                ++unquoted_prefix;
            }
            ++i;
        }
        tok.option_syntax = unquoted_prefix >= 2 && tok.text.starts_with("--");
        out.tokens.push_back(std::move(tok));
    }
    return out;
}

// --- checked integer arithmetic ---------------------------------------------------------------

bool mul_overflows(std::int64_t a, std::int64_t b, std::int64_t& out) {
    // Only used with non-negative operands.
    if (a != 0 && b > std::numeric_limits<std::int64_t>::max() / a) {
        return true;
    }
    out = a * b;
    return false;
}

bool add_overflows(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if (b > std::numeric_limits<std::int64_t>::max() - a) {
        return true;
    }
    out = a + b;
    return false;
}

// --- values -----------------------------------------------------------------------------------

[[noreturn]] void expected(std::string_view what, std::string_view got, std::string_view note = {}) {
    if (note.empty()) {
        throw CommandError(std::format("expected {}, got '{}'", what, got));
    }
    throw CommandError(std::format("expected {}, got '{}' ({})", what, got, note));
}

constexpr std::string_view duration_example = "duration like 30d, 6h or 90m";
constexpr std::string_view accel_example = "acceleration like 0.3g or 2.5m/s2";

std::int64_t parse_integer(std::string_view text) {
    std::int64_t v = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec == std::errc::result_out_of_range) {
        throw CommandError(std::format("integer out of range: '{}'", text));
    }
    if (ec != std::errc{} || ptr != last) {
        expected("integer", text);
    }
    return v;
}

double parse_number(std::string_view text) {
    double v = 0.0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc{} || ptr != last || !std::isfinite(v)) {
        expected("number like 0.25 or 12", text);
    }
    return v;
}

std::int64_t unit_seconds(char unit) {
    switch (unit) {
    case 'w': return 7 * 86400;
    case 'd': return 86400;
    case 'h': return 3600;
    case 'm': return 60;
    case 's': return 1;
    default: return 0;
    }
}

// Exact integer arithmetic so "1.5h" is exactly 5400 s and parsing never depends on rounding.
Duration parse_duration(std::string_view text) {
    std::string_view s = text;
    bool negative = false;
    if (!s.empty() && s.front() == '-') {
        negative = true;
        s.remove_prefix(1);
    }
    if (s.empty()) {
        expected(duration_example, text);
    }
    std::int64_t total = 0;
    while (!s.empty()) {
        std::size_t i = 0;
        std::int64_t whole = 0;
        while (i < s.size() && is_digit(s[i])) {
            if (mul_overflows(whole, 10, whole) || add_overflows(whole, s[i] - '0', whole)) {
                throw CommandError(std::format("duration out of range: '{}'", text));
            }
            ++i;
        }
        if (i == 0) {
            expected(duration_example, text);
        }
        std::int64_t frac = 0;
        std::int64_t frac_scale = 1;
        if (i < s.size() && s[i] == '.') {
            ++i;
            const std::size_t frac_start = i;
            while (i < s.size() && is_digit(s[i])) {
                if (i - frac_start >= 9) {
                    expected(duration_example, text, "too many decimal places");
                }
                frac = frac * 10 + (s[i] - '0');
                frac_scale *= 10;
                ++i;
            }
            if (i == frac_start) {
                expected(duration_example, text);
            }
        }
        if (i >= s.size()) {
            expected(duration_example, text, "missing unit: w, d, h, m or s");
        }
        const std::int64_t unit = unit_seconds(s[i]);
        if (unit == 0) {
            expected(duration_example, text, std::format("unknown unit '{}'", s[i]));
        }
        ++i;
        if ((frac * unit) % frac_scale != 0) {
            expected(duration_example, text, "not a whole number of seconds");
        }
        std::int64_t part = 0;
        if (mul_overflows(whole, unit, part) || add_overflows(part, frac * unit / frac_scale, part) ||
            add_overflows(total, part, total)) {
            throw CommandError(std::format("duration out of range: '{}'", text));
        }
        s.remove_prefix(i);
    }
    return Duration{negative ? -total : total};
}

std::string format_duration_token(Duration d) {
    if (d.seconds == 0) {
        return "0s";
    }
    // Magnitude as unsigned so INT64_MIN has one.
    const bool negative = d.seconds < 0;
    std::uint64_t s = negative ? 0 - static_cast<std::uint64_t>(d.seconds)
                               : static_cast<std::uint64_t>(d.seconds);
    std::string out = negative ? "-" : "";
    constexpr std::pair<std::uint64_t, char> units[] = {{86400, 'd'}, {3600, 'h'}, {60, 'm'}, {1, 's'}};
    for (const auto& [size, suffix] : units) {
        if (s >= size) {
            out += std::format("{}{}", s / size, suffix);
            s %= size;
        }
    }
    return out;
}

Acceleration parse_acceleration(std::string_view text) {
    double v = 0.0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc{} || !std::isfinite(v)) {
        expected(accel_example, text);
    }
    const std::string_view unit(ptr, static_cast<std::size_t>(last - ptr));
    double mps2 = 0.0;
    if (unit == "g") {
        mps2 = v * standard_gravity_mps2;
    } else if (unit == "m/s2" || unit == "m/s^2" || unit == "mps2") {
        mps2 = v;
    } else if (unit.empty()) {
        expected(accel_example, text, "missing unit: g or m/s2");
    } else {
        expected(accel_example, text, std::format("unknown unit '{}'", unit));
    }
    if (mps2 < 0.0) {
        throw CommandError(std::format("acceleration must not be negative, got '{}'", text));
    }
    return Acceleration{mps2};
}

std::string format_acceleration_token(Acceleration a) {
    // Prefer the human form, but only if it parses back to the identical double.
    std::string g = std::format("{}g", a.gees());
    if (parse_acceleration(g) == a) {
        return g;
    }
    return std::format("{}m/s2", a.mps2);
}

bool parse_flag(std::string_view text) {
    if (text == "true" || text == "yes" || text == "on" || text == "1") {
        return true;
    }
    if (text == "false" || text == "no" || text == "off" || text == "0") {
        return false;
    }
    expected("true or false", text);
}

// --- names ------------------------------------------------------------------------------------

bool valid_name(std::string_view name) {
    if (name.empty() || name.front() == '-') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '-' ||
               c == '_';
    });
}

std::vector<std::string> suggest_among(std::string_view word, const std::vector<std::string>& pool) {
    std::vector<std::string> prefixed;
    for (const std::string& c : pool) {
        if (!word.empty() && istarts_with(c, word)) {
            prefixed.push_back(c);
        }
    }
    if (!prefixed.empty()) {
        std::sort(prefixed.begin(), prefixed.end());
        return prefixed;
    }
    const std::size_t limit = word.size() <= 4 ? 1 : word.size() <= 8 ? 2 : 3;
    std::vector<std::pair<std::size_t, std::string>> close;
    for (const std::string& c : pool) {
        const std::size_t d = edit_distance(word, c);
        if (d <= limit) {
            close.emplace_back(d, c);
        }
    }
    std::sort(close.begin(), close.end());
    std::vector<std::string> out;
    for (auto& [d, c] : close) {
        if (out.size() == 3) {
            break;
        }
        out.push_back(std::move(c));
    }
    return out;
}

std::string did_you_mean(const std::vector<std::string>& suggestions, std::string_view prefix = "") {
    if (suggestions.empty()) {
        return {};
    }
    if (suggestions.size() == 1) {
        return std::format("; did you mean '{}{}'?", prefix, suggestions.front());
    }
    std::string list;
    for (const std::string& s : suggestions) {
        list += list.empty() ? "" : ", ";
        list += std::format("'{}{}'", prefix, s);
    }
    return std::format("; did you mean one of {}?", list);
}

std::string arg_label(const ArgSpec& a) { return std::format("<{}>", a.name); }

std::string option_label(const ArgSpec& a) {
    if (a.type == ArgType::flag) {
        return "--" + a.name;
    }
    return std::format("--{} <{}>", a.name, type_name(a.type));
}

ArgValue parse_arg(const ArgSpec& spec, std::string_view text, std::string_view label) {
    try {
        ArgValue v = parse_value(spec.type, text);
        if (!spec.choices.empty() &&
            std::find(spec.choices.begin(), spec.choices.end(), text) == spec.choices.end()) {
            std::string list;
            for (const std::string& c : spec.choices) {
                list += list.empty() ? "" : ", ";
                list += c;
            }
            throw CommandError(std::format("expected one of {}; got '{}'", list, text));
        }
        return v;
    } catch (const CommandError& e) {
        throw CommandError(std::format("{}: {}", label, e.what()));
    }
}

void validate_arg(const CommandSpec& cmd, const ArgSpec& a, bool is_option,
                  std::optional<ArgValue>& default_out) {
    auto fail = [&](std::string_view what) {
        throw std::invalid_argument(
            std::format("command '{}': argument '{}': {}", cmd.name, a.name, what));
    };
    if (!valid_name(a.name)) {
        fail("invalid name");
    }
    if (a.type == ArgType::flag) {
        if (!is_option) {
            fail("flags must be options");
        }
        if (a.default_value) {
            fail("flags cannot have a default (they default to false)");
        }
    }
    if (!a.choices.empty() && a.type != ArgType::string) {
        fail("choices are only supported for string arguments");
    }
    if (a.rest && (is_option || a.type != ArgType::string || !a.choices.empty() ||
                   &a != &cmd.positionals.back())) {
        fail("a rest argument must be the last positional, of string type, without choices");
    }
    if (a.default_value) {
        try {
            default_out = parse_arg(a, *a.default_value, a.name);
        } catch (const CommandError& e) {
            fail(std::format("bad default: {}", e.what()));
        }
    }
}

} // namespace

// --- public: tokens & values --------------------------------------------------------------------

std::vector<Token> tokenize(std::string_view line) { return lex(line, false).tokens; }

std::string quote_token(std::string_view text) {
    bool needs = text.empty() || text.front() == '#' || text.starts_with("--");
    for (char c : text) {
        if (c == '\n' || c == '\r') {
            throw std::invalid_argument("quote_token: text contains a line break");
        }
        if (is_space(c) || c == '"' || c == '\'' || c == '\\') {
            needs = true;
        }
    }
    if (!needs) {
        return std::string(text);
    }
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
    return out;
}

std::string_view type_name(ArgType type) {
    switch (type) {
    case ArgType::integer: return "integer";
    case ArgType::number: return "number";
    case ArgType::string: return "string";
    case ArgType::duration: return "duration";
    case ArgType::acceleration: return "acceleration";
    case ArgType::flag: return "flag";
    }
    return "?";
}

ArgValue parse_value(ArgType type, std::string_view text) {
    switch (type) {
    case ArgType::integer: return parse_integer(text);
    case ArgType::number: return parse_number(text);
    case ArgType::string: return std::string(text);
    case ArgType::duration: return parse_duration(text);
    case ArgType::acceleration: return parse_acceleration(text);
    case ArgType::flag: return parse_flag(text);
    }
    throw std::invalid_argument("parse_value: unknown ArgType");
}

std::string format_value(const ArgValue& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>) {
                return std::format("{}", v); // shortest round-trip form for doubles
            } else if constexpr (std::is_same_v<T, std::string>) {
                return quote_token(v);
            } else if constexpr (std::is_same_v<T, Duration>) {
                return format_duration_token(v);
            } else if constexpr (std::is_same_v<T, Acceleration>) {
                return format_acceleration_token(v);
            } else {
                return v ? "true" : "false";
            }
        },
        value);
}


// --- registry -----------------------------------------------------------------------------------

const CommandSpec& CommandRegistry::add(CommandSpec spec) {
    if (!valid_name(spec.name)) {
        throw std::invalid_argument(std::format("invalid command name '{}'", spec.name));
    }
    std::vector<std::string_view> names{spec.name};
    for (const std::string& alias : spec.aliases) {
        if (!valid_name(alias)) {
            throw std::invalid_argument(
                std::format("command '{}': invalid alias '{}'", spec.name, alias));
        }
        names.push_back(alias);
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (find(names[i]) != nullptr ||
            std::find(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(i), names[i]) !=
                names.begin() + static_cast<std::ptrdiff_t>(i)) {
            throw std::invalid_argument(std::format("command name '{}' is already taken", names[i]));
        }
    }

    Entry entry;
    std::vector<std::string_view> arg_names;
    bool seen_optional = false;
    for (ArgSpec& a : spec.positionals) {
        if (a.default_value) {
            a.required = false;
        }
        std::optional<ArgValue> def;
        validate_arg(spec, a, false, def);
        if (a.required && seen_optional) {
            throw std::invalid_argument(std::format(
                "command '{}': required argument '{}' follows an optional one", spec.name, a.name));
        }
        seen_optional = seen_optional || !a.required;
        arg_names.push_back(a.name);
        entry.positional_defaults.push_back(std::move(def));
    }
    for (ArgSpec& a : spec.options) {
        a.required = false;
        std::optional<ArgValue> def;
        validate_arg(spec, a, true, def);
        arg_names.push_back(a.name);
        entry.option_defaults.push_back(std::move(def));
    }
    std::sort(arg_names.begin(), arg_names.end());
    if (std::adjacent_find(arg_names.begin(), arg_names.end()) != arg_names.end()) {
        throw std::invalid_argument(std::format("command '{}': duplicate argument name", spec.name));
    }
    entry.spec = std::move(spec);
    entries_.push_back(std::move(entry));
    return entries_.back().spec;
}

const CommandRegistry::Entry* CommandRegistry::find_entry(std::string_view name) const {
    for (const Entry& e : entries_) {
        if (e.spec.name == name ||
            std::find(e.spec.aliases.begin(), e.spec.aliases.end(), name) != e.spec.aliases.end()) {
            return &e;
        }
    }
    return nullptr;
}

const CommandSpec* CommandRegistry::find(std::string_view name) const {
    const Entry* e = find_entry(name);
    return e != nullptr ? &e->spec : nullptr;
}

const CommandSpec& CommandRegistry::at(std::string_view name) const {
    const CommandSpec* s = find(name);
    if (s == nullptr) {
        throw std::out_of_range(std::format("no command '{}'", name));
    }
    return *s;
}

std::vector<const CommandSpec*> CommandRegistry::commands() const {
    std::vector<const CommandSpec*> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) {
        out.push_back(&e.spec);
    }
    std::sort(out.begin(), out.end(),
              [](const CommandSpec* a, const CommandSpec* b) { return a->name < b->name; });
    return out;
}

std::vector<std::string> CommandRegistry::suggest(std::string_view word) const {
    std::vector<std::string> pool;
    for (const Entry& e : entries_) {
        pool.push_back(e.spec.name);
        pool.insert(pool.end(), e.spec.aliases.begin(), e.spec.aliases.end());
    }
    return suggest_among(word, pool);
}

std::optional<Invocation> CommandRegistry::parse(std::string_view line) const {
    const std::vector<Token> tokens = tokenize(line);
    if (tokens.empty()) {
        return std::nullopt;
    }
    return bind(tokens);
}

Invocation CommandRegistry::bind(std::span<const Token> tokens) const {
    if (tokens.empty()) {
        throw CommandError("empty command");
    }
    const Token& head = tokens.front();
    const Entry* entry = head.option_syntax ? nullptr : find_entry(head.text);
    if (entry == nullptr) {
        const std::vector<std::string> s = suggest(head.text);
        throw CommandError(std::format("unknown command '{}'{}", head.text,
                                       s.empty() ? "; type 'help' for a list" : did_you_mean(s)));
    }
    const CommandSpec& spec = entry->spec;
    const std::string usage_note = std::format("; usage: {}", usage(spec));

    std::vector<std::optional<ArgValue>> pos(spec.positionals.size());
    std::vector<std::optional<ArgValue>> opt(spec.options.size());
    std::size_t next_pos = 0;
    bool options_done = false;

    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        if (!options_done && t.option_syntax) {
            if (t.text == "--") {
                options_done = true;
                continue;
            }
            const std::string_view body = std::string_view(t.text).substr(2);
            const std::size_t eq = body.find('=');
            const std::string_view name = body.substr(0, eq);
            std::size_t k = 0;
            while (k < spec.options.size() && spec.options[k].name != name) {
                ++k;
            }
            if (k == spec.options.size()) {
                std::vector<std::string> pool;
                for (const ArgSpec& a : spec.options) {
                    pool.push_back(a.name);
                }
                const std::vector<std::string> close = suggest_among(name, pool);
                throw CommandError(std::format("unknown option '--{}' for '{}'{}", name, spec.name,
                                               close.empty() ? usage_note : did_you_mean(close, "--")));
            }
            const ArgSpec& a = spec.options[k];
            if (opt[k]) {
                throw CommandError(std::format("option '--{}' given more than once", name));
            }
            const std::string label = "--" + a.name;
            if (eq != std::string_view::npos) {
                opt[k] = parse_arg(a, body.substr(eq + 1), label);
            } else if (a.type == ArgType::flag) {
                opt[k] = true;
            } else if (i + 1 >= tokens.size() || tokens[i + 1].option_syntax) {
                throw CommandError(
                    std::format("option '--{}' needs a {} value{}", name, type_name(a.type), usage_note));
            } else {
                ++i;
                opt[k] = parse_arg(a, tokens[i].text, label);
            }
            continue;
        }
        if (next_pos >= spec.positionals.size()) {
            throw CommandError(std::format("too many arguments for '{}' (unexpected '{}'){}", spec.name,
                                           t.text, usage_note));
        }
        const ArgSpec& a = spec.positionals[next_pos];
        if (a.rest) {
            if (!pos[next_pos]) {
                pos[next_pos].emplace(std::string());
            }
            std::string& text = std::get<std::string>(*pos[next_pos]);
            text += text.empty() ? "" : " ";
            text += quote_token(t.text);
            continue;
        }
        pos[next_pos] = parse_arg(a, t.text, arg_label(a));
        ++next_pos;
    }

    Invocation inv;
    inv.command = spec.name;
    for (std::size_t k = 0; k < spec.positionals.size(); ++k) {
        const ArgSpec& a = spec.positionals[k];
        if (!pos[k]) {
            if (a.required) {
                throw CommandError(std::format("missing argument {}{}", arg_label(a), usage_note));
            }
            pos[k] = entry->positional_defaults[k];
        }
        if (pos[k]) {
            inv.args.push_back({a.name, std::move(*pos[k])});
        }
    }
    for (std::size_t k = 0; k < spec.options.size(); ++k) {
        const ArgSpec& a = spec.options[k];
        if (!opt[k]) {
            opt[k] = a.type == ArgType::flag ? std::optional<ArgValue>(false) : entry->option_defaults[k];
        }
        if (opt[k]) {
            inv.args.push_back({a.name, std::move(*opt[k])});
        }
    }
    return inv;
}

std::string CommandRegistry::format(const Invocation& inv) const {
    const Entry* entry = find_entry(inv.command);
    if (entry == nullptr || entry->spec.name != inv.command) {
        throw std::invalid_argument(std::format("format: unknown command '{}'", inv.command));
    }
    const CommandSpec& spec = entry->spec;
    std::size_t used = 0;
    auto value_of = [&](const ArgSpec& a) -> const ArgValue* {
        const ArgValue* v = inv.find(a.name);
        if (v != nullptr) {
            if (v->index() != static_cast<std::size_t>(a.type)) {
                throw std::invalid_argument(
                    std::format("format: '{}' argument '{}' has the wrong type", spec.name, a.name));
            }
            ++used;
        }
        return v;
    };

    std::string out = spec.name;
    bool gap = false;
    for (const ArgSpec& a : spec.positionals) {
        const ArgValue* v = value_of(a);
        if (v == nullptr) {
            gap = true;
            continue;
        }
        if (gap) {
            throw std::invalid_argument(
                std::format("format: '{}' positional '{}' follows an absent one", spec.name, a.name));
        }
        out += ' ';
        if (a.rest) {
            // Written as-is, so it must already be canonical or it would not parse back the same.
            const std::string& text = std::get<std::string>(*v);
            std::string canonical;
            for (const Token& t : tokenize(text)) {
                canonical += canonical.empty() ? "" : " ";
                canonical += quote_token(t.text);
            }
            if (canonical != text || canonical.empty()) {
                throw std::invalid_argument(
                    std::format("format: '{}' rest argument '{}' is not canonical", spec.name, a.name));
            }
            out += text;
            continue;
        }
        out += format_value(*v);
    }
    for (const ArgSpec& a : spec.options) {
        const ArgValue* v = value_of(a);
        if (v == nullptr) {
            continue;
        }
        if (a.type == ArgType::flag) {
            if (std::get<bool>(*v)) {
                out += " --" + a.name;
            }
            continue;
        }
        out += std::format(" --{} {}", a.name, format_value(*v));
    }
    if (used != inv.args.size()) {
        throw std::invalid_argument(std::format("format: '{}' has unknown or repeated arguments", spec.name));
    }
    return out;
}

std::string CommandRegistry::usage(const CommandSpec& spec) const {
    std::string out = spec.name;
    for (const ArgSpec& a : spec.positionals) {
        const std::string label = arg_label(a) + (a.rest ? "..." : "");
        out += a.required ? std::format(" {}", label) : std::format(" [{}]", label);
    }
    for (const ArgSpec& a : spec.options) {
        out += std::format(" [{}]", option_label(a));
    }
    return out;
}

void CommandRegistry::write_help(std::ostream& out) const {
    const auto cmds = commands();
    std::size_t width = 0;
    for (const CommandSpec* c : cmds) {
        width = std::max(width, c->name.size());
    }
    out << "Commands:\n";
    for (const CommandSpec* c : cmds) {
        out << std::format("  {:<{}}  {}", c->name, width, c->summary);
        if (!c->aliases.empty()) {
            std::string list;
            for (const std::string& a : c->aliases) {
                list += list.empty() ? a : ", " + a;
            }
            out << std::format(" (alias: {})", list);
        }
        out << '\n';
    }
    out << "Type 'help <command>' for details.\n";
}

void CommandRegistry::write_help(std::ostream& out, const CommandSpec& spec) const {
    out << "usage: " << usage(spec) << '\n';
    if (!spec.summary.empty()) {
        out << "  " << spec.summary << '\n';
    }
    if (!spec.details.empty()) {
        std::istringstream lines(spec.details);
        std::string line;
        while (std::getline(lines, line)) {
            out << (line.empty() ? "" : "  ") << line << '\n';
        }
    }
    if (!spec.aliases.empty()) {
        out << "aliases:";
        for (const std::string& a : spec.aliases) {
            out << ' ' << a;
        }
        out << '\n';
    }
    if (spec.kind == CommandKind::action) {
        out << "Changes the simulation; recorded in the session log for replay.\n";
    }

    // One label column across both sections so they line up.
    std::size_t width = 0;
    for (const ArgSpec& a : spec.positionals) {
        width = std::max(width, arg_label(a).size());
    }
    for (const ArgSpec& a : spec.options) {
        width = std::max(width, a.name.size() + 2);
    }
    auto write_args = [&](std::string_view title, const std::vector<ArgSpec>& args, bool options) {
        if (args.empty()) {
            return;
        }
        out << title << ":\n";
        for (const ArgSpec& a : args) {
            std::string text = a.help;
            if (!a.choices.empty()) {
                std::string list;
                for (const std::string& c : a.choices) {
                    list += list.empty() ? c : "|" + c;
                }
                text += std::format(" [{}]", list);
            }
            if (a.default_value) {
                text += std::format(" (default: {})", *a.default_value);
            } else if (!options && !a.required) {
                text += " (optional)";
            }
            out << std::format("  {:<{}}  {:<12}  {}\n", options ? "--" + a.name : arg_label(a), width,
                               type_name(a.type), text);
        }
    };
    write_args("arguments", spec.positionals, false);
    write_args("options", spec.options, true);
}

Completion CommandRegistry::complete(std::string_view line) const {
    const Lexed lexed = lex(line, true);
    Completion out;
    if (lexed.comment) {
        return out;
    }
    // The word being completed: the last token if the cursor touches it, else a new empty one.
    const bool at_new_word =
        !lexed.open_quote && (lexed.tokens.empty() || is_space(line.back()));
    const std::size_t index = at_new_word ? lexed.tokens.size() : lexed.tokens.size() - 1;
    std::string word = at_new_word ? std::string() : lexed.tokens.back().text;
    out.replace_from = at_new_word ? line.size() : lexed.tokens.back().column;

    auto offer = [&](const std::vector<std::string>& pool, std::string_view prefix, bool quote) {
        for (const std::string& c : pool) {
            if (istarts_with(c, word)) {
                out.candidates.push_back(std::string(prefix) + (quote ? quote_token(c) : c));
            }
        }
        std::sort(out.candidates.begin(), out.candidates.end());
        out.candidates.erase(std::unique(out.candidates.begin(), out.candidates.end()),
                             out.candidates.end());
    };
    auto values_of = [](const ArgSpec& a) {
        std::vector<std::string> pool = a.choices;
        if (a.completer) {
            std::vector<std::string> more = a.completer();
            pool.insert(pool.end(), more.begin(), more.end());
        }
        return pool;
    };

    if (index == 0) {
        std::vector<std::string> pool;
        for (const Entry& e : entries_) {
            pool.push_back(e.spec.name);
            pool.insert(pool.end(), e.spec.aliases.begin(), e.spec.aliases.end());
        }
        offer(pool, "", false);
        return out;
    }
    const Entry* entry = find_entry(lexed.tokens.front().text);
    if (entry == nullptr) {
        return out;
    }
    const CommandSpec& spec = entry->spec;
    auto option_named = [&](std::string_view name) -> const ArgSpec* {
        for (const ArgSpec& a : spec.options) {
            if (a.name == name) {
                return &a;
            }
        }
        return nullptr;
    };

    // Replay the argument grammar over the complete words before the cursor.
    std::size_t positional = 0;
    bool options_done = false;
    const ArgSpec* awaiting_value = nullptr;
    for (std::size_t i = 1; i < index; ++i) {
        const Token& t = lexed.tokens[i];
        if (awaiting_value != nullptr) {
            awaiting_value = nullptr;
            continue;
        }
        if (!options_done && t.option_syntax) {
            if (t.text == "--") {
                options_done = true;
                continue;
            }
            const std::string_view body = std::string_view(t.text).substr(2);
            const ArgSpec* a = option_named(body);
            if (a != nullptr && a->type != ArgType::flag) {
                awaiting_value = a;
            }
            continue;
        }
        ++positional;
    }

    if (awaiting_value != nullptr) {
        offer(values_of(*awaiting_value), "", true);
        return out;
    }
    const bool word_is_option = !at_new_word && lexed.tokens.back().option_syntax;
    if (!options_done && (word_is_option || word == "-")) {
        const std::size_t eq = word.find('=');
        if (eq != std::string::npos) {
            const ArgSpec* a = option_named(std::string_view(word).substr(2, eq - 2));
            if (a == nullptr) {
                return out;
            }
            out.replace_from += eq + 1;
            word = word.substr(eq + 1);
            offer(values_of(*a), "", true);
            return out;
        }
        std::vector<std::string> pool;
        for (const ArgSpec& a : spec.options) {
            pool.push_back("--" + a.name);
        }
        offer(pool, "", false);
        return out;
    }
    if (positional < spec.positionals.size()) {
        offer(values_of(spec.positionals[positional]), "", true);
    } else if (!spec.positionals.empty() && spec.positionals.back().rest) {
        offer(values_of(spec.positionals.back()), "", true);
    }
    return out;
}

std::vector<Invocation> parse_script(const CommandRegistry& registry, std::string_view text) {
    std::vector<Invocation> out;
    std::size_t line_no = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        ++line_no;
        try {
            if (auto inv = registry.parse(line)) {
                out.push_back(std::move(*inv));
            }
        } catch (const CommandError& e) {
            throw CommandError(std::format("line {}: {}", line_no, e.what()));
        }
    }
    return out;
}

} // namespace sim
