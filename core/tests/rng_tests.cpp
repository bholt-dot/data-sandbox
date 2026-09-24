#include <doctest/doctest.h>

#include "simcore/rng.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <set>
#include <vector>

using namespace sim;

namespace {

std::vector<std::uint64_t> take(Rng r, std::size_t n) {
    std::vector<std::uint64_t> out(n);
    for (auto& v : out) {
        v = r.next_u64();
    }
    return out;
}

} // namespace

TEST_CASE("fnv1a64 matches reference vectors") {
    // Test vectors from the FNV reference (Fowler/Noll/Vo, isthe.com/chongo/tech/comp/fnv).
    static_assert(fnv1a64("") == 0xcbf29ce484222325ULL);
    CHECK(fnv1a64("a") == 0xaf63dc4c8601ec8cULL);
    CHECK(fnv1a64("foobar") == 0x85944171f73967e8ULL);
}

TEST_CASE("SplitMix64 matches reference output") {
    // Output of Vigna's splitmix64.c for x = 1477776061723855037 (as used by rand_xoshiro).
    SplitMix64 sm{1477776061723855037ULL};
    const std::array<std::uint64_t, 5> expected{
        1985237415132408290ULL, 2979275885539914483ULL, 13511426838097143398ULL,
        8488337342461049707ULL, 15141737807933549159ULL};
    for (auto e : expected) {
        CHECK(sm.next() == e);
    }
}

TEST_CASE("xoshiro256starstar matches reference output") {
    // Output of Vigna's xoshiro256starstar.c from state {1, 2, 3, 4} (as used by rand_xoshiro).
    Rng r{{1, 2, 3, 4}};
    const std::array<std::uint64_t, 10> expected{
        11520ULL, 0ULL, 1509978240ULL, 1215971899390074240ULL, 1216172134540287360ULL,
        607988272756665600ULL, 16172922978634559625ULL, 8476171486693032832ULL,
        10595114339597558777ULL, 2904607092377533576ULL};
    for (auto e : expected) {
        CHECK(r.next_u64() == e);
    }
}

TEST_CASE("xoshiro256starstar is usable at compile time") {
    constexpr auto first = [] {
        Rng r{{1, 2, 3, 4}};
        r.next_u64();
        r.next_u64();
        return r.next_u64();
    }();
    static_assert(first == 1509978240ULL);
}

TEST_CASE("rng_for golden values are stable") {
    // Pinned so any change to seeding/derivation (which would silently change every save and
    // replay) fails loudly. Cross-checked against an independent Python implementation.
    CHECK(take(rng_for(42, "economy"), 3) ==
          std::vector<std::uint64_t>{0x349371c4e5bcec9eULL, 0xc9a1ed1b7c313f52ULL,
                                     0x87820ed375d72102ULL});
    CHECK(take(rng_for(42, "economy", 7), 3) ==
          std::vector<std::uint64_t>{0xf4e089b0da16acceULL, 0x7b2047f9098d890cULL,
                                     0xd70cac34dbdfe979ULL});
    CHECK(take(rng_for(42, "traffic"), 3) ==
          std::vector<std::uint64_t>{0xf12d669b97597a7bULL, 0xa4249dc9fdfc0bc4ULL,
                                     0x8469cc9ae9c7021bULL});
    static_assert(rng_for(42, "economy") == rng_for(42, fnv1a64("economy")));
}

TEST_CASE("rng is a plain reproducible value") {
    static_assert(std::uniform_random_bit_generator<Rng>);
    static_assert(std::is_trivially_copyable_v<Rng>);

    Rng a = rng_for(7, "test");
    for (int i = 0; i < 100; ++i) {
        a.next_u64();
    }
    // Round-trip through the raw state, as save/load would.
    const std::array<std::uint64_t, 4> saved = a.s;
    Rng b{saved};
    CHECK(a == b);
    CHECK(take(a, 50) == take(b, 50));

    // Distributions carry no hidden state: copying mid-normal() sequence stays in lockstep.
    Rng c = rng_for(7, "normal");
    c.normal();
    Rng d = c;
    CHECK(c.normal() == d.normal());
    CHECK(c == d);
}

