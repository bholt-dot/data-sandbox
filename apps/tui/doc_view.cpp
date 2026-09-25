#include "doc_view.hpp"

#include <utility>

namespace sim::tui {

using namespace ftxui;

Decorator decorator(Style style, bool color) {
    const Decorator none = [](Element e) { return e; };
    switch (style) {
    case Style::plain: return none;
    case Style::heading: return bold;
    case Style::emphasis: return bold;
    case Style::dim: return dim;
    case Style::key: return color ? ftxui::color(Color::Cyan) : none;
    case Style::money: return color ? ftxui::color(Color::Yellow) : none;
    case Style::positive: return color ? ftxui::color(Color::Green) : none;
    case Style::negative: return color ? ftxui::color(Color::Red) : none;
    case Style::good: return color ? Decorator(bold) | ftxui::color(Color::Green) : Decorator(bold);
    case Style::bad: return color ? Decorator(bold) | ftxui::color(Color::Red) : Decorator(bold);
    case Style::warning: return color ? Decorator(bold) | ftxui::color(Color::Yellow) : Decorator(bold);
    case Style::urgent:
        return color ? Decorator(bold) | ftxui::color(Color::White) | bgcolor(Color::Red)
                     : Decorator(bold) | Decorator(inverted);
    }
    return none;
}

Element row(const Line& line, bool color) {
    Elements parts;
    parts.reserve(line.spans.size());
    for (const Span& s : line.spans) {
        parts.push_back(text(s.text) | decorator(s.style, color));
    }
    if (parts.empty()) {
        return text("");
    }
    return hbox(std::move(parts));
}

Elements rows(const std::vector<Line>& lines, bool color) {
    Elements out;
    out.reserve(lines.size());
    for (const Line& l : lines) {
        out.push_back(row(l, color));
    }
    return out;
}

std::string plain_text(const Screen& screen) {
    std::string out;
    for (int y = 0; y < screen.dimy(); ++y) {
        std::string line;
        for (int x = 0; x < screen.dimx(); ++x) {
            const std::string& c = screen.CellAt(x, y).character;
            line += c.empty() ? " " : c;
        }
        line.erase(line.find_last_not_of(' ') + 1);
        out += line + '\n';
    }
    return out;
}

} // namespace sim::tui
