#pragma once

// Contracts: paid work that needs a ship, not capital. Every populated station keeps a job board;
// a captain docked there can take a cargo haul (the client provides the cargo) or carry
// passengers, and is paid on delivery.
//
// Generation (daily, World::rng("contracts"), stations in key order): each open slot on a board
// (board_capacity(population)) fills with probability post_chance. An offer is
//   * cargo, from a station that has the commodity to spare (net producer, or stock above
//     1.2x target) to one that consumes it. Pairs are weighted by proximity (1 / (1 + (d/0.3 AU)^2))
//     times the destination's need (price multiplier at its current stock, doubled while it has
//     unmet demand), so shortages pull freight and the Ceres cluster, being close, always has
//     local work. Size: 3-12 days of the destination's consumption, 1-450 t, at most half the
//     origin's stock. The client's cargo leaves the origin's stock on acceptance and joins the
//     destination's on delivery.
//   * passengers (passenger_share of offers): destination weighted by proximity x log10(pop);
//     1..clamp(round(log10(min population) - 2), 1, 6) people.
// Time allowed = T x slack + 12 h, where T is the flip-and-burn transit at reference_accel_g
// (0.1 g) at posting and slack ~ U(1.3, 2.0): a coasting course at 0.3 g makes it with fuel to
// spare. The deadline is set when the offer is taken (accepted_at + time_allowed).
//
// Reward (credits, rounded to 10):
//   cargo      = S x P x U x K x (base_fee + day_rate T + tonne_day_rate t T + value_share V)
//   passengers = S x P x U x n x (fare + fare_per_day T)
//   V = t x base_price (cargo value); T in days.
//   U (urgency) = 1 + urgency_bonus x (2.0 - slack) / 0.7  (1.0 relaxed .. 1.8 rush)
//   K (risk)    = commodity category (medical 1.3, luxury 1.2, food 1.1, else 1.0),
//                 x 1.15 across faction lines
//   S (standing)= 1 + 0.03 x standing with the origin's faction (clamped to +-10)
//   P (premium) = 1.35 for offers reserved to captains of standing >= premium_standing, else 1.
// Deposit (cargo only, EVE-style collateral): 25% of V, scaled by 1 - 0.06 x standing (0.4..1.6),
// halved on premium offers, zero when V < free_deposit_value. Paid on acceptance, refunded on
// delivery, forfeited on failure or abandonment.
//
// Delivery is automatic when the carrying ship docks at the destination (ships.cpp calls
// on_docked), so a captain never loses a job to a forgotten command. Lateness L = arrival -
// deadline; grace = max(1 day, 25% of time allowed):
//   L <= 0          full reward, deposit back, standing +1
//   0 < L <= grace  reward x (1 - 0.5 L / grace), deposit back, standing -1
//   L > grace       failed: no reward, deposit forfeited, standing -3 (checked daily too)
// Abandoning: deposit forfeited (or, with no deposit, a fee of 10% of the reward if the cash is
// there), standing -2. Cargo and passengers of a failed/abandoned job leave the ship at its next
// port. Standing <= refuse_standing: that faction's boards won't deal with you.
// Deadline warnings (urgent journal entries, once per contract): when the ship is underway to the
// destination with an ETA past the deadline, or within a day of the deadline when not on the way.

#include "expanse/content.hpp"
#include "expanse/world.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace expanse::contracts {

