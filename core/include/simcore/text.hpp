#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace sim {

// Optimal-string-alignment edit distance (Levenshtein + adjacent transpositions, which covers the
// most common typing mistakes), ASCII case-insensitive. Used for "did you mean" suggestions.
std::size_t edit_distance(std::string_view a, std::string_view b);

// Joins hard-wrapped lines into paragraphs so a renderer can wrap them to its own width: single
// newlines become spaces, blank lines separate paragraphs ("\n\n"), surrounding space is trimmed.
std::string reflow(std::string_view text);

} // namespace sim
