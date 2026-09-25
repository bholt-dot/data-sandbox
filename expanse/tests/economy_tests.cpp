#include <doctest/doctest.h>

#include "expanse/economy.hpp"
#include "expanse/finance.hpp"
#include "expanse/scenario.hpp"
#include "expanse/simulation.hpp"
#include "expanse/transit.hpp"
#include "expanse/units.hpp"
#include "expanse/world.hpp"

#include <cmath>
#include <format>
#include <limits>
#include <memory>
#include <string>

using namespace expanse;
namespace eco = expanse::economy;

namespace {

const Content& game_content() {
    static const std::unique_ptr<Content> content = [] {
        sim::Diagnostics diags;
        auto c = Content::load(EXPANSE_DATA_DIR, diags);
        if (!c) {
            FAIL(diags.to_string());
        }
        return c;
    }();
    return *content;
}

// A small, fixed market so unit tests don't depend on the shipped data's tuning.
//   alpha: water producer-consumer, ore consumer. No parts market.
//   beta:  water consumer, ore producer.
const Content& fixture() {
    static const std::unique_ptr<Content> content = [] {
        const sim::DataSource src{"fixture.toml", R"(
[commodity.water]
name = "Water"
category = "volatiles"
base_price = 30
[commodity.ore]
name = "Ore"
category = "industrial"
base_price = 60
[commodity.parts]
name = "Parts"
category = "industrial"
base_price = 1000

[body.sun]
name = "Sun"
kind = "star"
radius_km = 695700
gm_km3_s2 = 1.32712440018e11
[body.rock]
name = "Rock"
kind = "asteroid"
parent = "sun"
radius_km = 100
gm_km3_s2 = 1
orbit = { a_au = 2.5 }

[station.alpha]
name = "Alpha"
faction = "belt"
body = "rock"
market = [
  { commodity = "water", production = 100, consumption = 50, stock = 3000 },
  { commodity = "ore", consumption = 20, stock = 400 },
]
[station.beta]
name = "Beta"
faction = "belt"
body = "sun"
orbit = { a_au = 2.6 }
market = [
  { commodity = "water", consumption = 10, stock = 100 },
  { commodity = "ore", production = 50, stock = 2000 },
]

[ship_class.tub]
name = "Tub"
dry_mass_t = 100
cargo_capacity_t = 50
reaction_mass_capacity_t = 40
exhaust_velocity_km_s = 10000
max_accel_g = 1
crew_berths = 1
price = 1

[scenario.test]
name = "Test"
start_date = "2350-01-01"
start_station = "alpha"
ship_class = "tub"
ship_name = "Tub"
company_name = "Tub Co"
cash = 1000
reaction_mass_fraction = 0.5
)"};
        sim::Diagnostics diags;
        auto c = Content::load(std::span{&src, 1}, diags);
        if (!c) {
            FAIL(diags.to_string());
        }
        return c;
    }();
    return *content;
}

struct Fx {
    const Content& c = fixture();
    World w = new_game(c, "test", 5);
    StationId alpha = c.find<StationDef>("alpha");
    StationId beta = c.find<StationDef>("beta");
    CommodityId water = c.find<CommodityDef>("water");
    CommodityId ore = c.find<CommodityDef>("ore");
    CommodityId parts = c.find<CommodityDef>("parts");
    ShipId ship = w.ships.handle_at(0);

    double& stock(StationId s, CommodityId k) { return w.stations[index_of(s)].stock[index_of(k)]; }
    Ship& the_ship() { return w.ships.at(ship); }
    Credits& cash() { return w.companies.at(w.player).cash; }
    double mid(StationId s, CommodityId k) const { return *eco::price(c, w, s, k); }
};

void run_days(const Content& c, World& w, int days) {
    advance_to(c, w, w.now() + sim::days(days), false);
}

} // namespace

// ---- Commodities and stockpiles -----------------------------------------------------------------

TEST_CASE("shipped commodities include the survival basics") {
    const Content& c = game_content();
    for (const char* key : {"water", "food", "parts", "oxygen"}) {
        CAPTURE(key);
        CHECK(c.find<CommodityDef>(key));
    }
    CHECK(eco::reaction_mass_commodity(c) == c.find<CommodityDef>("water"));
}

