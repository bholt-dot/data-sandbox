#include <doctest/doctest.h>

#include "simcore/stats.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "simcore/rng.hpp"

using namespace sim;

namespace {

constexpr StatId accel{0};
constexpr StatId cargo{1};

constexpr std::uint32_t ship_kind = 1;
constexpr std::uint32_t station_kind = 2;
constexpr std::uint32_t part_kind = 3;

constexpr EntityKey ship(std::uint32_t i) { return {ship_kind, i, 1}; }
constexpr EntityKey station(std::uint32_t i) { return {station_kind, i, 1}; }
constexpr EntityKey part(std::uint32_t i) { return {part_kind, i, 1}; }

Modifier mod(EntityKey source, EntityKey target, StatId stat, ModOp op, double value,
             Time expires = never) {
    return {source, target, stat, op, value, expires};
}

std::uint64_t bits(double d) { return std::bit_cast<std::uint64_t>(d); }

} // namespace

TEST_CASE("stats: stacking formula") {
    StatPipeline p;
    const EntityKey s = ship(0);
    CHECK(p.get(s, accel) == 0.0);

    p.set_base(s, accel, 10.0);
    p.add(mod(part(0), s, accel, ModOp::add, 5.0));
    p.add(mod(part(1), s, accel, ModOp::add, -3.0));
    // increased stacks additively: +10% +40% = +50%
    p.add(mod(part(2), s, accel, ModOp::increased, 0.10));
    p.add(mod(part(3), s, accel, ModOp::increased, 0.40));
    CHECK(p.get(s, accel) == doctest::Approx(12.0 * 1.5));

    // more compounds: x1.5 x2 = x3
    p.add(mod(part(4), s, accel, ModOp::more, 0.5));
    p.add(mod(part(5), s, accel, ModOp::more, 1.0));
    CHECK(p.get(s, accel) == doctest::Approx(12.0 * 1.5 * 3.0));

    SUBCASE("highest floor and lowest cap apply") {
        p.add(mod(part(6), s, accel, ModOp::cap, 40.0));
        p.add(mod(part(7), s, accel, ModOp::cap, 30.0));
        CHECK(p.get(s, accel) == 30.0);
        p.add(mod(part(8), s, accel, ModOp::floor, 100.0));
        p.add(mod(part(9), s, accel, ModOp::floor, 60.0));
        CHECK(p.get(s, accel) == 30.0); // conflict: cap wins
        p.remove_source(part(6));
        p.remove_source(part(7));
        CHECK(p.get(s, accel) == 100.0);
    }

    SUBCASE("no implicit clamp below zero") {
        p.add(mod(part(6), s, accel, ModOp::increased, -2.0));
        CHECK(p.get(s, accel) < 0.0);
        p.add(mod(part(7), s, accel, ModOp::floor, 0.0));
        CHECK(p.get(s, accel) == 0.0);
    }

    SUBCASE("non-finite values are rejected") {
        CHECK_THROWS_AS(p.add(mod(part(6), s, accel, ModOp::add, std::numeric_limits<double>::quiet_NaN())),
                        std::invalid_argument);
        CHECK_THROWS_AS(p.set_base(s, accel, std::numeric_limits<double>::infinity()),
                        std::invalid_argument);
    }
}

TEST_CASE("stats: base falls back to the global default") {
    StatPipeline p;
    p.set_base(global_scope, cargo, 100.0);
    CHECK(p.get(ship(0), cargo) == 100.0);
    p.set_base(ship(0), cargo, 250.0);
    CHECK(p.get(ship(0), cargo) == 250.0);
    CHECK(p.get(ship(1), cargo) == 100.0);
    p.clear_base(ship(0), cargo);
    CHECK(p.get(ship(0), cargo) == 100.0);
}

