#pragma once

// Canonical binary encoding of simulation state, shared by snapshots (save/load) and state hashing.
//
// Encoding rules (fixed, platform-independent):
//   * integers: sizeof(T) bytes, little-endian, two's complement. Use <cstdint> types in state so
//     the width is the same on every platform (long / size_t are not).
//   * bool: one byte, 0 or 1 (anything else is rejected on load).
//   * float/double: the IEEE-754 bit pattern as u32/u64, so -0.0 and NaN payloads round-trip.
//   * enums: their underlying integer.
//   * string/vector/map: u64 count, then the elements. std::array: elements only.
//   * optional: u8 engaged flag, then the value. variant: u32 index, then the alternative.
//   * structs: their fields in the order listed by the type's field list (below).
// Nothing is memcpy'd as a whole struct, so padding bytes never reach a save or a hash.
//
// Customisation, in order of preference:
//   1. A field list, written once per type and driving both save/load and hashing:
//
//        struct Ship {
//            std::string name;
//            double mass = 0.0;
//            sim::Handle<StationTag> docked;
//            static auto fields(auto& self) { return std::tie(self.name, self.mass, self.docked); }
//        };
//
//      `self` is deduced as const for encoding and non-const for decoding, so one line serves
//      both. Adding a field to the struct without adding it here silently drops it from saves and
//      hashes, so review the list whenever the struct changes.
//   2. A full specialization of sim::Codec<T> for types that need custom logic or that you can't
//      edit (see Codec<Table> in snapshot.hpp):
//        template <> struct sim::Codec<T> {
//            template <class Sink> static void encode(Sink& s, const T& v);
//            static void decode(Reader& r, T& v);
//        };
//
// Deliberately unsupported: unordered containers (their iteration order is not canonical), raw
// pointers, long double.

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sim {

// Thrown for truncated, corrupt or otherwise unloadable data.
class SerializeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Anything that accepts the canonical byte stream: Writer (saves) and Hasher (state hashes).
template <class S>
concept ByteSink = requires(S& s, const std::uint8_t* p, std::size_t n) { s.write_bytes(p, n); };

class Writer {
public:
    void write_bytes(const std::uint8_t* p, std::size_t n) { buf_.insert(buf_.end(), p, p + n); }

    std::span<const std::uint8_t> bytes() const { return buf_; }
    std::vector<std::uint8_t> take() { return std::move(buf_); }
    std::size_t size() const { return buf_.size(); }

private:
    std::vector<std::uint8_t> buf_;
};

// Bounds-checked cursor over an encoded buffer. schema_version() is the game-side schema of the
// data being read, so decoders can branch on it when the format of a type evolves.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data, std::uint32_t schema_version = 0)
        : data_(data), schema_version_(schema_version) {}

    std::span<const std::uint8_t> read_bytes(std::size_t n) {
        if (n > remaining()) {
            fail("unexpected end of data (need " + std::to_string(n) + " bytes, have " +
                 std::to_string(remaining()) + ")");
        }
        const auto out = data_.subspan(pos_, n);
        pos_ += n;
        return out;
    }

    std::size_t remaining() const { return data_.size() - pos_; }
    std::size_t position() const { return pos_; }
    bool at_end() const { return pos_ == data_.size(); }
    std::uint32_t schema_version() const { return schema_version_; }

    void expect_end() const {
        if (!at_end()) {
            fail(std::to_string(remaining()) + " trailing bytes");
        }
    }

    [[noreturn]] void fail(const std::string& what) const {
        throw SerializeError("sim::Reader: " + what + " at offset " + std::to_string(pos_));
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    std::uint32_t schema_version_ = 0;
};

template <class T>
struct Codec; // specialised below for supported types

template <class T>
concept HasFields = requires(T& mut, const T& c) {
    T::fields(mut);
    T::fields(c);
};

template <class T>
concept Serializable = requires { sizeof(Codec<T>); };

template <ByteSink Sink, class T>
void encode(Sink& sink, const T& value) {
    static_assert(Serializable<T>,
                  "sim::encode: no Codec for this type; add `static auto fields(auto& self)` "
                  "or specialise sim::Codec<T>");
    Codec<T>::encode(sink, value);
}

template <class T>
void decode(Reader& r, T& value) {
    static_assert(Serializable<T>,
                  "sim::decode: no Codec for this type; add `static auto fields(auto& self)` "
                  "or specialise sim::Codec<T>");
    Codec<T>::decode(r, value);
}

template <class T>
    requires std::default_initializable<T>
T decode_as(Reader& r) {
    T v = T(); // not T{}: aggregate init would copy-list-init members (explicit ctors)
    decode(r, v);
    return v;
}

// Encodes/decodes `n` as the u64 element count used by strings and containers. Decoding
// rejects counts larger than the remaining input so corrupt lengths fail fast instead of
// allocating; every supported element type encodes to at least one byte.
template <ByteSink Sink>
void encode_count(Sink& sink, std::size_t n) {
    encode(sink, static_cast<std::uint64_t>(n));
}

inline std::size_t decode_count(Reader& r) {
    const auto n = decode_as<std::uint64_t>(r);
    if (n > r.remaining()) {
        r.fail("corrupt element count " + std::to_string(n));
    }
    return static_cast<std::size_t>(n);
}

// --- arithmetic & enums ------------------------------------------------------------------

template <class T>
    requires std::integral<T> && (!std::same_as<T, bool>)
struct Codec<T> {
    using U = std::make_unsigned_t<T>;

    template <ByteSink Sink>
    static void encode(Sink& sink, const T& v) {
        std::array<std::uint8_t, sizeof(T)> b{};
        auto u = static_cast<U>(v);
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            b[i] = static_cast<std::uint8_t>(u & 0xffu);
            if constexpr (sizeof(T) > 1) {
                u = static_cast<U>(u >> 8);
            }
        }
        sink.write_bytes(b.data(), b.size());
    }

    static void decode(Reader& r, T& v) {
        const auto b = r.read_bytes(sizeof(T));
        U u = 0;
        for (std::size_t i = sizeof(T); i-- > 0;) {
            if constexpr (sizeof(T) > 1) {
                u = static_cast<U>(u << 8);
            }
            u = static_cast<U>(u | b[i]);
        }
        v = static_cast<T>(u); // modular conversion, well-defined since C++20
    }
};