TEST_CASE("station stockpiles start from the data") {
    const Content& c = game_content();
    const World w = new_game(c, "secondhand", 1);
    for (auto [id, st] : c.table<StationDef>()) {
        const StationState& s = w.stations[index_of(id)];
        REQUIRE(s.stock.size() == c.table<CommodityDef>().size());
        CHECK(s.unmet.size() == s.stock.size());
        CHECK(s.disrupted_days.size() == s.stock.size());
        for (const MarketEntryDef& m : st.market) {
            CHECK(s.stock[index_of(m.commodity)] == m.stock);
        }
        // Every station sells reaction mass.
        CHECK(eco::market_entry(c, id, eco::reaction_mass_commodity(c)) != nullptr);
    }
}

// ---- Prices -------------------------------------------------------------------------------------

TEST_CASE("price is bounded and monotonic in stock") {
    const double target = 600.0;
    CHECK(eco::price_multiplier(target, target) == doctest::Approx(1.0));
    CHECK(eco::price_multiplier(0.0, target) == eco::tuning::max_multiplier);
    CHECK(eco::price_multiplier(1e12, target) == eco::tuning::min_multiplier);
    double prev = std::numeric_limits<double>::infinity();
    for (double s = 0.0; s < 20.0 * target; s += 7.3) {
        const double m = eco::price_multiplier(s, target);
        CHECK(m <= prev);
        CHECK(m >= eco::tuning::min_multiplier);
        CHECK(m <= eco::tuning::max_multiplier);
        prev = m;
    }
    // Scale-free: only the ratio to target matters.
    CHECK(eco::price_multiplier(150.0, 600.0) == doctest::Approx(eco::price_multiplier(1500.0, 6000.0)));
}

TEST_CASE("quotes have a spread and untraded goods have no price") {
    Fx f;
    const auto q = eco::quote(f.c, f.w, f.alpha, f.ore);
    REQUIRE(q);
    const double target = 20.0 * eco::tuning::cover_days;
    CHECK(q->target == doctest::Approx(target));
    CHECK(q->mid == doctest::Approx(60.0 * std::pow(target / 400.0, eco::tuning::elasticity)));
    CHECK(q->ask > q->mid);
    CHECK(q->bid < q->mid);
    CHECK(q->ask / q->bid == doctest::Approx((1 + eco::tuning::half_spread) / (1 - eco::tuning::half_spread)));
    CHECK_FALSE(eco::price(f.c, f.w, f.alpha, f.parts));
    CHECK_FALSE(eco::quote(f.c, f.w, f.alpha, CommodityId{}));
    // Producers are cheap, consumers dear.
    CHECK(f.mid(f.beta, f.ore) < f.mid(f.alpha, f.ore));
}

TEST_CASE("trade cost integrates the price curve") {
    Fx f;
    const double q = 300.0;
    const double cost = *eco::buy_cost(f.c, f.w, f.alpha, f.ore, q);
    // Riemann sum of the ask price over the stock the purchase removes.
    const int steps = 30000;
    double sum = 0.0;
    const double s0 = f.stock(f.alpha, f.ore);
    const double target = 20.0 * eco::tuning::cover_days;
    for (int i = 0; i < steps; ++i) {
        const double s = s0 - (i + 0.5) * q / steps;
        sum += 60.0 * eco::price_multiplier(s, target) * (1 + eco::tuning::half_spread) * q / steps;
    }
    CHECK(cost == doctest::Approx(sum).epsilon(1e-6));
    // Past the price cap (stock < target/16) the integral is linear.
    const double below_cap = *eco::buy_cost(f.c, f.w, f.alpha, f.ore, 400.0) -
                             *eco::buy_cost(f.c, f.w, f.alpha, f.ore, 390.0);
    CHECK(below_cap == doctest::Approx(10 * 60.0 * 4.0 * (1 + eco::tuning::half_spread)));
    // Selling then buying back the same amount loses exactly the spread.
    const double proceeds = *eco::sell_proceeds(f.c, f.w, f.alpha, f.ore, 100.0);
    f.stock(f.alpha, f.ore) += 100.0;
    const double back = *eco::buy_cost(f.c, f.w, f.alpha, f.ore, 100.0);
    CHECK(back / proceeds == doctest::Approx((1 + eco::tuning::half_spread) / (1 - eco::tuning::half_spread)));
}

