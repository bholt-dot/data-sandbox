#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "simcore/hash.hpp"
#include "simcore/serialize.hpp"
#include "simcore/table.hpp"
#include "simcore/time.hpp"

using namespace sim;

namespace {

enum class Kind : std::uint8_t { hauler = 1, tug = 2 };
struct StationTag {};

struct Cargo {
    std::string commodity;
    std::int32_t units = 0;
    bool operator==(const Cargo&) const = default;
    static auto fields(auto& self) { return std::tie(self.commodity, self.units); }
};

struct Ship {
    std::string name;
    Kind kind = Kind::hauler;
    double mass = 0.0;
    float heat = 0.0f;
    Handle<StationTag> docked;
    Time eta;
    std::optional<Duration> burn;
    std::vector<Cargo> hold;
    std::array<std::int16_t, 3> crew{};
    std::variant<std::monostate, Cargo, std::uint64_t> slot;
    std::map<std::string, std::int64_t> ledger;
    std::pair<bool, std::uint8_t> flags;
    std::tuple<std::int8_t, char> misc;

    bool operator==(const Ship&) const = default;
    static auto fields(auto& self) {
        return std::tie(self.name, self.kind, self.mass, self.heat, self.docked, self.eta, self.burn,
                        self.hold, self.crew, self.slot, self.ledger, self.flags, self.misc);
    }
};

Ship sample_ship() {
    Ship s;
    s.name = "Rocinante";
    s.kind = Kind::tug;
    s.mass = 1.25e6;
    s.heat = -3.5f;
    s.docked = {7, 3};
    s.eta = Time{123456789};
    s.burn = hours(3);
    s.hold = {{"water", 40}, {"ore", -2}};
    s.crew = {1, -2, 3};
    s.slot = Cargo{"he3", 9};
    s.ledger = {{"fuel", -500}, {"wages", -1200}};
    s.flags = {true, 200};
    s.misc = {-5, 'x'};
    return s;
}

template <class T>
std::vector<std::uint8_t> bytes_of(const T& v) {
    Writer w;
    encode(w, v);
    return w.take();
}

template <class T>
T round_trip(const T& v) {
    const auto b = bytes_of(v);
    Reader r(b);
    T out{};
    decode(r, out);
    r.expect_end();
    return out;
}

} // namespace

TEST_CASE("integers encode as fixed width little endian") {
    CHECK(bytes_of(std::uint32_t{0x01020304}) == std::vector<std::uint8_t>{4, 3, 2, 1});
    CHECK(bytes_of(std::int16_t{-2}) == std::vector<std::uint8_t>{0xfe, 0xff});
    CHECK(bytes_of(std::uint8_t{7}) == std::vector<std::uint8_t>{7});
    CHECK(bytes_of(Kind::tug) == std::vector<std::uint8_t>{2});
    CHECK(bytes_of(std::int64_t{-1}) == std::vector<std::uint8_t>(8, 0xff));
    CHECK(bytes_of(true) == std::vector<std::uint8_t>{1});
    CHECK(bytes_of(std::string("ab")) ==
          std::vector<std::uint8_t>{2, 0, 0, 0, 0, 0, 0, 0, 'a', 'b'});
    CHECK(bytes_of(1.0) == std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0xf0, 0x3f});
}

TEST_CASE("integer extremes round trip") {
    CHECK(round_trip(std::numeric_limits<std::int64_t>::min()) ==
          std::numeric_limits<std::int64_t>::min());
    CHECK(round_trip(std::numeric_limits<std::uint64_t>::max()) ==
          std::numeric_limits<std::uint64_t>::max());
    CHECK(round_trip(std::numeric_limits<std::int32_t>::min()) ==
          std::numeric_limits<std::int32_t>::min());
    CHECK(round_trip(std::int8_t{-128}) == std::int8_t{-128});
}

TEST_CASE("doubles round trip by bit pattern") {
    const double neg_zero = -0.0;
    CHECK(std::signbit(round_trip(neg_zero)));
    CHECK(std::isnan(round_trip(std::numeric_limits<double>::quiet_NaN())));
    CHECK(round_trip(std::numeric_limits<double>::infinity()) ==
          std::numeric_limits<double>::infinity());
    CHECK(round_trip(0.1) == 0.1);
    CHECK(round_trip(std::numeric_limits<double>::denorm_min()) ==
          std::numeric_limits<double>::denorm_min());
    // Documented choice: no normalisation, so -0.0 and 0.0 hash differently.
    CHECK(hash_state(0.0) != hash_state(neg_zero));
}

TEST_CASE("field listed struct round trips with nested std types") {
    const Ship s = sample_ship();
    CHECK(round_trip(s) == s);

    Ship empty;
    CHECK(round_trip(empty) == empty);

    Ship variant_int = s;
    variant_int.slot = std::uint64_t{42};
    variant_int.burn.reset();
    CHECK(round_trip(variant_int) == variant_int);
}

