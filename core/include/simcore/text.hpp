#pragma once

#include <cstddef>
#include <string_view>

namespace sim {

// Optimal-string-alignment edit distance (Levenshtein + adjacent transpositions, which covers the
// most common typing mistakes), ASCII case-insensitive. Used for "did you mean" suggestions.
std::size_t edit_distance(std::string_view a, std::string_view b);

} // namespace sim
