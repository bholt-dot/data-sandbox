#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "simcore/hash.hpp"
#include "simcore/rng.hpp"
#include "simcore/scheduler.hpp"
#include "simcore/snapshot.hpp"
#include "simcore/table.hpp"

using namespace sim;

namespace {

struct ShipTag {};
using ShipId = Handle<ShipTag>;

struct ShipRow {
    std::string name;
    double fuel = 0.0;
    ShipId escort;

    bool operator==(const ShipRow&) const = default;
    static auto fields(auto& self) { return std::tie(self.name, self.fuel, self.escort); }
};

using Ships = Table<ShipTag, ShipRow>;

template <class T>
T round_trip(const T& v) {
    Writer w;
    encode(w, v);
    const auto b = w.take();
    Reader r(b);
    T out{};
    decode(r, out);
    r.expect_end();
    return out;
}

template <class Tbl>
void check_same_internal_state(const Tbl& a, const Tbl& b) {
    REQUIRE(a.slots().size() == b.slots().size());
    for (std::size_t i = 0; i < a.slots().size(); ++i) {
        CHECK(a.slots()[i] == b.slots()[i]);
    }
    CHECK(std::vector(a.dense_slots().begin(), a.dense_slots().end()) ==
          std::vector(b.dense_slots().begin(), b.dense_slots().end()));
    CHECK(std::vector(a.rows().begin(), a.rows().end()) ==
          std::vector(b.rows().begin(), b.rows().end()));
    CHECK(a.free_list() == b.free_list());
}

// Deterministic churn: inserts, erases (swap-and-pop reorders), clear().
Ships churned_table() {
    Ships t;
    Rng rng = Rng::from_seed(99);
    std::vector<ShipId> live;
    for (int step = 0; step < 400; ++step) {
        if (step == 150) {
            t.clear();
            live.clear();
        }
        if (live.empty() || rng.chance(0.6)) {
            live.push_back(t.insert({"s" + std::to_string(step), rng.uniform01(),
                                     live.empty() ? ShipId{} : live.front()}));
        } else {
            const auto k = static_cast<std::size_t>(rng.below(live.size()));
            CHECK(t.erase(live[k]));
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
        }
    }
    return t;
}

} // namespace

// --- Table -------------------------------------------------------------------------------

TEST_CASE("table round trip after churn preserves internals and future handles") {
    Ships original = churned_table();
    REQUIRE(!original.free_list().empty());
    Ships restored = round_trip(original);
    check_same_internal_state(original, restored);
    CHECK(hash_state(original) == hash_state(restored));

    // Every stored handle still resolves to the same row.
    for (auto [id, row] : original) {
        REQUIRE(restored.contains(id));
        CHECK(restored.at(id) == row);
    }

    // Identical operation sequences on both now produce identical handles and state.
    for (int i = 0; i < 50; ++i) {
        const ShipId a = original.insert({"n", 1.0, {}});
        const ShipId b = restored.insert({"n", 1.0, {}});
        CHECK(a == b);
        if (i % 3 == 0) {
            original.erase(original.handle_at(0));
            restored.erase(restored.handle_at(0));
        }
    }
    check_same_internal_state(original, restored);
    CHECK(hash_state(original) == hash_state(restored));
}

TEST_CASE("table hash changes on a single field and on allocation state") {
    Ships t = churned_table();
    const auto h = hash_state(t);

    Ships changed = t;
    changed.rows()[0].fuel += 1.0;
    CHECK(hash_state(changed) != h);

    // Same rows, different free list / generations: future handles differ, so must the hash.
    Ships churned = t;
    const ShipId extra = churned.insert({"tmp", 0.0, {}});
    churned.erase(extra);
    CHECK(hash_state(churned) != h);
}

TEST_CASE("empty and cleared tables round trip") {
    Ships empty;
    Ships r = round_trip(empty);
    CHECK(r.empty());
    CHECK(r.insert({}) == ShipId{0, 1});

    Ships cleared;
    cleared.insert({});
    cleared.insert({});
    cleared.clear();
    Ships rc = round_trip(cleared);
    check_same_internal_state(cleared, rc);
    CHECK(rc.insert({}) == ShipId{0, 2});
}

