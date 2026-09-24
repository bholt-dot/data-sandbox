#pragma once

// Typed tables with generational handles: the storage primitive for all simulation state.
//
// Layout (slot map / dense table, cf. WG21 P0661 slot_map):
//   slots_          sparse, indexed by Handle::index; holds generation + position in the dense arrays.
//   rows_           dense, contiguous Row structs (AoS); what systems iterate.
//   dense_to_slot_  dense, parallel to rows_; maps a row back to its slot for swap-and-pop and
//                   for recovering handles during iteration.
//
// Why AoS rows rather than struct-of-arrays: our tables are small (hundreds to thousands of rows)
// and most systems touch most fields of a row, so per-field arrays would add plumbing without a
// measurable cache win. The dense storage is private and only reached through rows()/iteration,
// so a table can later move to SoA (or split hot/cold columns) without changing handle semantics.
//
// Determinism guarantees (everything below depends only on the sequence of operations, never on
// addresses, hashing or allocator behaviour):
//   * Iteration order is dense order. insert() appends at the end; erase() moves the last row
//     into the erased position (swap-and-pop), so erase changes order, but reproducibly.
//   * Slot reuse is FIFO: the slot freed longest ago is reused first. FIFO (vs LIFO) spreads
//     generation bumps across slots so churn on one entity doesn't race a single slot toward
//     generation exhaustion.
//   * A slot whose generation would wrap is retired (never reused) instead of wrapping, so a
//     stale handle can never validate again (ABA protection without silent aliasing).
//   * clear() invalidates every outstanding handle and rebuilds the free list in slot order.
//
// Snapshot/hash friendliness: slots(), free_list_head(), rows() and dense_slots() expose the full
// internal state in a stable order, which is enough to hash or serialise a table and restore it
// bit-for-bit (including future handle allocation order).

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace sim {

// A generational reference to a row in a Table<Tag, Row>. Tag makes handles of different
// tables distinct types, so a ShipId can't be passed where a StationId is expected.
// Default-constructed handles are null; live slots never have generation 0.
template <typename Tag>
struct Handle {
    static constexpr std::uint32_t null_index = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t index = null_index;
    std::uint32_t generation = 0;

    static constexpr Handle null() { return {}; }
    constexpr bool is_null() const { return generation == 0; }
    constexpr explicit operator bool() const { return !is_null(); }

    constexpr auto operator<=>(const Handle&) const = default;
};

template <typename Tag, typename Row>
class Table {
public:
    using handle_type = Handle<Tag>;
    using row_type = Row;

    static constexpr std::uint32_t npos = std::numeric_limits<std::uint32_t>::max();

    // Per-slot metadata, exposed read-only for snapshotting/hashing.
    struct Slot {
        std::uint32_t generation = 1; // current generation; handles must match to be valid
        std::uint32_t dense = npos;   // position in rows_ when live, npos when free/retired
        std::uint32_t next_free = npos; // intrusive FIFO free-list link

        constexpr bool operator==(const Slot&) const = default;
    };

    template <typename R>
    struct Entry {
        handle_type handle;
        R& row;
    };

    template <typename R>
    class Iterator {
    public:
        using table_ptr = std::conditional_t<std::is_const_v<R>, const Table*, Table*>;

        Iterator(table_ptr table, std::size_t pos) : table_(table), pos_(pos) {}
        Entry<R> operator*() const { return {table_->handle_at(pos_), table_->rows_[pos_]}; }
        Iterator& operator++() { ++pos_; return *this; }
        Iterator operator++(int) { Iterator t = *this; ++pos_; return t; }
        bool operator==(const Iterator& o) const { return pos_ == o.pos_; }

    private:
        table_ptr table_;
        std::size_t pos_;
    };

    using iterator = Iterator<Row>;
    using const_iterator = Iterator<const Row>;

    // --- capacity ---

    std::size_t size() const { return rows_.size(); }
    bool empty() const { return rows_.empty(); }

    void reserve(std::size_t n) {
        rows_.reserve(n);
        dense_to_slot_.reserve(n);
        slots_.reserve(n);
    }

    // --- modifiers ---

    template <typename... Args>
    handle_type emplace(Args&&... args) {
        const std::uint32_t slot_index = acquire_slot();
        rows_.emplace_back(std::forward<Args>(args)...);
        dense_to_slot_.push_back(slot_index);
        Slot& s = slots_[slot_index];
        s.dense = static_cast<std::uint32_t>(rows_.size() - 1);
        return {slot_index, s.generation};
    }

    handle_type insert(const Row& row) { return emplace(row); }
    handle_type insert(Row&& row) { return emplace(std::move(row)); }

    // Returns false (and does nothing) for null/stale handles.
    bool erase(handle_type h) {
        if (!contains(h)) {
            return false;
        }
        erase_dense(slots_[h.index].dense);
        return true;
    }

    // Erases every row matching pred(handle, row). Walks dense order back to front so the row
    // swapped into an erased position has already been visited; result is deterministic.
    template <typename Pred>
    std::size_t erase_if(Pred pred) {
        std::size_t erased = 0;
        for (std::size_t i = rows_.size(); i-- > 0;) {
            if (pred(handle_at(i), static_cast<const Row&>(rows_[i]))) {
                erase_dense(static_cast<std::uint32_t>(i));
                ++erased;
            }
        }
        return erased;
    }

    // Invalidates all outstanding handles; free slots are then reused in ascending slot order.
    void clear() {
        rows_.clear();
        dense_to_slot_.clear();
        free_head_ = npos;
        free_tail_ = npos;
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& s = slots_[i];
            if (s.dense != npos) {
                s.dense = npos;
                if (!bump_generation(s)) {
                    continue;
                }
            } else if (is_retired(i)) {
                continue;
            }
            s.next_free = npos;
            push_free(i);
        }
    }

    // --- lookup ---

    bool contains(handle_type h) const {
        return h.index < slots_.size() && slots_[h.index].generation == h.generation &&
               slots_[h.index].dense != npos;
    }

    // nullptr for null/stale handles. The pointer is only valid until the next insert/erase.
    Row* get(handle_type h) { return contains(h) ? &rows_[slots_[h.index].dense] : nullptr; }
    const Row* get(handle_type h) const {
        return contains(h) ? &rows_[slots_[h.index].dense] : nullptr;
    }

    // Throws std::out_of_range for null/stale handles; for call sites where absence is a bug.
    Row& at(handle_type h) {
        if (Row* r = get(h)) {
            return *r;
        }
        throw std::out_of_range("sim::Table::at: stale or null handle");
    }
    const Row& at(handle_type h) const {
        if (const Row* r = get(h)) {
            return *r;
        }
        throw std::out_of_range("sim::Table::at: stale or null handle");
    }

    // --- dense iteration ---
    // Range-for yields Entry{handle, row}: `for (auto [id, row] : table)`.
    // Do not insert or erase while iterating; collect handles and erase afterwards, or use erase_if.

    iterator begin() { return {this, 0}; }
    iterator end() { return {this, rows_.size()}; }
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, rows_.size()}; }

    std::span<Row> rows() { return rows_; }
    std::span<const Row> rows() const { return rows_; }

    handle_type handle_at(std::size_t dense_index) const {
        const std::uint32_t slot = dense_to_slot_[dense_index];
        return {slot, slots_[slot].generation};
    }

    // --- raw state for snapshot/hash ---

    std::span<const std::uint32_t> dense_slots() const { return dense_to_slot_; }
    std::span<const Slot> slots() const { return slots_; }
    std::uint32_t free_list_head() const { return free_head_; }

