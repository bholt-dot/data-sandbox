#pragma once

// Data definitions: static game content (commodities, bodies, ship classes, ...) loaded from TOML
// into typed, read-only tables.
//
// A definition type is a plain struct with a static describe() that declares its fields:
//
//     enum class Kind { rocky, icy };
//     constexpr auto enum_names(Kind) {                       // found by ADL
//         return std::array{std::pair{std::string_view{"rocky"}, Kind::rocky},
//                           std::pair{std::string_view{"icy"}, Kind::icy}};
//     }
//     struct Body {
//         std::string name;
//         Kind kind = Kind::rocky;
//         double radius_km = 0.0;
//         static void describe(sim::Schema<Body>& s) {
//             s.field("name", &Body::name).non_empty();
//             s.optional("kind", &Body::kind);                // keeps the member's default
//             s.field("radius_km", &Body::radius_km).min(0.0);
//         }
//     };
//     struct Station {
//         sim::DefId<Body> body;                              // reference by key, resolved later
//         static void describe(sim::Schema<Station>& s) { s.field("body", &Station::body); }
//     };
//
//     sim::DefRegistry defs;
//     defs.define<Body>("body");
//     defs.define<Station>("station");
//     sim::Diagnostics errors = defs.load_directory("data");
//
// and the TOML layout is one table per definition, keyed by section then definition key:
//
//     [body.ceres]
//     name = "Ceres"
//     radius_km = 473.0
//
//     [station.ceres_station]
//     body = "ceres"
//
// Loading is two-pass. Pass 1 parses every file and reads every row, recording references as
// (target type, key) pairs. Pass 2 assigns ids and resolves the references, so files and
// definitions may appear in any order. All errors from all files are collected and reported with
// file:line:col; if there are any, the registry keeps its previous contents (load is atomic).
//
// Ids are deterministic: each type's ids are assigned in lexicographic key order, independent of
// which file a definition lives in or the order files were read.

#include "simcore/data_node.hpp"
#include "simcore/table.hpp"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace sim {

// Definitions use the row type itself as the handle tag.
template <typename Row>
using DefId = Handle<Row>;

template <typename Row>
class Schema;

// A struct readable from a TOML table: default-constructible with `static void describe(Schema&)`.
template <typename T>
concept Describable = std::default_initializable<T> && requires(Schema<T>& s) { T::describe(s); };

// An enum readable from a string: an ADL-visible `enum_names(E)` returning a range of
// {std::string_view, E} pairs.
template <typename E>
concept NamedEnum = std::is_enum_v<E> && requires { enum_names(E{}); };

// Per-load reading state: error reporting with context, and deferred references.
class ReadContext {
public:
    using RefWriter = std::function<void(std::uint32_t index, std::uint32_t generation)>;

    struct PendingRef {
        std::type_index target;
        std::string key;
        SourceLoc where;
        std::string prefix; // "station 'x': field 'body'"
        RefWriter write;
    };

    explicit ReadContext(Diagnostics& diags) : diags_(diags) {}

    // What is being read, e.g. "station 'ceres_station'"; prefixed to every message.
    void set_subject(std::string subject) { subject_ = std::move(subject); }

    // "<subject>: field '<path>': <message>" (the field part is omitted at the row's top level).
    void error(const SourceLoc& where, std::string_view message);
    // "<subject>: <message>"
    void error_in_subject(const SourceLoc& where, std::string_view message);
    // "expected <expected>, got <description of node>"
    void type_error(DataNode got, std::string_view expected);

    std::size_t error_count() const { return diags_.size(); }

