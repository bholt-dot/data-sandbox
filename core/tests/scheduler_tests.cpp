#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "simcore/scheduler.hpp"

using namespace sim;

namespace {

struct Arrival {
    std::uint32_t ship = 0;
};
struct Tick {
    int system = 0;
};
struct Note {
    int value = 0;
};
using Payload = std::variant<Arrival, Tick, Note>;
using Sched = Scheduler<Payload>;
using Occ = Occurrence<Payload>;

constexpr EventFlags interesting = 1u << 0;

// Records every occurrence as "t:label" for order assertions.
struct Recorder {
    std::vector<std::string> log;

    void operator()(Sched&, const Occ& o) {
        std::string label = std::visit(
            [](const auto& p) -> std::string {
                using T = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<T, Arrival>) {
                    return "A" + std::to_string(p.ship);
                } else if constexpr (std::is_same_v<T, Tick>) {
                    return "T" + std::to_string(p.system);
                } else {
                    return "N" + std::to_string(p.value);
                }
            },
            o.payload);
        log.push_back(std::to_string(o.time.seconds) + ":" + label);
    }
};

} // namespace

TEST_CASE("events fire in time order and the clock jumps") {
    Sched s;
    Recorder rec;
    s.schedule_at(Time{30}, Note{3});
    s.schedule_at(Time{10}, Note{1});
    s.schedule_at(Time{20}, Note{2});

    auto r = s.advance_to(Time{25}, rec);
    CHECK(r.events_fired == 2);
    CHECK(s.now() == Time{25});
    CHECK(rec.log == std::vector<std::string>{"10:N1", "20:N2"});

    r = s.advance_to(Time{100}, rec);
    CHECK(r.events_fired == 1);
    CHECK(s.now() == Time{100});
    CHECK(s.pending_event_count() == 0);
}

TEST_CASE("same-time events are FIFO within a priority; lower priority value first") {
    Sched s;
    Recorder rec;
    s.schedule_at(Time{5}, Note{1});
    s.schedule_at(Time{5}, Note{2});
    s.schedule_at(Time{5}, Note{3}, {.priority = -1});
    s.schedule_at(Time{5}, Note{4});
    s.schedule_at(Time{5}, Note{5}, {.priority = 1});
    s.advance_to(Time{6}, rec);
    CHECK(rec.log == std::vector<std::string>{"5:N3", "5:N1", "5:N2", "5:N4", "5:N5"});
}

TEST_CASE("advance is half-open: occurrences at the target stay pending") {
    Sched s;
    Recorder rec;
    s.schedule_at(Time{10}, Note{1});
    s.advance_to(Time{10}, rec);
    CHECK(rec.log.empty());
    CHECK(s.now() == Time{10});
    s.advance_by(seconds(0), rec);
    CHECK(rec.log.empty());
    s.advance_by(seconds(1), rec);
    CHECK(rec.log == std::vector<std::string>{"10:N1"});
}

TEST_CASE("periodic cadence across long jumps") {
    Sched s;
    int daily = 0;
    int six_hourly = 0;
    int weekly = 0;
    s.add_periodic(days(1), Tick{1});
    s.add_periodic(hours(6), Tick{2});
    s.add_periodic(days(7), Tick{3});
    auto count = [&](Sched&, const Occ& o) {
        switch (std::get<Tick>(o.payload).system) {
        case 1: ++daily; break;
        case 2: ++six_hourly; break;
        default: ++weekly; break;
        }
    };
    auto r = s.advance_by(days(365), count);
    CHECK(daily == 365);
    CHECK(six_hourly == 365 * 4);
    CHECK(weekly == 53); // days 0, 7, ..., 364
    CHECK(r.periodic_fired == static_cast<std::uint64_t>(365 + 365 * 4 + 53));
    CHECK(r.events_fired == 0);
    CHECK(s.now() == Time{} + days(365));

    // Splitting the same span into uneven chunks yields the same counts.
    Sched s2;
    daily = six_hourly = weekly = 0;
    s2.add_periodic(days(1), Tick{1});
    s2.add_periodic(hours(6), Tick{2});
    s2.add_periodic(days(7), Tick{3});
    for (int i = 0; i < 73; ++i) {
        s2.advance_by(days(5), count);
    }
    CHECK(daily == 365);
    CHECK(six_hourly == 365 * 4);
    CHECK(weekly == 53);
}

