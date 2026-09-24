#include <doctest/doctest.h>

#include "simcore/table.hpp"

#include <memory>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace sim;

namespace {

struct ShipTag;
struct StationTag;
using ShipId = Handle<ShipTag>;
using StationId = Handle<StationTag>;

struct Ship {
    std::string name;
    int cargo = 0;
};

using Ships = Table<ShipTag, Ship>;

// Typed separation: handles of different tables are unrelated types.
static_assert(!std::is_convertible_v<ShipId, StationId>);
static_assert(!std::is_constructible_v<StationId, ShipId>);
static_assert(!std::is_invocable_v<decltype(&Ships::contains), const Ships&, StationId>);
static_assert(std::is_same_v<Ships::handle_type, ShipId>);
static_assert(sizeof(ShipId) == 8);
static_assert(std::is_trivially_copyable_v<ShipId>);

std::vector<std::pair<std::uint32_t, int>> dump(const Ships& t) {
    std::vector<std::pair<std::uint32_t, int>> out;
    for (auto [id, ship] : t) {
        out.emplace_back(id.index, ship.cargo);
    }
    return out;
}

} // namespace

TEST_CASE("null handle") {
    ShipId h;
    CHECK(h.is_null());
    CHECK_FALSE(h);
    CHECK(h == ShipId::null());

    Ships t;
    CHECK_FALSE(t.contains(h));
    CHECK(t.get(h) == nullptr);
    CHECK_FALSE(t.erase(h));
    CHECK_THROWS_AS(t.at(h), std::out_of_range);
}

TEST_CASE("insert, lookup, size") {
    Ships t;
    CHECK(t.empty());
    const ShipId a = t.insert({"Rocinante", 10});
    const ShipId b = t.emplace(Ship{"Canterbury", 20});
    CHECK(a);
    CHECK(a != b);
    CHECK(t.size() == 2);
    CHECK_FALSE(t.empty());
    CHECK(t.contains(a));
    REQUIRE(t.get(a) != nullptr);
    CHECK(t.get(a)->name == "Rocinante");
    CHECK(t.at(b).cargo == 20);
    t.at(b).cargo = 25;
    CHECK(std::as_const(t).at(b).cargo == 25);
}

TEST_CASE("stale handle detection and generation bump on reuse") {
    Ships t;
    const ShipId a = t.insert({"a", 1});
    CHECK(t.erase(a));
    CHECK_FALSE(t.contains(a));
    CHECK(t.get(a) == nullptr);
    CHECK_FALSE(t.erase(a)); // double erase is a no-op

    const ShipId b = t.insert({"b", 2});
    CHECK(b.index == a.index); // slot reused
    CHECK(b.generation == a.generation + 1);
    CHECK_FALSE(t.contains(a)); // ABA: old handle does not alias the new row
    CHECK(t.contains(b));
    CHECK(t.at(b).name == "b");

    // Handle from a different (larger) table is out of range, not UB.
    CHECK_FALSE(t.contains(ShipId{1000, 1}));
}

TEST_CASE("free list is FIFO") {
    Ships t;
    std::vector<ShipId> ids;
    for (int i = 0; i < 4; ++i) {
        ids.push_back(t.insert({"", i}));
    }
    t.erase(ids[2]);
    t.erase(ids[0]);
    t.erase(ids[3]);
    CHECK(t.insert({}).index == 2);
    CHECK(t.insert({}).index == 0);
    CHECK(t.insert({}).index == 3);
    CHECK(t.insert({}).index == 4); // free list exhausted -> new slot
}

TEST_CASE("swap-and-pop keeps rows dense and handles valid") {
    Ships t;
    const ShipId a = t.insert({"a", 1});
    const ShipId b = t.insert({"b", 2});
    const ShipId c = t.insert({"c", 3});
    t.erase(a);
    CHECK(t.size() == 2);
    // c moved into a's dense position.
    CHECK(t.rows()[0].name == "c");
    CHECK(t.rows()[1].name == "b");
    CHECK(t.handle_at(0) == c);
    CHECK(t.at(c).cargo == 3);
    CHECK(t.at(b).cargo == 2);

    // Erasing the last dense row needs no move.
    t.erase(b);
    CHECK(t.size() == 1);
    CHECK(t.at(c).name == "c");
}