    // Field path tracking ("inputs[2].amount"); push/pop via RAII scopes.
    class Scope {
    public:
        explicit Scope(ReadContext& ctx) : ctx_(ctx) {}
        ~Scope() { ctx_.path_.pop_back(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        ReadContext& ctx_;
    };
    [[nodiscard]] Scope field(std::string_view key) {
        path_.emplace_back(key);
        return Scope{*this};
    }
    [[nodiscard]] Scope index(std::size_t i) {
        path_.push_back(std::format("[{}]", i));
        return Scope{*this};
    }
    std::string path() const;
    std::string path_with(std::string_view key) const; // path() + "." + key

    // Records a reference to a definition of type `target`; resolved after all files are read.
    void defer_ref(std::type_index target, std::string key, SourceLoc where, RefWriter write);
    std::vector<PendingRef>& pending_refs() { return pending_; }

private:
    std::string prefix() const;

    Diagnostics& diags_;
    std::string subject_;
    std::vector<std::string> path_;
    std::vector<PendingRef> pending_;
};

// ---------------------------------------------------------------------------------------------
// Value codecs: how a single C++ value is read from a DataNode. Specialise ValueCodec<T> to
// teach the loader a new value type. read() reports its own errors and returns false on failure.

template <typename T>
struct ValueCodec; // intentionally undefined: unsupported field type

template <typename T>
concept Readable = requires(DataNode n, T& v, ReadContext& c) {
    { ValueCodec<T>::read(n, v, c) } -> std::same_as<bool>;
    { ValueCodec<T>::expected() } -> std::convertible_to<std::string>;
};

template <typename T>
const Schema<T>& schema_of();

template <>
struct ValueCodec<bool> {
    static std::string expected() { return "boolean"; }
    static bool read(DataNode n, bool& out, ReadContext& ctx) {
        if (auto v = n.as_bool()) {
            out = *v;
            return true;
        }
        ctx.type_error(n, expected());
        return false;
    }
};

template <std::integral T>
    requires(!std::same_as<T, bool>)
struct ValueCodec<T> {
    static std::string expected() { return "integer"; }
    static bool read(DataNode n, T& out, ReadContext& ctx) {
        const auto v = n.as_integer();
        if (!v) {
            ctx.type_error(n, expected());
            return false;
        }
        if (!std::in_range<T>(*v)) {
            ctx.error(n.loc(), std::format("value {} is out of range; must be between {} and {}", *v,
                                           std::numeric_limits<T>::min(),
                                           std::numeric_limits<T>::max()));
            return false;
        }
        out = static_cast<T>(*v);
        return true;
    }
};

template <std::floating_point T>
struct ValueCodec<T> {
    static std::string expected() { return "number"; }
    static bool read(DataNode n, T& out, ReadContext& ctx) {
        const auto v = n.as_floating();
        if (!v) {
            ctx.type_error(n, expected());
            return false;
        }
        // TOML allows inf/nan; they are never meaningful in content data and poison comparisons.
        if (!(*v >= -std::numeric_limits<double>::max() && *v <= std::numeric_limits<double>::max())) {
            ctx.error(n.loc(), std::format("expected a finite number, got {}", *v));
            return false;
        }
        out = static_cast<T>(*v);
        return true;
    }
};

template <>
struct ValueCodec<std::string> {
    static std::string expected() { return "string"; }
    static bool read(DataNode n, std::string& out, ReadContext& ctx) {
        if (auto v = n.as_string()) {
            out.assign(*v);
            return true;
        }
        ctx.type_error(n, expected());
        return false;
    }
};

template <NamedEnum E>
struct ValueCodec<E> {
    static std::string expected() {
        std::string s = "one of ";
        bool first = true;
        for (const auto& [name, value] : enum_names(E{})) {
            s += first ? "" : ", ";
            s += '\'';
            s += name;
            s += '\'';
            first = false;
        }
        return s;
    }
    static bool read(DataNode n, E& out, ReadContext& ctx) {
        const auto v = n.as_string();
        if (!v) {
            ctx.type_error(n, "string (" + expected() + ")");
            return false;
        }
        std::vector<std::string_view> names;
        for (const auto& [name, value] : enum_names(E{})) {
            if (name == *v) {
                out = value;
                return true;
            }
            names.push_back(name);
        }
        ctx.error(n.loc(), std::format("unknown value '{}'; expected {}{}", *v, expected(),
                                       did_you_mean(*v, names)));
        return false;
    }
};

// Reference to another definition, written in TOML as its key.
template <typename Row>
struct ValueCodec<Handle<Row>> {
    static std::string expected() { return "string (definition key)"; }
    static bool read(DataNode n, Handle<Row>& out, ReadContext& ctx) {
        const auto v = n.as_string();
        if (!v) {
            ctx.type_error(n, expected());
            return false;
        }
        Handle<Row>* target = &out; // stable: rows and their arrays are not moved until resolved
        ctx.defer_ref(std::type_index(typeid(Row)), std::string(*v), n.loc(),
                      [target](std::uint32_t index, std::uint32_t generation) {
                          target->index = index;
                          target->generation = generation;
                      });
        return true;
    }
};

template <Readable T>
struct ValueCodec<std::vector<T>> {
    static_assert(!std::same_as<T, bool>, "std::vector<bool> is not supported; use std::vector<char>");