TEST_CASE("state hash is stable across round trip and sensitive to single fields") {
    const Ship s = sample_ship();
    CHECK(hash_state(s) == hash_state(round_trip(s)));
    CHECK(hash_state(s) == xxh64(bytes_of(s)));

    auto differs = [&](auto mutate) {
        Ship t = s;
        mutate(t);
        return hash_state(t) != hash_state(s);
    };
    CHECK(differs([](Ship& t) { t.name[0] = 'r'; }));
    CHECK(differs([](Ship& t) { t.mass = std::nextafter(t.mass, 0.0); }));
    CHECK(differs([](Ship& t) { t.docked.generation++; }));
    CHECK(differs([](Ship& t) { t.eta.seconds++; }));
    CHECK(differs([](Ship& t) { t.hold[1].units = 2; }));
    CHECK(differs([](Ship& t) { t.crew[2] = 4; }));
    CHECK(differs([](Ship& t) { t.slot = std::monostate{}; }));
    CHECK(differs([](Ship& t) { t.ledger["fuel"] = -501; }));
    CHECK(differs([](Ship& t) { t.flags.first = false; }));
    // Length prefixes keep ("ab","c") and ("a","bc") apart.
    CHECK(hash_state(std::string("ab"), std::string("c")) !=
          hash_state(std::string("a"), std::string("bc")));
}

TEST_CASE("xxh64 matches reference vectors") {
    CHECK(xxh64(std::string_view{}) == 0xef46db3751d8e999ULL);
    CHECK(xxh64("a") == 0xd24ec4f1a98c6e5bULL);
    CHECK(xxh64("abc") == 0x44bc2cf5ad770999ULL);
    CHECK(xxh64("0123456789abcdef0123456789abcdef") == 0x642a94958e71e6c5ULL);
    CHECK(xxh64("Nobody inspects the spammish repetition") == 0xfbcea83c8a378bf1ULL);
    CHECK(xxh64("abc", 12345) == 0x01700e64f6f23509ULL);

    std::vector<std::uint8_t> hundred(100);
    for (std::size_t i = 0; i < hundred.size(); ++i) {
        hundred[i] = static_cast<std::uint8_t>(i);
    }
    CHECK(xxh64(hundred) == 0x6ac1e58032166597ULL);
    CHECK(xxh64(hundred, 12345) == 0x028ba1ae2de4de27ULL);

    // Streaming in uneven pieces gives the one-shot result.
    for (std::size_t chunk : {1u, 3u, 7u, 31u, 32u, 33u}) {
        Hasher h;
        for (std::size_t i = 0; i < hundred.size(); i += chunk) {
            h.write_bytes(hundred.data() + i, std::min(chunk, hundred.size() - i));
        }
        CHECK(h.digest() == 0x6ac1e58032166597ULL);
    }
    static_assert(xxh64(std::span<const std::uint8_t>{}) == 0xef46db3751d8e999ULL);
}

TEST_CASE("truncated input is rejected at every cut point") {
    const auto b = bytes_of(sample_ship());
    for (std::size_t n = 0; n < b.size(); ++n) {
        Reader r{std::span(b).first(n)};
        Ship out;
        CHECK_THROWS_AS(decode(r, out), SerializeError);
    }
}

TEST_CASE("corrupt input is rejected with a clear error") {
    SUBCASE("invalid bool") {
        const std::vector<std::uint8_t> b{2};
        Reader r(b);
        bool v = false;
        CHECK_THROWS_WITH_AS(decode(r, v), doctest::Contains("invalid bool"), SerializeError);
    }
    SUBCASE("variant index out of range") {
        std::vector<std::uint8_t> b = bytes_of(std::variant<std::int32_t, float>{1.0f});
        b[0] = 9;
        Reader r(b);
        std::variant<std::int32_t, float> v;
        CHECK_THROWS_WITH_AS(decode(r, v), doctest::Contains("variant index"), SerializeError);
    }
    SUBCASE("huge length does not allocate") {
        Writer w;
        encode(w, std::uint64_t{1} << 60);
        const auto b = w.take();
        Reader r(b);
        std::vector<std::uint64_t> v;
        CHECK_THROWS_WITH_AS(decode(r, v), doctest::Contains("corrupt element count"),
                             SerializeError);
    }
    SUBCASE("unsorted map keys") {
        Writer w;
        encode_count(w, 2);
        encode(w, std::int32_t{5});
        encode(w, std::int32_t{0});
        encode(w, std::int32_t{3});
        encode(w, std::int32_t{0});
        const auto b = w.take();
        Reader r(b);
        std::map<std::int32_t, std::int32_t> m;
        CHECK_THROWS_WITH_AS(decode(r, m), doctest::Contains("ascending"), SerializeError);
    }
    SUBCASE("trailing bytes") {
        const std::vector<std::uint8_t> b{1, 0};
        Reader r(b);
        std::uint8_t v = 0;
        decode(r, v);
        CHECK_THROWS_WITH_AS(r.expect_end(), doctest::Contains("trailing"), SerializeError);
    }
}
