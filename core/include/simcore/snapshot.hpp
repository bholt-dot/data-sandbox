#pragma once

// Snapshots: codecs for the stateful core containers plus the on-disk envelope.
//
// Envelope layout (all little-endian):
//   magic "SIMS" | u32 format_version | u32 schema_version | u64 body_size | body | u64 xxh64(body)
// format_version covers this envelope and the primitive encoding in serialize.hpp; it only changes
// when core does. schema_version belongs to the game: bump it whenever a saved type's field list
// changes, and branch on Reader::schema_version() in decoders that must read older saves.
// The checksum catches corruption up front; decoders still bounds-check everything.

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "simcore/hash.hpp"
#include "simcore/scheduler.hpp"
#include "simcore/serialize.hpp"
#include "simcore/table.hpp"

namespace sim {

// Table: generations of every slot, dense order, rows, free list in reuse order. Enough to
// rebuild it via Table::restore so that future inserts allocate identical handles.
template <class Tag, class Row>
struct Codec<Table<Tag, Row>> {
    using T = Table<Tag, Row>;

    template <ByteSink Sink>
    static void encode(Sink& sink, const T& t) {
        const auto slots = t.slots();
        encode_count(sink, slots.size());
        for (const auto& s : slots) {
            sim::encode(sink, s.generation);
        }
        const auto dense = t.dense_slots();
        encode_count(sink, dense.size());
        for (std::size_t i = 0; i < dense.size(); ++i) {
            sim::encode(sink, dense[i]);
            sim::encode(sink, t.rows()[i]);
        }
        sim::encode(sink, t.free_list());
    }

    static void decode(Reader& r, T& t) {
        std::vector<std::uint32_t> generations(decode_count(r));
        for (auto& g : generations) {
            sim::decode(r, g);
        }
        const std::size_t live = decode_count(r);
        std::vector<std::uint32_t> dense(live);
        std::vector<Row> rows(live);
        for (std::size_t i = 0; i < live; ++i) {
            sim::decode(r, dense[i]);
            sim::decode(r, rows[i]);
        }
        const auto free_list = decode_as<std::vector<std::uint32_t>>(r);
        try {
            t.restore(generations, dense, std::move(rows), free_list);
        } catch (const std::invalid_argument& e) {
            r.fail(e.what());
        }
    }
};

template <class Payload>
struct Codec<Scheduler<Payload>> {
    template <ByteSink Sink>
    static void encode(Sink& sink, const Scheduler<Payload>& s) {
        sim::encode(sink, s.snapshot());
    }

    static void decode(Reader& r, Scheduler<Payload>& s) {
        auto st = decode_as<SchedulerState<Payload>>(r);
        try {
            s.restore(std::move(st));
        } catch (const std::invalid_argument& e) {
            r.fail(e.what());
        }
    }
};

inline constexpr std::array<std::uint8_t, 4> snapshot_magic{'S', 'I', 'M', 'S'};
inline constexpr std::uint32_t snapshot_format_version = 1;

// write_body(Writer&) encodes the game state. Returns the complete envelope.
template <class WriteBody>
    requires std::invocable<WriteBody&, Writer&>
std::vector<std::uint8_t> save_snapshot(std::uint32_t schema_version, WriteBody&& write_body) {
    Writer body;
    write_body(body);

    Writer out;
    out.write_bytes(snapshot_magic.data(), snapshot_magic.size());
    encode(out, snapshot_format_version);
    encode(out, schema_version);
    encode_count(out, body.size());
    out.write_bytes(body.bytes().data(), body.size());
    encode(out, xxh64(body.bytes()));
    return out.take();
}

// Validates the envelope, then calls read_body(Reader&) on the body. The reader's
// schema_version() is the saved one; saves newer than current_schema_version are rejected.
// read_body must consume the body exactly. Throws SerializeError on any problem.
template <class ReadBody>
    requires std::invocable<ReadBody&, Reader&>
void load_snapshot(std::span<const std::uint8_t> bytes, std::uint32_t current_schema_version,
                   ReadBody&& read_body) {
    Reader header(bytes);
    const auto magic = header.read_bytes(snapshot_magic.size());
    if (!std::equal(magic.begin(), magic.end(), snapshot_magic.begin())) {
        header.fail("not a snapshot (bad magic)");
    }
    const auto format = decode_as<std::uint32_t>(header);
    if (format != snapshot_format_version) {
        header.fail("unsupported snapshot format version " + std::to_string(format));
    }
    const auto schema = decode_as<std::uint32_t>(header);
    if (schema > current_schema_version) {
        header.fail("snapshot schema version " + std::to_string(schema) +
                    " is newer than supported " + std::to_string(current_schema_version));
    }
    const auto body_size = decode_as<std::uint64_t>(header);
    if (body_size > header.remaining() || header.remaining() - body_size != sizeof(std::uint64_t)) {
        header.fail("snapshot size mismatch (truncated or trailing data)");
    }
    const auto body = header.read_bytes(static_cast<std::size_t>(body_size));
    const auto checksum = decode_as<std::uint64_t>(header);
    if (checksum != xxh64(body)) {
        header.fail("snapshot checksum mismatch");
    }

    Reader r(body, schema);
    read_body(r);
    r.expect_end();
}

// Convenience for a single state object (typically a struct with a field list naming every
// table, scheduler and RNG stream of the world).
template <class State>
    requires(!std::invocable<const State&, Writer&>)
std::vector<std::uint8_t> save_snapshot(std::uint32_t schema_version, const State& state) {
    return save_snapshot(schema_version, [&](Writer& w) { encode(w, state); });
}

// Decodes into a fresh object and only then assigns, so `state` is untouched on failure.
template <class State>
    requires(!std::invocable<State&, Reader&>)
void load_snapshot(std::span<const std::uint8_t> bytes, std::uint32_t current_schema_version,
                   State& state) {
    State loaded = State();
    load_snapshot(bytes, current_schema_version, [&](Reader& r) { decode(r, loaded); });
    state = std::move(loaded);
}

} // namespace sim
