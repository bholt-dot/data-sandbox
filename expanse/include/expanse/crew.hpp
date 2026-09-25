#pragma once

// Crew: the hiring pool on each dock, hiring and firing, weekly wages, morale, and the daily
// life-support draw on a ship's stores.
//
// Model summary
//   * Crew are CrewMember rows. Aboard: `ship` set. Ashore job-seekers: `ship` null, `station` set.
//   * Each station keeps a pool sized by log10(population); weekly, some seekers move on and new
//     ones arrive (World::rng("crew")). Seekers are drawn mostly from origins of the station's
//     faction (CrewOriginDef in data/crew_names.toml).
//   * Wages are paid weekly from the owner's cash, all-or-nothing per person (arrears first).
//     A missed payday costs morale, more each consecutive week. Morale otherwise drifts to neutral.
//   * Crew whose morale falls below quit_morale walk off at the next port the ship is docked at.
//   * Every person aboard eats and drinks daily from cargo lots of "food"/"water"/"oxygen"
//     (life_support_per_person). Shortages post urgent messages, cost morale and health.
//   * Skills feed the StatPipeline as ship multipliers (stat_ids.hpp): best pilot / engineer /
//     medic aboard, captain counts as a pilot.
//
// Player mistakes never throw: hire()/fire() return a status and a human-readable reason.

#include "expanse/content.hpp"
#include "expanse/world.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace expanse::crew {

// ---- Tunables ---------------------------------------------------------------------------------

inline constexpr double neutral_morale = 0.6;
inline constexpr double warn_morale = 0.4;  // "grumbling" warning when crossed downwards
inline constexpr double quit_morale = 0.25; // below this, walks off at the next port

// Life support make-up per person per day, in tonnes, after recycling (see crew.cpp for sources).
struct Provisions {
    double water_t = 0.0;
    double food_t = 0.0;
    double oxygen_t = 0.0;
};
inline constexpr Provisions life_support_per_person{0.0004, 0.0018, 0.00042};
// Cabin-air leakage of a worn hull: oxygen_t per day at hull_condition 0 (linear to 0 at 1.0).
inline constexpr double hull_leak_oxygen_t_per_day = 0.0004;

// ---- Queries ----------------------------------------------------------------------------------

std::string_view role_name(CrewRole role);

// Target number of job-seekers on a dock of this population (0 for an unpopulated dock).
std::size_t pool_target(std::uint32_t population);

// Job-seekers ashore at `station`, in table order.
std::vector<CrewId> pool_at(const World& world, StationId station);
// Everyone aboard `ship` (captain included), in table order.
std::vector<CrewId> aboard(const World& world, ShipId ship);
// Sum of weekly wages of everyone aboard.
Credits weekly_payroll(const World& world, ShipId ship);
// Signing bonus demanded on hire: one week's wage.
Credits signing_bonus(const CrewMember& member);
// Going weekly rate for a role and skill from an origin (CrewOriginDef::wage_multiplier).
// Job-seekers ask this +-10%, rounded to 10 cr.
Credits expected_wage(CrewRole role, std::uint8_t skill, double origin_multiplier);

// Daily make-up the ship draws with its current complement (docked ships breathe station air,
// so oxygen_t is 0 while docked). Applies the life_support_use stat.
Provisions daily_need(const Content& content, World& world, ShipId ship);
// Tonnes of each provision aboard.
Provisions stores(const Content& content, const Ship& ship);

// Skill contribution of one crew member in `role` (0..100 skill) to its stat: an `increased`
// value (e.g. -0.4 = 40% less wear). Exposed for tooling and tests.
double skill_effect(sim::StatId stat, std::uint8_t skill);

// ---- Actions ----------------------------------------------------------------------------------

enum class HireStatus : std::uint8_t {
    ok,
    no_such_ship,
    no_such_crew,
    not_looking,     // already aboard a ship
    not_docked,      // the ship is underway
    wrong_station,   // the job-seeker is on another dock
    no_berth,        // every berth is taken
    cannot_afford,   // owner can't pay the signing bonus
};

struct HireResult {
    HireStatus status = HireStatus::ok;
    std::string reason; // empty on success
    Credits signing_bonus = 0;
    bool ok() const { return status == HireStatus::ok; }
};

// Signs on a job-seeker ashore at the ship's dock. Pays one week's wage up front.
HireResult hire(const Content& content, World& world, ShipId ship, CrewId member);

enum class FireStatus : std::uint8_t {
    ok,
    no_such_crew,
    not_aboard, // already ashore
    captain,    // can't fire yourself
    underway,   // nowhere to put them off
};

struct FireResult {
    FireStatus status = FireStatus::ok;
    std::string reason;
    Credits wages_settled = 0; // arrears paid on the way out
    bool ok() const { return status == FireStatus::ok; }
};

// Puts a crew member ashore at the ship's current dock; they join the hiring pool there.
// Settles wages owed if the owner can pay; otherwise the arrears are written off.
FireResult fire(World& world, CrewId member);

// ---- Setup and upkeep -------------------------------------------------------------------------

// Called by new_game: the player's captain aboard `player_ship`, the scenario's provisions in the
// hold, crew stat bases, and an initial hiring pool on every dock.
void start_new_game(const Content& content, World& world, const ScenarioDef& scenario,
                    ShipId player_ship);

// Rebuilds the stat modifiers the crew of `ship` contributes. Called on every roster change.
void refresh_modifiers(World& world, ShipId ship);

// Hook for the eventual injury/death system: called once when a crew member's health reaches 0.
// Currently only posts an urgent message; the member stays aboard, incapacitated.
void on_health_depleted(World& world, CrewId member);

} // namespace expanse::crew