TEST_CASE("iteration yields handle and row in dense order") {
    Ships t;
    const ShipId a = t.insert({"a", 1});
    const ShipId b = t.insert({"b", 2});
    std::vector<ShipId> seen;
    for (auto [id, ship] : t) {
        seen.push_back(id);
        ship.cargo *= 10; // mutable access through non-const iteration
    }
    CHECK(seen == std::vector<ShipId>{a, b});
    CHECK(t.at(a).cargo == 10);
    CHECK(t.at(b).cargo == 20);

    const Ships& ct = t;
    int total = 0;
    for (auto [id, ship] : ct) {
        static_assert(std::is_same_v<decltype(ship), const Ship&>);
        total += ship.cargo;
    }
    CHECK(total == 30);
}

TEST_CASE("identical op sequences produce identical state") {
    auto run = [] {
        Ships t;
        std::vector<ShipId> live;
        std::uint32_t x = 12345;
        for (int step = 0; step < 2000; ++step) {
            x = x * 1664525u + 1013904223u; // local LCG, deterministic
            if (!live.empty() && (x >> 16) % 3 == 0) {
                const std::size_t k = (x >> 8) % live.size();
                t.erase(live[k]);
                live[k] = live.back();
                live.pop_back();
            } else {
                live.push_back(t.insert({"", step}));
            }
        }
        return t;
    };
    const Ships t1 = run();
    const Ships t2 = run();
    CHECK(dump(t1) == dump(t2));
    CHECK(std::vector(t1.slots().begin(), t1.slots().end()) ==
          std::vector(t2.slots().begin(), t2.slots().end()));
    CHECK(std::vector(t1.dense_slots().begin(), t1.dense_slots().end()) ==
          std::vector(t2.dense_slots().begin(), t2.dense_slots().end()));
    CHECK(t1.free_list_head() == t2.free_list_head());
}

TEST_CASE("erase_if during iteration") {
    Ships t;
    std::vector<ShipId> ids;
    for (int i = 0; i < 10; ++i) {
        ids.push_back(t.insert({"", i}));
    }
    const std::size_t n = t.erase_if([](ShipId, const Ship& s) { return s.cargo % 2 == 0; });
    CHECK(n == 5);
    CHECK(t.size() == 5);
    for (int i = 0; i < 10; ++i) {
        CHECK(t.contains(ids[static_cast<std::size_t>(i)]) == (i % 2 == 1));
    }
    for (auto [id, ship] : t) {
        CHECK(ship.cargo % 2 == 1);
        CHECK(t.get(id) == &ship);
    }

    // Recommended alternative: collect handles, then erase.
    std::vector<ShipId> doomed;
    for (auto [id, ship] : t) {
        if (ship.cargo > 5) {
            doomed.push_back(id);
        }
    }
    for (ShipId id : doomed) {
        t.erase(id);
    }
    CHECK(t.size() == 3);
}

TEST_CASE("clear invalidates handles and reuses slots in order") {
    Ships t;
    const ShipId a = t.insert({"a", 1});
    const ShipId b = t.insert({"b", 2});
    const ShipId c = t.insert({"c", 3});
    t.erase(b);
    t.clear();
    CHECK(t.empty());
    CHECK_FALSE(t.contains(a));
    CHECK_FALSE(t.contains(c));
    const ShipId d = t.insert({"d", 4});
    const ShipId e = t.insert({"e", 5});
    CHECK(d.index == 0);
    CHECK(d.generation == a.generation + 1);
    CHECK(e.index == 1);
    CHECK(t.insert({}).index == 2);
    CHECK(t.insert({}).index == 3);
}

TEST_CASE("reserve and hashing") {
    Ships t;
    t.reserve(64);
    CHECK(t.empty());
    std::unordered_set<ShipId> set;
    for (int i = 0; i < 64; ++i) {
        set.insert(t.insert({}));
    }
    CHECK(set.size() == 64);
    CHECK(set.contains(t.handle_at(10)));
    CHECK(std::hash<ShipId>{}(ShipId{1, 2}) != std::hash<ShipId>{}(ShipId{2, 1}));
}

TEST_CASE("move-only rows") {
    struct Cargo {
        std::unique_ptr<int> amount;
    };
    struct CargoTag;
    Table<CargoTag, Cargo> t;
    const auto a = t.emplace(std::make_unique<int>(1));
    const auto b = t.emplace(std::make_unique<int>(2));
    t.erase(a);
    CHECK(*t.at(b).amount == 2);
}
