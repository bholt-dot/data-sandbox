#include "simcore/text.hpp"

#include <algorithm>
#include <numeric>
#include <vector>

namespace sim {

namespace {
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
} // namespace

std::size_t edit_distance(std::string_view a, std::string_view b) {
    // Three rolling rows of the OSA DP matrix.
    std::vector<std::size_t> prev2(b.size() + 1), prev(b.size() + 1), cur(b.size() + 1);
    std::iota(prev.begin(), prev.end(), std::size_t{0});
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t cost = lower(a[i - 1]) == lower(b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && lower(a[i - 1]) == lower(b[j - 2]) &&
                lower(a[i - 2]) == lower(b[j - 1])) {
                cur[j] = std::min(cur[j], prev2[j - 2] + 1);
            }
        }
        std::swap(prev2, prev);
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string reflow(std::string_view text) {
    std::string out;
    std::string_view rest = text;
    bool pending_break = false; // a blank line was seen since the last word
    while (!rest.empty()) {
        const std::size_t nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string_view::npos) {
            pending_break = !out.empty();
            continue;
        }
        line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
        if (!out.empty()) {
            out += pending_break ? "\n\n" : " ";
        }
        out += line;
        pending_break = false;
    }
    return out;
}

} // namespace sim