TEST_CASE("stats: totals are bit-identical regardless of insertion order and history") {
    // Values chosen so naive left-to-right summation depends on order.
    const std::vector<double> values = {0.1, 0.2, 0.3, 1e16, -1e16, 1.0 / 3.0, 7e-17, -0.7, 2.5e15};
    {
        double fwd = 0.0;
        for (double v : values) {
            fwd += v;
        }
        double rev = 0.0;
        for (auto it = values.rbegin(); it != values.rend(); ++it) {
            rev += *it;
        }
        REQUIRE(bits(fwd) != bits(rev)); // otherwise this test proves nothing
    }

    auto build = [&](std::uint64_t seed, bool churn) {
        std::vector<double> order = values;
        Rng rng = Rng::from_seed(seed);
        rng.shuffle(order);
        StatPipeline p;
        p.set_base(ship(0), accel, 1.0);
        std::vector<ModifierId> junk;
        std::uint32_t n = 0;
        for (double v : order) {
            if (churn) {
                junk.push_back(p.add(mod(part(100 + n), ship(0), accel, ModOp::add, 123.456)));
            }
            p.add(mod(part(n), ship(0), accel, ModOp::add, v));
            p.add(mod(part(n), ship(0), accel, ModOp::increased, v * 1e-16));
            p.add(mod(part(n), ship(0), accel, ModOp::more, v * 1e-17));
            ++n;
        }
        // Removing extra rows reshuffles the table's dense order.
        std::reverse(junk.begin(), junk.end());
        for (ModifierId id : junk) {
            p.remove(id);
        }
        return p;
    };

    StatPipeline reference = build(1, false);
    const double expected = reference.get(ship(0), accel);
    for (std::uint64_t seed = 2; seed < 12; ++seed) {
        StatPipeline p = build(seed, seed % 2 == 0);
        CHECK(bits(p.get(ship(0), accel)) == bits(expected));
        CHECK(bits(p.evaluate(ship(0), accel)) == bits(expected));
        CHECK(bits(p.breakdown(ship(0), accel).value) == bits(expected));
    }
}

TEST_CASE("stats: negative zero does not break canonical ordering") {
    StatPipeline a;
    StatPipeline b;
    a.add(mod(part(0), ship(0), accel, ModOp::add, -0.0));
    a.add(mod(part(1), ship(0), accel, ModOp::add, 0.0));
    b.add(mod(part(1), ship(0), accel, ModOp::add, 0.0));
    b.add(mod(part(0), ship(0), accel, ModOp::add, -0.0));
    CHECK(bits(a.get(ship(0), accel)) == bits(b.get(ship(0), accel)));
    CHECK(bits(a.find(ModifierId{0, 1})->value) == bits(0.0));
}

TEST_CASE("stats: only dirty totals are recomputed") {
    StatPipeline p;
    for (std::uint32_t i = 0; i < 4; ++i) {
        p.set_base(ship(i), accel, 1.0);
        p.set_base(ship(i), cargo, 10.0);
    }
    auto read_all = [&] {
        for (std::uint32_t i = 0; i < 4; ++i) {
            p.get(ship(i), accel);
            p.get(ship(i), cargo);
        }
    };
    read_all();
    CHECK(p.recompute_count() == 8);
    read_all();
    CHECK(p.recompute_count() == 8); // all cached

    const ModifierId m = p.add(mod(part(0), ship(2), accel, ModOp::add, 1.0));
    read_all();
    CHECK(p.recompute_count() == 9); // only (ship 2, accel)
    CHECK(p.get(ship(2), accel) == 2.0);

    p.set_value(m, 3.0);
    read_all();
    CHECK(p.recompute_count() == 10);
    CHECK(p.get(ship(2), accel) == 4.0);

    p.set_value(m, 3.0); // no-op change doesn't dirty
    read_all();
    CHECK(p.recompute_count() == 10);

    p.remove(m);
    read_all();
    CHECK(p.recompute_count() == 11);
    CHECK(p.get(ship(2), accel) == 1.0);

    // A global modifier dirties every target's accel (lazily) but no cargo totals.
    p.add(mod(part(1), global_scope, accel, ModOp::increased, 1.0));
    read_all();
    CHECK(p.recompute_count() == 15);
    CHECK(p.get(ship(3), accel) == 2.0);

    // Unread totals are never recomputed.
    p.add(mod(part(2), ship(1), cargo, ModOp::add, 5.0));
    p.add(mod(part(3), ship(1), cargo, ModOp::add, 5.0));
    CHECK(p.recompute_count() == 15);
    CHECK(p.get(ship(1), cargo) == 20.0);
    CHECK(p.recompute_count() == 16);
}

