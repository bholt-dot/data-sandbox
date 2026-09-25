// The only translation unit that includes toml++; everything else goes through DataNode.

#include "simcore/data_node.hpp"

#include "toml_document.hpp"

#include <algorithm>
#include <format>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#define TOML_EXCEPTIONS 0
#define TOML_ENABLE_FORMATTERS 0
#include <toml++/toml.hpp>

namespace sim {

struct DataNodeAccess {
    static DataNode make(const toml::node* n) { return DataNode{static_cast<const void*>(n)}; }
    static const toml::node* raw(const DataNode& d) { return static_cast<const toml::node*>(d.node_); }
};

namespace {

SourceLoc to_loc(const toml::source_region& r) {
    SourceLoc loc;
    if (r.path) {
        loc.file = *r.path;
    }
    loc.line = static_cast<std::uint32_t>(r.begin.line);
    loc.column = static_cast<std::uint32_t>(r.begin.column);
    return loc;
}

} // namespace

// --- diagnostics ---

std::string SourceLoc::to_string() const {
    const std::string f = file.empty() ? std::string("<unknown>") : file;
    if (line == 0) {
        return f;
    }
    if (column == 0) {
        return std::format("{}:{}", f, line);
    }
    return std::format("{}:{}:{}", f, line, column);
}

std::string Diagnostic::to_string() const {
    return std::format("{}: error: {}", where.to_string(), message);
}

bool Diagnostics::contains(std::string_view needle) const {
    return std::any_of(items_.begin(), items_.end(),
                       [&](const Diagnostic& d) { return d.message.find(needle) != std::string::npos; });
}

void Diagnostics::sort(std::span<const std::string> file_order) {
    auto rank = [&](const std::string& file) {
        auto it = std::find(file_order.begin(), file_order.end(), file);
        return static_cast<std::size_t>(it - file_order.begin());
    };
    std::stable_sort(items_.begin(), items_.end(), [&](const Diagnostic& a, const Diagnostic& b) {
        const std::size_t ra = rank(a.where.file);
        const std::size_t rb = rank(b.where.file);
        if (ra != rb) {
            return ra < rb;
        }
        if (a.where.file != b.where.file) {
            return a.where.file < b.where.file;
        }
        return std::pair{a.where.line, a.where.column} < std::pair{b.where.line, b.where.column};
    });
}

std::string Diagnostics::to_string() const {
    std::string out;
    for (const Diagnostic& d : items_) {
        out += d.to_string();
        out += '\n';
    }
    return out;
}

// --- DataNode ---

const char* kind_name(DataNode::Kind kind) {
    switch (kind) {
    case DataNode::Kind::none: return "nothing";
    case DataNode::Kind::table: return "table";
    case DataNode::Kind::array: return "array";
    case DataNode::Kind::string: return "string";
    case DataNode::Kind::integer: return "integer";
    case DataNode::Kind::floating: return "float";
    case DataNode::Kind::boolean: return "boolean";
    case DataNode::Kind::datetime: return "date/time";
    }
    return "unknown";
}

DataNode::Kind DataNode::kind() const {
    const toml::node* n = DataNodeAccess::raw(*this);
    if (n == nullptr) {
        return Kind::none;
    }
    switch (n->type()) {
    case toml::node_type::table: return Kind::table;
    case toml::node_type::array: return Kind::array;
    case toml::node_type::string: return Kind::string;
    case toml::node_type::integer: return Kind::integer;
    case toml::node_type::floating_point: return Kind::floating;
    case toml::node_type::boolean: return Kind::boolean;
    case toml::node_type::date:
    case toml::node_type::time:
    case toml::node_type::date_time: return Kind::datetime;
    case toml::node_type::none: break;
    }
    return Kind::none;
}

std::string DataNode::describe() const {
    const toml::node* n = DataNodeAccess::raw(*this);
    switch (kind()) {
    case Kind::string: {
        std::string_view s = n->as_string()->get();
        constexpr std::size_t max_len = 40;
        if (s.size() > max_len) {
            return std::format("string \"{}...\"", s.substr(0, max_len));
        }
        return std::format("string \"{}\"", s);
    }
    case Kind::integer: return std::format("integer {}", n->as_integer()->get());
    case Kind::floating: return std::format("float {}", n->as_floating_point()->get());
    case Kind::boolean: return std::format("boolean {}", n->as_boolean()->get());
    default: return kind_name(kind());
    }
}

SourceLoc DataNode::loc() const {
    const toml::node* n = DataNodeAccess::raw(*this);
    return n != nullptr ? to_loc(n->source()) : SourceLoc{};
}

std::optional<std::int64_t> DataNode::as_integer() const {
    const toml::node* n = DataNodeAccess::raw(*this);
    if (n != nullptr) {
        if (const auto* v = n->as_integer()) {
            return v->get();
        }
    }
    return std::nullopt;
}

std::optional<double> DataNode::as_floating() const {
    const toml::node* n = DataNodeAccess::raw(*this);
    if (n != nullptr) {
        if (const auto* v = n->as_floating_point()) {
            return v->get();
        }
        if (const auto* v = n->as_integer()) {
            return static_cast<double>(v->get());
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> DataNode::as_string() const {
    const toml::node* n = DataNodeAccess::raw(*this);
    if (n != nullptr) {
        if (const auto* v = n->as_string()) {
            return std::string_view{v->get()};
        }
    }
    return std::nullopt;
}

std::optional<bool> DataNode::as_bool() const {
    const toml::node* n = DataNodeAccess::raw(*this);
    if (n != nullptr) {
        if (const auto* v = n->as_boolean()) {
            return v->get();
        }
    }
    return std::nullopt;
}

DataNode DataNode::get(std::string_view key) const {
    const toml::node* n = DataNodeAccess::raw(*this);
    if (n != nullptr) {
        if (const auto* t = n->as_table()) {
            return DataNodeAccess::make(t->get(key));
        }
    }
    return {};
}

void DataNode::for_each(
    const std::function<void(std::string_view, const SourceLoc&, DataNode)>& fn) const {
    const toml::node* n = DataNodeAccess::raw(*this);
    if (n == nullptr) {
        return;
    }
    if (const auto* t = n->as_table()) {
        for (const auto& [key, value] : *t) {
            SourceLoc key_loc = to_loc(key.source());
            if (key_loc.line == 0) {
                key_loc = to_loc(value.source());
            }
            fn(key.str(), key_loc, DataNodeAccess::make(&value));
        }
    }
}

std::size_t DataNode::size() const {
    const toml::node* n = DataNodeAccess::raw(*this);
    if (n != nullptr) {
        if (const auto* a = n->as_array()) {
            return a->size();
        }
        if (const auto* t = n->as_table()) {
            return t->size();
        }
    }
    return 0;
}

DataNode DataNode::at(std::size_t i) const {
    const toml::node* n = DataNodeAccess::raw(*this);
    if (n != nullptr) {
        if (const auto* a = n->as_array()) {
            return DataNodeAccess::make(a->get(i));
        }
    }
    return {};
}

// --- suggestions ---


std::optional<std::string_view> closest_match(std::string_view word,
                                              std::span<const std::string_view> candidates) {
    // Case-insensitive (like rustc's suggestions), so `Name` suggests `name`.
    auto lower = [](std::string_view s) {
        std::string out(s);
        for (char& ch : out) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        return out;
    };
    const std::string lowered = lower(word);
    const std::size_t limit = std::max<std::size_t>(1, word.size() / 3);
    std::optional<std::string_view> best;
    std::size_t best_distance = limit + 1;
    for (std::string_view c : candidates) {
        const std::size_t d = edit_distance(lowered, lower(c));
        if (d < best_distance) {
            best = c;
            best_distance = d;
        }
    }
    return best;
}

std::string did_you_mean(std::string_view word, std::span<const std::string_view> candidates) {
    if (auto m = closest_match(word, candidates)) {
        return std::format(" (did you mean '{}'?)", *m);
    }
    return {};
}

// --- TomlDocument ---

namespace detail {

struct TomlDocument::Impl {
    toml::table table;
};

TomlDocument::TomlDocument(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TomlDocument::TomlDocument(TomlDocument&&) noexcept = default;
TomlDocument& TomlDocument::operator=(TomlDocument&&) noexcept = default;
TomlDocument::~TomlDocument() = default;

std::optional<TomlDocument> TomlDocument::parse(std::string_view name, std::string_view text,
                                                Diagnostics& diags) {
    toml::parse_result result = toml::parse(text, name);
    if (!result) {
        const toml::parse_error& err = result.error();
        SourceLoc loc = to_loc(err.source());
        if (loc.file.empty()) {
            loc.file = std::string(name);
        }
        diags.error(std::move(loc), std::format("TOML syntax error: {}", err.description()));
        return std::nullopt;
    }
    auto impl = std::make_unique<Impl>();
    impl->table = std::move(result).table();
    return TomlDocument{std::move(impl)};
}

DataNode TomlDocument::root() const { return DataNodeAccess::make(&impl_->table); }

} // namespace detail
} // namespace sim
