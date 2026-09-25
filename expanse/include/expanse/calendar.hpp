#pragma once

#include "simcore/time.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace expanse::calendar {

// Game time zero (sim::Time{0}) is 2350-01-01 00:00 UTC. Proleptic Gregorian calendar, no leap
// seconds; dates are presentation only, the simulation runs on integral seconds.
inline constexpr int epoch_year = 2350;

struct Date {
    std::int32_t year = epoch_year;
    std::uint8_t month = 1; // 1..12
    std::uint8_t day = 1;   // 1..31

    constexpr auto operator<=>(const Date&) const = default;
};

struct DateTime {
    Date date;
    std::uint8_t hour = 0;
    std::uint8_t minute = 0;
    std::uint8_t second = 0;
};

// Days since 1970-01-01 for a civil date (H. Hinnant's algorithm), and its inverse.
constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? static_cast<unsigned>(-3) : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

Date civil_from_days(std::int64_t days_since_unix_epoch);

inline constexpr std::int64_t epoch_unix_days = days_from_civil(epoch_year, 1, 1);

constexpr sim::Time to_time(Date d) {
    return {(days_from_civil(d.year, d.month, d.day) - epoch_unix_days) * 86400};
}

DateTime to_datetime(sim::Time t);

// J2000.0 = 2000-01-01 12:00 TT; treated as UTC (the ~1 min difference is irrelevant here).
inline constexpr sim::Time j2000 = {(days_from_civil(2000, 1, 1) - epoch_unix_days) * 86400 + 43200};

// "2350-03-14"; "2350-03-14 06:00".
std::string format_date(sim::Time t);
std::string format_datetime(sim::Time t);

// Parses "YYYY-MM-DD"; nullopt if malformed or not a real date.
std::optional<Date> parse_date(std::string_view s);

} // namespace expanse::calendar
