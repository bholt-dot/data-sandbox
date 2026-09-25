#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "simcore/time.hpp"

namespace sim {

// Hybrid discrete-event scheduler: one-shot events and periodic systems share a single
// timeline, so the clock jumps straight from one occurrence to the next.
//
// Everything the scheduler stores is plain data (`Payload`, typically a std::variant of
// small structs). Handlers are not stored; the caller passes one to each advance call and
// dispatches on the payload. This keeps pending state snapshot-able (no opaque closures)
// and keeps the scheduler free of references into world state.
//
// Ordering of occurrences is the total order (time, priority, ordinal):
//  - lower `priority` fires first at equal time;
//  - `ordinal` is a global counter taken when an event is scheduled or a periodic system is
//    registered (a periodic system keeps its ordinal for every recurrence). Equal
//    (time, priority) therefore fires FIFO by scheduling/registration order.
//
// Advancing is half-open: advance_to(T) fires every occurrence with time < T and leaves the
// clock at T. Occurrences at exactly now() may still be pending (e.g. scheduled by a
// command at the current time, or left after an early stop) and fire on the next advance.

using EventFlags = std::uint32_t;

struct EventId {
    std::uint32_t slot = 0;
    std::uint64_t seq = 0; // 0 = null id

    constexpr bool valid() const { return seq != 0; }
    constexpr auto operator<=>(const EventId&) const = default;

    static constexpr auto fields(auto& self) { return std::tie(self.slot, self.seq); }
};

struct PeriodicId {
    static constexpr std::uint32_t null_index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t index = null_index;

    constexpr bool valid() const { return index != null_index; }
    constexpr auto operator<=>(const PeriodicId&) const = default;

    static constexpr auto fields(auto& self) { return std::tie(self.index); }
};

struct EventOptions {
    std::int32_t priority = 0;
    EventFlags flags = 0;
};

struct PeriodicOptions {
    // Occurrences land on the grid anchor + k * period (k any integer), so cadence never
    // drifts and phase is expressed by the anchor (e.g. Time{} + hours(6) for "06:00 daily").
    Time anchor{};
    std::int32_t priority = 0;
    EventFlags flags = 0;
};

template <class Payload>
struct Occurrence {
    Time time;
    EventId event;       // valid for one-shot events
    PeriodicId periodic; // valid for periodic systems
    std::int32_t priority = 0;
    EventFlags flags = 0;
    Payload payload;

    bool is_periodic() const { return periodic.valid(); }
};

template <class Payload>
struct PendingEvent {
    EventId id;
    Time time;
    std::int32_t priority = 0;
    EventFlags flags = 0;
    Payload payload;

    static auto fields(auto& self) {
        return std::tie(self.id, self.time, self.priority, self.flags, self.payload);
    }
};

template <class Payload>
struct PeriodicInfo {
    PeriodicId id;
    Duration period;
    Time anchor;
    Time next;
    std::int32_t priority = 0;
    EventFlags flags = 0;
    bool active = false;
    Payload payload;
};

// Complete scheduler state, as captured by Scheduler::snapshot() and consumed by restore().
// Carries everything that influences future behaviour: the sequence counter (future EventIds and
// tie-break ordinals), the event slot free list (future EventId slots) and each periodic's
// ordinal, so a restored scheduler fires, and allocates ids, exactly like the original.
template <class Payload>
struct PeriodicState {
    Duration period;
    Time anchor;
    Time next;
    std::int32_t priority = 0;
    EventFlags flags = 0;
    std::uint64_t ordinal = 0;
    bool active = false;
    Payload payload;

    static auto fields(auto& self) {
        return std::tie(self.period, self.anchor, self.next, self.priority, self.flags,
                        self.ordinal, self.active, self.payload);
    }
};

template <class Payload>
struct SchedulerState {
    Time now;
    std::uint64_t next_seq = 1;
    std::uint64_t fires_at_instant = 0;
    std::uint32_t slot_count = 0;
    std::vector<std::uint32_t> free_slots;        // LIFO stack order, as used for reuse
    std::vector<PendingEvent<Payload>> events;    // by slot index
    std::vector<PeriodicState<Payload>> periodics; // index = PeriodicId::index