TEST_CASE("table restore near generation exhaustion retires the slot") {
    constexpr std::uint32_t max_gen = std::numeric_limits<std::uint32_t>::max();
    Ships t;
    const std::vector<std::uint32_t> gens{max_gen - 2, 5};
    const std::vector<std::uint32_t> dense{0};
    const std::vector<std::uint32_t> free{1};
    t.restore(gens, dense, {ShipRow{"old", 0.0, {}}}, free);

    const ShipId old{0, max_gen - 2};
    REQUIRE(t.contains(old));
    CHECK(t.erase(old));
    CHECK(t.free_list() == std::vector<std::uint32_t>{1, 0});

    CHECK(t.insert({}) == ShipId{1, 5});
    const ShipId last = t.insert({});
    CHECK(last == ShipId{0, max_gen - 1});
    CHECK(t.erase(last));
    // Slot 0 would wrap: it is retired, not reused, and the stale handle stays dead.
    CHECK(t.slots()[0].generation == max_gen);
    CHECK(t.free_list().empty());
    CHECK_FALSE(t.contains(last));
    CHECK(t.insert({}) == ShipId{2, 1});

    // Retired slots survive a round trip and clear().
    Ships r = round_trip(t);
    check_same_internal_state(t, r);
    r.clear();
    CHECK(r.slots()[0].generation == max_gen);
    CHECK(r.free_list() == std::vector<std::uint32_t>{1, 2});
}

TEST_CASE("table restore rejects inconsistent state and leaves table unchanged") {
    constexpr std::uint32_t max_gen = std::numeric_limits<std::uint32_t>::max();
    using V = std::vector<std::uint32_t>;
    Ships t;
    const ShipId keep = t.insert({"keep", 1.0, {}});

    auto bad = [&](V gens, V dense, std::size_t rows, V free) {
        CHECK_THROWS_AS(t.restore(gens, dense, std::vector<ShipRow>(rows), free),
                        std::invalid_argument);
    };
    bad({1}, {0}, 2, {});           // rows vs dense mismatch
    bad({0}, {0}, 1, {});           // generation 0
    bad({1}, {1}, 1, {});           // dense out of range
    bad({1, 1}, {0, 0}, 2, {});     // dense duplicated
    bad({1, 1}, {0}, 1, {});        // free slot missing from free list
    bad({1, 1}, {0}, 1, {0});       // live slot on free list
    bad({1, 1}, {0}, 1, {1, 1});    // free slot duplicated
    bad({max_gen}, {0}, 1, {});     // live retired slot
    bad({1, max_gen}, {0}, 1, {1}); // retired slot on free list

    CHECK(t.size() == 1);
    CHECK(t.at(keep).name == "keep");
}

// --- Rng ---------------------------------------------------------------------------------

TEST_CASE("rng round trip continues the same stream") {
    Rng a = rng_for(7, "combat");
    for (int i = 0; i < 13; ++i) {
        a.next_u64();
    }
    Rng b = round_trip(a);
    CHECK(a == b);
    CHECK(hash_state(a) == hash_state(b));
    for (int i = 0; i < 100; ++i) {
        CHECK(a.next_u64() == b.next_u64());
    }
    b.next_u64();
    CHECK(hash_state(a) != hash_state(b));
}

// --- Scheduler ---------------------------------------------------------------------------

namespace {

struct Arrival {
    ShipId ship;
    static auto fields(auto& self) { return std::tie(self.ship); }
};
struct Tick {
    std::int32_t system = 0;
    static auto fields(auto& self) { return std::tie(self.system); }
};
struct Note {
    std::string text;
    static auto fields(auto& self) { return std::tie(self.text); }
};
using Payload = std::variant<Arrival, Tick, Note>;
using Sched = Scheduler<Payload>;
using Occ = Occurrence<Payload>;

constexpr EventFlags notable = 1;

// Deterministic handler that reschedules, cancels and removes, recording everything it sees
// (including ids it is handed and allocates) so id allocation is compared too.
struct Driver {
    std::vector<std::string> log;
    Rng rng = Rng::from_seed(3);
    std::vector<EventId> cancellable;

