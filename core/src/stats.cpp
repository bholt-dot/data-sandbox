#include "simcore/stats.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <tuple>

namespace sim {

namespace {

double canonical(double v) {
    if (!std::isfinite(v)) {
        throw std::invalid_argument("sim::StatPipeline: modifier value must be finite");
    }
    return v == 0.0 ? 0.0 : v; // -0.0 -> +0.0
}

// Sorting first makes the result depend only on the multiset of values, not on the order the
// buckets or modifiers were visited.
double sorted_sum(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    double s = 0.0;
    for (double x : v) {
        s += x;
    }
    return s;
}

double sorted_product_of_one_plus(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    double p = 1.0;
    for (double x : v) {
        p *= 1.0 + x;
    }
    return p;
}

} // namespace

std::size_t StatPipeline::BucketKeyHash::operator()(const BucketKey& k) const noexcept {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::uint32_t part : {k.target.kind, k.target.index, k.target.generation, k.stat.value}) {
        h = (h ^ part) * 0x100000001b3ull;
    }
    return static_cast<std::size_t>(h);
}

StatPipeline::Bucket& StatPipeline::touch(BucketKey key) {
    Bucket& b = buckets_[key];
    b.changed = ++epoch_;
    return b;
}

const StatPipeline::Bucket* StatPipeline::bucket(BucketKey key) const {
    auto it = buckets_.find(key);
    return it == buckets_.end() ? nullptr : &it->second;
}

ModifierId StatPipeline::add(const Modifier& m) {
    Modifier row = m;
    row.value = canonical(m.value);
    const ModifierId id = modifiers_.insert(row);
    touch({row.target, row.stat}).mods.push_back(id);
    by_source_.emplace(row.source, id);
    if (row.expires != never) {
        by_expiry_.emplace(row.expires, id);
    }
    return id;
}

void StatPipeline::erase_indexed(ModifierId id, const Modifier& m) {
    auto& mods = touch({m.target, m.stat}).mods;
    auto it = std::find(mods.begin(), mods.end(), id);
    if (it != mods.end()) {
        *it = mods.back();
        mods.pop_back();
    }
    by_source_.erase({m.source, id});
    if (m.expires != never) {
        by_expiry_.erase({m.expires, id});
    }
}

bool StatPipeline::remove(ModifierId id) {
    const Modifier* m = modifiers_.get(id);
    if (m == nullptr) {
        return false;
    }
    erase_indexed(id, *m);
    modifiers_.erase(id);
    return true;
}

bool StatPipeline::set_value(ModifierId id, double value) {
    Modifier* m = modifiers_.get(id);
    if (m == nullptr) {
        return false;
    }
    const double v = canonical(value);
    if (v != m->value) {
        m->value = v;
        touch({m->target, m->stat});
    }
    return true;
}

std::size_t StatPipeline::remove_source(EntityKey source, std::vector<Modifier>* removed) {
    std::vector<ModifierId> ids;
    for (auto it = by_source_.lower_bound({source, ModifierId{0, 0}});
         it != by_source_.end() && it->first == source; ++it) {
        ids.push_back(it->second);
    }
    for (ModifierId id : ids) {
        if (removed != nullptr) {
            removed->push_back(modifiers_.at(id));
        }
        remove(id);
    }
    return ids.size();
}

std::size_t StatPipeline::expire_until(Time now, std::vector<Modifier>* expired) {
    std::size_t n = 0;
    while (!by_expiry_.empty() && by_expiry_.begin()->first <= now) {
        const ModifierId id = by_expiry_.begin()->second;
        if (expired != nullptr) {
            expired->push_back(modifiers_.at(id));
        }
        remove(id);
        ++n;
    }
    return n;
}

std::optional<Time> StatPipeline::next_expiry() const {
    if (by_expiry_.empty()) {
        return std::nullopt;
    }
    return by_expiry_.begin()->first;
}

void StatPipeline::set_base(EntityKey target, StatId stat, double value) {
    touch({target, stat}).base = canonical(value);
}