TEST_CASE("stats: removal by source") {
    StatPipeline p;
    p.set_base(ship(0), accel, 1.0);
    const EntityKey drive = part(7);
    p.add(mod(drive, ship(0), accel, ModOp::add, 2.0));
    p.add(mod(drive, ship(0), accel, ModOp::increased, 0.5));
    p.add(mod(drive, ship(0), cargo, ModOp::add, -20.0));
    p.add(mod(drive, ship(1), accel, ModOp::add, 9.0));
    p.add(mod(part(8), ship(0), accel, ModOp::add, 1.0));
    CHECK(p.get(ship(0), accel) == doctest::Approx(4.0 * 1.5));
    CHECK(p.get(ship(0), cargo) == -20.0);

    std::vector<Modifier> removed;
    CHECK(p.remove_source(drive, &removed) == 4);
    CHECK(removed.size() == 4);
    CHECK(std::all_of(removed.begin(), removed.end(),
                      [&](const Modifier& m) { return m.source == drive; }));
    CHECK(p.get(ship(0), accel) == 2.0);
    CHECK(p.get(ship(0), cargo) == 0.0);
    CHECK(p.get(ship(1), accel) == 0.0);
    CHECK(p.modifiers().size() == 1);
    CHECK(p.remove_source(drive) == 0);
}

TEST_CASE("stats: remove by handle rejects stale handles") {
    StatPipeline p;
    const ModifierId m = p.add(mod(part(0), ship(0), accel, ModOp::add, 1.0));
    CHECK(p.remove(m));
    CHECK_FALSE(p.remove(m));
    CHECK_FALSE(p.set_value(m, 2.0));
    CHECK(p.find(m) == nullptr);
}