    void operator()(Sched& s, const Occ& o) {
        std::string line = std::to_string(o.time.seconds) + ":" + std::to_string(o.event.slot) +
                           "/" + std::to_string(o.event.seq) + "/" +
                           std::to_string(o.periodic.index) + ":" + std::to_string(o.payload.index());
        if (const auto* a = std::get_if<Arrival>(&o.payload)) {
            line += ":A" + std::to_string(a->ship.index);
            const EventId id = s.schedule_in(seconds(static_cast<std::int64_t>(rng.below(500))),
                                             Arrival{{a->ship.index + 1, 1}},
                                             {static_cast<std::int32_t>(rng.below(3)), 0});
            cancellable.push_back(id);
            line += ">" + std::to_string(id.slot) + "/" + std::to_string(id.seq);
        } else if (const auto* t = std::get_if<Tick>(&o.payload)) {
            line += ":T" + std::to_string(t->system);
            if (!cancellable.empty() && rng.chance(0.3)) {
                line += s.cancel(cancellable.front()) ? ":c" : ":-";
                cancellable.erase(cancellable.begin());
            }
            if (t->system == 2 && o.time.seconds > 3000) {
                s.remove_periodic(o.periodic);
            }
            if (rng.chance(0.2)) {
                s.schedule_in(Duration{}, Note{"same-instant"}, {0, notable});
            }
        } else {
            line += ":N" + std::get<Note>(o.payload).text;
        }
        log.push_back(std::move(line));
    }
};

Sched make_world() {
    Sched s;
    s.add_periodic(seconds(60), Tick{1});
    s.add_periodic(seconds(90), Tick{2}, {Time{15}, -1, 0});
    s.add_periodic(seconds(3600), Tick{3}, {Time{0}, 1, notable});
    for (std::uint32_t i = 0; i < 5; ++i) {
        s.schedule_at(Time{100 * i}, Arrival{{i * 10, 1}});
    }
    s.schedule_at(Time{250}, Note{"hello"}, {0, notable});
    return s;
}

} // namespace

TEST_CASE("scheduler round trip mid run fires identically to an uninterrupted run") {
    Sched reference = make_world();
    Driver ref_driver;
    reference.advance_to(Time{2500}, ref_driver);
    reference.advance_until(Time{9000}, ref_driver, notable); // stops mid-instant possibly
    reference.advance_to(Time{20000}, ref_driver);

    Sched first = make_world();
    Driver driver;
    first.advance_to(Time{2500}, driver);

    const auto bytes = save_snapshot(1, [&](Writer& w) { encode(w, first); encode(w, driver.rng); });
    Sched resumed(Time{999999}); // garbage state, fully replaced
    resumed.schedule_at(Time{1000000}, Note{"junk"});
    load_snapshot(bytes, 1, [&](Reader& r) {
        decode(r, resumed);
        decode(r, driver.rng);
    });
    CHECK(hash_state(resumed) == hash_state(first));
    CHECK(resumed.now() == first.now());
    CHECK(resumed.pending_event_count() == first.pending_event_count());

    resumed.advance_until(Time{9000}, driver, notable);
    resumed.advance_to(Time{20000}, driver);

    CHECK(ref_driver.log.size() > 300); // the scenario actually exercises the scheduler
    REQUIRE(driver.log.size() == ref_driver.log.size());
    CHECK(driver.log == ref_driver.log);
    CHECK(hash_state(resumed) == hash_state(reference));
}

TEST_CASE("scheduler round trip after early stop keeps same-instant events pending") {
    Sched s;
    s.schedule_at(Time{10}, Note{"a"}, {0, notable});
    s.schedule_at(Time{10}, Note{"b"});
    s.schedule_at(Time{10}, Note{"c"});
    s.add_periodic(seconds(10), Tick{1}, {Time{0}, 5, 0});
    const auto cancelled = s.schedule_at(Time{50}, Note{"x"});
    s.cancel(cancelled);
    Driver d1;
    auto r = s.advance_until(Time{100}, d1, notable);
    REQUIRE(r.stopped());

    Sched copy = round_trip(s);
    Driver d2 = d1;
    s.advance_to(Time{40}, d1);
    copy.advance_to(Time{40}, d2);
    CHECK(d1.log == d2.log);
    // Next allocated ids match too (slot free list and sequence counter restored).
    const EventId a = s.schedule_in(seconds(1), Note{"z"});
    const EventId b = copy.schedule_in(seconds(1), Note{"z"});
    CHECK(a == b);
}

TEST_CASE("scheduler hash changes with pending payload") {
    Sched a = make_world();
    Sched b = make_world();
    CHECK(hash_state(a) == hash_state(b));
    b.schedule_at(Time{5}, Note{"extra"});
    CHECK(hash_state(a) != hash_state(b));

    Sched c = make_world();
    auto st = c.snapshot();
    std::get<Note>(st.events.back().payload).text = "hellp";
    c.restore(st);
    CHECK(hash_state(a) != hash_state(c));
}