    static auto fields(auto& self) {
        return std::tie(self.now, self.next_seq, self.fires_at_instant, self.slot_count,
                        self.free_slots, self.events, self.periodics);
    }
};

template <class Payload>
struct AdvanceResult {
    std::uint64_t events_fired = 0;
    std::uint64_t periodic_fired = 0;
    // Set when the stop condition matched; the clock is then at that occurrence's time.
    std::optional<Occurrence<Payload>> stopped_on;

    bool stopped() const { return stopped_on.has_value(); }
};

template <std::copy_constructible Payload>
class Scheduler {
public:
    using OccurrenceT = Occurrence<Payload>;
    using Result = AdvanceResult<Payload>;

    explicit Scheduler(Time start = Time{}) : now_(start) {}

    Time now() const { return now_; }

    // --- one-shot events -------------------------------------------------------------

    EventId schedule_at(Time at, Payload payload, EventOptions opts = {}) {
        if (at < now_) {
            throw std::invalid_argument("Scheduler::schedule_at: time is in the past");
        }
        std::uint32_t slot_index;
        if (!free_slots_.empty()) {
            slot_index = free_slots_.back();
            free_slots_.pop_back();
        } else {
            slot_index = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
        }
        Slot& s = slots_[slot_index];
        s.seq = next_seq_++;
        s.time = at;
        s.priority = opts.priority;
        s.flags = opts.flags;
        s.payload.emplace(std::move(payload));
        ++live_events_;
        push({at, opts.priority, s.seq, slot_index, EntryKind::event});
        return EventId{slot_index, s.seq};
    }

    EventId schedule_in(Duration delay, Payload payload, EventOptions opts = {}) {
        return schedule_at(now_ + delay, std::move(payload), opts);
    }

    bool is_pending(EventId id) const {
        return id.valid() && id.slot < slots_.size() && slots_[id.slot].seq == id.seq;
    }

    // Lazy cancellation: the slot is released now; its heap entry becomes a tombstone that
    // is skipped when it reaches the top (or dropped by compaction).
    bool cancel(EventId id) {
        if (!is_pending(id)) {
            return false;
        }
        release_slot(id.slot);
        note_tombstone();
        return true;
    }

    std::size_t pending_event_count() const { return live_events_; }