void StatPipeline::clear_base(EntityKey target, StatId stat) {
    auto it = buckets_.find({target, stat});
    if (it != buckets_.end() && it->second.base) {
        touch({target, stat}).base.reset();
    }
}

void StatPipeline::set_scopes(EntityKey target, std::span<const EntityKey> scopes) {
    std::vector<EntityKey> list;
    for (const EntityKey& s : scopes) {
        if (s != global_scope && s != target) {
            list.push_back(s);
        }
    }
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    ScopeList& entry = scopes_[target];
    if (entry.changed == 0 || entry.scopes != list) {
        entry.scopes = std::move(list);
        entry.changed = ++epoch_;
    }
}

std::span<const EntityKey> StatPipeline::scopes(EntityKey target) const {
    auto it = scopes_.find(target);
    if (it == scopes_.end()) {
        return {};
    }
    return it->second.scopes;
}

void StatPipeline::forget_target(EntityKey target) {
    std::vector<ModifierId> ids;
    for (auto [id, m] : modifiers_) {
        if (m.target == target) {
            ids.push_back(id);
        }
    }
    for (ModifierId id : ids) {
        remove(id);
    }
    remove_source(target);
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        it = it->first.target == target ? buckets_.erase(it) : std::next(it);
    }
    scopes_.erase(target);
    // Buckets carried the change epochs other targets' caches validate against; once they are
    // gone a stale cache could look valid, so drop the cache wholesale.
    cache_.clear();
    ++epoch_;
}

StatPipelineState StatPipeline::snapshot() const {
    StatPipelineState st;
    st.modifiers = modifiers_;
    for (const auto& [key, b] : buckets_) {
        if (b.base) {
            st.bases.emplace_hint(st.bases.end(), key, *b.base);
        }
    }
    for (const auto& [target, list] : scopes_) {
        if (!list.scopes.empty()) {
            st.scopes.emplace_hint(st.scopes.end(), target, list.scopes);
        }
    }
    return st;
}

void StatPipeline::restore(StatPipelineState st) {
    auto invalid = [](const char* what) {
        throw std::invalid_argument(std::string("sim::StatPipeline::restore: ") + what);
    };
    // Exactly the values canonical() lets in, so restored data sorts and hashes like live data.
    auto is_canonical = [](double v) { return std::isfinite(v) && !(v == 0.0 && std::signbit(v)); };
    for (const Modifier& m : st.modifiers.rows()) {
        if (!is_canonical(m.value)) {
            invalid("modifier value is not finite or is -0.0");
        }
        if (static_cast<std::uint8_t>(m.op) > static_cast<std::uint8_t>(ModOp::cap)) {
            invalid("unknown modifier op");
        }
    }
    for (const auto& [key, v] : st.bases) {
        if (!is_canonical(v)) {
            invalid("base value is not finite or is -0.0");
        }
    }
    for (const auto& [target, list] : st.scopes) {
        if (list.empty()) {
            invalid("empty scope list");
        }
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i] == global_scope || list[i] == target) {
                invalid("scope list contains the global scope or its own target");
            }
            if (i > 0 && !(list[i - 1] < list[i])) {
                invalid("scope list not strictly ascending");
            }
        }
    }

    // Built aside and moved in, so a failure (e.g. bad_alloc) leaves *this unchanged.
    StatPipeline fresh;
    fresh.modifiers_ = std::move(st.modifiers);
    fresh.epoch_ = 1;
    for (auto& [key, v] : st.bases) {
        fresh.buckets_.emplace_hint(fresh.buckets_.end(), key, Bucket{v, {}, fresh.epoch_});
    }
    for (auto& [target, list] : st.scopes) {
        fresh.scopes_.emplace_hint(fresh.scopes_.end(), target,
                                   ScopeList{std::move(list), fresh.epoch_});
    }
    fresh.rebuild_indices();
    *this = std::move(fresh);
}