TEST_CASE("scheduler restore rejects inconsistent state") {
    Sched base = make_world();
    const auto good = base.snapshot();
    Sched target;

    auto expect_bad = [&](auto mutate) {
        auto st = good;
        mutate(st);
        CHECK_THROWS_AS(target.restore(st), std::invalid_argument);
    };
    expect_bad([](auto& st) { st.next_seq = 0; });
    expect_bad([](auto& st) { st.slot_count += 1; });
    expect_bad([](auto& st) { st.events[0].id.slot = 99; });
    expect_bad([](auto& st) { st.events[1].id.slot = st.events[0].id.slot; });
    expect_bad([](auto& st) { st.events[0].id.seq = st.events[1].id.seq; });
    expect_bad([](auto& st) { st.events[0].id.seq = st.next_seq; });
    expect_bad([](auto& st) { st.periodics[0].ordinal = st.events[0].id.seq; });
    expect_bad([](auto& st) { st.periodics[0].period = Duration{}; });
    expect_bad([](auto& st) { st.now = Time{1'000'000}; });
    CHECK(target.pending_event_count() == 0);
    CHECK(target.periodics().empty());
}

// --- Envelope ----------------------------------------------------------------------------

namespace {

struct World {
    Ships ships;
    Sched scheduler;
    Rng rng;
    static auto fields(auto& self) { return std::tie(self.ships, self.scheduler, self.rng); }
};

World make_full_world() {
    World w;
    w.ships = churned_table();
    w.scheduler = make_world();
    w.rng = rng_for(1, "world");
    return w;
}

} // namespace

TEST_CASE("whole world snapshot round trips and hashes equal") {
    const World w = make_full_world();
    const auto bytes = save_snapshot(3, w);
    World loaded;
    load_snapshot(bytes, 3, loaded);
    CHECK(hash_state(loaded) == hash_state(w));
    check_same_internal_state(w.ships, loaded.ships);
    // Deterministic: saving twice gives identical bytes.
    CHECK(save_snapshot(3, loaded) == bytes);
}

TEST_CASE("snapshot envelope rejects corruption and leaves target untouched") {
    const World w = make_full_world();
    const auto bytes = save_snapshot(2, w);
    World target;
    target.rng = Rng::from_seed(5);
    const auto before = hash_state(target);

    auto rejects = [&](const std::vector<std::uint8_t>& b, std::uint32_t schema,
                       const char* message) {
        CHECK_THROWS_WITH_AS(load_snapshot(b, schema, target), doctest::Contains(message),
                             SerializeError);
    };

    SUBCASE("truncation at any length") {
        for (std::size_t n = 0; n < bytes.size(); n += 7) {
            std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
            CHECK_THROWS_AS(load_snapshot(cut, 2, target), SerializeError);
        }
        rejects({bytes.begin(), bytes.end() - 1}, 2, "size mismatch");
    }
    SUBCASE("flipped body bit") {
        auto b = bytes;
        b[40] ^= 0x10;
        rejects(b, 2, "checksum");
    }
    SUBCASE("bad magic") {
        auto b = bytes;
        b[0] = 'X';
        rejects(b, 2, "magic");
    }
    SUBCASE("future format version") {
        auto b = bytes;
        b[4] = 2;
        rejects(b, 2, "format version");
    }
    SUBCASE("newer schema") { rejects(bytes, 1, "newer"); }
    SUBCASE("trailing data") {
        auto b = bytes;
        b.push_back(0);
        rejects(b, 2, "size mismatch");
    }
    SUBCASE("body that is well formed but semantically invalid") {
        // Valid envelope around a table whose free list omits a free slot.
        Writer body;
        encode_count(body, 2); // two slots
        encode(body, std::uint32_t{1});
        encode(body, std::uint32_t{1});
        encode_count(body, 0); // no live rows
        encode(body, std::vector<std::uint32_t>{0});
        const auto b = save_snapshot(2, [&](Writer& out) {
            out.write_bytes(body.bytes().data(), body.size());
        });
        Ships t;
        CHECK_THROWS_WITH_AS(load_snapshot(b, 2, t), doctest::Contains("free list"),
                             SerializeError);
    }
    CHECK(hash_state(target) == before);
}

TEST_CASE("older schema version is visible to decoders") {
    const auto bytes = save_snapshot(4, [](Writer& w) { encode(w, std::int32_t{7}); });
    std::uint32_t seen = 0;
    load_snapshot(bytes, 9, [&](Reader& r) {
        seen = r.schema_version();
        CHECK(decode_as<std::int32_t>(r) == 7);
    });
    CHECK(seen == 4);

    // A decoder that does not consume the whole body is an error.
    CHECK_THROWS_WITH_AS(load_snapshot(bytes, 9, [](Reader&) {}), doctest::Contains("trailing"),
                         SerializeError);
}