    // Pending one-shot events in firing order. Intended for inspection and snapshots.
    std::vector<PendingEvent<Payload>> pending_events() const {
        std::vector<PendingEvent<Payload>> out;
        out.reserve(live_events_);
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            const Slot& s = slots_[i];
            if (s.seq != 0) {
                out.push_back({EventId{i, s.seq}, s.time, s.priority, s.flags, *s.payload});
            }
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return std::tie(a.time, a.priority, a.id.seq) < std::tie(b.time, b.priority, b.id.seq);
        });
        return out;
    }

    // --- periodic systems ------------------------------------------------------------

    // The first occurrence is the first grid point >= now() (it may be now() itself).
    PeriodicId add_periodic(Duration period, Payload payload, PeriodicOptions opts = {}) {
        if (period.seconds <= 0) {
            throw std::invalid_argument("Scheduler::add_periodic: period must be positive");
        }
        const auto index = static_cast<std::uint32_t>(periodics_.size());
        Periodic p{period, opts.anchor, first_grid_point_at_or_after(opts.anchor, period, now_),
                   opts.priority, opts.flags, next_seq_++, true, std::move(payload)};
        periodics_.push_back(std::move(p));
        const Periodic& ref = periodics_.back();
        push({ref.next, ref.priority, ref.ordinal, index, EntryKind::periodic});
        return PeriodicId{index};
    }

    bool remove_periodic(PeriodicId id) {
        if (!id.valid() || id.index >= periodics_.size() || !periodics_[id.index].active) {
            return false;
        }
        periodics_[id.index].active = false;
        note_tombstone();
        return true;
    }

    std::vector<PeriodicInfo<Payload>> periodics() const {
        std::vector<PeriodicInfo<Payload>> out;
        out.reserve(periodics_.size());
        for (std::uint32_t i = 0; i < periodics_.size(); ++i) {
            const Periodic& p = periodics_[i];
            out.push_back({PeriodicId{i}, p.period, p.anchor, p.next, p.priority, p.flags,
                           p.active, p.payload});
        }
        return out;
    }

    // --- advancing -------------------------------------------------------------------

    // Time of the next live occurrence (event or periodic), if any.
    std::optional<Time> next_time() {
        prune_top();
        if (heap_.empty()) {
            return std::nullopt;
        }
        return heap_.front().time;
    }

    // Fires everything with time < target in order; clock ends at target.
    template <class Handler>
    Result advance_to(Time target, Handler&& handler) {
        return run(target, handler, [](const OccurrenceT&) { return false; });
    }

    template <class Handler>
    Result advance_by(Duration d, Handler&& handler) {
        return advance_to(now_ + d, std::forward<Handler>(handler));
    }

    // Fires in order until an occurrence satisfying `stop` has fired (clock stays at its
    // time; later same-time occurrences remain pending), else behaves like advance_to(limit).
    template <class Handler, class StopPred>
        requires std::predicate<StopPred&, const Occurrence<Payload>&>
    Result advance_until(Time limit, Handler&& handler, StopPred&& stop) {
        return run(limit, handler, stop);
    }

    // Shell-style "advance until event": stop after the first occurrence whose flags
    // intersect `stop_mask`; routine occurrences without those bits are fired and skipped.
    template <class Handler>
    Result advance_until(Time limit, Handler&& handler, EventFlags stop_mask) {
        return run(limit, handler,
                   [stop_mask](const OccurrenceT& o) { return (o.flags & stop_mask) != 0; });
    }

    // Fires exactly the next occurrence if it is before `limit`, else advances to `limit`.
    template <class Handler>
    Result advance_until_next_event(Time limit, Handler&& handler) {
        return run(limit, handler, [](const OccurrenceT&) { return true; });
    }

    // --- snapshot ----------------------------------------------------------------------

    SchedulerState<Payload> snapshot() const {
        if (running_) {
            throw std::logic_error("Scheduler::snapshot: called from a handler");
        }
        SchedulerState<Payload> st;
        st.now = now_;
        st.next_seq = next_seq_;
        st.fires_at_instant = fires_at_instant_;
        st.slot_count = static_cast<std::uint32_t>(slots_.size());
        st.free_slots = free_slots_;
        st.events.reserve(live_events_);
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            const Slot& s = slots_[i];
            if (s.seq != 0) {
                st.events.push_back({EventId{i, s.seq}, s.time, s.priority, s.flags, *s.payload});
            }
        }
        st.periodics.reserve(periodics_.size());
        for (const Periodic& p : periodics_) {
            st.periodics.push_back({p.period, p.anchor, p.next, p.priority, p.flags, p.ordinal,
                                    p.active, p.payload});
        }
        return st;
    }

    // Replaces all state with `st`. Throws std::invalid_argument if `st` is inconsistent (the
    // scheduler is then unchanged). The instant fire limit is configuration and is kept.
    void restore(SchedulerState<Payload> st) {
        if (running_) {
            throw std::logic_error("Scheduler::restore: called from a handler");
        }
        auto invalid = [](const char* what) {
            throw std::invalid_argument(std::string("Scheduler::restore: ") + what);
        };
        if (st.next_seq == 0) {
            invalid("next_seq must be positive");
        }
        if (st.periodics.size() >= PeriodicId::null_index) {
            invalid("too many periodic systems");
        }
        // Checked before allocating so a corrupt slot_count cannot trigger a huge allocation.
        if (st.free_slots.size() + st.events.size() != st.slot_count) {
            invalid("every slot must be either pending or free");
        }
        std::vector<std::uint64_t> ordinals;
        ordinals.reserve(st.events.size() + st.periodics.size());

        std::vector<Slot> slots(st.slot_count);
        std::vector<Entry> heap;
        for (PendingEvent<Payload>& e : st.events) {
            if (e.id.slot >= slots.size() || slots[e.id.slot].seq != 0) {
                invalid("event slot out of range or duplicated");
            }
            if (e.time < st.now) {
                invalid("pending event is in the past");
            }
            ordinals.push_back(e.id.seq);
            Slot& s = slots[e.id.slot];
            s.seq = e.id.seq;
            s.time = e.time;
            s.priority = e.priority;
            s.flags = e.flags;
            s.payload.emplace(std::move(e.payload));
            heap.push_back({e.time, e.priority, e.id.seq, e.id.slot, EntryKind::event});
        }
        std::vector<bool> is_free(slots.size(), false);
        for (std::uint32_t i : st.free_slots) {
            if (i >= slots.size() || slots[i].seq != 0 || is_free[i]) {
                invalid("free slot out of range, occupied or duplicated");
            }
            is_free[i] = true;
        }

        std::vector<Periodic> periodics;
        periodics.reserve(st.periodics.size());
        for (PeriodicState<Payload>& p : st.periodics) {
            if (p.period.seconds <= 0) {
                invalid("periodic period must be positive");
            }
            if (p.active && p.next < st.now) {
                invalid("active periodic is in the past");
            }
            ordinals.push_back(p.ordinal);
            if (p.active) {
                heap.push_back({p.next, p.priority, p.ordinal,
                                static_cast<std::uint32_t>(periodics.size()), EntryKind::periodic});
            }
            periodics.push_back({p.period, p.anchor, p.next, p.priority, p.flags, p.ordinal,
                                 p.active, std::move(p.payload)});
        }

        std::sort(ordinals.begin(), ordinals.end());
        if (!ordinals.empty() && (ordinals.front() == 0 || ordinals.back() >= st.next_seq)) {
            invalid("sequence number out of range");
        }
        if (std::adjacent_find(ordinals.begin(), ordinals.end()) != ordinals.end()) {
            invalid("duplicate sequence number");
        }
        std::make_heap(heap.begin(), heap.end(), fires_later);

        now_ = st.now;
        next_seq_ = st.next_seq;
        fires_at_instant_ = st.fires_at_instant;
        live_events_ = st.events.size();
        tombstones_ = 0;
        heap_ = std::move(heap);
        slots_ = std::move(slots);
        free_slots_ = std::move(st.free_slots);
        periodics_ = std::move(periodics);
    }

    // Progress guard: a handler chain that keeps scheduling at the current instant would
    // otherwise never let time advance. Exceeding this many firings at one instant throws.
    void set_instant_fire_limit(std::uint64_t limit) { instant_fire_limit_ = limit; }

