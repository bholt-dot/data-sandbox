#pragma once

// Stat/modifier pipeline: derived numbers (max acceleration, cargo capacity, a price multiplier...)
// computed as a base value plus modifiers from many sources, cached and recomputed only when an
// input changed.
//
// Stacking formula (the same for every stat; games pick the op per modifier):
//
//     raw   = (base + Σ add) × (1 + Σ increased) × Π (1 + more)
//     value = min(max(raw, max(floor...)), min(cap...))
//
//   add        flat amount                       (+50 t cargo)
//   increased  additive percentage, 0.10 = +10%  (Paradox "+10% X", PoE "increased", GAS bias-1 mul)
//   more       compounding factor, 0.10 = ×1.10  (PoE "more"; each one multiplies separately)
//   floor/cap  clamps; the highest floor and the lowest cap apply; if they conflict, the cap wins.
//
// There is no implicit clamp: Σ increased below -1 turns the value negative. Add a floor modifier
// (e.g. on the global scope) if a stat must stay non-negative.
//
// Order independence: each group of values is sorted before it is summed (or multiplied), so the
// result is a pure function of the *multiset* of contributing values. Two histories that end with
// the same modifiers produce bit-identical totals, even though floating-point addition is not
// associative and table/dense order differs between them. Values must be finite; -0.0 is stored
// as +0.0 so the sort is a strict total order on bit patterns.
//
// Base: the target's own base if set, else the global scope's base (a per-stat default), else 0.
//
// Scopes: a modifier's target is any EntityKey. Every target implicitly sees modifiers on the
// global scope, plus modifiers on each scope listed by set_scopes() (e.g. a ship docked at a
// station lists the station). Scopes are flat, not nested: a station's own scopes do not
// propagate to ships that list the station.
//
// Caching and cost model (B = number of (target, stat) buckets, S = scopes of a target,
// k = modifiers contributing to one (target, stat)):
//   * Every mutation takes a new epoch number and stamps it on the one bucket it touches
//     (target, stat) — or on the target's scope membership. That is O(log B) no matter how many
//     entities see that bucket, so a global or station-wide modifier never dirties N entries.
//   * get() validates a cached value by checking that none of the buckets it read (own, global,
//     each scope) nor the membership changed after it was computed: O((S + 2) log B) per hit.
//     A miss recomputes in O(k log k). Validation is pull-based, so nothing is recomputed until
//     somebody asks.
//   * forget_target() drops the entire cache (rare: despawns).
//
// Expiry: a modifier with `expires = T` applies while now < T. It is removed by expire_until(now)
// for any now >= T, in (expires, handle) order. get() does not look at the clock, so callers
// either drive expire_until() from a periodic system or schedule an event at next_expiry().
//
// Snapshots: the authoritative state is the modifier table (including its handle allocation
// state, so ids allocated after a load match an uninterrupted run), the bases and the scope
// lists; snapshot() captures exactly that, and restore() validates it and rebuilds everything
// else (per-bucket modifier lists, source and expiry indexes, change epochs; the cache starts
// empty). Nothing derived is saved, so it cannot drift from the data it was computed from.
// Derived order may differ after a load (a bucket's modifier list follows the table's dense
// order rather than insertion/removal history), which is unobservable: totals are sorted sums,
// breakdowns are sorted, and removal/expiry walk ordered indexes.
//
// The state hash (hash_state, via Codec<StatPipeline> in snapshot.hpp) covers the exact
// authoritative state, not just the logical modifier multiset: two pipelines with the same
// modifiers built through different histories can hash differently (different handles, dense
// order or free list), just as Table hashes do. That matches the project rule "same seed + same
// commands => same hash" and catches divergence in handle allocation, which would change future
// ModifierIds. Bases and scope lists are encoded canonically (sorted; empty scope lists omitted).

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "simcore/table.hpp"
#include "simcore/time.hpp"

namespace sim {

// Games define their own stat list, e.g. `inline constexpr StatId max_accel{0};`.
struct StatId {
    std::uint32_t value = 0;
    constexpr auto operator<=>(const StatId&) const = default;
    static constexpr auto fields(auto& self) { return std::tie(self.value); }
};

// Identifies a modifier target, scope or source. `kind` namespaces different tables (handles of
// different tables can share index/generation); kind 0 with index 0 is the global scope.
struct EntityKey {
    std::uint32_t kind = 0;
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    constexpr auto operator<=>(const EntityKey&) const = default;
    static constexpr auto fields(auto& self) {
        return std::tie(self.kind, self.index, self.generation);
    }
};

inline constexpr EntityKey global_scope{};

template <typename Tag>
constexpr EntityKey entity_key(std::uint32_t kind, Handle<Tag> h) {
    return {kind, h.index, h.generation};
}

enum class ModOp : std::uint8_t { add, increased, more, floor, cap };

inline constexpr Time never{std::numeric_limits<std::int64_t>::max()};

struct Modifier {
    EntityKey source;
    EntityKey target;
    StatId stat;
    ModOp op = ModOp::add;
    double value = 0.0;
    Time expires = never;