    static std::string expected() { return "array of " + ValueCodec<T>::expected(); }
    static bool read(DataNode n, std::vector<T>& out, ReadContext& ctx) {
        if (!n.is_array()) {
            ctx.type_error(n, expected());
            return false;
        }
        // Size once, then fill in place: deferred references point into these elements.
        out.clear();
        out.resize(n.size());
        bool ok = true;
        for (std::size_t i = 0; i < out.size(); ++i) {
            auto scope = ctx.index(i);
            ok = ValueCodec<T>::read(n.at(i), out[i], ctx) && ok;
        }
        return ok;
    }
};

template <Describable T>
    requires(!std::is_enum_v<T>)
struct ValueCodec<T> {
    static std::string expected() { return "table"; }
    static bool read(DataNode n, T& out, ReadContext& ctx);
};

// ---------------------------------------------------------------------------------------------
// Schema: a list of field descriptors for one struct.

namespace detail {

template <typename T>
struct unwrap_optional {
    using type = T;
    static constexpr bool is_optional = false;
};
template <typename T>
struct unwrap_optional<std::optional<T>> {
    using type = T;
    static constexpr bool is_optional = true;
};

template <typename Row>
class FieldBase {
public:
    FieldBase(std::string key, bool required) : key_(std::move(key)), required_(required) {}
    virtual ~FieldBase() = default;
    FieldBase(const FieldBase&) = delete;
    FieldBase& operator=(const FieldBase&) = delete;

    const std::string& key() const { return key_; }
    bool required() const { return required_; }

    virtual std::string expected() const = 0;
    virtual void read(DataNode value, Row& row, ReadContext& ctx) const = 0;
    virtual void apply_default(Row& row) const = 0;

protected:
    std::string key_;
    bool required_;
};

} // namespace detail

template <typename Row, typename T>
class FieldSpec final : public detail::FieldBase<Row> {
public:
    // For std::optional<U> members, validators and defaults work on U.
    using value_type = typename detail::unwrap_optional<T>::type;
    using Validator = std::function<std::string(const value_type&)>; // "" = valid

    static_assert(Readable<value_type>, "no sim::ValueCodec for this field type");

    FieldSpec(std::string key, T Row::*member, bool required)
        : detail::FieldBase<Row>(std::move(key), required), member_(member) {}

    // Missing is fine; the row keeps its default member value.
    FieldSpec& optional() {
        this->required_ = false;
        return *this;
    }
    // Missing is fine; the field is set to `value`.
    FieldSpec& default_value(value_type value) {
        this->required_ = false;
        default_ = std::move(value);
        return *this;
    }

    FieldSpec& check(Validator v) {
        validators_.push_back(std::move(v));
        return *this;
    }