TEST_CASE("periodic anchor sets phase; late registration aligns to the grid") {
    Sched s(Time{} + hours(10));
    std::vector<std::int64_t> times;
    s.add_periodic(days(1), Tick{1}, {.anchor = Time{} + hours(6)});
    s.advance_by(days(3), [&](Sched&, const Occ& o) { times.push_back(o.time.seconds); });
    const std::int64_t d = 86400;
    const std::int64_t h = 3600;
    CHECK(times == std::vector<std::int64_t>{d + 6 * h, 2 * d + 6 * h, 3 * d + 6 * h});

    // Anchor in the future with negative diff: first grid point >= now.
    Sched s2(Time{110});
    auto id = s2.add_periodic(seconds(30), Tick{1}, {.anchor = Time{1000}});
    CHECK(s2.periodics()[id.index].next == Time{130});
    Sched s3(Time{100});
    id = s3.add_periodic(seconds(30), Tick{1}, {.anchor = Time{1000}});
    CHECK(s3.periodics()[id.index].next == Time{100}); // exactly on the grid: fires at now
}

TEST_CASE("periodic systems and events at the same instant: priority, then ordinal") {
    Sched s;
    Recorder rec;
    s.add_periodic(days(1), Tick{1});                       // ordinal 1
    s.schedule_at(Time{} + days(1), Note{1});                // ordinal 2
    s.add_periodic(days(7), Tick{7});                        // ordinal 3
    s.add_periodic(days(1), Tick{0}, {.priority = -10});     // ordinal 4, but runs first
    s.schedule_at(Time{} + days(7), Note{7}, {.priority = 5});
    s.advance_to(Time{} + days(7) + seconds(1), rec);
    REQUIRE(rec.log.size() >= 6);
    CHECK(rec.log[0] == "0:T0");

    std::vector<std::string> at_day7;
    for (const auto& e : rec.log) {
        if (e.starts_with("604800:")) {
            at_day7.push_back(e);
        }
    }
    // Weekly registered after daily keeps its place behind daily on every recurrence.
    CHECK(at_day7 ==
          std::vector<std::string>{"604800:T0", "604800:T1", "604800:T7", "604800:N7"});
    CHECK(rec.log[3] == "86400:T0");
    CHECK(rec.log[4] == "86400:T1");
    CHECK(rec.log[5] == "86400:N1");
}

TEST_CASE("handlers may schedule events, including at the current instant") {
    Sched s;
    Recorder rec;
    s.schedule_at(Time{10}, Note{1});
    s.schedule_at(Time{10}, Note{2});
    auto handler = [&](Sched& sch, const Occ& o) {
        rec(sch, o);
        if (const auto* n = std::get_if<Note>(&o.payload)) {
            if (n->value == 1) {
                sch.schedule_in(seconds(0), Note{10});                  // same instant, FIFO tail
                sch.schedule_in(seconds(0), Note{11}, {.priority = -1}); // same instant, jumps
                sch.schedule_in(seconds(5), Note{12});
            }
        }
    };
    s.advance_to(Time{100}, handler);
    CHECK(rec.log == std::vector<std::string>{"10:N1", "10:N11", "10:N2", "10:N10", "15:N12"});
}

TEST_CASE("scheduling in the past or advancing backwards is rejected") {
    Sched s(Time{50});
    CHECK_THROWS_AS(s.schedule_at(Time{49}, Note{}), std::invalid_argument);
    CHECK_THROWS_AS(s.advance_to(Time{10}, Recorder{}), std::invalid_argument);
    CHECK_THROWS_AS(s.add_periodic(seconds(0), Tick{}), std::invalid_argument);
}

TEST_CASE("progress guard stops a same-instant scheduling loop") {
    Sched s;
    s.set_instant_fire_limit(100);
    s.schedule_at(Time{1}, Note{});
    auto forever = [](Sched& sch, const Occ&) { sch.schedule_in(seconds(0), Note{}); };
    CHECK_THROWS_AS(s.advance_to(Time{10}, forever), std::runtime_error);
    // The scheduler stays usable afterwards (not stuck in a re-entrancy state).
    CHECK(s.now() == Time{1});
    s.set_instant_fire_limit(1'000'000);
    CHECK(s.pending_event_count() == 1);
    CHECK(s.next_time() == Time{1});
    int fired = 0;
    s.advance_to(Time{10}, [&](Sched&, const Occ&) { ++fired; });
    CHECK(fired == 1);
    CHECK(s.pending_event_count() == 0);
}