template <>
struct Codec<bool> {
    template <ByteSink Sink>
    static void encode(Sink& sink, const bool& v) {
        const std::uint8_t b = v ? 1 : 0;
        sink.write_bytes(&b, 1);
    }
    static void decode(Reader& r, bool& v) {
        const std::uint8_t b = r.read_bytes(1)[0];
        if (b > 1) {
            r.fail("invalid bool byte " + std::to_string(b));
        }
        v = b == 1;
    }
};

template <class T>
    requires std::same_as<T, float> || std::same_as<T, double>
struct Codec<T> {
    using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
    static_assert(sizeof(T) == sizeof(Bits) && std::numeric_limits<T>::is_iec559);

    template <ByteSink Sink>
    static void encode(Sink& sink, const T& v) {
        sim::encode(sink, std::bit_cast<Bits>(v));
    }
    static void decode(Reader& r, T& v) { v = std::bit_cast<T>(decode_as<Bits>(r)); }
};

template <class T>
    requires std::is_enum_v<T>
struct Codec<T> {
    using U = std::underlying_type_t<T>;

    template <ByteSink Sink>
    static void encode(Sink& sink, const T& v) {
        sim::encode(sink, static_cast<U>(v));
    }
    static void decode(Reader& r, T& v) { v = static_cast<T>(decode_as<U>(r)); }
};

// --- field-listed structs ----------------------------------------------------------------

template <HasFields T>
struct Codec<T> {
    template <ByteSink Sink>
    static void encode(Sink& sink, const T& v) {
        std::apply([&](const auto&... f) { (sim::encode(sink, f), ...); }, T::fields(v));
    }
    static void decode(Reader& r, T& v) {
        std::apply([&](auto&... f) { (sim::decode(r, f), ...); }, T::fields(v));
    }
};

// --- standard library ----------------------------------------------------------------------

template <>
struct Codec<std::string> {
    template <ByteSink Sink>
    static void encode(Sink& sink, const std::string& v) {
        encode_count(sink, v.size());
        // uint8_t is the byte type of the stream; chars are reinterpreted, not converted.
        sink.write_bytes(reinterpret_cast<const std::uint8_t*>(v.data()), v.size());
    }
    static void decode(Reader& r, std::string& v) {
        const auto b = r.read_bytes(decode_count(r));
        v.assign(reinterpret_cast<const char*>(b.data()), b.size());
    }
};

