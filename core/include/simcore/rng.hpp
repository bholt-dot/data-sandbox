#pragma once

// Reproducible randomness for the simulation.
//
// Generator: xoshiro256** 1.0 (Blackman & Vigna), seeded through SplitMix64 as its authors
// recommend. All distributions below are our own, fully specified algorithms. The standard
// <random> distributions (std::uniform_int_distribution, std::normal_distribution, ...) are
// implementation-defined and produce different values on libstdc++/libc++/MSVC; they must never
// feed simulation results. Rng models std::uniform_random_bit_generator only so it can be
// handed to generic code whose output does not matter (e.g. test tooling).
//
// No global RNG: each system derives its own stream with rng_for(world_seed, "system", index),
// so adding a stream never perturbs the sequences of existing ones.

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace sim {

// FNV-1a 64-bit. Used for stable stream keys; std::hash is not stable across implementations.
constexpr std::uint64_t fnv1a64(std::string_view s) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

// SplitMix64 output function: a bijective 64-bit mixer.
constexpr std::uint64_t mix64(std::uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// SplitMix64 (Steele, Lea & Flood). Only used to expand seeds into xoshiro state.
struct SplitMix64 {
    std::uint64_t state = 0;

    constexpr std::uint64_t next() {
        state += 0x9e3779b97f4a7c15ULL;
        return mix64(state);
    }
};

namespace detail {

struct U128 {
    std::uint64_t hi;
    std::uint64_t lo;
};

// Portable 64x64 -> 128-bit multiply (no __int128, which -Wpedantic rejects and MSVC lacks).
constexpr U128 mul_64x64(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t a_lo = a & 0xffffffffULL;
    const std::uint64_t a_hi = a >> 32;
    const std::uint64_t b_lo = b & 0xffffffffULL;
    const std::uint64_t b_hi = b >> 32;
    const std::uint64_t ll = a_lo * b_lo;
    const std::uint64_t lh = a_lo * b_hi;
    const std::uint64_t hl = a_hi * b_lo;
    const std::uint64_t hh = a_hi * b_hi;
    const std::uint64_t mid = (ll >> 32) + (lh & 0xffffffffULL) + (hl & 0xffffffffULL);
    return {hh + (lh >> 32) + (hl >> 32) + (mid >> 32), (mid << 32) | (ll & 0xffffffffULL)};
}

} // namespace detail

// xoshiro256** generator plus portable distributions.
//
// A plain aggregate: the four state words are the complete state (distributions keep no hidden
// cache), so copying, serializing or hashing `s` captures the stream exactly. The all-zero state
// is invalid; from_seed() and rng_for() never produce it.
struct Rng {
    std::array<std::uint64_t, 4> s{};

    using result_type = std::uint64_t;
    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }

    static constexpr Rng from_seed(std::uint64_t seed) {
        SplitMix64 sm{seed};
        Rng r;
        for (auto& w : r.s) {
            w = sm.next();
        }
        return r;
    }

    constexpr bool operator==(const Rng&) const = default;

    // Field list for serialize.hpp / hash.hpp: the state words are the whole stream.
    static constexpr auto fields(auto& self) { return std::tie(self.s); }

    constexpr std::uint64_t next_u64() {
        const std::uint64_t result = std::rotl(s[1] * 5, 7) * 9;
        const std::uint64_t t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = std::rotl(s[3], 45);
        return result;
    }

    constexpr result_type operator()() { return next_u64(); }

    // Uniform in [0, bound), bound > 0. Lemire's multiply-shift with rejection: unbiased and
    // almost always division-free.
    constexpr std::uint64_t below(std::uint64_t bound) {
        assert(bound > 0);
        detail::U128 m = detail::mul_64x64(next_u64(), bound);
        if (m.lo < bound) {
            const std::uint64_t threshold = (0 - bound) % bound;
            while (m.lo < threshold) {
                m = detail::mul_64x64(next_u64(), bound);
            }
        }
        return m.hi;
    }

    // Uniform integer in the closed range [lo, hi].
    template <std::integral T>
    constexpr T uniform_int(T lo, T hi) {
        assert(lo <= hi);
        using U = std::make_unsigned_t<T>;
        const auto span = static_cast<std::uint64_t>(static_cast<U>(static_cast<U>(hi) - static_cast<U>(lo)));
        const std::uint64_t offset =
            span == std::numeric_limits<std::uint64_t>::max() ? next_u64() : below(span + 1);
        // Modular wrap back into T is well-defined since C++20.
        return static_cast<T>(static_cast<U>(static_cast<U>(lo) + static_cast<U>(offset)));
    }

    // Uniform double in [0, 1): the top 53 bits, so every value is an exact multiple of 2^-53.
    constexpr double uniform01() { return static_cast<double>(next_u64() >> 11) * 0x1.0p-53; }

    // Uniform double in [lo, hi).
    constexpr double uniform_real(double lo, double hi) { return lo + (hi - lo) * uniform01(); }

    // True with probability p (p <= 0 never, p >= 1 always). Always consumes one draw.
    constexpr bool chance(double p) { return uniform01() < p; }

    // Standard normal via the Marsaglia polar method, discarding the second variate so no
    // cached value lives outside `s`. std::log is not correctly rounded, so results are
    // reproducible per build/platform (our guarantee), not bit-identical across libms.
    double normal() {
        double u = 0.0;
        double v = 0.0;
        double q = 0.0;
        do {
            u = 2.0 * uniform01() - 1.0;
            v = 2.0 * uniform01() - 1.0;
            q = u * u + v * v;
        } while (q >= 1.0 || q == 0.0);
        return u * std::sqrt(-2.0 * std::log(q) / q);
    }

    double normal(double mean, double stddev) { return mean + stddev * normal(); }

    // Index chosen with probability weight[i] / sum(weights). Integer weights are exact.
    // Requires a non-empty span with a positive total that does not overflow.
    std::size_t pick_weighted(std::span<const std::uint64_t> weights) {
        std::uint64_t total = 0;
        for (auto w : weights) {
            total += w;
        }
        assert(total > 0);
        std::uint64_t r = below(total);
        for (std::size_t i = 0; i < weights.size(); ++i) {
            if (r < weights[i]) {
                return i;
            }
            r -= weights[i];
        }
        return weights.size() - 1; // unreachable
    }

    // Floating-point weights (non-negative, finite, positive total). Zero-weight entries are
    // never chosen, even when rounding pushes the draw past the running sum.
    std::size_t pick_weighted(std::span<const double> weights) {
        double total = 0.0;
        std::size_t last_positive = 0;
        for (std::size_t i = 0; i < weights.size(); ++i) {
            assert(weights[i] >= 0.0);
            total += weights[i];
            if (weights[i] > 0.0) {
                last_positive = i;
            }
        }
        assert(total > 0.0);
        const double r = uniform01() * total;
        double acc = 0.0;
        for (std::size_t i = 0; i < weights.size(); ++i) {
            acc += weights[i];
            if (r < acc && weights[i] > 0.0) {
                return i;
            }
        }
        return last_positive;
    }

    // Fisher-Yates (Durstenfeld) shuffle; unlike std::shuffle, the result is specified.
    template <std::ranges::random_access_range R>
    constexpr void shuffle(R&& items) {
        const auto first = std::ranges::begin(items);
        for (auto i = static_cast<std::uint64_t>(std::ranges::distance(items)); i > 1; --i) {
            const std::uint64_t j = below(i);
            std::ranges::iter_swap(first + static_cast<std::ptrdiff_t>(i - 1),
                                   first + static_cast<std::ptrdiff_t>(j));
        }
    }
};

static_assert(std::uniform_random_bit_generator<Rng>);
static_assert(std::is_trivially_copyable_v<Rng> && std::is_standard_layout_v<Rng>);
static_assert(sizeof(Rng) == 32);

// Stream for (world seed, system key, optional entity/sub-stream index). Each component goes
// through the bijective mixer before being combined, then the result seeds xoshiro via SplitMix64.
constexpr Rng rng_for(std::uint64_t world_seed, std::uint64_t key, std::uint64_t index = 0) {
    std::uint64_t h = mix64(world_seed + 0x9e3779b97f4a7c15ULL);
    h = mix64(h ^ key);
    h = mix64(h + index);
    return Rng::from_seed(h);
}

constexpr Rng rng_for(std::uint64_t world_seed, std::string_view key, std::uint64_t index = 0) {
    return rng_for(world_seed, fnv1a64(key), index);
}

} // namespace sim