void StatPipeline::rebuild_indices() {
    for (auto& [key, b] : buckets_) {
        b.mods.clear();
    }
    by_source_.clear();
    by_expiry_.clear();
    cache_.clear();
    for (auto [id, m] : modifiers_) {
        Bucket& b = buckets_[{m.target, m.stat}];
        b.mods.push_back(id);
        b.changed = std::max(b.changed, epoch_);
        by_source_.emplace(m.source, id);
        if (m.expires != never) {
            by_expiry_.emplace(m.expires, id);
        }
    }
}

std::uint64_t StatPipeline::last_change(EntityKey target, StatId stat) const {
    std::uint64_t latest = 0;
    auto note = [&](EntityKey key) {
        if (const Bucket* b = bucket({key, stat})) {
            latest = std::max(latest, b->changed);
        }
    };
    note(target);
    if (target != global_scope) {
        note(global_scope);
    }
    if (auto it = scopes_.find(target); it != scopes_.end()) {
        latest = std::max(latest, it->second.changed);
        for (const EntityKey& s : it->second.scopes) {
            note(s);
        }
    }
    return latest;
}

double StatPipeline::get(EntityKey target, StatId stat) {
    const BucketKey key{target, stat};
    auto it = cache_.find(key);
    if (it != cache_.end() && last_change(target, stat) <= it->second.computed) {
        return it->second.value;
    }
    const double v = compute(target, stat, nullptr);
    ++recomputes_;
    cache_[key] = CacheEntry{v, epoch_};
    return v;
}

double StatPipeline::evaluate(EntityKey target, StatId stat) const {
    return compute(target, stat, nullptr);
}

StatBreakdown StatPipeline::breakdown(EntityKey target, StatId stat) const {
    StatBreakdown out;
    compute(target, stat, &out);
    return out;
}

double StatPipeline::compute(EntityKey target, StatId stat, StatBreakdown* out) const {
    std::vector<double> adds;
    std::vector<double> increases;
    std::vector<double> mores;
    std::optional<double> floor;
    std::optional<double> cap;

    auto gather = [&](EntityKey key) {
        const Bucket* b = bucket({key, stat});
        if (b == nullptr) {
            return;
        }
        for (ModifierId id : b->mods) {
            const Modifier& m = modifiers_.at(id);
            switch (m.op) {
            case ModOp::add: adds.push_back(m.value); break;
            case ModOp::increased: increases.push_back(m.value); break;
            case ModOp::more: mores.push_back(m.value); break;
            case ModOp::floor: floor = floor ? std::max(*floor, m.value) : m.value; break;
            case ModOp::cap: cap = cap ? std::min(*cap, m.value) : m.value; break;
            }
            if (out != nullptr) {
                out->contributions.push_back({id, m});
            }
        }
    };

    gather(target);
    if (target != global_scope) {
        gather(global_scope);
    }
    for (const EntityKey& s : scopes(target)) {
        gather(s);
    }

    double base = 0.0;
    bool base_from_global = false;
    if (const Bucket* own = bucket({target, stat}); own != nullptr && own->base) {
        base = *own->base;
    } else if (const Bucket* g = bucket({global_scope, stat}); g != nullptr && g->base) {
        base = *g->base;
        base_from_global = target != global_scope;
    }

    const double add = sorted_sum(adds);
    const double increased = sorted_sum(increases);
    const double more = sorted_product_of_one_plus(mores);
    double value = (base + add) * (1.0 + increased) * more;
    if (floor) {
        value = std::max(value, *floor);
    }
    if (cap) {
        value = std::min(value, *cap);
    }

    if (out != nullptr) {
        out->base = base;
        out->base_from_global = base_from_global;
        out->add = add;
        out->increased = increased;
        out->more = more;
        out->floor = floor;
        out->cap = cap;
        out->value = value;
        std::sort(out->contributions.begin(), out->contributions.end(),
                  [](const StatContribution& a, const StatContribution& b) {
                      return std::tie(a.modifier.op, a.modifier.source, a.modifier.target, a.id) <
                             std::tie(b.modifier.op, b.modifier.source, b.modifier.target, b.id);
                  });
    }
    return value;
}

} // namespace sim