    FieldSpec& min(value_type lo)
        requires std::is_arithmetic_v<value_type>
    {
        return check([lo](const value_type& v) {
            return v < lo ? std::format("must be at least {}, got {}", +lo, +v) : std::string{};
        });
    }
    FieldSpec& max(value_type hi)
        requires std::is_arithmetic_v<value_type>
    {
        return check([hi](const value_type& v) {
            return v > hi ? std::format("must be at most {}, got {}", +hi, +v) : std::string{};
        });
    }
    FieldSpec& range(value_type lo, value_type hi)
        requires std::is_arithmetic_v<value_type>
    {
        return check([lo, hi](const value_type& v) {
            return (v < lo || v > hi) ? std::format("must be between {} and {}, got {}", +lo, +hi, +v)
                                      : std::string{};
        });
    }
    FieldSpec& non_empty()
        requires requires(const value_type& v) { v.empty(); }
    {
        return check([](const value_type& v) { return v.empty() ? "must not be empty" : std::string{}; });
    }

    std::string expected() const override { return ValueCodec<value_type>::expected(); }

    void read(DataNode value, Row& row, ReadContext& ctx) const override {
        value_type* target = nullptr;
        if constexpr (detail::unwrap_optional<T>::is_optional) {
            target = &(row.*member_).emplace();
        } else {
            target = &(row.*member_);
        }
        if (!ValueCodec<value_type>::read(value, *target, ctx)) {
            return;
        }
        for (const auto& v : validators_) {
            if (std::string msg = v(*target); !msg.empty()) {
                ctx.error(value.loc(), msg);
            }
        }
    }

    void apply_default(Row& row) const override {
        if (default_) {
            row.*member_ = *default_;
        }
    }

private:
    T Row::*member_;
    std::optional<value_type> default_;
    std::vector<Validator> validators_;
};

template <typename Row>
class Schema {
public:
    // Row-level validator for cross-field rules; runs only if every field read cleanly.
    // References are not yet resolved when it runs.
    using RowCheck = std::function<std::string(const Row&)>; // "" = valid

    // A required field (std::optional<U> members are implicitly optional).
    template <typename T>
    FieldSpec<Row, T>& field(std::string key, T Row::*member) {
        return add<T>(std::move(key), member, !detail::unwrap_optional<T>::is_optional);
    }
    // An optional field that keeps the row's default member value when absent.
    template <typename T>
    FieldSpec<Row, T>& optional(std::string key, T Row::*member) {
        return add<T>(std::move(key), member, false);
    }
    // An optional field set to `fallback` when absent.
    template <typename T, typename D>
    FieldSpec<Row, T>& optional(std::string key, T Row::*member, D&& fallback) {
        using V = typename FieldSpec<Row, T>::value_type;
        return add<T>(std::move(key), member, false).default_value(V(std::forward<D>(fallback)));
    }

    Schema& check(RowCheck fn) {
        checks_.push_back(std::move(fn));
        return *this;
    }

    std::vector<std::string_view> keys() const {
        std::vector<std::string_view> out;
        out.reserve(fields_.size());
        for (const auto& f : fields_) {
            out.emplace_back(f->key());
        }
        return out;
    }

    // Reads `table` into `row`: every declared field, then rejects undeclared keys (typos),
    // then row-level checks. Reports all problems to ctx; returns true if there were none.
    bool read(DataNode table, Row& row, ReadContext& ctx) const {
        const std::size_t errors_before = ctx.error_count();
        if (!table.is_table()) {
            ctx.type_error(table, "table");
            return false;
        }
        for (const auto& f : fields_) {
            DataNode value = table.get(f->key());
            if (!value) {
                if (f->required()) {
                    ctx.error_in_subject(table.loc(),
                                         std::format("missing required field '{}' ({})",
                                                     ctx.path_with(f->key()), f->expected()));
                } else {
                    f->apply_default(row);
                }
                continue;
            }
            auto scope = ctx.field(f->key());
            f->read(value, row, ctx);
        }
        const std::vector<std::string_view> known = keys();
        table.for_each([&](std::string_view key, const SourceLoc& key_loc, DataNode) {
            if (std::find(known.begin(), known.end(), key) == known.end()) {
                ctx.error_in_subject(key_loc, std::format("unknown field '{}'{}", ctx.path_with(key),
                                                          did_you_mean(key, known)));
            }
        });
        if (ctx.error_count() == errors_before) {
            for (const auto& c : checks_) {
                if (std::string msg = c(row); !msg.empty()) {
                    ctx.error(table.loc(), msg);
                }
            }
        }
        return ctx.error_count() == errors_before;
    }

private:
    template <typename T>
    FieldSpec<Row, T>& add(std::string key, T Row::*member, bool required) {
        for (const auto& f : fields_) {
            if (f->key() == key) {
                throw std::logic_error("sim::Schema: field '" + key + "' declared twice");
            }
        }
        auto spec = std::make_unique<FieldSpec<Row, T>>(std::move(key), member, required);
        FieldSpec<Row, T>& ref = *spec;
        fields_.push_back(std::move(spec));
        return ref;
    }