TEST_CASE("derived streams are independent") {
    // Same inputs => same stream, regardless of which other streams exist.
    CHECK(rng_for(1, "economy") == rng_for(1, "economy"));

    std::vector<Rng> streams{rng_for(1, "economy"), rng_for(1, "traffic"),
                             rng_for(2, "economy"), rng_for(1, "economy", 1),
                             rng_for(1, "economy", 2), rng_for(1, "")};
    // Adjacent seeds/indices are a classic source of correlation; check the first outputs share
    // no values and that roughly half the bits differ between paired outputs.
    std::set<std::uint64_t> seen;
    std::size_t total = 0;
    for (const auto& s : streams) {
        for (auto v : take(s, 1000)) {
            seen.insert(v);
            ++total;
        }
    }
    CHECK(seen.size() == total);

    for (std::size_t i = 0; i < streams.size(); ++i) {
        for (std::size_t j = i + 1; j < streams.size(); ++j) {
            const auto x = take(streams[i], 1000);
            const auto y = take(streams[j], 1000);
            int bits = 0;
            for (std::size_t k = 0; k < x.size(); ++k) {
                bits += std::popcount(x[k] ^ y[k]);
            }
            const double avg = static_cast<double>(bits) / 1000.0;
            CHECK(avg > 31.0);
            CHECK(avg < 33.0);
        }
    }

    // Many entity sub-streams: all distinct states.
    std::set<std::array<std::uint64_t, 4>> states;
    for (std::uint64_t e = 0; e < 10000; ++e) {
        states.insert(rng_for(99, "crew", e).s);
    }
    CHECK(states.size() == 10000);
}

TEST_CASE("from_seed never yields the all-zero state") {
    for (std::uint64_t seed : {0ULL, 1ULL, ~0ULL}) {
        const Rng r = Rng::from_seed(seed);
        CHECK(r.s != std::array<std::uint64_t, 4>{});
    }
}

TEST_CASE("uniform_int bounds") {
    Rng r = rng_for(3, "bounds");
    for (int i = 0; i < 10000; ++i) {
        const int v = r.uniform_int(-3, 4);
        CHECK(v >= -3);
        CHECK(v <= 4);
    }
    CHECK(r.uniform_int(5, 5) == 5);
    CHECK(r.uniform_int<std::int64_t>(-1, -1) == -1);

    // Full-width ranges must not overflow the span computation.
    constexpr auto lo64 = std::numeric_limits<std::int64_t>::min();
    constexpr auto hi64 = std::numeric_limits<std::int64_t>::max();
    bool saw_negative = false;
    bool saw_positive = false;
    for (int i = 0; i < 100; ++i) {
        const auto v = r.uniform_int(lo64, hi64);
        saw_negative = saw_negative || v < 0;
        saw_positive = saw_positive || v > 0;
    }
    CHECK(saw_negative);
    CHECK(saw_positive);
    CHECK(r.uniform_int(hi64 - 1, hi64) >= hi64 - 1);
    CHECK(r.uniform_int(lo64, lo64 + 1) <= lo64 + 1);

    std::set<std::uint8_t> bytes;
    for (int i = 0; i < 5000; ++i) {
        bytes.insert(r.uniform_int<std::uint8_t>(0, 255));
    }
    CHECK(bytes.size() == 256);

    std::array<int, 8> hits{};
    for (int i = 0; i < 1000; ++i) {
        ++hits[static_cast<std::size_t>(r.uniform_int(-3, 4) + 3)];
    }
    CHECK(std::ranges::all_of(hits, [](int h) { return h > 0; }));
}

TEST_CASE("uniform_int is unbiased") {
    Rng r = rng_for(4, "bias");

    // Small range: each face of a die within 2% of its expectation over 600k rolls
    // (~7 standard deviations; deterministic given the fixed seed).
    std::array<int, 6> faces{};
    constexpr int rolls = 600000;
    for (int i = 0; i < rolls; ++i) {
        ++faces[static_cast<std::size_t>(r.uniform_int(1, 6) - 1)];
    }
    for (int f : faces) {
        CHECK(std::abs(f - rolls / 6) < rolls / 6 / 50);
    }

    // Large bound where modulo reduction is grossly biased: with bound = 3 * 2^62,
    // `x % bound` lands in [0, 2^62) half the time instead of the correct third.
    constexpr std::uint64_t bound = 3ULL << 62;
    int low = 0;
    constexpr int n = 60000;
    for (int i = 0; i < n; ++i) {
        low += r.below(bound) < (1ULL << 62) ? 1 : 0;
    }
    CHECK(std::abs(low - n / 3) < n / 100);
}