private:
    enum class EntryKind : std::uint8_t { event, periodic };

    struct Entry {
        Time time;
        std::int32_t priority;
        std::uint64_t ordinal;
        std::uint32_t index; // slot index (event) or periodic index
        EntryKind kind;
    };

    struct Slot {
        std::optional<Payload> payload;
        std::uint64_t seq = 0; // 0 = free
        Time time;
        std::int32_t priority = 0;
        EventFlags flags = 0;
    };

    struct Periodic {
        Duration period;
        Time anchor;
        Time next;
        std::int32_t priority;
        EventFlags flags;
        std::uint64_t ordinal;
        bool active;
        Payload payload;
    };

    // std heap algorithms build a max-heap; "fires later" as less-than yields the earliest on top.
    static bool fires_later(const Entry& a, const Entry& b) {
        return std::tie(a.time.seconds, a.priority, a.ordinal) >
               std::tie(b.time.seconds, b.priority, b.ordinal);
    }

    static Time first_grid_point_at_or_after(Time anchor, Duration period, Time t) {
        const std::int64_t diff = t.seconds - anchor.seconds;
        std::int64_t k = diff / period.seconds; // truncates toward zero
        if (diff % period.seconds > 0) {
            ++k; // positive remainder: truncation floored, round up
        }
        return anchor + period * k;
    }

    void push(const Entry& e) {
        heap_.push_back(e);
        std::push_heap(heap_.begin(), heap_.end(), fires_later);
    }

    Entry pop() {
        std::pop_heap(heap_.begin(), heap_.end(), fires_later);
        Entry e = heap_.back();
        heap_.pop_back();
        return e;
    }

    bool is_live(const Entry& e) const {
        if (e.kind == EntryKind::event) {
            return slots_[e.index].seq == e.ordinal;
        }
        const Periodic& p = periodics_[e.index];
        return p.active && p.next == e.time;
    }

    void release_slot(std::uint32_t slot_index) {
        Slot& s = slots_[slot_index];
        s.seq = 0;
        s.payload.reset();
        free_slots_.push_back(slot_index);
        --live_events_;
    }

    void note_tombstone() {
        ++tombstones_;
        // Compact when dead entries dominate so mass cancellation cannot bloat the heap.
        // The key is a total order, so rebuilding cannot change firing order.
        if (tombstones_ > 64 && tombstones_ * 2 > heap_.size()) {
            std::erase_if(heap_, [this](const Entry& e) { return !is_live(e); });
            std::make_heap(heap_.begin(), heap_.end(), fires_later);
            tombstones_ = 0;
        }
    }

    void prune_top() {
        while (!heap_.empty() && !is_live(heap_.front())) {
            pop();
            if (tombstones_ > 0) {
                --tombstones_;
            }
        }
    }

    void set_now(Time t) {
        if (t != now_) {
            now_ = t;
            fires_at_instant_ = 0;
        }
    }

    // Materializes the occurrence and retires/reschedules its entry *before* the handler
    // runs, so the handler sees consistent state and may cancel/remove/reschedule freely.
    OccurrenceT take(const Entry& e) {
        if (e.kind == EntryKind::event) {
            Slot& s = slots_[e.index];
            OccurrenceT occ{e.time, EventId{e.index, s.seq}, PeriodicId{}, s.priority, s.flags,
                            std::move(*s.payload)};
            release_slot(e.index);
            return occ;
        }
        Periodic& p = periodics_[e.index];
        OccurrenceT occ{e.time, EventId{}, PeriodicId{e.index}, p.priority, p.flags, p.payload};
        p.next = e.time + p.period;
        push({p.next, p.priority, p.ordinal, e.index, EntryKind::periodic});
        return occ;
    }

    template <class Handler, class StopPred>
    Result run(Time limit, Handler& handler, StopPred&& stop) {
        if (limit < now_) {
            throw std::invalid_argument("Scheduler: cannot advance backwards in time");
        }
        if (running_) {
            throw std::logic_error("Scheduler: advance called re-entrantly from a handler");
        }
        running_ = true;
        struct Reset {
            bool& flag;
            ~Reset() { flag = false; }
        } reset{running_};

        Result result;
        for (;;) {
            prune_top();
            if (heap_.empty() || heap_.front().time >= limit) {
                break;
            }
            set_now(heap_.front().time);
            // Checked before popping so the offending occurrence stays pending.
            if (fires_at_instant_ >= instant_fire_limit_) {
                throw std::runtime_error("Scheduler: instant fire limit exceeded (no progress)");
            }
            ++fires_at_instant_;
            const Entry e = pop();
            const OccurrenceT occ = take(e);
            std::invoke(handler, *this, occ);
            if (occ.is_periodic()) {
                ++result.periodic_fired;
            } else {
                ++result.events_fired;
            }
            if (std::invoke(stop, occ)) {
                result.stopped_on = occ;
                return result;
            }
        }
        set_now(limit);
        return result;
    }

    Time now_;
    std::uint64_t next_seq_ = 1;
    std::vector<Entry> heap_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slots_;
    std::vector<Periodic> periodics_;
    std::size_t live_events_ = 0;
    std::size_t tombstones_ = 0;
    std::uint64_t fires_at_instant_ = 0;
    std::uint64_t instant_fire_limit_ = 1'000'000;
    bool running_ = false;
};

} // namespace sim
