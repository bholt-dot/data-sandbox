#include "simcore/doc.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sim {

namespace {

bool is_continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u; }

// Byte length of the first `columns` code points of `s`.
std::size_t prefix_bytes(std::string_view s, std::size_t columns) {
    std::size_t i = 0;
    for (std::size_t seen = 0; i < s.size(); ++i) {
        if (!is_continuation(s[i])) {
            if (seen == columns) {
                break;
            }
            ++seen;
        }
    }
    return i;
}

Span spaces(std::size_t n) { return {std::string(n, ' '), Style::plain}; }

void trim_right(Line& line) {
    while (!line.spans.empty()) {
        std::string& t = line.spans.back().text;
        t.erase(t.find_last_not_of(' ') + 1);
        if (!t.empty()) {
            return;
        }
        line.spans.pop_back();
    }
}

// A word or a run of spaces, in one style.
struct Atom {
    std::string_view text;
    Style style;
    bool space;
};

std::vector<Atom> atoms_of(const Line& line) {
    std::vector<Atom> out;
    for (const Span& s : line.spans) {
        std::string_view t = s.text;
        while (!t.empty()) {
            const bool space = t.front() == ' ';
            const std::size_t end = space ? t.find_first_not_of(' ') : t.find(' ');
            const std::size_t n = end == std::string_view::npos ? t.size() : end;
            out.push_back({t.substr(0, n), s.style, space});
            t.remove_prefix(n);
        }
    }
    return out;
}

// Greedy word wrap. Continuation rows get the line's leading indentation (capped at half the
// width); words longer than a row are split.
std::vector<Line> wrap(const Line& line, std::size_t width) {
    if (width == 0 || line.width() <= width) {
        return {line};
    }
    const std::string text = line.text();
    const std::size_t hang = std::min(text.find_first_not_of(' ') == std::string::npos
                                          ? std::size_t{0}
                                          : text.find_first_not_of(' '),
                                      width / 2);

    std::vector<Line> rows;
    Line cur;
    std::size_t cur_w = 0;
    bool has_word = false;
    std::vector<Span> pending; // spaces between the last word and the next
    std::size_t pending_w = 0;

    auto next_row = [&] {
        trim_right(cur);
        rows.push_back(std::move(cur));
        cur = Line();
        if (hang > 0) {
            cur.append(spaces(hang));
        }
        cur_w = hang;
        has_word = false;
        pending.clear();
        pending_w = 0;
    };

    for (const Atom& a : atoms_of(line)) {
        const std::size_t w = display_width(a.text);
        if (a.space) {
            if (has_word) {
                pending.push_back({std::string(a.text), a.style});
                pending_w += w;
            } else if (rows.empty()) {
                cur.append({std::string(a.text), a.style}); // the line's own indentation
                cur_w += w;
            }
            continue;
        }
        if (has_word && cur_w + pending_w + w > width) {
            next_row();
        } else {
            for (Span& p : pending) {
                cur.append(std::move(p));
            }
            cur_w += pending_w;
        }
        pending.clear();
        pending_w = 0;

        std::string_view rest = a.text;
        std::size_t rest_w = w;
        while (cur_w + rest_w > width) {
            if (cur_w >= width) {
                next_row();
                continue;
            }
            const std::size_t n = prefix_bytes(rest, width - cur_w);
            cur.append({std::string(rest.substr(0, n)), a.style});
            rest.remove_prefix(n);
            rest_w = display_width(rest);
            next_row();
        }
        if (!rest.empty()) {
            cur.append({std::string(rest), a.style});
            cur_w += rest_w;
            has_word = true;
        }
    }
    trim_right(cur);
    rows.push_back(std::move(cur));
    return rows;
}

void layout_table(const TextTable& t, std::size_t width, std::vector<Line>& out) {
    const std::size_t n = t.columns.size();
    if (n == 0) {
        return;
    }
    std::vector<std::size_t> col(n, 0);
    bool header = false;
    for (std::size_t i = 0; i < n; ++i) {
        col[i] = display_width(t.columns[i].header);
        header = header || !t.columns[i].header.empty();
    }
    for (const auto& r : t.rows) {
        for (std::size_t i = 0; i < r.size(); ++i) {
            col[i] = std::max(col[i], r[i].width());
        }
    }
    if (width > 0) {
        std::size_t fixed = t.indent + t.gap * (n - 1);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            fixed += col[i];
        }
        if (fixed + col[n - 1] > width) {
            constexpr std::size_t min_last = 12;
            const std::size_t floor = std::min(col[n - 1], min_last);
            col[n - 1] = std::max(width > fixed ? width - fixed : 0, floor);
        }
    }

    auto emit = [&](const std::vector<Line>& cells) {
        std::vector<std::vector<Line>> parts(n);
        std::size_t height = 1;
        for (std::size_t i = 0; i < n; ++i) {
            const Line cell = i < cells.size() ? cells[i] : Line();
            parts[i] = (i + 1 == n && width > 0) ? wrap(cell, col[i]) : std::vector<Line>{cell};
            height = std::max(height, parts[i].size());
        }
        for (std::size_t r = 0; r < height; ++r) {
            Line row;
            row.append(spaces(t.indent));
            for (std::size_t i = 0; i < n; ++i) {
                const Line c = r < parts[i].size() ? parts[i][r] : Line();
                const std::size_t pad = col[i] > c.width() ? col[i] - c.width() : 0;
                if (t.columns[i].align == Align::right) {
                    row.append(spaces(pad));
                }
                for (const Span& s : c.spans) {
                    row.append(s);
                }
                if (t.columns[i].align == Align::left && i + 1 < n) {
                    row.append(spaces(pad));
                }
                if (i + 1 < n) {
                    row.append(spaces(t.gap));
                }
            }
            trim_right(row);
            out.push_back(std::move(row));
        }
    };

    if (header) {
        std::vector<Line> cells;
        for (const Column& c : t.columns) {
            cells.emplace_back(Span{c.header, Style::heading});
        }
        emit(cells);
    }
    for (const auto& r : t.rows) {
        emit(r);
    }
}

} // namespace

