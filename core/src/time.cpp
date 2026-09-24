#include "simcore/time.hpp"

#include <format>

namespace sim {

std::string format_duration(Duration d) {
    std::string sign = d.seconds < 0 ? "-" : "";
    std::int64_t s = d.seconds < 0 ? -d.seconds : d.seconds;
    const std::int64_t dd = s / 86400;
    const std::int64_t hh = (s % 86400) / 3600;
    const std::int64_t mm = (s % 3600) / 60;
    if (dd > 0) return std::format("{}{}d {}h {}m", sign, dd, hh, mm);
    if (hh > 0) return std::format("{}{}h {}m", sign, hh, mm);
    if (mm > 0) return std::format("{}{}m {}s", sign, mm, s % 60);
    return std::format("{}{}s", sign, s);
}

} // namespace sim