    std::vector<std::unique_ptr<detail::FieldBase<Row>>> fields_;
    std::vector<RowCheck> checks_;
};

// The schema of a Describable type, built once on first use.
template <typename T>
const Schema<T>& schema_of() {
    static const Schema<T> schema = [] {
        Schema<T> s;
        T::describe(s);
        return s;
    }();
    return schema;
}

template <Describable T>
    requires(!std::is_enum_v<T>)
bool ValueCodec<T>::read(DataNode n, T& out, ReadContext& ctx) {
    return schema_of<T>().read(n, out, ctx);
}

// ---------------------------------------------------------------------------------------------
// Definition tables and the registry.

namespace detail {
template <typename Row>
class DefType;
}

// Read-only table of one definition type. Ids are dense and assigned in key order, so
// `for (auto [id, row] : table)` visits definitions sorted by key.
template <typename Row>
class DefTable {
public:
    using id_type = DefId<Row>;

    std::size_t size() const { return rows_.size(); }
    bool empty() const { return rows_.empty(); }

    // Null id if no definition has this key. O(log n).
    id_type find(std::string_view key) const {
        auto it = std::lower_bound(keys_.begin(), keys_.end(), key);
        if (it == keys_.end() || *it != key) {
            return id_type::null();
        }
        return rows_.handle_at(static_cast<std::size_t>(it - keys_.begin()));
    }

    bool contains(id_type id) const { return rows_.contains(id); }
    const Row* get(id_type id) const { return rows_.get(id); }
    const Row& at(id_type id) const { return rows_.at(id); }
    const Row& operator[](id_type id) const { return rows_.at(id); }

    // Requires a valid id (throws std::out_of_range otherwise).
    const std::string& key(id_type id) const { return keys_[position(id)]; }
    const SourceLoc& source(id_type id) const { return sources_[position(id)]; }

    std::span<const std::string> keys() const { return keys_; } // sorted
    const Table<Row, Row>& table() const { return rows_; }

    auto begin() const { return rows_.begin(); }
    auto end() const { return rows_.end(); }

private:
    friend class detail::DefType<Row>;

    std::size_t position(id_type id) const {
        if (!rows_.contains(id)) {
            throw std::out_of_range("sim::DefTable: stale or null id");
        }
        return id.index; // rows are never erased, so slot index == dense index == key position
    }

    Table<Row, Row> rows_;
    std::vector<std::string> keys_;
    std::vector<SourceLoc> sources_;
};

namespace detail {

class DefTypeBase {
public:
    DefTypeBase(std::string section, std::type_index type) : section_(std::move(section)), type_(type) {}
    virtual ~DefTypeBase() = default;
    DefTypeBase(const DefTypeBase&) = delete;
    DefTypeBase& operator=(const DefTypeBase&) = delete;

    const std::string& section() const { return section_; }
    std::type_index type() const { return type_; }

    // Load protocol, driven by DefRegistry::load:
    //   begin -> stage* -> assign_ids -> (resolve refs via find/keys) -> commit | discard
    virtual void begin() = 0;
    virtual void stage(std::string key, SourceLoc where, DataNode table, ReadContext& ctx) = 0;
    virtual void assign_ids() = 0;
    virtual std::optional<std::pair<std::uint32_t, std::uint32_t>> find_staged(std::string_view key) const = 0;
    virtual std::span<const std::string> staged_keys() const = 0;
    virtual void commit() = 0;
    virtual void discard() = 0;

private:
    std::string section_;
    std::type_index type_;
};

template <typename Row>
class DefType final : public DefTypeBase {
public:
    explicit DefType(std::string section) : DefTypeBase(std::move(section), std::type_index(typeid(Row))) {}