// ---- Daily production and consumption -----------------------------------------------------------

TEST_CASE("one day of production and consumption") {
    Fx f;
    systems::daily_economy(f.c, f.w);
    // Flows with +-10% noise, then 1/tau of the gap to target closes (monotonic in the stock).
    const double d = eco::tuning::cover_days;
    auto relax = [](double s, double target) { return s + (target - s) / eco::tuning::offscreen_tau_days; };
    // water: 3000 + (100 +- 10%) - (50 +- 10%)
    const double water = f.stock(f.alpha, f.water);
    CHECK(water >= relax(3000 + 90 - 55, 100 * d) - 1e-9);
    CHECK(water <= relax(3000 + 110 - 45, 100 * d) + 1e-9);
    // ore (pure consumer): 400 - (20 +- 10%)
    const double ore = f.stock(f.alpha, f.ore);
    CHECK(ore >= relax(400 - 22, 20 * d) - 1e-9);
    CHECK(ore <= relax(400 - 18, 20 * d) + 1e-9);
    // Untraded goods stay untouched.
    CHECK(f.stock(f.alpha, f.parts) == 0.0);
}

TEST_CASE("markets settle near equilibrium without traders") {
    Fx f;
    run_days(f.c, f.w, 240);
    // Equilibrium stock = target + net_flow * tau.
    const double tau = eco::tuning::offscreen_tau_days;
    const double d = eco::tuning::cover_days;
    CHECK(f.stock(f.alpha, f.ore) == doctest::Approx(20 * (d - tau)).epsilon(0.1));
    CHECK(f.stock(f.beta, f.ore) == doctest::Approx(50 * (d + tau)).epsilon(0.1));
    CHECK(f.mid(f.beta, f.ore) < 60.0);
    CHECK(f.mid(f.alpha, f.ore) > 60.0);
}

TEST_CASE("stock never goes negative and shortages are visible") {
    Fx f;
    f.stock(f.alpha, f.ore) = 30.0;
    f.w.stations[index_of(f.alpha)].disrupted_days[index_of(f.ore)] = 20;
    const std::size_t first = f.w.messages.size();
    for (int d = 0; d < 20; ++d) {
        systems::daily_economy(f.c, f.w);
        CHECK(f.stock(f.alpha, f.ore) >= 0.0);
    }
    // A disrupted consumer burns through a 1.5-day buffer and runs dry.
    CHECK(f.stock(f.alpha, f.ore) == 0.0);
    CHECK(f.w.stations[index_of(f.alpha)].unmet[index_of(f.ore)] > 15.0);
    CHECK(f.mid(f.alpha, f.ore) == doctest::Approx(60.0 * eco::tuning::max_multiplier));
    bool ran_out = false;
    bool restored = false;
    for (std::size_t i = first; i < f.w.messages.size(); ++i) {
        ran_out = ran_out || f.w.messages[i].text == "Alpha has run out of Ore";
        restored = restored || f.w.messages[i].text == "Alpha: Ore supply restored";
    }
    CHECK(ran_out);
    CHECK(restored);
    // Supply resumes and imports refill it.
    for (int d = 0; d < 30; ++d) {
        systems::daily_economy(f.c, f.w);
    }
    CHECK(f.stock(f.alpha, f.ore) > 300.0);

    // Shipped data over a long run: never negative.
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 99);
    for (int d = 0; d < 400; d += 10) {
        run_days(c, w, 10);
        for (const StationState& s : w.stations) {
            for (double v : s.stock) {
                CHECK(v >= 0.0);
            }
        }
    }
}

// ---- Transactions -------------------------------------------------------------------------------

