#pragma once

// Draws laid-out sim::Doc rows (see simcore/doc.hpp) as FTXUI elements. Kept out of core so the
// framework has no UI dependency.

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "simcore/doc.hpp"

namespace sim::tui {

// How a semantic style looks: the 16-colour palette (so the terminal's theme applies) or, without
// colour (NO_COLOR), bold/dim/inverse only.
ftxui::Decorator decorator(Style style, bool color);

// One row of spans.
ftxui::Element row(const Line& line, bool color);

// Rows exactly as layout() produced them.
ftxui::Elements rows(const std::vector<Line>& lines, bool color);

// The screen's characters without styling, one '\n'-terminated line per row with trailing blanks
// trimmed (for tests and text snapshots of a frame).
std::string plain_text(const ftxui::Screen& screen);

} // namespace sim::tui
