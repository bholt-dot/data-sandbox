#pragma once

// Parser-agnostic view of parsed data files, plus diagnostics with source locations.
//
// DataNode wraps a node of a parsed TOML document without exposing toml++ in public headers:
// toml++ is only included by core/src, which keeps its (large) headers and warnings out of every
// translation unit that declares definition types. A DataNode is a non-owning view; it is valid
// only while the document it came from is alive (i.e. during DefRegistry::load).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sim {

struct SourceLoc {
    std::string file;
    std::uint32_t line = 0;   // 1-based; 0 = unknown
    std::uint32_t column = 0; // 1-based; 0 = unknown

    // "file:line:col", "file:line" or "file", matching compiler-style diagnostics so editors and
    // terminals can jump to the location.
    std::string to_string() const;
};

struct Diagnostic {
    SourceLoc where;
    std::string message;

    std::string to_string() const; // "file:line:col: error: message"
};

// An ordered list of errors. Loaders collect everything they can find rather than stopping at the
// first problem, so a modder can fix a whole file in one round trip.
class Diagnostics {
public:
    void error(SourceLoc where, std::string message) {
        items_.push_back({std::move(where), std::move(message)});
    }
    void append(const Diagnostics& other) {
        items_.insert(items_.end(), other.items_.begin(), other.items_.end());
    }

    bool ok() const { return items_.empty(); }
    std::size_t size() const { return items_.size(); }
    std::span<const Diagnostic> items() const { return items_; }
    auto begin() const { return items_.begin(); }
    auto end() const { return items_.end(); }

    // True if any message contains `needle`; convenient for tests and tooling.
    bool contains(std::string_view needle) const;

    // Stable sort by (file, line, column) so output reads top to bottom per file. Files are
    // ordered by their position in `file_order` (load order); unlisted files sort last.
    void sort(std::span<const std::string> file_order);

    std::string to_string() const; // one diagnostic per line

private:
    std::vector<Diagnostic> items_;
};

class DataNode {
public:
    enum class Kind : std::uint8_t { none, table, array, string, integer, floating, boolean, datetime };

    DataNode() = default;

    Kind kind() const;
    explicit operator bool() const { return node_ != nullptr; }
    bool is_table() const { return kind() == Kind::table; }
    bool is_array() const { return kind() == Kind::array; }

    // Human-readable description of the node for "expected X, got Y" messages, e.g.
    // `string "fast"` or `integer 12`.
    std::string describe() const;
    SourceLoc loc() const;

    // Scalar accessors return nullopt on a kind mismatch. as_floating also accepts integers,
    // because modders will write `mass = 5` for a floating-point field.
    std::optional<std::int64_t> as_integer() const;
    std::optional<double> as_floating() const;
    std::optional<std::string_view> as_string() const;
    std::optional<bool> as_bool() const;

    // Tables. Missing keys yield a null DataNode. for_each visits entries in key order.
    DataNode get(std::string_view key) const;
    void for_each(const std::function<void(std::string_view key, const SourceLoc& key_loc,
                                           DataNode value)>& fn) const;

    // Arrays (size() is also the entry count for tables).
    std::size_t size() const;
    DataNode at(std::size_t i) const;

private:
    friend struct DataNodeAccess;
    explicit DataNode(const void* node) : node_(node) {}

    const void* node_ = nullptr; // const toml::node*
};

const char* kind_name(DataNode::Kind kind);

// Optimal-string-alignment edit distance (Levenshtein + adjacent transpositions, which covers
// the most common typing mistakes).
std::size_t edit_distance(std::string_view a, std::string_view b);

// The candidate closest to `word` (ignoring ASCII case), if it is close enough to plausibly be a typo
// (distance <= max(1, word.size() / 3)). Ties go to the earliest candidate, so results are
// deterministic for a given candidate order.
std::optional<std::string_view> closest_match(std::string_view word,
                                              std::span<const std::string_view> candidates);

// " (did you mean 'x'?)" or "".
std::string did_you_mean(std::string_view word, std::span<const std::string_view> candidates);

} // namespace sim