TEST_CASE("buy and sell move cargo cash and stock") {
    Fx f;
    f.cash() = 10'000;
    const double ask_before = eco::quote(f.c, f.w, f.alpha, f.water)->ask;
    const double expected = *eco::buy_cost(f.c, f.w, f.alpha, f.water, 20.0);
    const auto r = eco::buy(f.c, f.w, f.ship, f.water, 20.0);
    REQUIRE(r.ok());
    CHECK(r.tonnes == 20.0);
    CHECK(r.credits == static_cast<Credits>(std::ceil(expected)));
    CHECK(f.cash() == 10'000 - r.credits);
    CHECK(f.stock(f.alpha, f.water) == 3000.0 - 20.0);
    REQUIRE(f.the_ship().cargo.size() == 1);
    CHECK(f.the_ship().cargo[0].tonnes == 20.0);
    CHECK(f.the_ship().cargo[0].cost_basis == r.credits);
    // Price impact: the average paid is above the pre-trade ask, and the price has risen.
    CHECK(r.unit_price >= ask_before - 0.05);
    CHECK(eco::quote(f.c, f.w, f.alpha, f.water)->ask > ask_before);
    CHECK(f.w.messages.back().kind == MessageKind::market);

    // A second purchase merges into the same lot.
    const auto r2 = eco::buy(f.c, f.w, f.ship, f.water, 10.0);
    REQUIRE(r2.ok());
    REQUIRE(f.the_ship().cargo.size() == 1);
    CHECK(f.the_ship().cargo[0].tonnes == 30.0);
    CHECK(f.the_ship().cargo[0].cost_basis == r.credits + r2.credits);

    // Sell a third: basis leaves pro rata; selling back here loses the spread.
    const Credits basis = f.the_ship().cargo[0].cost_basis;
    const auto s = eco::sell(f.c, f.w, f.ship, f.water, 10.0);
    REQUIRE(s.ok());
    CHECK(s.cost_basis == std::llround(static_cast<double>(basis) / 3.0));
    CHECK(s.profit == s.credits - s.cost_basis);
    CHECK(s.profit < 0);
    CHECK(f.the_ship().cargo[0].tonnes == doctest::Approx(20.0));
    // Selling the rest removes the lot.
    const auto s2 = eco::sell(f.c, f.w, f.ship, f.water, 20.0);
    REQUIRE(s2.ok());
    CHECK(f.the_ship().cargo.empty());
    CHECK(s.cost_basis + s2.cost_basis == basis);
    CHECK(f.stock(f.alpha, f.water) == doctest::Approx(3000.0));

    // Whole-credit rounding favours the station: a sliver of cargo is never free.
    const auto tiny = eco::buy(f.c, f.w, f.ship, f.water, 0.001);
    REQUIRE(tiny.ok());
    CHECK(tiny.credits == 1);
    const auto tiny_sale = eco::sell(f.c, f.w, f.ship, f.water, 0.001);
    REQUIRE(tiny_sale.ok());
    CHECK(tiny_sale.credits == 0);
}

TEST_CASE("large trades get worse prices") {
    Fx f;
    f.cash() = 1'000'000;
    const auto small = eco::buy(f.c, f.w, f.ship, f.ore, 1.0);
    REQUIRE(small.ok());
    f.stock(f.alpha, f.ore) += 1.0;
    f.the_ship().cargo.clear();
    const auto large = eco::buy(f.c, f.w, f.ship, f.ore, 50.0);
    REQUIRE(large.ok());
    CHECK(large.unit_price > small.unit_price * 1.02);
    // Selling a lot into a small market depresses the price.
    const double bid_before = eco::quote(f.c, f.w, f.beta, f.water)->bid;
    f.stock(f.beta, f.water) += 50.0; // as if a hauler had just sold 50 t there
    CHECK(eco::quote(f.c, f.w, f.beta, f.water)->bid < bid_before * 0.9);
}

TEST_CASE("trade failures change nothing and say why") {
    Fx f;
    const auto hash_before = world_hash(f.w);
    auto expect = [&](const eco::TradeResult& r, eco::TradeStatus s) {
        CAPTURE(r.reason);
        CHECK(r.status == s);
        CHECK_FALSE(r.reason.empty());
        CHECK(world_hash(f.w) == hash_before);
    };
    expect(eco::buy(f.c, f.w, ShipId{}, f.water, 1.0), eco::TradeStatus::invalid_ship);
    expect(eco::buy(f.c, f.w, f.ship, CommodityId{}, 1.0), eco::TradeStatus::invalid_commodity);
    expect(eco::buy(f.c, f.w, f.ship, f.water, 0.0), eco::TradeStatus::invalid_quantity);
    expect(eco::buy(f.c, f.w, f.ship, f.water, -3.0), eco::TradeStatus::invalid_quantity);
    expect(eco::sell(f.c, f.w, f.ship, f.water, std::nan("")), eco::TradeStatus::invalid_quantity);
    expect(eco::buy(f.c, f.w, f.ship, f.parts, 1.0), eco::TradeStatus::not_traded);
    expect(eco::buy(f.c, f.w, f.ship, f.water, 51.0), eco::TradeStatus::insufficient_cargo_space);
    expect(eco::buy(f.c, f.w, f.ship, f.ore, 30.0), eco::TradeStatus::insufficient_funds);
    expect(eco::sell(f.c, f.w, f.ship, f.ore, 1.0), eco::TradeStatus::insufficient_cargo);
    expect(eco::refuel(f.c, f.w, f.ship, 21.0), eco::TradeStatus::insufficient_tank_space);
    expect(eco::refuel(f.c, f.w, f.ship, -1.0), eco::TradeStatus::invalid_quantity);

    f.stock(f.alpha, f.ore) = 5.0;
    const auto h2 = world_hash(f.w);
    const auto r = eco::buy(f.c, f.w, f.ship, f.ore, 6.0);
    CHECK(r.status == eco::TradeStatus::insufficient_stock);
    CHECK(world_hash(f.w) == h2);

    f.the_ship().reaction_mass_t = 40.0;
    const auto h3 = world_hash(f.w);
    CHECK(eco::refuel(f.c, f.w, f.ship, std::nullopt).status == eco::TradeStatus::insufficient_tank_space);
    CHECK(world_hash(f.w) == h3);

    f.the_ship().location = Underway{f.alpha, f.beta, f.w.now(), f.w.now() + sim::days(3), {}, {}, {}, {}};
    const auto h4 = world_hash(f.w);
    CHECK(eco::buy(f.c, f.w, f.ship, f.water, 1.0).status == eco::TradeStatus::not_docked);
    CHECK(eco::sell(f.c, f.w, f.ship, f.water, 1.0).status == eco::TradeStatus::not_docked);
    CHECK(eco::refuel(f.c, f.w, f.ship, std::nullopt).status == eco::TradeStatus::not_docked);
    CHECK(world_hash(f.w) == h4);
}

TEST_CASE("refuel buys water as reaction mass") {
    Fx f;
    CHECK(f.the_ship().reaction_mass_t == doctest::Approx(20.0));
    const double ask = eco::quote(f.c, f.w, f.alpha, f.water)->ask;
    const auto r = eco::refuel(f.c, f.w, f.ship, 5.0);
    REQUIRE(r.ok());
    CHECK(r.commodity == f.water);
    CHECK(f.the_ship().reaction_mass_t == doctest::Approx(25.0));
    CHECK(std::abs(static_cast<double>(r.credits) - ask * 5.0) <= 1.0); // tiny trade: ~ the ask
    CHECK(f.stock(f.alpha, f.water) == 2995.0);
    CHECK(f.the_ship().cargo.empty());

    // Fill up: capped by the tank.
    const auto full = eco::refuel(f.c, f.w, f.ship, std::nullopt);
    REQUIRE(full.ok());
    CHECK(full.tonnes == doctest::Approx(15.0));
    CHECK(f.the_ship().reaction_mass_t == doctest::Approx(40.0));

    // Fill up when broke: buys what the cash covers.
    Fx g;
    g.cash() = 100;
    const auto partial = eco::refuel(g.c, g.w, g.ship, std::nullopt);
    REQUIRE(partial.ok());
    CHECK(partial.credits <= 100);
    CHECK(partial.credits >= 98);
    CHECK(g.cash() >= 0);
    CHECK(partial.tonnes < 20.0);
    CHECK(eco::refuel(g.c, g.w, g.ship, 10.0).status == eco::TradeStatus::insufficient_funds);
    g.cash() = 0;
    CHECK(eco::refuel(g.c, g.w, g.ship, std::nullopt).status == eco::TradeStatus::insufficient_funds);
    g.stock(g.alpha, g.water) = 0.0;
    g.cash() = 1000;
    CHECK(eco::refuel(g.c, g.w, g.ship, std::nullopt).status == eco::TradeStatus::insufficient_stock);
}

// ---- Determinism --------------------------------------------------------------------------------

TEST_CASE("economy is reproducible by seed") {
    const Content& c = game_content();
    World a = new_game(c, "secondhand", 2024);
    World b = new_game(c, "secondhand", 2024);
    World other = new_game(c, "secondhand", 2025);
    run_days(c, a, 90);
    run_days(c, b, 90);
    run_days(c, other, 90);
    CHECK(world_hash(a) == world_hash(b));
    CHECK(world_hash(a) != world_hash(other));
    CHECK(a.stations[0].stock != other.stations[0].stock);
}

TEST_CASE("economy save and load mid run matches an uninterrupted run") {
    const Content& c = game_content();
    const auto water = c.find<CommodityDef>("water");
    auto play = [&](World& w) {
        const ShipId ship = w.ships.handle_at(0);
        run_days(c, w, 20);
        // Twenty days of docking fees leave the captain broke; fund the trades.
        REQUIRE(transact(w, w.player, 50'000, LedgerCategory::other, "windfall"));
        REQUIRE(eco::buy(c, w, ship, water, 10.0).ok());
        REQUIRE(eco::refuel(c, w, ship, std::nullopt).ok());
    };
    World a = new_game(c, "secondhand", 77);
    play(a);
    run_days(c, a, 70);

    World b = new_game(c, "secondhand", 77);
    play(b);
    run_days(c, b, 25);
    World b2 = load_world(save_world(b), c);
    run_days(c, b2, 45);
    CHECK(world_hash(a) == world_hash(b2));
}

// ---- Tuning sanity: a starter trade route ------------------------------------------------------

namespace {

struct RouteEstimate {
    StationId from;
    StationId to;
    CommodityId commodity;
    double tonnes = 0.0;
    double cost = 0.0;
    double proceeds = 0.0;
    double fuel_t = 0.0;    // out laden + back empty
    double fuel_cost = 0.0; // at the origin's water ask
    double days = 0.0;
    double margin() const { return proceeds - cost - fuel_cost; }
};

// Buys the margin-maximising amount (<= hold, <= budget) at `from`, flies to `to` (0.1 g, dv capped
// to a slow boat), sells, flies back empty. Prices are those at departure.
std::optional<RouteEstimate> estimate(const Content& c, const World& w, StationId from, StationId to,
                                      CommodityId k, double budget) {
    const auto& cls = c.table<ShipClassDef>()[w.ships.rows()[0].ship_class];
    const auto from_q = eco::quote(c, w, from, k);
    if (!from_q || !eco::quote(c, w, to, k)) {
        return std::nullopt;
    }
    RouteEstimate r{from, to, k};
    const double max_q = std::min(cls.cargo_capacity_t, from_q->stock);
    for (double q = 1.0; q <= max_q; q += 1.0) {
        const double cost = *eco::buy_cost(c, w, from, k, q);
        const double proceeds = *eco::sell_proceeds(c, w, to, k, q);
        if (cost > budget) {
            break;
        }
        if (proceeds - cost > r.proceeds - r.cost) {
            r.tonnes = q;
            r.cost = cost;
            r.proceeds = proceeds;
        }
    }
    if (r.tonnes <= 0.0) {
        return std::nullopt;
    }

    transit::PlotRequest out;
    out.origin = transit::Origin::at_body(c.orbit_of(from));
    out.destination = c.orbit_of(to);
    out.departure = w.now();
    out.accel = units::gees(0.1);
    out.max_delta_v = units::km_per_s(300.0);
    out.ship.dry_mass = units::tonnes(cls.dry_mass_t);
    out.ship.cargo_mass = units::tonnes(r.tonnes);
    out.ship.reaction_mass = units::tonnes(cls.reaction_mass_capacity_t);
    out.ship.exhaust_velocity = units::km_per_s(cls.exhaust_velocity_km_s);
    const transit::Plot p1 = transit::plot_transit(c.orbits(), out);
    if (!p1.feasible()) {
        return std::nullopt;
    }
    transit::PlotRequest back = out;
    back.origin = transit::Origin::at_body(c.orbit_of(to));
    back.destination = c.orbit_of(from);
    back.departure = p1.arrival;
    back.ship.cargo_mass = 0.0;
    const transit::Plot p2 = transit::plot_transit(c.orbits(), back);
    if (!p2.feasible()) {
        return std::nullopt;
    }
    r.fuel_t = (p1.reaction_mass_used + p2.reaction_mass_used) / units::tonne_kg;
    r.fuel_cost = r.fuel_t * eco::quote(c, w, from, eco::reaction_mass_commodity(c))->ask;
    r.days = (p2.arrival - out.departure).to_seconds_f() / units::day_s;
    return r;
}

std::string describe_route(const Content& c, const RouteEstimate& r);

// Best round trip between stations within `radius_au` of the scenario's start station.
std::optional<RouteEstimate> best_route(const Content& c, const World& w, double radius_au, double budget) {
    const auto& sc = c.table<ScenarioDef>()[c.find<ScenarioDef>("secondhand")];
    const Vec3 home = c.orbits().world_position(c.orbit_of(sc.start_station), w.now());
    auto near = [&](StationId s) {
        return units::to_au(distance(c.orbits().world_position(c.orbit_of(s), w.now()), home)) <= radius_au;
    };
    std::optional<RouteEstimate> best;
    for (auto [a, sa] : c.table<StationDef>()) {
        for (auto [b, sb] : c.table<StationDef>()) {
            if (a == b || !near(a) || !near(b)) {
                continue;
            }
            for (auto [k, kd] : c.table<CommodityDef>()) {
                const auto r = estimate(c, w, a, b, k, budget);
                if (r && (!best || r->margin() > best->margin())) {
                    best = r;
                }
            }
        }
    }
    return best;
}

std::string describe_route(const Content& c, const RouteEstimate& r) {
    return std::format("{} -> {}: {:.0f} t {} for {:.0f} cr, sold for {:.0f} cr, reaction mass {:.0f} t "
                       "({:.0f} cr); margin {:.0f} cr ({:.1f}% on {:.0f} cr) over a {:.0f}-day round trip",
                       c.table<StationDef>()[r.from].name, c.table<StationDef>()[r.to].name, r.tonnes,
                       c.table<CommodityDef>()[r.commodity].name, r.cost, r.proceeds, r.fuel_t,
                       r.fuel_cost, r.margin(), 100.0 * r.margin() / (r.cost + r.fuel_cost),
                       r.cost + r.fuel_cost, r.days);
}

} // namespace

TEST_CASE("secondhand has a thin but real starter trade route") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const auto& sc = c.table<ScenarioDef>()[c.find<ScenarioDef>("secondhand")];
    const double unlimited = std::numeric_limits<double>::infinity();

    // With a full hold between stations near the start (1.5 AU).
    const auto best = best_route(c, w, 1.5, unlimited);
    REQUIRE(best);
    MESSAGE("starter route: " << describe_route(c, *best));
    CHECK(best->margin() > 0.0);
    // Thin: one trip must not pay off a real share of the loan or double the money.
    CHECK(best->margin() < 0.05 * static_cast<double>(sc.loan_principal));
    CHECK(best->margin() < 0.5 * (best->cost + best->fuel_cost));

    // The same after the markets have settled to their equilibrium.
    World settled = new_game(c, "secondhand", 1);
    run_days(c, settled, 60);
    const auto later = best_route(c, settled, 2.0, unlimited);
    REQUIRE(later);
    MESSAGE("settled route: " << describe_route(c, *later));
    CHECK(later->margin() > 0.0);
    CHECK(later->margin() < 0.5 * (later->cost + later->fuel_cost));

    // Cash on hand at the start.
    const auto poor = best_route(c, w, 1.5, static_cast<double>(sc.cash));
    if (poor) {
        MESSAGE("with starting cash: " << describe_route(c, *poor));
    }
}