namespace tuning {
inline constexpr double reference_accel_g = 0.1;
inline constexpr double min_slack = 1.3;
inline constexpr double max_slack = 2.0;
inline constexpr std::int64_t handling_hours = 12;
inline constexpr std::int64_t offer_min_days = 3; // offer lifetime on the board
inline constexpr std::int64_t offer_max_days = 6;
inline constexpr double post_chance = 0.5; // per open board slot per day
inline constexpr double passenger_share = 0.35;
inline constexpr double proximity_au = 0.3;
inline constexpr double max_job_tonnes = 450.0;

inline constexpr double base_fee = 60.0;
inline constexpr double day_rate = 40.0;
inline constexpr double tonne_day_rate = 0.2;
inline constexpr double value_share = 0.01;
inline constexpr double fare = 50.0;
inline constexpr double fare_per_day = 30.0;
inline constexpr double urgency_bonus = 0.5;
inline constexpr double cross_faction_risk = 1.15;

inline constexpr double deposit_fraction = 0.25;
inline constexpr double free_deposit_value = 1500.0;
inline constexpr double abandon_fee_fraction = 0.10;
inline constexpr double grace_fraction = 0.25;
inline constexpr double max_late_cut = 0.5;

inline constexpr std::int32_t standing_limit = 10;
inline constexpr double reward_per_standing = 0.03;
inline constexpr double deposit_per_standing = 0.06;
inline constexpr double premium_chance = 0.2;
inline constexpr std::int8_t premium_standing = 3;
inline constexpr double premium_reward = 1.25;
inline constexpr std::int32_t refuse_standing = -5;
inline constexpr std::int32_t on_time_gain = 1;
inline constexpr std::int32_t late_loss = 1;
inline constexpr std::int32_t fail_loss = 3;
inline constexpr std::int32_t abandon_loss = 2;

inline constexpr std::size_t max_active = 5; // accepted contracts per company
inline constexpr std::int64_t keep_closed_days = 30;
} // namespace tuning

// ---- Queries ----------------------------------------------------------------------------------

// Offers a board of this population holds at most (0 for an unpopulated station).
std::size_t board_capacity(std::uint32_t population);
// Open offers at `station`, oldest first.
std::vector<ContractId> board_at(const World& world, StationId station);
// Contracts `company` holds (accepted), oldest first.
std::vector<ContractId> held_by(const World& world, CompanyId company);
// People aboard `ship` under contract (they take berths and eat like crew).
std::uint32_t passengers_aboard(const World& world, ShipId ship);
// Standing of `company` with `faction` (0 if never dealt with).
std::int32_t standing(const World& world, CompanyId company, Faction faction);
// Grace period after the deadline in which delivery is still accepted at a cut.
sim::Duration grace(const Contract& contract);
// Flip-and-burn transit time at reference_accel_g departing at `t`; nullopt without a course.
std::optional<sim::Duration> reference_transit(const Content& content, StationId from, StationId to,
                                               sim::Time t);
// One line: "210 t Raw ore -> Sakai Drift" / "2 passengers -> Ceres Station".
std::string describe(const Content& content, const Contract& contract);

// Reward and deposit `company` would get on this offer (its standing applied; an accepted
// contract's terms are fixed).
struct Terms {
    Credits reward = 0;
    Credits deposit = 0;
};
Terms terms_for(const Content& content, const World& world, const Contract& contract, CompanyId company);

struct Check {
    bool ok = false;
    std::string reason; // player-facing when !ok
};

// Whether `ship` can take the offer now (docked at its origin, room, cash for the deposit,
// standing, not too many jobs already).
Check can_accept(const Content& content, const World& world, ShipId ship, ContractId contract);

// ---- Actions ----------------------------------------------------------------------------------

struct Result {
    bool ok = false;
    std::string message; // player-facing, success or reason for refusal
};

// Takes the offer: pays the deposit, loads the cargo / boards the passengers, sets the deadline.
Result accept(const Content& content, World& world, ShipId ship, ContractId contract);
// Gives up a held contract (penalties above).
Result abandon(const Content& content, World& world, CompanyId company, ContractId contract);

// ---- Hooks ------------------------------------------------------------------------------------

// Fills every board at the start of a game.
void start_new_game(const Content& content, World& world);
// A ship has just docked: deliver what is bound here, put off what a failed job left aboard.
void on_docked(const Content& content, World& world, ShipId ship);

} // namespace expanse::contracts