private:
    // Retired slots have exhausted their generations: not live, not on the free list.
    static constexpr std::uint32_t retired_generation = std::numeric_limits<std::uint32_t>::max();

    bool is_retired(std::uint32_t i) const {
        return slots_[i].generation == retired_generation && slots_[i].dense == npos;
    }

    // Returns false if the slot was retired instead.
    static bool bump_generation(Slot& s) {
        if (s.generation == retired_generation - 1) {
            s.generation = retired_generation;
            return false;
        }
        ++s.generation;
        return true;
    }

    void push_free(std::uint32_t i) {
        if (free_tail_ == npos) {
            free_head_ = i;
        } else {
            slots_[free_tail_].next_free = i;
        }
        free_tail_ = i;
    }

    std::uint32_t acquire_slot() {
        if (free_head_ != npos) {
            const std::uint32_t i = free_head_;
            free_head_ = slots_[i].next_free;
            if (free_head_ == npos) {
                free_tail_ = npos;
            }
            slots_[i].next_free = npos;
            return i;
        }
        // index npos is reserved for the null handle.
        if (slots_.size() >= npos) {
            throw std::length_error("sim::Table: slot index space exhausted");
        }
        slots_.push_back(Slot{});
        return static_cast<std::uint32_t>(slots_.size() - 1);
    }

    void erase_dense(std::uint32_t pos) {
        const std::uint32_t slot_index = dense_to_slot_[pos];
        const std::uint32_t last = static_cast<std::uint32_t>(rows_.size() - 1);
        if (pos != last) {
            rows_[pos] = std::move(rows_[last]);
            dense_to_slot_[pos] = dense_to_slot_[last];
            slots_[dense_to_slot_[pos]].dense = pos;
        }
        rows_.pop_back();
        dense_to_slot_.pop_back();

        Slot& s = slots_[slot_index];
        s.dense = npos;
        if (bump_generation(s)) {
            push_free(slot_index);
        }
    }

    std::vector<Slot> slots_;
    std::vector<Row> rows_;
    std::vector<std::uint32_t> dense_to_slot_;
    std::uint32_t free_head_ = npos;
    std::uint32_t free_tail_ = npos;
};

} // namespace sim

template <typename Tag>
struct std::hash<sim::Handle<Tag>> {
    std::size_t operator()(const sim::Handle<Tag>& h) const noexcept {
        return std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(h.generation) << 32) | h.index);
    }
};
