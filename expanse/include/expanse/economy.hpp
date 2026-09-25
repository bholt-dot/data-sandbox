#pragma once

// Station markets: daily production/consumption, local prices, and the market transactions ships
// make while docked (buy, sell, refuel).
//
// Stock model (per station, per commodity in the station's market list), once a day:
//   1. production and consumption from the StationDef market entry, each with +-10% noise from
//      world.rng("economy"). Consumption rates in the data are authored per station and already
//      reflect its population; there is no separate population multiplier. Stock never goes
//      negative: demand that cannot be met is recorded in StationState::unmet (a shortage).
//   2. unsimulated traffic ("the rest of the system trades too") moves 1/offscreen_tau_days of
//      the gap between stock and the station's target stock. Equilibrium stock is therefore
//      target + net_flow * tau: consumers settle below target (dearer), producers above
//      (cheaper), which is what creates trade routes. Once NPC traders exist this can be tuned
//      down so they carry more of the flow.
//   3. rarely, a supply disruption (station-side outage or broken import line) stops production
//      and incoming offscreen traffic for a few weeks. A consumer runs down its buffer and can run
//      dry: prices climb to the cap and a message is posted.
//
// Price model: constant-elasticity (power-law) curve on stock relative to a days-of-cover target,
//   mid(S) = base_price * clamp((target / S)^elasticity, min_multiplier, max_multiplier)
//   target = cover_days * max(production, consumption, min_reference_flow)
// Smooth, monotonic (never rises with stock), scale-free (depends only on S / target) and bounded.
// The station sells at mid * (1 + half_spread) and buys at mid * (1 - half_spread). Trades move
// the stock and so the price: a trade of q tonnes is charged the exact integral of the curve over
// the stock it moves, so splitting a trade into pieces costs the same as making it at once and
// buying then selling back always loses exactly the spread (no single-station arbitrage).
//
// Stations only trade commodities in their market list; everything else is refused ("no market
// for ore at Tycho"). A station without an entry has no buyers, storage or handling for the good.

#include "expanse/content.hpp"
#include "expanse/world.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace expanse::economy {

namespace tuning {
inline constexpr double cover_days = 60.0;          // target stock, in days of the larger flow
inline constexpr double min_reference_flow = 0.05;  // t/day; floor for entries with no flows (small outposts keep small targets)
inline constexpr double elasticity = 0.5;           // price ~ (target/stock)^elasticity; in (0, 1)
inline constexpr double min_multiplier = 0.25;      // of base_price
inline constexpr double max_multiplier = 4.0;       // of base_price
inline constexpr double half_spread = 0.03;         // ask = mid*(1+h), bid = mid*(1-h)
inline constexpr double offscreen_tau_days = 30.0;  // relaxation time of unsimulated traffic
inline constexpr double flow_noise = 0.10;          // +- fraction on daily production/consumption
inline constexpr double disruption_chance = 1.0 / 365.0; // per market entry per day
inline constexpr std::uint16_t disruption_min_days = 10;
inline constexpr std::uint16_t disruption_max_days = 45;
} // namespace tuning

// ---- Queries --------------------------------------------------------------------------------

// The station's market entry for a commodity, or nullptr if it does not trade it.
const MarketEntryDef* market_entry(const Content& content, StationId station, CommodityId commodity);

// Target ("normal") stock for a market entry [t].
double target_stock(const MarketEntryDef& entry);

// Price multiplier on base_price at a given stock (pure; exposed for tests and tools).
double price_multiplier(double stock, double target);

struct MarketQuote {
    StationId station;
    CommodityId commodity;
    double stock = 0.0;  // t
    double target = 0.0; // t
    double mid = 0.0;    // credits/t at the current stock
    double ask = 0.0;    // station sells at (player buys)
    double bid = 0.0;    // station buys at (player sells)
    double unmet = 0.0;  // t of demand not met on the last daily tick
    std::uint16_t disrupted_days = 0;
};

// Mid price per tonne, or nullopt if the station does not trade the commodity.
std::optional<double> price(const Content& content, const World& world, StationId station,
                            CommodityId commodity);
std::optional<MarketQuote> quote(const Content& content, const World& world, StationId station,
                                 CommodityId commodity);

// Exact (unrounded) credits for trading `tonnes` against the station's current stock, including
// price impact and spread. nullopt if the station does not trade the commodity. Buying more than
// the station's stock is priced as if stock could reach zero (the transaction itself refuses it).
std::optional<double> buy_cost(const Content& content, const World& world, StationId station,
                               CommodityId commodity, double tonnes);
std::optional<double> sell_proceeds(const Content& content, const World& world, StationId station,
                                    CommodityId commodity, double tonnes);

// ---- Transactions ---------------------------------------------------------------------------
// Player/NPC actions. They never throw for bad requests: a failed trade changes nothing and says
// why. Money changes hands in whole credits at the end of the transaction: the buyer's cost is
// rounded up and the seller's proceeds down (the station keeps the fraction), so a trade too small
// to cost a credit can't be repeated for free.

enum class TradeStatus : std::uint8_t {
    ok,
    invalid_ship,            // null/stale ship handle, or its owner is gone
    invalid_commodity,
    invalid_quantity,        // not a positive finite amount
    not_docked,
    not_traded,              // the station has no market for this commodity
    insufficient_stock,      // the station doesn't have that much
    insufficient_cargo_space,
    insufficient_tank_space, // refuel: tank already full
    insufficient_funds,
    insufficient_cargo,      // sell: not that much aboard
};

std::string_view describe(TradeStatus status);

struct TradeResult {
    TradeStatus status = TradeStatus::invalid_ship;
    std::string reason; // human-readable; also set on success
    StationId station;
    CommodityId commodity;
    double tonnes = 0.0;    // actually traded
    Credits credits = 0;    // paid (buy/refuel) or received (sell)
    double unit_price = 0.0; // credits / tonnes
    Credits cost_basis = 0; // sell: basis of the tonnes sold
    Credits profit = 0;     // sell: credits - cost_basis

    bool ok() const { return status == TradeStatus::ok; }
};

TradeResult buy(const Content& content, World& world, ShipId ship, CommodityId commodity, double tonnes);
TradeResult sell(const Content& content, World& world, ShipId ship, CommodityId commodity, double tonnes);

// Buys water from the docked station as reaction mass. `tonnes` = nullopt fills the tank as far
// as tank space, station stock and cash allow (fails only if that is nothing); an explicit amount
// is all-or-nothing.
TradeResult refuel(const Content& content, World& world, ShipId ship, std::optional<double> tonnes);

// The commodity that doubles as reaction mass ("water").
CommodityId reaction_mass_commodity(const Content& content);

} // namespace expanse::economy
