#pragma once

// Deterministic state hashing for reproducibility checks.
//
// The hash consumes exactly the canonical byte stream that serialize.hpp produces for saves, so
// hash_state(x) == hash_state(load(save(x))) holds by construction and every type that can be saved
// can be hashed with no extra code.
//
// Algorithm: XXH64 (Yann Collet), implemented here from the spec (streaming variant) and checked
// against reference vectors. It is portable (defined on bytes, no endian or alignment
// dependence), fast, and has a streaming form, which rapidhash/wyhash lack. std::hash is never
// used: its values are implementation-defined.
//
// Doubles are hashed by bit pattern with no normalisation: -0.0 and 0.0 hash differently (they
// can diverge later, e.g. via atan2 or 1/x), and so do NaNs with different payloads. This is the
// strict choice a determinism check wants: two states hash equal only if they serialise
// identically.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "simcore/serialize.hpp"

namespace sim {

// Streaming XXH64; a ByteSink, so sim::encode(hasher, value) hashes a value.
class Hasher {
public:
    explicit constexpr Hasher(std::uint64_t seed = 0)
        : acc_{seed + p1 + p2, seed + p2, seed, seed - p1}, seed_(seed) {}

    constexpr void write_bytes(const std::uint8_t* p, std::size_t n) {
        total_ += n;
        if (buffered_ + n < stripe) {
            for (std::size_t i = 0; i < n; ++i) {
                buf_[buffered_ + i] = p[i];
            }
            buffered_ += n;
            return;
        }
        if (buffered_ > 0) {
            const std::size_t fill = stripe - buffered_;
            for (std::size_t i = 0; i < fill; ++i) {
                buf_[buffered_ + i] = p[i];
            }
            consume_stripe(buf_.data());
            p += fill;
            n -= fill;
            buffered_ = 0;
        }
        while (n >= stripe) {
            consume_stripe(p);
            p += stripe;
            n -= stripe;
        }
        for (std::size_t i = 0; i < n; ++i) {
            buf_[i] = p[i];
        }
        buffered_ = n;
    }

    constexpr void write_bytes(std::span<const std::uint8_t> bytes) {
        write_bytes(bytes.data(), bytes.size());
    }

    // Digest of everything written so far; the hasher can keep accepting input afterwards.
    constexpr std::uint64_t digest() const {
        std::uint64_t h = 0;
        if (total_ >= stripe) {
            h = std::rotl(acc_[0], 1) + std::rotl(acc_[1], 7) + std::rotl(acc_[2], 12) +
                std::rotl(acc_[3], 18);
            for (std::uint64_t a : acc_) {
                h = merge_round(h, a);
            }
        } else {
            h = seed_ + p5;
        }
        h += total_;

        std::size_t i = 0;
        for (; i + 8 <= buffered_; i += 8) {
            h ^= round(0, read_le64(&buf_[i]));
            h = std::rotl(h, 27) * p1 + p4;
        }
        if (i + 4 <= buffered_) {
            h ^= static_cast<std::uint64_t>(read_le32(&buf_[i])) * p1;
            h = std::rotl(h, 23) * p2 + p3;
            i += 4;
        }
        for (; i < buffered_; ++i) {
            h ^= static_cast<std::uint64_t>(buf_[i]) * p5;
            h = std::rotl(h, 11) * p1;
        }

        h ^= h >> 33;
        h *= p2;
        h ^= h >> 29;
        h *= p3;
        h ^= h >> 32;
        return h;
    }

private:
    static constexpr std::uint64_t p1 = 0x9E3779B185EBCA87ULL;
    static constexpr std::uint64_t p2 = 0xC2B2AE3D27D4EB4FULL;
    static constexpr std::uint64_t p3 = 0x165667B19E3779F9ULL;
    static constexpr std::uint64_t p4 = 0x85EBCA77C2B2AE63ULL;
    static constexpr std::uint64_t p5 = 0x27D4EB2F165667C5ULL;
    static constexpr std::size_t stripe = 32;

    static constexpr std::uint64_t round(std::uint64_t acc, std::uint64_t input) {
        acc += input * p2;
        acc = std::rotl(acc, 31);
        return acc * p1;
    }

    static constexpr std::uint64_t merge_round(std::uint64_t acc, std::uint64_t val) {
        acc ^= round(0, val);
        return acc * p1 + p4;
    }

    static constexpr std::uint64_t read_le64(const std::uint8_t* p) {
        std::uint64_t v = 0;
        for (std::size_t i = 8; i-- > 0;) {
            v = (v << 8) | static_cast<std::uint64_t>(p[i]);
        }
        return v;
    }

    static constexpr std::uint32_t read_le32(const std::uint8_t* p) {
        std::uint32_t v = 0;
        for (std::size_t i = 4; i-- > 0;) {
            v = (v << 8) | static_cast<std::uint32_t>(p[i]);
        }
        return v;
    }

    constexpr void consume_stripe(const std::uint8_t* p) {
        for (std::size_t lane = 0; lane < 4; ++lane) {
            acc_[lane] = round(acc_[lane], read_le64(p + lane * 8));
        }
    }

    std::array<std::uint64_t, 4> acc_;
    std::array<std::uint8_t, stripe> buf_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_ = 0;
    std::uint64_t seed_;
};

static_assert(ByteSink<Hasher>);

constexpr std::uint64_t xxh64(std::span<const std::uint8_t> bytes, std::uint64_t seed = 0) {
    Hasher h(seed);
    h.write_bytes(bytes);
    return h.digest();
}

inline std::uint64_t xxh64(std::string_view s, std::uint64_t seed = 0) {
    Hasher h(seed);
    h.write_bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    return h.digest();
}

// Hash of the canonical encoding of all `parts`, in order. Equivalent to xxh64 over the bytes
// sim::encode would write for them.
template <class... Ts>
std::uint64_t hash_state(const Ts&... parts) {
    Hasher h;
    (encode(h, parts), ...);
    return h.digest();
}

} // namespace sim
