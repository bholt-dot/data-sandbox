#pragma once

// Styled command output. Handlers describe *what* their text is (a heading, an amount of money,
// a warning, a table) and a renderer decides how it looks:
//
//   plain       scripts, tests, pipes, the determinism check: text only, byte-stable
//   ANSI        an interactive terminal: SGR colour (or bold/inverse only under NO_COLOR)
//   rows        layout() at a given width, for a full-screen UI to draw (apps/ turns the rows
//               into widgets, so core stays dependency-free)
//
// A Doc is plain data: a list of lines and tables, each line a list of styled spans.
//
//   sim::Doc out;
//   out.heading("Ceres Station market");
//   auto& t = out.table({{"commodity"}, {"stock", sim::Align::right}});
//   t.row({"water", sim::styled(sim::Style::warning, "12 t")});
//   out << "cash now " << sim::styled(sim::Style::money, "1,850 cr") << "\n";

#include <cstddef>
#include <cstdint>
#include <deque>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace sim {

// Semantic styles; the renderer's theme maps them to colours. Keep the set small.
enum class Style : std::uint8_t {
    plain,
    heading,  // section titles and table headers
    emphasis, // a name or figure worth a second look
    dim,      // secondary detail: timestamps, units, hints
    key,      // something the player can type: commands, station and commodity keys
    money,    // an amount of money, neither gain nor loss
    positive, // a gain, a healthy figure
    negative, // a loss, a debt, a worrying figure
    good,     // a verdict: GO, done
    bad,      // a verdict: NO GO, errors
    warning,  // needs attention soon
    urgent,   // needs attention now
};

struct Span {
    std::string text{}; // no line breaks
    Style style = Style::plain;

    bool operator==(const Span&) const = default;
};

inline Span styled(Style style, std::string text) { return {std::move(text), style}; }

// Terminal columns of UTF-8 text (one per code point; the game's text has no wide characters).
std::size_t display_width(std::string_view utf8);

// One output line: a run of styled spans.
struct Line {
    std::vector<Span> spans;

    Line() = default;
    // Implicit, so table cells can be written as plain strings or single spans.
    Line(std::string text) { if (!text.empty()) spans.push_back({std::move(text), Style::plain}); }
    Line(const char* text) : Line(std::string(text)) {}
    Line(Span span) { if (!span.text.empty()) spans.push_back(std::move(span)); }
    Line(std::vector<Span> s) : spans(std::move(s)) {}

    bool operator==(const Line&) const = default;

    void append(Span span);           // merges with the last span when the style matches
    std::size_t width() const;        // display columns
    std::string text() const;         // unstyled
    bool empty() const { return spans.empty(); }
};

enum class Align : std::uint8_t { left, right };

struct Column {
    std::string header{}; // tables whose headers are all empty print no header row
    Align align = Align::left;
};

// Columns are sized to their widest cell. When a width limit applies, the last column wraps.
struct TextTable {
    std::vector<Column> columns{};
    std::vector<std::vector<Line>> rows{};
    std::size_t indent = 2; // spaces before the first column
    std::size_t gap = 2;    // spaces between columns

    // Missing trailing cells are left empty; extra cells are a programming error.
    void row(std::vector<Line> cells);
};

using Block = std::variant<Line, TextTable>;

class Doc {
public:
    // Text is appended to the current line; '\n' ends it.
    Doc& operator<<(std::string_view text) { return write(Style::plain, text); }
    Doc& operator<<(const std::string& text) { return write(Style::plain, text); }
    Doc& operator<<(const char* text) { return write(Style::plain, text); }
    Doc& operator<<(char c) { return write(Style::plain, std::string_view(&c, 1)); }
    Doc& operator<<(const Span& span) { return write(span.style, span.text); }
    Doc& operator<<(const Line& line);
    // Numbers must be formatted explicitly (std::format), not silently converted to a char.
    template <class T>
        requires(std::is_arithmetic_v<T> && !std::is_same_v<T, char>)
    Doc& operator<<(T) = delete;
    Doc& write(Style style, std::string_view text);

    // A complete line in heading style (ends any line in progress first).
    Doc& heading(std::string_view text);

    // Starts a table; rows can be added while writing on (the reference lives as long as the Doc).
    TextTable& table(std::vector<Column> columns);

    // Appends another document's blocks.
    Doc& append(const Doc& other);

    const std::deque<Block>& blocks() const { return blocks_; }
    bool empty() const { return blocks_.empty(); }

private:
    Line& open_line();

    std::deque<Block> blocks_; // a deque, so table references survive later writes
    bool open_ = false; // the last block is a Line still being written
};

// Lays the document out as rows of at most `width` columns (0 = unlimited): tables become padded
// rows; long lines word-wrap with a hanging indent. Rows carry no trailing padding.
std::vector<Line> layout(const Doc& doc, std::size_t width = 0);

// How a renderer may use escape codes.
enum class Ansi : std::uint8_t {
    none,  // plain text
    mono,  // bold/underline/inverse only (NO_COLOR is set)
    color, // 16-colour SGR, so the user's terminal palette applies
};

// The SGR parameters for a style ("1;31"), empty when it renders as plain text.
std::string_view sgr(Style style, Ansi ansi);

// Renders every row followed by '\n'.
void render(const Doc& doc, std::ostream& out, Ansi ansi = Ansi::none, std::size_t width = 0);
void render(const std::vector<Line>& rows, std::ostream& out, Ansi ansi = Ansi::none);
std::string to_text(const Doc& doc);

// Escape codes suitable for a terminal on the given stream: none unless it is a terminal
// (`is_terminal`), none for TERM=dumb, mono when NO_COLOR is set to a non-empty value
// (https://no-color.org), colour otherwise.
Ansi ansi_for_terminal(bool is_terminal);

// True when NO_COLOR is set to a non-empty value.
bool no_color_requested();

} // namespace sim