    bool operator==(const Modifier&) const = default;
    static auto fields(auto& self) {
        return std::tie(self.source, self.target, self.stat, self.op, self.value, self.expires);
    }
};

struct ModifierTag;
using ModifierId = Handle<ModifierTag>;
using ModifierTable = Table<ModifierTag, Modifier>;

// One (target, stat) pair.
struct StatKey {
    EntityKey target;
    StatId stat;
    constexpr auto operator<=>(const StatKey&) const = default;
    static constexpr auto fields(auto& self) { return std::tie(self.target, self.stat); }
};

// Authoritative StatPipeline state, as captured by snapshot() and consumed by restore().
struct StatPipelineState {
    ModifierTable modifiers;
    std::map<StatKey, double> bases;                      // includes global defaults
    std::map<EntityKey, std::vector<EntityKey>> scopes;   // non-empty lists as set_scopes stores them

    static auto fields(auto& self) { return std::tie(self.modifiers, self.bases, self.scopes); }
};

struct StatContribution {
    ModifierId id;
    Modifier modifier; // modifier.target says whether it came from the target, a scope or global
};

struct StatBreakdown {
    double base = 0.0;
    bool base_from_global = false;
    double add = 0.0;        // Σ add
    double increased = 0.0;  // Σ increased
    double more = 1.0;       // Π (1 + more)
    std::optional<double> floor;
    std::optional<double> cap;
    double value = 0.0;
    // Sorted by (op, source, target, id): deterministic for a given history.
    std::vector<StatContribution> contributions;
};

class StatPipeline {
public:
    // Throws std::invalid_argument for a non-finite value.
    ModifierId add(const Modifier& m);
    bool remove(ModifierId id);
    // Changes a modifier's magnitude in place (e.g. wear); false for stale ids.
    bool set_value(ModifierId id, double value);
    // Removes every modifier from `source` (unequipping a component, ending a policy).
    std::size_t remove_source(EntityKey source, std::vector<Modifier>* removed = nullptr);
    // Removes every modifier with expires <= now, in (expires, id) order.
    std::size_t expire_until(Time now, std::vector<Modifier>* expired = nullptr);
    std::optional<Time> next_expiry() const;

    void set_base(EntityKey target, StatId stat, double value);
    void clear_base(EntityKey target, StatId stat);

    // Replaces the scopes `target` inherits from (global is always implicit). Order and
    // duplicates don't matter.
    void set_scopes(EntityKey target, std::span<const EntityKey> scopes);
    std::span<const EntityKey> scopes(EntityKey target) const;

    // Despawn: removes the target's modifiers, bases and scope list, and modifiers it is the
    // source of. Clears the whole cache.
    void forget_target(EntityKey target);

    // Cached total; recomputes only if an input changed since the last call.
    double get(EntityKey target, StatId stat);
    // Uncached; bit-identical to get().
    double evaluate(EntityKey target, StatId stat) const;
    StatBreakdown breakdown(EntityKey target, StatId stat) const;

    const Modifier* find(ModifierId id) const { return modifiers_.get(id); }
    const ModifierTable& modifiers() const { return modifiers_; }

    StatPipelineState snapshot() const;
    // Replaces all state with `st` and rebuilds the derived indexes; the cache starts empty and
    // recompute_count() restarts at 0.
    // Throws std::invalid_argument if `st` is inconsistent (the pipeline is then unchanged):
    // non-finite or -0.0 values, unknown ops, or scope lists that are empty, unsorted, contain
    // duplicates, the global scope or the target itself.
    void restore(StatPipelineState st);

    void invalidate_cache() { cache_.clear(); }
    // Diagnostics: number of cache misses that recomputed a total.
    std::uint64_t recompute_count() const { return recomputes_; }

private:
    using BucketKey = StatKey;
    struct BucketKeyHash {
        std::size_t operator()(const BucketKey& k) const noexcept;
    };
    struct Bucket {
        std::optional<double> base;
        std::vector<ModifierId> mods;
        std::uint64_t changed = 0;
    };
    struct ScopeList {
        std::vector<EntityKey> scopes; // sorted, unique, no global, no self
        std::uint64_t changed = 0;
    };
    struct CacheEntry {
        double value = 0.0;
        std::uint64_t computed = 0;
    };

    Bucket& touch(BucketKey key);
    const Bucket* bucket(BucketKey key) const;
    void rebuild_indices();
    void erase_indexed(ModifierId id, const Modifier& m);
    std::uint64_t last_change(EntityKey target, StatId stat) const;
    double compute(EntityKey target, StatId stat, StatBreakdown* out) const;

    ModifierTable modifiers_;
    std::map<BucketKey, Bucket> buckets_;
    std::map<EntityKey, ScopeList> scopes_;
    std::set<std::pair<EntityKey, ModifierId>> by_source_;
    std::set<std::pair<Time, ModifierId>> by_expiry_;
    std::unordered_map<BucketKey, CacheEntry, BucketKeyHash> cache_; // never iterated
    std::uint64_t epoch_ = 0;
    std::uint64_t recomputes_ = 0;
};

} // namespace sim