TEST_CASE("re-entrant advance from a handler is rejected") {
    Sched s;
    s.schedule_at(Time{1}, Note{});
    auto bad = [](Sched& sch, const Occ&) { sch.advance_to(Time{5}, Recorder{}); };
    CHECK_THROWS_AS(s.advance_to(Time{10}, bad), std::logic_error);
}

TEST_CASE("cancellation") {
    Sched s;
    Recorder rec;
    const EventId a = s.schedule_at(Time{100}, Arrival{1});
    const EventId b = s.schedule_at(Time{200}, Arrival{2});
    CHECK(s.is_pending(a));
    CHECK(s.cancel(a));
    CHECK_FALSE(s.is_pending(a));
    CHECK_FALSE(s.cancel(a));
    CHECK_FALSE(s.cancel(EventId{}));

    // Slot reuse must not resurrect the stale id.
    const EventId c = s.schedule_at(Time{150}, Arrival{3});
    CHECK(c.slot == a.slot);
    CHECK_FALSE(s.is_pending(a));
    CHECK_FALSE(s.cancel(a));
    CHECK(s.is_pending(c));

    // A handler re-routes ship 3: cancel its other arrival and schedule a new one.
    auto handler = [&](Sched& sch, const Occ& o) {
        rec(sch, o);
        if (std::get<Arrival>(o.payload).ship == 3) {
            CHECK(sch.cancel(b));
            sch.schedule_in(seconds(10), Arrival{4});
        }
    };
    s.advance_to(Time{1000}, handler);
    CHECK(rec.log == std::vector<std::string>{"150:A3", "160:A4"});
    CHECK(s.pending_event_count() == 0);
    CHECK_FALSE(s.next_time().has_value());
}

TEST_CASE("mass cancellation compacts without changing order") {
    Sched s;
    std::vector<EventId> ids;
    for (int i = 0; i < 1000; ++i) {
        ids.push_back(s.schedule_at(Time{1000 - i % 7}, Note{i}));
    }
    for (int i = 0; i < 1000; ++i) {
        if (i % 3 != 0) {
            CHECK(s.cancel(ids[static_cast<std::size_t>(i)]));
        }
    }
    const auto pending = s.pending_events();
    REQUIRE(pending.size() == 334);
    std::vector<int> fired;
    s.advance_to(Time{2000},
                 [&](Sched&, const Occ& o) { fired.push_back(std::get<Note>(o.payload).value); });
    REQUIRE(fired.size() == pending.size());
    for (std::size_t i = 0; i < fired.size(); ++i) {
        CHECK(fired[i] == std::get<Note>(pending[i].payload).value);
        CHECK(fired[i] % 3 == 0);
    }
}

TEST_CASE("removing a periodic system, including from its own handler") {
    Sched s;
    int ticks = 0;
    PeriodicId self{};
    self = s.add_periodic(hours(1), Tick{1});
    auto handler = [&](Sched& sch, const Occ& o) {
        if (o.is_periodic()) {
            CHECK(o.periodic == self);
            if (++ticks == 3) {
                CHECK(sch.remove_periodic(o.periodic));
            }
        }
    };
    s.advance_by(days(1), handler);
    CHECK(ticks == 3);
    CHECK_FALSE(s.remove_periodic(self));
    CHECK_FALSE(s.next_time().has_value());
}