TEST_CASE("stats: expiry is half-open and deterministic") {
    StatPipeline p;
    p.set_base(ship(0), accel, 1.0);
    p.add(mod(part(0), ship(0), accel, ModOp::add, 1.0, Time{200}));
    p.add(mod(part(1), ship(0), accel, ModOp::add, 10.0, Time{100}));
    p.add(mod(part(2), ship(0), accel, ModOp::add, 100.0, Time{100}));
    p.add(mod(part(3), ship(0), accel, ModOp::add, 1000.0));
    CHECK(p.next_expiry() == Time{100});
    CHECK(p.get(ship(0), accel) == 1112.0);

    CHECK(p.expire_until(Time{99}) == 0);
    CHECK(p.get(ship(0), accel) == 1112.0);

    std::vector<Modifier> expired;
    CHECK(p.expire_until(Time{100}, &expired) == 2);
    REQUIRE(expired.size() == 2);
    CHECK(expired[0].source == part(1)); // (expires, id) order
    CHECK(expired[1].source == part(2));
    CHECK(p.get(ship(0), accel) == 1002.0);
    CHECK(p.next_expiry() == Time{200});

    CHECK(p.expire_until(Time{10'000}) == 1);
    CHECK(p.get(ship(0), accel) == 1001.0);
    CHECK_FALSE(p.next_expiry().has_value());

    // Removing an expiring modifier early also drops it from the expiry index.
    const ModifierId m = p.add(mod(part(4), ship(0), accel, ModOp::add, 1.0, Time{500}));
    p.remove(m);
    CHECK_FALSE(p.next_expiry().has_value());
}

TEST_CASE("stats: global scope and entity modifiers stack") {
    StatPipeline p;
    const EntityKey tycho = station(0);
    const EntityKey ceres = station(1);
    p.set_base(ship(0), accel, 1.0);
    p.set_base(ship(1), accel, 1.0);
    p.set_base(ship(2), accel, 1.0);

    p.add(mod(part(0), global_scope, accel, ModOp::increased, 0.5));
    p.add(mod(part(1), tycho, accel, ModOp::add, 1.0));
    p.add(mod(part(2), ceres, accel, ModOp::add, 2.0));
    p.add(mod(part(3), ship(0), accel, ModOp::increased, 0.5));

    const EntityKey at_tycho[] = {tycho};
    const EntityKey at_both[] = {ceres, tycho, tycho, global_scope};
    p.set_scopes(ship(0), at_tycho);
    p.set_scopes(ship(1), at_both);
    CHECK(p.scopes(ship(1)).size() == 2);

    CHECK(p.get(ship(0), accel) == doctest::Approx(2.0 * 2.0));
    CHECK(p.get(ship(1), accel) == doctest::Approx(4.0 * 1.5));
    CHECK(p.get(ship(2), accel) == doctest::Approx(1.5));

    const auto before = p.recompute_count();
    // A station modifier dirties only ships that list the station: ship 2 stays cached.
    p.add(mod(part(4), tycho, accel, ModOp::add, 1.0));
    CHECK(p.get(ship(0), accel) == doctest::Approx(3.0 * 2.0));
    CHECK(p.get(ship(1), accel) == doctest::Approx(5.0 * 1.5));
    CHECK(p.get(ship(2), accel) == doctest::Approx(1.5));
    CHECK(p.recompute_count() == before + 2);

    // Changing membership (undocking) invalidates that ship only.
    p.set_scopes(ship(0), {});
    CHECK(p.get(ship(0), accel) == doctest::Approx(1.0 * 2.0));
    CHECK(p.get(ship(1), accel) == doctest::Approx(5.0 * 1.5));
    CHECK(p.recompute_count() == before + 3);
}

TEST_CASE("stats: forget_target drops everything about a despawned entity") {
    StatPipeline p;
    const EntityKey tycho = station(0);
    p.set_base(ship(0), accel, 1.0);
    p.add(mod(part(0), tycho, accel, ModOp::add, 5.0));
    p.add(mod(tycho, ship(1), cargo, ModOp::add, 7.0)); // tycho as a source
    const EntityKey at_tycho[] = {tycho};
    p.set_scopes(ship(0), at_tycho);
    CHECK(p.get(ship(0), accel) == 6.0);
    CHECK(p.get(ship(1), cargo) == 7.0);

    p.forget_target(tycho);
    CHECK(p.modifiers().empty());
    CHECK(p.get(ship(0), accel) == 1.0);
    CHECK(p.get(ship(1), cargo) == 0.0);

    p.forget_target(ship(0));
    CHECK(p.get(ship(0), accel) == 0.0);
}

TEST_CASE("stats: breakdown lists contributions") {
    StatPipeline p;
    const EntityKey tycho = station(0);
    p.set_base(global_scope, accel, 2.0);
    p.add(mod(part(1), ship(0), accel, ModOp::add, 1.0));
    p.add(mod(part(0), ship(0), accel, ModOp::increased, 0.25));
    p.add(mod(part(2), global_scope, accel, ModOp::more, -0.5));
    p.add(mod(part(3), tycho, accel, ModOp::cap, 1.0));
    p.add(mod(part(4), ship(1), accel, ModOp::add, 99.0)); // other ship: not listed
    const EntityKey at_tycho[] = {tycho};
    p.set_scopes(ship(0), at_tycho);

    const StatBreakdown b = p.breakdown(ship(0), accel);
    CHECK(b.base == 2.0);
    CHECK(b.base_from_global);
    CHECK(b.add == 1.0);
    CHECK(b.increased == 0.25);
    CHECK(b.more == 0.5);
    CHECK_FALSE(b.floor.has_value());
    CHECK(b.cap == 1.0);
    CHECK(b.value == 1.0);
    CHECK(bits(b.value) == bits(p.get(ship(0), accel)));
    REQUIRE(b.contributions.size() == 4);
    CHECK(b.contributions[0].modifier.op == ModOp::add);
    CHECK(b.contributions[1].modifier.op == ModOp::increased);
    CHECK(b.contributions[2].modifier.op == ModOp::more);
    CHECK(b.contributions[2].modifier.target == global_scope);
    CHECK(b.contributions[3].modifier.op == ModOp::cap);
    CHECK(b.contributions[3].modifier.target == tycho);
    for (const StatContribution& c : b.contributions) {
        REQUIRE(p.find(c.id) != nullptr);
        CHECK(*p.find(c.id) == c.modifier);
    }
}
