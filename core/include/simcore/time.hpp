#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <tuple>

namespace sim {

// Simulation time is integral seconds so event ordering is exact and reproducible.
// Continuous models (e.g. orbits) convert to double at the boundary via to_seconds_f().
struct Duration {
    std::int64_t seconds = 0;

    constexpr auto operator<=>(const Duration&) const = default;
    constexpr Duration operator+(Duration o) const { return {seconds + o.seconds}; }
    constexpr Duration operator-(Duration o) const { return {seconds - o.seconds}; }
    constexpr Duration operator*(std::int64_t k) const { return {seconds * k}; }
    constexpr Duration& operator+=(Duration o) { seconds += o.seconds; return *this; }
    constexpr double to_seconds_f() const { return static_cast<double>(seconds); }

    static constexpr auto fields(auto& self) { return std::tie(self.seconds); }
};

constexpr Duration seconds(std::int64_t s) { return {s}; }
constexpr Duration minutes(std::int64_t m) { return {m * 60}; }
constexpr Duration hours(std::int64_t h) { return {h * 3600}; }
constexpr Duration days(std::int64_t d) { return {d * 86400}; }

// An absolute point in simulation time: seconds since the scenario epoch.
struct Time {
    std::int64_t seconds = 0;

    constexpr auto operator<=>(const Time&) const = default;
    constexpr Time operator+(Duration d) const { return {seconds + d.seconds}; }
    constexpr Time operator-(Duration d) const { return {seconds - d.seconds}; }
    constexpr Duration operator-(Time o) const { return {seconds - o.seconds}; }
    constexpr Time& operator+=(Duration d) { seconds += d.seconds; return *this; }
    constexpr double to_seconds_f() const { return static_cast<double>(seconds); }

    static constexpr auto fields(auto& self) { return std::tie(self.seconds); }
};

// Human-readable duration, e.g. "11d 4h 3m".
std::string format_duration(Duration d);

} // namespace sim