TEST_CASE("advance until an interesting event skips routine ticks") {
    Sched s;
    Recorder rec;
    s.add_periodic(hours(6), Tick{1});
    s.schedule_at(Time{} + days(3), Note{1});                       // routine, not flagged
    s.schedule_at(Time{} + days(10), Arrival{7}, {.flags = interesting});
    s.schedule_at(Time{} + days(10), Note{2});                      // same instant, after
    const Time limit = Time{} + days(30);

    auto r = s.advance_until(limit, rec, interesting);
    REQUIRE(r.stopped());
    CHECK(std::get<Arrival>(r.stopped_on->payload).ship == 7);
    CHECK(s.now() == Time{} + days(10));
    // Ticks at 0h..240h inclusive: the day-10 tick was registered first, so it precedes
    // the arrival at the same instant.
    CHECK(r.periodic_fired == 41);
    CHECK(rec.log.back() == "864000:A7");
    CHECK(s.pending_event_count() == 1); // Note{2} at the same instant still pending

    // Resuming continues the same instant, then runs to the limit with no stop.
    rec.log.clear();
    r = s.advance_until(limit, rec, interesting);
    CHECK_FALSE(r.stopped());
    CHECK(s.now() == limit);
    CHECK(rec.log.front() == "864000:N2");
    CHECK(r.events_fired == 1);
    CHECK(r.periodic_fired == 79); // (10d, 30d) at 6h
}

TEST_CASE("advance_until with a predicate and advance_until_next_event") {
    Sched s;
    s.add_periodic(days(1), Tick{1});
    s.schedule_at(Time{} + days(2) + hours(1), Arrival{5});
    auto noop = [](Sched&, const Occ&) {};

    auto r = s.advance_until(Time{} + days(10), noop,
                             [](const Occ& o) { return std::holds_alternative<Arrival>(o.payload); });
    REQUIRE(r.stopped());
    CHECK(s.now() == Time{} + days(2) + hours(1));
    CHECK(r.periodic_fired == 3);

    r = s.advance_until_next_event(Time{} + days(10), noop);
    REQUIRE(r.stopped());
    CHECK(r.stopped_on->is_periodic());
    CHECK(s.now() == Time{} + days(3));

    // Nothing before the limit: clock moves to the limit.
    Sched empty;
    r = empty.advance_until_next_event(Time{500}, noop);
    CHECK_FALSE(r.stopped());
    CHECK(empty.now() == Time{500});
}

namespace {

// A tiny deterministic "world" driven entirely by the scheduler.
std::uint64_t run_scenario() {
    Sched s;
    std::uint64_t hash = 1469598103934665603ull;
    auto mix = [&](std::int64_t v) {
        hash ^= static_cast<std::uint64_t>(v);
        hash *= 1099511628211ull;
    };
    s.add_periodic(hours(6), Tick{1});
    s.add_periodic(days(1), Tick{2}, {.anchor = Time{} + hours(3)});
    s.add_periodic(days(7), Tick{3}, {.priority = -1});
    std::vector<EventId> arrivals;
    for (std::uint32_t ship = 0; ship < 20; ++ship) {
        arrivals.push_back(s.schedule_at(Time{} + hours(5 * ship + 1), Arrival{ship}));
    }
    std::uint32_t next_ship = 100;
    auto handler = [&](Sched& sch, const Occ& o) {
        mix(o.time.seconds);
        mix(static_cast<std::int64_t>(o.payload.index()));
        if (const auto* a = std::get_if<Arrival>(&o.payload)) {
            mix(a->ship);
            if (a->ship % 3 == 0) {
                sch.schedule_in(days(static_cast<std::int64_t>(a->ship % 5) + 1), Arrival{next_ship++},
                                {.flags = interesting});
            }
            if (a->ship % 4 == 1 && a->ship + 1 < arrivals.size()) {
                sch.cancel(arrivals[a->ship + 1]);
            }
        } else if (const auto* t = std::get_if<Tick>(&o.payload)) {
            mix(t->system);
            if (t->system == 2) {
                sch.schedule_in(seconds(0), Note{static_cast<int>(o.time.seconds % 97)});
            }
        } else {
            mix(std::get<Note>(o.payload).value);
        }
    };
    // Mix of stop-at-interesting and fixed jumps, as a shell session would do.
    for (int i = 0; i < 5; ++i) {
        s.advance_until(s.now() + days(30), handler, interesting);
    }
    s.advance_by(days(90), handler);
    mix(s.now().seconds);
    return hash;
}

} // namespace

TEST_CASE("reproducibility: identical runs produce identical traces") {
    const auto a = run_scenario();
    const auto b = run_scenario();
    CHECK(a == b);
}
