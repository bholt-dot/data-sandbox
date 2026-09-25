#include "expanse/economy.hpp"

#include "expanse/finance.hpp"
#include "expanse/simulation.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <format>
#include <variant>

namespace expanse::economy {

namespace {

using namespace tuning;

static_assert(elasticity > 0.0 && elasticity < 1.0, "price integral assumes 0 < elasticity < 1");
static_assert(min_multiplier > 0.0 && min_multiplier < 1.0 && max_multiplier > 1.0);
static_assert(half_spread >= 0.0 && half_spread < 1.0);

constexpr double epsilon_t = 1e-9;

// Antiderivative of the mid-price curve with respect to stock:
// F(s) = integral of base * price_multiplier(x, target) dx from 0 to s. Linear below s_hi (capped
// price) and above s_lo (floor price), c * s^(1-e) in between. Also valid for s < 0 (capped).
double price_integral(double s, double base, double target) {
    const double s_hi = target * std::pow(max_multiplier, -1.0 / elasticity);
    const double s_lo = target * std::pow(min_multiplier, -1.0 / elasticity);
    const double at_hi = base * max_multiplier * s_hi;
    if (s <= s_hi) {
        return base * max_multiplier * s;
    }
    const double k = base * std::pow(target, elasticity) / (1.0 - elasticity);
    const double one_minus_e = 1.0 - elasticity;
    if (s <= s_lo) {
        return at_hi + k * (std::pow(s, one_minus_e) - std::pow(s_hi, one_minus_e));
    }
    return at_hi + k * (std::pow(s_lo, one_minus_e) - std::pow(s_hi, one_minus_e)) +
           base * min_multiplier * (s - s_lo);
}

struct MarketRef {
    const MarketEntryDef* entry = nullptr;
    double base = 0.0;
    double target = 0.0;
    double stock = 0.0;
};

std::optional<MarketRef> market_ref(const Content& content, const World& world, StationId station,
                                    CommodityId commodity) {
    if (!content.table<StationDef>().contains(station) ||
        !content.table<CommodityDef>().contains(commodity)) {
        return std::nullopt;
    }
    const MarketEntryDef* entry = market_entry(content, station, commodity);
    if (entry == nullptr) {
        return std::nullopt;
    }
    MarketRef m;
    m.entry = entry;
    m.base = static_cast<double>(content.table<CommodityDef>()[commodity].base_price);
    m.target = target_stock(*entry);
    m.stock = world.stations[index_of(station)].stock[index_of(commodity)];
    return m;
}

double raw_buy_cost(const MarketRef& m, double tonnes) {
    return (1.0 + half_spread) *
           (price_integral(m.stock, m.base, m.target) - price_integral(m.stock - tonnes, m.base, m.target));
}

double raw_sell_proceeds(const MarketRef& m, double tonnes) {
    return (1.0 - half_spread) *
           (price_integral(m.stock + tonnes, m.base, m.target) - price_integral(m.stock, m.base, m.target));
}

Credits to_credits(double v) { return static_cast<Credits>(std::llround(v)); }

// The station keeps the fraction: buyers pay rounded up, sellers get rounded down, so trades too
// small to cost a whole credit can't be repeated for free. The tolerance absorbs float noise on
// exact prices.
Credits price_to_pay(double v) { return static_cast<Credits>(std::ceil(v - 1e-6)); }
Credits price_to_receive(double v) { return static_cast<Credits>(std::floor(v + 1e-6)); }

const std::string& station_name(const Content& content, StationId s) {
    return content.table<StationDef>()[s].name;
}
const std::string& commodity_name(const Content& content, CommodityId c) {
    return content.table<CommodityDef>()[c].name;
}

TradeResult fail(TradeStatus status, std::string reason) {
    TradeResult r;
    r.status = status;
    r.reason = std::move(reason);
    return r;
}

// Common checks for all transactions: live ship and owner, docked, commodity traded there.
struct Context {
    Ship* ship = nullptr;
    Company* owner = nullptr;
    StationId station;
    MarketRef market;
};

std::variant<Context, TradeResult> resolve(const Content& content, World& world, ShipId ship_id,
                                           CommodityId commodity, double tonnes, bool check_quantity) {
    Ship* ship = world.ships.get(ship_id);
    if (ship == nullptr) {
        return fail(TradeStatus::invalid_ship, "no such ship");
    }
    Company* owner = world.companies.get(ship->owner);
    if (owner == nullptr) {
        return fail(TradeStatus::invalid_ship, std::format("{} has no owner", ship->name));
    }
    if (!content.table<CommodityDef>().contains(commodity)) {
        return fail(TradeStatus::invalid_commodity, "no such commodity");
    }
    if (check_quantity && !(std::isfinite(tonnes) && tonnes > 0.0)) {
        return fail(TradeStatus::invalid_quantity, "quantity must be a positive number of tonnes");
    }
    const auto* docked = std::get_if<Docked>(&ship->location);
    if (docked == nullptr) {
        return fail(TradeStatus::not_docked, std::format("{} is not docked at a station", ship->name));
    }
    const auto m = market_ref(content, world, docked->station, commodity);
    if (!m) {
        return fail(TradeStatus::not_traded,
                    std::format("{} has no market for {}", station_name(content, docked->station),
                                commodity_name(content, commodity)));
    }
    return Context{ship, owner, docked->station, *m};
}

void add_cargo(Ship& ship, CommodityId commodity, double tonnes, Credits paid) {
    for (CargoLot& lot : ship.cargo) {
        if (lot.commodity == commodity) {
            lot.tonnes += tonnes;
            lot.cost_basis += paid;
            return;
        }
    }
    ship.cargo.push_back(CargoLot{commodity, tonnes, paid});
}

double cargo_of(const Ship& ship, CommodityId commodity) {
    double total = 0.0;
    for (const CargoLot& lot : ship.cargo) {
        if (lot.commodity == commodity) {
            total += lot.tonnes;
        }
    }
    return total;
}

// Removes `tonnes` of a commodity (lots in order) and returns the cost basis removed, pro rata.
Credits remove_cargo(Ship& ship, CommodityId commodity, double tonnes) {
    Credits basis = 0;
    double left = tonnes;
    for (CargoLot& lot : ship.cargo) {
        if (lot.commodity != commodity || left <= 0.0) {
            continue;
        }
        if (lot.tonnes <= left + epsilon_t) {
            basis += lot.cost_basis;
            left -= lot.tonnes;
            lot.tonnes = 0.0;
            lot.cost_basis = 0;
        } else {
            const Credits part =
                to_credits(static_cast<double>(lot.cost_basis) * (left / lot.tonnes));
            basis += part;
            lot.cost_basis -= part;
            lot.tonnes -= left;
            left = 0.0;
        }
    }
    std::erase_if(ship.cargo, [](const CargoLot& lot) { return lot.tonnes <= epsilon_t; });
    return basis;
}

// Largest q in [0, max_q] whose rounded cost fits in `cash` (cost is increasing in q).
double max_affordable(const MarketRef& m, double max_q, Credits cash) {
    if (cash <= 0 || max_q <= 0.0) {
        return 0.0;
    }
    if (price_to_pay(raw_buy_cost(m, max_q)) <= cash) {
        return max_q;
    }
    double lo = 0.0;
    double hi = max_q;
    for (int i = 0; i < 64; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (price_to_pay(raw_buy_cost(m, mid)) <= cash) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

std::string format_tonnes(double t) { return std::format("{:.1f} t", t); }

} // namespace

// ---- Queries --------------------------------------------------------------------------------

const MarketEntryDef* market_entry(const Content& content, StationId station, CommodityId commodity) {
    for (const MarketEntryDef& m : content.table<StationDef>()[station].market) {
        if (m.commodity == commodity) {
            return &m;
        }
    }
    return nullptr;
}

double target_stock(const MarketEntryDef& entry) {
    return cover_days * std::max({entry.production, entry.consumption, min_reference_flow});
}

double price_multiplier(double stock, double target) {
    if (!(stock > 0.0)) {
        return max_multiplier;
    }
    return std::clamp(std::pow(target / stock, elasticity), min_multiplier, max_multiplier);
}

std::optional<double> price(const Content& content, const World& world, StationId station,
                            CommodityId commodity) {
    const auto m = market_ref(content, world, station, commodity);
    if (!m) {
        return std::nullopt;
    }
    return m->base * price_multiplier(m->stock, m->target);
}

std::optional<MarketQuote> quote(const Content& content, const World& world, StationId station,
                                 CommodityId commodity) {
    const auto m = market_ref(content, world, station, commodity);
    if (!m) {
        return std::nullopt;
    }
    const StationState& st = world.stations[index_of(station)];
    MarketQuote q;
    q.station = station;
    q.commodity = commodity;
    q.stock = m->stock;
    q.target = m->target;
    q.mid = m->base * price_multiplier(m->stock, m->target);
    q.ask = q.mid * (1.0 + half_spread);
    q.bid = q.mid * (1.0 - half_spread);
    q.unmet = st.unmet[index_of(commodity)];
    q.disrupted_days = st.disrupted_days[index_of(commodity)];
    return q;
}

std::optional<double> buy_cost(const Content& content, const World& world, StationId station,
                               CommodityId commodity, double tonnes) {
    const auto m = market_ref(content, world, station, commodity);
    if (!m) {
        return std::nullopt;
    }
    return raw_buy_cost(*m, tonnes);
}

std::optional<double> sell_proceeds(const Content& content, const World& world, StationId station,
                                    CommodityId commodity, double tonnes) {
    const auto m = market_ref(content, world, station, commodity);
    if (!m) {
        return std::nullopt;
    }
    return raw_sell_proceeds(*m, tonnes);
}

CommodityId reaction_mass_commodity(const Content& content) {
    return content.find<CommodityDef>("water");
}

// ---- Transactions ---------------------------------------------------------------------------

std::string_view describe(TradeStatus status) {
    switch (status) {
    case TradeStatus::ok: return "ok";
    case TradeStatus::invalid_ship: return "invalid ship";
    case TradeStatus::invalid_commodity: return "invalid commodity";
    case TradeStatus::invalid_quantity: return "invalid quantity";
    case TradeStatus::not_docked: return "not docked";
    case TradeStatus::not_traded: return "not traded here";
    case TradeStatus::insufficient_stock: return "insufficient station stock";
    case TradeStatus::insufficient_cargo_space: return "insufficient cargo space";
    case TradeStatus::insufficient_tank_space: return "reaction mass tank full";
    case TradeStatus::insufficient_funds: return "insufficient funds";
    case TradeStatus::insufficient_cargo: return "not enough cargo aboard";
    }
    return "unknown";
}

TradeResult buy(const Content& content, World& world, ShipId ship_id, CommodityId commodity, double tonnes) {
    auto resolved = resolve(content, world, ship_id, commodity, tonnes, true);
    if (auto* err = std::get_if<TradeResult>(&resolved)) {
        return std::move(*err);
    }
    auto& ctx = std::get<Context>(resolved);
    const std::string& cname = commodity_name(content, commodity);
    const std::string& sname = station_name(content, ctx.station);

    if (tonnes > ctx.market.stock + epsilon_t) {
        return fail(TradeStatus::insufficient_stock,
                    std::format("{} has only {} of {}", sname, format_tonnes(ctx.market.stock), cname));
    }
    const ShipClassDef& cls = content.table<ShipClassDef>()[ctx.ship->ship_class];
    const double space = cls.cargo_capacity_t - cargo_mass_t(*ctx.ship);
    if (tonnes > space + epsilon_t) {
        return fail(TradeStatus::insufficient_cargo_space,
                    std::format("{} has room for {} more cargo", ctx.ship->name,
                                format_tonnes(std::max(space, 0.0))));
    }
    const double qty = std::min(tonnes, ctx.market.stock);
    const Credits cost = price_to_pay(raw_buy_cost(ctx.market, qty));
    if (cost > ctx.owner->cash) {
        return fail(TradeStatus::insufficient_funds,
                    std::format("{} of {} costs {} cr; {} has {} cr", format_tonnes(qty), cname,
                                cost, ctx.owner->name, ctx.owner->cash));
    }

    if (!transact(world, ctx.ship->owner, -cost, LedgerCategory::trade,
                  std::format("bought {} {} at {}", format_tonnes(qty), cname, sname))) {
        return fail(TradeStatus::insufficient_funds, std::format("{} can't cover {} cr", ctx.owner->name, cost));
    }
    double& stock = world.stations[index_of(ctx.station)].stock[index_of(commodity)];
    stock = std::max(0.0, stock - qty);
    add_cargo(*ctx.ship, commodity, qty, cost);

    TradeResult r;
    r.status = TradeStatus::ok;
    r.station = ctx.station;
    r.commodity = commodity;
    r.tonnes = qty;
    r.credits = cost;
    r.unit_price = static_cast<double>(cost) / qty;
    r.reason = std::format("{} bought {} of {} at {} for {} cr ({:.0f} cr/t)", ctx.ship->name,
                           format_tonnes(qty), cname, sname, cost, r.unit_price);
    post(world, MessageKind::market, r.reason);
    return r;
}

TradeResult sell(const Content& content, World& world, ShipId ship_id, CommodityId commodity, double tonnes) {
    auto resolved = resolve(content, world, ship_id, commodity, tonnes, true);
    if (auto* err = std::get_if<TradeResult>(&resolved)) {
        return std::move(*err);
    }
    auto& ctx = std::get<Context>(resolved);
    const std::string& cname = commodity_name(content, commodity);
    const std::string& sname = station_name(content, ctx.station);

    const double aboard = cargo_of(*ctx.ship, commodity);
    if (tonnes > aboard + epsilon_t) {
        return fail(TradeStatus::insufficient_cargo,
                    std::format("{} carries {} of {}", ctx.ship->name, format_tonnes(aboard), cname));
    }
    const double qty = std::min(tonnes, aboard);
    const Credits proceeds = price_to_receive(raw_sell_proceeds(ctx.market, qty));

    transact(world, ctx.ship->owner, proceeds, LedgerCategory::trade,
             std::format("sold {} {} at {}", format_tonnes(qty), cname, sname));
    world.stations[index_of(ctx.station)].stock[index_of(commodity)] += qty;
    const Credits basis = remove_cargo(*ctx.ship, commodity, qty);

    TradeResult r;
    r.status = TradeStatus::ok;
    r.station = ctx.station;
    r.commodity = commodity;
    r.tonnes = qty;
    r.credits = proceeds;
    r.unit_price = static_cast<double>(proceeds) / qty;
    r.cost_basis = basis;
    r.profit = proceeds - basis;
    r.reason = std::format("{} sold {} of {} at {} for {} cr ({:.0f} cr/t, profit {} cr)",
                           ctx.ship->name, format_tonnes(qty), cname, sname, proceeds,
                           r.unit_price, r.profit);
    post(world, MessageKind::market, r.reason);
    return r;
}

TradeResult refuel(const Content& content, World& world, ShipId ship_id, std::optional<double> tonnes) {
    const CommodityId water = reaction_mass_commodity(content);
    auto resolved = resolve(content, world, ship_id, water, tonnes.value_or(1.0), true);
    if (auto* err = std::get_if<TradeResult>(&resolved)) {
        return std::move(*err);
    }
    auto& ctx = std::get<Context>(resolved);
    const std::string& sname = station_name(content, ctx.station);
    const ShipClassDef& cls = content.table<ShipClassDef>()[ctx.ship->ship_class];
    const double space = cls.reaction_mass_capacity_t - ctx.ship->reaction_mass_t;
    if (space <= epsilon_t) {
        return fail(TradeStatus::insufficient_tank_space,
                    std::format("{}'s reaction mass tank is full", ctx.ship->name));
    }

    double qty = 0.0;
    if (tonnes) {
        if (*tonnes > space + epsilon_t) {
            return fail(TradeStatus::insufficient_tank_space,
                        std::format("{}'s tank has room for {}", ctx.ship->name, format_tonnes(space)));
        }
        if (*tonnes > ctx.market.stock + epsilon_t) {
            return fail(TradeStatus::insufficient_stock,
                        std::format("{} has only {} of water", sname, format_tonnes(ctx.market.stock)));
        }
        qty = std::min({*tonnes, space, ctx.market.stock});
        const Credits cost = price_to_pay(raw_buy_cost(ctx.market, qty));
        if (cost > ctx.owner->cash) {
            return fail(TradeStatus::insufficient_funds,
                        std::format("{} of reaction mass costs {} cr; {} has {} cr",
                                    format_tonnes(qty), cost, ctx.owner->name, ctx.owner->cash));
        }
    } else {
        const double wanted = std::min(space, ctx.market.stock);
        if (wanted <= epsilon_t) {
            return fail(TradeStatus::insufficient_stock, std::format("{} is out of water", sname));
        }
        qty = max_affordable(ctx.market, wanted, ctx.owner->cash);
        if (qty <= epsilon_t) {
            return fail(TradeStatus::insufficient_funds,
                        std::format("{} cannot afford any reaction mass ({} cr)", ctx.owner->name,
                                    ctx.owner->cash));
        }
    }

    const Credits cost = price_to_pay(raw_buy_cost(ctx.market, qty));
    if (!transact(world, ctx.ship->owner, -cost, LedgerCategory::fuel,
                  std::format("{} reaction mass at {}", format_tonnes(qty), sname))) {
        return fail(TradeStatus::insufficient_funds, std::format("{} can't cover {} cr", ctx.owner->name, cost));
    }
    double& stock = world.stations[index_of(ctx.station)].stock[index_of(water)];
    stock = std::max(0.0, stock - qty);
    ctx.ship->reaction_mass_t = std::min(cls.reaction_mass_capacity_t, ctx.ship->reaction_mass_t + qty);

    TradeResult r;
    r.status = TradeStatus::ok;
    r.station = ctx.station;
    r.commodity = water;
    r.tonnes = qty;
    r.credits = cost;
    r.unit_price = static_cast<double>(cost) / qty;
    r.reason = std::format("{} took on {} of reaction mass at {} for {} cr ({:.0f} cr/t); tank {:.0f}/{:.0f} t",
                           ctx.ship->name, format_tonnes(qty), sname, cost, r.unit_price,
                           ctx.ship->reaction_mass_t, cls.reaction_mass_capacity_t);
    post(world, MessageKind::market, r.reason);
    return r;
}

} // namespace expanse::economy

namespace expanse::systems {

void daily_economy(const Content& content, World& world) {
    using namespace economy::tuning;
    const auto& stations = content.table<StationDef>();
    const auto& commodities = content.table<CommodityDef>();
    sim::Rng& rng = world.rng("economy");
    assert(world.stations.size() == stations.size());

    // Stations in dense (key) order, entries in data order: the draw sequence is fixed.
    for (auto [station_id, def] : stations) {
        StationState& st = world.stations[index_of(station_id)];
        for (const MarketEntryDef& entry : def.market) {
            const std::size_t c = index_of(entry.commodity);
            const double target = economy::target_stock(entry);
            const double prod_noise = rng.uniform_real(-flow_noise, flow_noise);
            const double cons_noise = rng.uniform_real(-flow_noise, flow_noise);
            const bool roll_disruption = rng.chance(disruption_chance);

            std::uint16_t& disrupted = st.disrupted_days[c];
            if (disrupted == 0 && roll_disruption) {
                disrupted = rng.uniform_int(disruption_min_days, disruption_max_days);
                post(world, MessageKind::market,
                     std::format("{}: {} supply disrupted (about {} days)", def.name,
                                 commodities[entry.commodity].name, disrupted));
            }
            const bool is_disrupted = disrupted > 0;

            double& stock = st.stock[c];
            const double before = stock;
            const double produced = is_disrupted ? 0.0 : entry.production * (1.0 + prod_noise);
            const double demand = entry.consumption * (1.0 + cons_noise);
            const double available = stock + produced;
            const double consumed = std::min(available, demand);
            st.unmet[c] = demand - consumed;
            stock = available - consumed;

            // Unsimulated traffic; a disruption stops the inbound half of it.
            const double gap = target - stock;
            if (gap < 0.0 || !is_disrupted) {
                stock += gap / offscreen_tau_days;
            }
            stock = std::max(0.0, stock);

            if (before > 0.0 && stock <= 0.0) {
                post(world, MessageKind::market,
                     std::format("{} has run out of {}", def.name, commodities[entry.commodity].name));
            }
            if (is_disrupted && --disrupted == 0) {
                post(world, MessageKind::market,
                     std::format("{}: {} supply restored", def.name, commodities[entry.commodity].name));
            }
        }
    }
}

} // namespace expanse::systems