std::size_t display_width(std::string_view utf8) {
    return static_cast<std::size_t>(std::count_if(utf8.begin(), utf8.end(), [](char c) { return !is_continuation(c); }));
}

void Line::append(Span span) {
    if (span.text.empty()) {
        return;
    }
    if (!spans.empty() && spans.back().style == span.style) {
        spans.back().text += span.text;
        return;
    }
    spans.push_back(std::move(span));
}

std::size_t Line::width() const {
    std::size_t w = 0;
    for (const Span& s : spans) {
        w += display_width(s.text);
    }
    return w;
}

std::string Line::text() const {
    std::string out;
    for (const Span& s : spans) {
        out += s.text;
    }
    return out;
}

void TextTable::row(std::vector<Line> cells) {
    if (cells.size() > columns.size()) {
        throw std::logic_error("TextTable::row: more cells than columns");
    }
    cells.resize(columns.size());
    rows.push_back(std::move(cells));
}

Line& Doc::open_line() {
    if (!open_) {
        blocks_.emplace_back(Line());
        open_ = true;
    }
    return std::get<Line>(blocks_.back());
}

Doc& Doc::write(Style style, std::string_view text) {
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        const std::string_view part = text.substr(0, nl);
        if (!part.empty()) {
            open_line().append({std::string(part), style});
        }
        if (nl == std::string_view::npos) {
            break;
        }
        open_line(); // a bare '\n' is a blank line
        open_ = false;
        text.remove_prefix(nl + 1);
    }
    return *this;
}

Doc& Doc::operator<<(const Line& line) {
    for (const Span& s : line.spans) {
        write(s.style, s.text);
    }
    return *this;
}

Doc& Doc::heading(std::string_view text) {
    open_ = false;
    blocks_.emplace_back(Line(Span{std::string(text), Style::heading}));
    return *this;
}

TextTable& Doc::table(std::vector<Column> columns) {
    open_ = false;
    TextTable t;
    t.columns = std::move(columns);
    blocks_.emplace_back(std::move(t));
    return std::get<TextTable>(blocks_.back());
}

Doc& Doc::append(const Doc& other) {
    blocks_.insert(blocks_.end(), other.blocks_.begin(), other.blocks_.end());
    open_ = other.open_;
    return *this;
}

std::vector<Line> layout(const Doc& doc, std::size_t width) {
    std::vector<Line> out;
    for (const Block& b : doc.blocks()) {
        if (const auto* line = std::get_if<Line>(&b)) {
            for (Line& row : wrap(*line, width)) {
                out.push_back(std::move(row));
            }
        } else {
            layout_table(std::get<TextTable>(b), width, out);
        }
    }
    return out;
}

std::string_view sgr(Style style, Ansi ansi) {
    if (ansi == Ansi::none) {
        return {};
    }
    const bool color = ansi == Ansi::color;
    switch (style) {
    case Style::plain: return {};
    case Style::heading: return "1";
    case Style::emphasis: return "1";
    case Style::dim: return "2";
    case Style::key: return color ? "36" : "";
    case Style::money: return color ? "33" : "";
    case Style::positive: return color ? "32" : "";
    case Style::negative: return color ? "31" : "";
    case Style::good: return color ? "1;32" : "1";
    case Style::bad: return color ? "1;31" : "1";
    case Style::warning: return color ? "1;33" : "1";
    case Style::urgent: return color ? "1;37;41" : "1;7";
    }
    return {};
}

void render(const std::vector<Line>& rows, std::ostream& out, Ansi ansi) {
    for (const Line& row : rows) {
        for (const Span& s : row.spans) {
            const std::string_view code = sgr(s.style, ansi);
            if (code.empty()) {
                out << s.text;
            } else {
                out << "\x1b[" << code << 'm' << s.text << "\x1b[0m";
            }
        }
        out << '\n';
    }
}

void render(const Doc& doc, std::ostream& out, Ansi ansi, std::size_t width) {
    render(layout(doc, width), out, ansi);
}

std::string to_text(const Doc& doc) {
    std::ostringstream out;
    render(doc, out);
    return out.str();
}

bool no_color_requested() {
    const char* v = std::getenv("NO_COLOR");
    return v != nullptr && *v != '\0';
}

Ansi ansi_for_terminal(bool is_terminal) {
    if (!is_terminal) {
        return Ansi::none;
    }
    const char* term = std::getenv("TERM");
    if (term != nullptr && std::string_view(term) == "dumb") {
        return Ansi::none;
    }
    return no_color_requested() ? Ansi::mono : Ansi::color;
}

} // namespace sim