TEST_CASE("uniform01 is 53-bit and half-open") {
    Rng r = rng_for(5, "unit");
    double sum = 0.0;
    constexpr int n = 100000;
    for (int i = 0; i < n; ++i) {
        const double u = r.uniform01();
        CHECK(u >= 0.0);
        CHECK(u < 1.0);
        const double scaled = u * 0x1.0p53;
        CHECK(scaled == std::floor(scaled));
        sum += u;
    }
    CHECK(sum / n == doctest::Approx(0.5).epsilon(0.01));

    // Extremes map exactly.
    Rng zero{{0, 0, 0, 0}}; // invalid as a generator, but next_u64() == 0 once
    CHECK(zero.uniform01() == 0.0);

    for (int i = 0; i < 1000; ++i) {
        const double v = r.uniform_real(-2.0, 3.0);
        CHECK(v >= -2.0);
        CHECK(v < 3.0);
    }
}

TEST_CASE("chance") {
    Rng r = rng_for(6, "chance");
    int hits = 0;
    constexpr int n = 100000;
    for (int i = 0; i < n; ++i) {
        CHECK_FALSE(r.chance(0.0));
        CHECK(r.chance(1.0));
        hits += r.chance(0.25) ? 1 : 0;
    }
    CHECK(std::abs(hits - n / 4) < n / 100);
}

TEST_CASE("pick_weighted") {
    Rng r = rng_for(8, "pick");
    constexpr int n = 100000;

    const std::array<std::uint64_t, 4> iw{1, 0, 3, 6};
    std::array<int, 4> ic{};
    for (int i = 0; i < n; ++i) {
        ++ic[r.pick_weighted(iw)];
    }
    CHECK(ic[1] == 0);
    CHECK(std::abs(ic[0] - n / 10) < n / 100);
    CHECK(std::abs(ic[2] - 3 * n / 10) < n / 100);
    CHECK(std::abs(ic[3] - 6 * n / 10) < n / 100);

    const std::array<double, 5> dw{0.0, 0.5, 0.0, 1.5, 0.0};
    std::array<int, 5> dc{};
    for (int i = 0; i < n; ++i) {
        ++dc[r.pick_weighted(std::span<const double>(dw))];
    }
    CHECK(dc[0] == 0);
    CHECK(dc[2] == 0);
    CHECK(dc[4] == 0);
    CHECK(std::abs(dc[1] - n / 4) < n / 100);

    const std::array<double, 1> single{2.0};
    CHECK(r.pick_weighted(std::span<const double>(single)) == 0);
}

TEST_CASE("normal distribution moments") {
    Rng r = rng_for(9, "normal");
    constexpr int n = 200000;
    double sum = 0.0;
    double sum_sq = 0.0;
    int within_one = 0;
    for (int i = 0; i < n; ++i) {
        const double x = r.normal();
        CHECK(std::isfinite(x));
        sum += x;
        sum_sq += x * x;
        within_one += std::abs(x) < 1.0 ? 1 : 0;
    }
    const double mean = sum / n;
    const double var = sum_sq / n - mean * mean;
    CHECK(std::abs(mean) < 0.01);
    CHECK(var == doctest::Approx(1.0).epsilon(0.02));
    CHECK(static_cast<double>(within_one) / n == doctest::Approx(0.6827).epsilon(0.01));

    double scaled = 0.0;
    for (int i = 0; i < 10000; ++i) {
        scaled += r.normal(100.0, 5.0);
    }
    CHECK(scaled / 10000.0 == doctest::Approx(100.0).epsilon(0.005));
}

TEST_CASE("shuffle is a deterministic permutation") {
    std::vector<int> a(52);
    std::iota(a.begin(), a.end(), 0);
    std::vector<int> b = a;

    Rng r1 = rng_for(10, "deck");
    Rng r2 = rng_for(10, "deck");
    r1.shuffle(a);
    r2.shuffle(b);
    CHECK(a == b);

    std::vector<int> sorted = a;
    std::ranges::sort(sorted);
    std::vector<int> expected(52);
    std::iota(expected.begin(), expected.end(), 0);
    CHECK(sorted == expected);
    CHECK(a != expected);

    // Every position reachable: first element of a 4-element shuffle is roughly uniform.
    std::array<int, 4> first{};
    Rng r = rng_for(10, "small");
    for (int i = 0; i < 40000; ++i) {
        std::array<int, 4> v{0, 1, 2, 3};
        r.shuffle(v);
        ++first[static_cast<std::size_t>(v[0])];
    }
    for (int f : first) {
        CHECK(std::abs(f - 10000) < 400);
    }

    std::vector<int> empty;
    r.shuffle(empty);
    CHECK(empty.empty());
}