    DefTable<Row>& table() { return table_; }

    void begin() override { discard(); }

    void stage(std::string key, SourceLoc where, DataNode node, ReadContext& ctx) override {
        // Heap-allocated so deferred reference targets inside the row stay put while staging grows.
        auto row = std::make_unique<Row>();
        schema_of<Row>().read(node, *row, ctx);
        staged_.push_back({std::move(key), std::move(where), std::move(row)});
    }

    void assign_ids() override {
        std::sort(staged_.begin(), staged_.end(),
                  [](const Staged& a, const Staged& b) { return a.key < b.key; });
        // Placeholder rows give each key its final id before references are resolved; the parsed
        // rows are moved in on commit.
        next_ = DefTable<Row>{};
        next_.rows_.reserve(staged_.size());
        for (const Staged& s : staged_) {
            next_.rows_.emplace();
            next_.keys_.push_back(s.key);
            next_.sources_.push_back(s.where);
        }
    }

    std::optional<std::pair<std::uint32_t, std::uint32_t>> find_staged(std::string_view key) const override {
        const DefId<Row> id = next_.find(key);
        if (id.is_null()) {
            return std::nullopt;
        }
        return std::pair{id.index, id.generation};
    }

    std::span<const std::string> staged_keys() const override { return next_.keys_; }

    void commit() override {
        for (std::size_t i = 0; i < staged_.size(); ++i) {
            next_.rows_.rows()[i] = std::move(*staged_[i].row);
        }
        table_ = std::move(next_);
        discard();
    }

    void discard() override {
        staged_.clear();
        next_ = DefTable<Row>{};
    }

private:
    struct Staged {
        std::string key;
        SourceLoc where;
        std::unique_ptr<Row> row;
    };

    DefTable<Row> table_;
    DefTable<Row> next_;
    std::vector<Staged> staged_;
};

} // namespace detail

// One in-memory data file.
struct DataSource {
    std::string name; // used in diagnostics, typically the path
    std::string text;
};

// Reads every *.toml file under `dir` (recursively), sorted by relative path so load order is
// deterministic across platforms. I/O problems are reported to `diags`.
std::vector<DataSource> read_data_directory(const std::filesystem::path& dir, Diagnostics& diags);

class DefRegistry {
public:
    // Registers a definition type under a TOML section name (lowercase snake_case). Throws
    // std::logic_error on a duplicate type or section.
    template <Describable Row>
    DefTable<Row>& define(std::string section) {
        check_new_type(section, std::type_index(typeid(Row)));
        (void)schema_of<Row>(); // build now so schema errors surface at startup, not mid-load
        auto type = std::make_unique<detail::DefType<Row>>(std::move(section));
        DefTable<Row>& table = type->table();
        types_.push_back(std::move(type));
        return table;
    }

    // Throws std::logic_error if Row was never defined.
    template <typename Row>
    const DefTable<Row>& get() const {
        return static_cast<detail::DefType<Row>&>(find_type(std::type_index(typeid(Row)))).table();
    }

    template <typename Row>
    DefId<Row> find(std::string_view key) const {
        return get<Row>().find(key);
    }

    // Replaces all definitions with the contents of `sources` (read in the given order). On any
    // error nothing changes and the returned diagnostics say why.
    Diagnostics load(std::span<const DataSource> sources);
    Diagnostics load_directory(const std::filesystem::path& dir);

private:
    void check_new_type(const std::string& section, std::type_index type) const;
    detail::DefTypeBase& find_type(std::type_index type) const;

    std::vector<std::unique_ptr<detail::DefTypeBase>> types_; // registration order
};

} // namespace sim