template <class T, class A>
struct Codec<std::vector<T, A>> {
    template <ByteSink Sink>
    static void encode(Sink& sink, const std::vector<T, A>& v) {
        encode_count(sink, v.size());
        for (const T& e : v) {
            sim::encode(sink, e);
        }
    }
    static void decode(Reader& r, std::vector<T, A>& v) {
        const std::size_t n = decode_count(r);
        v.clear();
        v.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            T e = T();
            sim::decode(r, e);
            v.push_back(std::move(e));
        }
    }
};

template <class T, std::size_t N>
struct Codec<std::array<T, N>> {
    template <ByteSink Sink>
    static void encode(Sink& sink, const std::array<T, N>& v) {
        for (const T& e : v) {
            sim::encode(sink, e);
        }
    }
    static void decode(Reader& r, std::array<T, N>& v) {
        for (T& e : v) {
            sim::decode(r, e);
        }
    }
};

template <class T>
struct Codec<std::optional<T>> {
    template <ByteSink Sink>
    static void encode(Sink& sink, const std::optional<T>& v) {
        sim::encode(sink, v.has_value());
        if (v) {
            sim::encode(sink, *v);
        }
    }
    static void decode(Reader& r, std::optional<T>& v) {
        if (decode_as<bool>(r)) {
            sim::decode(r, v.emplace());
        } else {
            v.reset();
        }
    }
};

template <class... Ts>
struct Codec<std::variant<Ts...>> {
    using V = std::variant<Ts...>;

    template <ByteSink Sink>
    static void encode(Sink& sink, const V& v) {
        if (v.valueless_by_exception()) {
            throw SerializeError("sim::encode: valueless variant");
        }
        sim::encode(sink, static_cast<std::uint32_t>(v.index()));
        std::visit([&](const auto& alt) { sim::encode(sink, alt); }, v);
    }

    static void decode(Reader& r, V& v) {
        const auto index = decode_as<std::uint32_t>(r);
        if (index >= sizeof...(Ts)) {
            r.fail("variant index " + std::to_string(index) + " out of range");
        }
        decode_alt(r, v, index, std::index_sequence_for<Ts...>{});
    }

private:
    template <std::size_t I>
    static void emplace_and_decode(Reader& r, V& v) {
        sim::decode(r, v.template emplace<I>());
    }

    template <std::size_t... Is>
    static void decode_alt(Reader& r, V& v, std::uint32_t index, std::index_sequence<Is...>) {
        using Fn = void (*)(Reader&, V&);
        static constexpr std::array<Fn, sizeof...(Ts)> table{&emplace_and_decode<Is>...};
        table[index](r, v);
    }
};

template <>
struct Codec<std::monostate> {
    template <ByteSink Sink>
    static void encode(Sink&, const std::monostate&) {}
    static void decode(Reader&, std::monostate&) {}
};

template <class A, class B>
struct Codec<std::pair<A, B>> {
    template <ByteSink Sink>
    static void encode(Sink& sink, const std::pair<A, B>& v) {
        sim::encode(sink, v.first);
        sim::encode(sink, v.second);
    }
    static void decode(Reader& r, std::pair<A, B>& v) {
        sim::decode(r, v.first);
        sim::decode(r, v.second);
    }
};

template <class... Ts>
struct Codec<std::tuple<Ts...>> {
    template <ByteSink Sink>
    static void encode(Sink& sink, const std::tuple<Ts...>& v) {
        std::apply([&](const auto&... e) { (sim::encode(sink, e), ...); }, v);
    }
    static void decode(Reader& r, std::tuple<Ts...>& v) {
        std::apply([&](auto&... e) { (sim::decode(r, e), ...); }, v);
    }
};

// Ordered map: iteration order is the key order, so the encoding is canonical. Loading requires
// strictly ascending keys, which also rejects duplicates.
template <class K, class T, class C, class A>
struct Codec<std::map<K, T, C, A>> {
    using M = std::map<K, T, C, A>;

    template <ByteSink Sink>
    static void encode(Sink& sink, const M& m) {
        encode_count(sink, m.size());
        for (const auto& [k, v] : m) {
            sim::encode(sink, k);
            sim::encode(sink, v);
        }
    }
    static void decode(Reader& r, M& m) {
        const std::size_t n = decode_count(r);
        m.clear();
        for (std::size_t i = 0; i < n; ++i) {
            K k = K();
            sim::decode(r, k);
            if (!m.empty() && !m.key_comp()(std::prev(m.end())->first, k)) {
                r.fail("map keys not strictly ascending");
            }
            T v = T();
            sim::decode(r, v);
            m.emplace_hint(m.end(), std::move(k), std::move(v));
        }
    }
};

} // namespace sim
