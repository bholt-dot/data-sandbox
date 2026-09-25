#pragma once

#include "expanse/content.hpp"
#include "expanse/world.hpp"

#include <cstdint>
#include <string_view>

namespace expanse {

// Builds the starting World for a scenario definition. Deterministic in (content, scenario, seed).
// Throws std::invalid_argument if the scenario key is unknown.
World new_game(const Content& content, std::string_view scenario_key, std::uint64_t seed);

// Scheduler priorities: lower fires first at the same instant.
inline constexpr std::int32_t priority_arrivals = -10; // ships dock before the day's accounting
inline constexpr std::int32_t priority_daily = 0;
inline constexpr std::int32_t priority_weekly = 10;
inline constexpr std::int32_t priority_finance = 20;

} // namespace expanse
