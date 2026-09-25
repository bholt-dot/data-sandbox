#include "expanse/calendar.hpp"

#include <charconv>
#include <format>

namespace expanse::calendar {

Date civil_from_days(std::int64_t z) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
    return {static_cast<std::int32_t>(y), static_cast<std::uint8_t>(m), static_cast<std::uint8_t>(d)};
}

DateTime to_datetime(sim::Time t) {
    std::int64_t days = t.seconds / 86400;
    std::int64_t rem = t.seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    return {civil_from_days(days + epoch_unix_days), static_cast<std::uint8_t>(rem / 3600),
            static_cast<std::uint8_t>(rem % 3600 / 60), static_cast<std::uint8_t>(rem % 60)};
}

std::string format_date(sim::Time t) {
    const Date d = to_datetime(t).date;
    return std::format("{:04}-{:02}-{:02}", d.year, d.month, d.day);
}

std::string format_datetime(sim::Time t) {
    const DateTime dt = to_datetime(t);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}", dt.date.year, dt.date.month, dt.date.day,
                       dt.hour, dt.minute);
}

std::optional<Date> parse_date(std::string_view s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
        return std::nullopt;
    }
    auto num = [&](std::size_t pos, std::size_t len, int& out) {
        const char* first = s.data() + pos;
        const char* last = first + len;
        auto [p, ec] = std::from_chars(first, last, out);
        return ec == std::errc{} && p == last;
    };
    int y = 0, m = 0, d = 0;
    if (!num(0, 4, y) || !num(5, 2, m) || !num(8, 2, d) || m < 1 || m > 12 || d < 1) {
        return std::nullopt;
    }
    const Date date{y, static_cast<std::uint8_t>(m), static_cast<std::uint8_t>(d)};
    // Reject e.g. 2350-02-30 by round-tripping through day numbers.
    if (civil_from_days(days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d))) != date) {
        return std::nullopt;
    }
    return date;
}

} // namespace expanse::calendar
