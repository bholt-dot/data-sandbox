// Balance tests: scripted captains play the Secondhand start through the real game APIs
// (economy::buy/sell/refuel, depart/plot, pay_loan, advance_to) and the tests assert the
// difficulty the design asks for:
//   * a captain who does nothing is repossessed (day 28);
//   * a careful trader survives the first half-year on every surveyed seed and ends richer than
//     he started, but is squeezed thin when the interest-only period ends (week 8);
//   * a greedy-but-careless trader (flip-and-burn everywhere, every credit in cargo, no fuel
//     planning) does clearly worse than the careful one.
// The scenario's loan terms (data/scenarios.toml) were tuned against these bots.
// Tuning aids (environment variables, all optional):
//   BELTER_BALANCE_TRACE=1        print weekly trajectories, leg logs and a seed survey summary
//   BELTER_BALANCE_TRACE_SEED=n   whose full trajectory to print (default 1)
//   BELTER_BALANCE_SEEDS=n        survey n seeds instead of 16
//   BELTER_BALANCE_TERMS=p,bp,w,x override the loan: principal, bp/week, interest-only weeks, payment
//   BELTER_BALANCE_CASH=n         override the starting cash

#include <doctest/doctest.h>

#include "expanse/contracts.hpp"
#include "expanse/crew.hpp"
#include "expanse/economy.hpp"
#include "expanse/finance.hpp"
#include "expanse/scenario.hpp"
#include "expanse/ships.hpp"
#include "expanse/simulation.hpp"
#include "expanse/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

constexpr std::array cluster_keys{"ceres_station", "hollow_nail", "dagu_rock", "sakai_drift"};
constexpr std::array provision_keys{"food", "water", "oxygen"};

std::vector<StationId> cluster(const Content& c) {
    std::vector<StationId> out;
    for (const char* key : cluster_keys) {
        const StationId id = c.find<StationDef>(key);
        REQUIRE(id);
        out.push_back(id);
    }
    return out;
}

ShipId player_ship(const World& w) {
    for (auto [id, ship] : w.ships) {
        if (ship.owner == w.player) {
            return id;
        }
    }
    return {};
}

const Loan* player_loan(const World& w) {
    for (auto [id, loan] : w.loans) {
        if (loan.borrower == w.player) {
            return &loan;
        }
    }
    return nullptr;
}

LoanId player_loan_id(const World& w) {
    for (auto [id, loan] : w.loans) {
        if (loan.borrower == w.player) {
            return id;
        }
    }
    return {};
}

bool tracing() {
    const char* v = std::getenv("BELTER_BALANCE_TRACE");
    return v != nullptr && *v != '\0' && *v != '0';
}

Credits cash(const World& w) { return w.companies.at(w.player).cash; }

double aboard(const Ship& ship, CommodityId c) {
    double t = 0.0;
    for (const CargoLot& lot : ship.cargo) {
        t += lot.commodity == c ? lot.tonnes : 0.0;
    }
    return t;
}

// Provisions the bots keep aboard for the captain, in tonnes (never traded).
double provision_floor(const Content& c, CommodityId commodity, double days) {
    const std::string& key = c.table<CommodityDef>().key(commodity);
    const crew::Provisions& p = crew::life_support_per_person;
    const double per_day = key == "food"     ? p.food_t
                           : key == "water"  ? p.water_t
                           : key == "oxygen" ? p.oxygen_t + crew::hull_leak_oxygen_t_per_day
                                             : 0.0;
    return per_day * days;
}

// The Secondhand start. For tuning, BELTER_BALANCE_TERMS="principal,bp,interest_only_weeks,payment"
// overrides the scenario's loan (and BELTER_BALANCE_CASH its cash) without editing the data.
World start_game(const Content& c, std::uint64_t seed) {
    World w = new_game(c, "secondhand", seed);
    if (const char* terms = std::getenv("BELTER_BALANCE_TERMS"); terms != nullptr && *terms != '\0') {
        long long principal = 0;
        long long payment = 0;
        unsigned bp = 0;
        unsigned weeks = 0;
        REQUIRE(std::sscanf(terms, "%lld,%u,%u,%lld", &principal, &bp, &weeks, &payment) == 4);
        for (auto [id, loan] : w.loans) {
            (void)id;
            loan.balance = principal;
            loan.weekly_interest_bp = bp;
            loan.weekly_payment = payment;
            loan.interest_only_until = w.now() + sim::days(7 * static_cast<std::int64_t>(weeks));
        }
    }
    if (const char* extra = std::getenv("BELTER_BALANCE_CASH"); extra != nullptr && *extra != '\0') {
        REQUIRE(transact(w, w.player, std::atoll(extra) - cash(w), LedgerCategory::other, "tuning"));
    }
    return w;
}

// Cargo valued at the best bid in the cluster, price impact included (the captain's few kilos of
// provisions count too; they are worth a few credits).
Credits cargo_value(const Content& c, const World& w, const Ship& ship) {
    double total = 0.0;
    for (const CargoLot& lot : ship.cargo) {
        double best = 0.0;
        for (const StationId st : cluster(c)) {
            best = std::max(best, eco::sell_proceeds(c, w, st, lot.commodity, lot.tonnes).value_or(0.0));
        }
        total += best;
    }
    return static_cast<Credits>(total);
}

struct WeekRow {
    int week = 0;
    Credits cash = 0;
    Credits loan = 0;
    Credits cargo = 0;
    Credits net_worth = 0; // cash + cargo at bid - loan - dock tabs
    std::uint8_t missed = 0;
    double tank_t = 0.0;
};

struct BotRun {
    std::string name;
    std::vector<WeekRow> weeks; // weeks[0] = the start; weeks[k] = just after week k's instalment
    std::optional<sim::Time> repossessed;
    int legs = 0;
    int missed_payments = 0; // lifetime
    std::vector<std::string> log;
    sim::Time start{};
    int contracts_delivered = 0;
    int contracts_failed = 0;
    Credits contract_income = 0; // rewards paid (deposits refunded not counted)
};

WeekRow snapshot(const Content& c, const World& w, int week) {
    WeekRow r;
    r.week = week;
    r.cash = cash(w);
    const Loan* loan = player_loan(w);
    r.loan = loan != nullptr ? loan->balance : 0;
    r.missed = loan != nullptr ? loan->missed_payments : 0;
    const ShipId ship = player_ship(w);
    if (ship) {
        r.cargo = cargo_value(c, w, w.ships.at(ship));
        r.tank_t = w.ships.at(ship).reaction_mass_t;
    }
    r.net_worth = r.cash + r.cargo - r.loan - total_dock_tabs(w, w.player);
    return r;
}

// Advances the clock, recording a row just after each weekly instalment (due at start + 7k days).
class Recorder {
public:
    Recorder(const Content& c, World& w, BotRun& run) : c_(c), w_(w), run_(run), start_(w.now()) {
        run_.start = start_;
        run_.weeks.push_back(snapshot(c_, w_, 0));
    }

    void advance(sim::Time until) {
        if (until <= w_.now()) {
            return;
        }
        while (next_mark() <= until) {
            advance_to(c_, w_, next_mark(), false);
            run_.weeks.push_back(snapshot(c_, w_, static_cast<int>(run_.weeks.size())));
            note_game_over();
        }
        advance_to(c_, w_, until, false);
        note_game_over();
    }

    sim::Time start() const { return start_; }

private:
    sim::Time next_mark() const {
        return start_ + sim::days(7 * static_cast<std::int64_t>(run_.weeks.size())) + sim::hours(1);
    }
    void note_game_over() {
        if (w_.game_over && !run_.repossessed) {
            run_.repossessed = w_.game_over->time;
        }
    }

    const Content& c_;
    World& w_;
    BotRun& run_;
    sim::Time start_;
};

// `people` scales the captain's stores for passengers aboard.
void keep_provisions(const Content& c, World& w, ShipId ship, double people = 1.0) {
    for (const char* key : provision_keys) {
        const CommodityId id = c.find<CommodityDef>(key);
        const double have = aboard(w.ships.at(ship), id);
        const StationId here = std::get<Docked>(w.ships.at(ship).location).station;
        if (have < people * provision_floor(c, id, 14.0) && eco::market_entry(c, here, id)) {
            (void)eco::buy(c, w, ship, id, people * provision_floor(c, id, 35.0) - have);
        }
    }
}

// ---- The careful trader -------------------------------------------------------------------------
// What the playtests found to work: shuttle ore from Hollow Nail (producer) to Sakai Drift
// (consumer), the Ceres cluster's one steady route, and never hire. Careful means:
//   * the trip length for each round (2.5-5 days, cheapest coasting course within it) is picked
//     for the best credits per day net of reaction mass, not the fastest;
//   * reaction mass is bought for the leg ahead and the leg back plus a margin before any cargo,
//     at the local price;
//   * an instalment falling due before the next sale is paid before undocking (it counts early),
//     unless paying it would leave too little to trade — then the bot trades to catch up;
//   * the opening leg from Ceres carries whatever pays best to Hollow Nail (water, usually).

struct CarefulParams {
    double accel_g = 0.3;
    std::vector<double> leg_days{2.5, 3.0, 4.0, 5.0}; // trip lengths considered
    double fuel_margin_t = 10.0;
    Credits min_trade_cash = 1000; // below this after an early instalment, trade instead
};

struct TradeIdea {
    CommodityId commodity;
    double tonnes = 0.0;
    double profit = 0.0; // sale proceeds - purchase cost - hauling extra, at current prices
};

// Best trade of `commodity` from `from` to `to` spending at most `budget`; each tonne also costs
// `extra_per_t` (the reaction mass to haul it).
TradeIdea best_trade_of(const Content& c, const World& w, StationId from, StationId to,
                        CommodityId cid, double budget, double space_t, double extra_per_t) {
    const auto q = eco::quote(c, w, from, cid);
    if (!q || !eco::market_entry(c, to, cid) || q->stock <= 1.0) {
        return {};
    }
    const double qmax = std::min({space_t, q->stock * 0.95, budget / (q->ask + extra_per_t)});
    if (qmax < 1.0) {
        return {};
    }
    auto profit = [&](double t) {
        return eco::sell_proceeds(c, w, to, cid, t).value_or(0.0) -
               eco::buy_cost(c, w, from, cid, t).value_or(0.0) - extra_per_t * t;
    };
    // Profit is concave in quantity (the buy cost is convex, the proceeds concave).
    double lo = 0.0;
    double hi = qmax;
    for (int i = 0; i < 60; ++i) {
        const double m1 = lo + (hi - lo) / 3.0;
        const double m2 = hi - (hi - lo) / 3.0;
        if (profit(m1) < profit(m2)) {
            lo = m1;
        } else {
            hi = m2;
        }
    }
    const double t = std::floor(0.5 * (lo + hi));
    const double p = t >= 1.0 ? profit(t) : 0.0;
    return p > 0.0 ? TradeIdea{cid, t, p} : TradeIdea{};
}

TradeIdea best_trade(const Content& c, const World& w, StationId from, StationId to, double budget,
                     double space_t, double extra_per_t) {
    TradeIdea best;
    for (auto [cid, def] : c.table<CommodityDef>()) {
        (void)def;
        const TradeIdea t = best_trade_of(c, w, from, to, cid, budget, space_t, extra_per_t);
        if (t.profit > best.profit) {
            best = t;
        }
    }
    return best;
}

double water_ask(const Content& c, const World& w, StationId st) {
    const auto q = eco::quote(c, w, st, eco::reaction_mass_commodity(c));
    return q ? q->ask : std::numeric_limits<double>::infinity();
}

sim::Duration days_f(double days) { return sim::seconds(static_cast<std::int64_t>(days * 86400.0)); }

BotRun run_careful(const Content& c, std::uint64_t seed, int weeks, const CarefulParams& prm = {}) {
    BotRun run;
    run.name = "careful";
    World w = start_game(c, seed);
    const ShipId ship = player_ship(w);
    const StationId nail = c.find<StationDef>("hollow_nail");
    const StationId sakai = c.find<StationDef>("sakai_drift");
    const CommodityId ore = c.find<CommodityDef>("ore");
    const ShipClassDef& cls = c.table<ShipClassDef>()[w.ships.at(ship).ship_class];
    Recorder rec(c, w, run);
    const sim::Time end = rec.start() + sim::days(7 * weeks) + sim::hours(2);
    std::optional<StationId> sell_at;
    double round_days = prm.leg_days.front();

    while (w.now() < end && !w.game_over) {
        const StationId here = std::get<Docked>(w.ships.at(ship).location).station;

        // 1. Sell what was bought for this port (keeping the captain's provisions).
        if (sell_at == here) {
            for (auto [cid, def] : c.table<CommodityDef>()) {
                (void)def;
                const double spare = aboard(w.ships.at(ship), cid) - provision_floor(c, cid, 35.0);
                if (spare > 0.01 && eco::market_entry(c, here, cid)) {
                    (void)eco::sell(c, w, ship, cid, spare);
                }
            }
            sell_at.reset();
        }
        keep_provisions(c, w, ship);

        // 2. Plan the leg: destination, trip length, cargo, reaction mass, early instalment.
        const StationId dest = here == nail ? sakai : nail;
        const double price = water_ask(c, w, here);
        const Ship& s = w.ships.at(ship);
        const double m0 = cls.dry_mass_t + s.reaction_mass_t + cargo_mass_t(s);
        struct Plan {
            double days = 0.0;
            TradeIdea trade;
            Credits instalment = 0; // paid before undocking
            double fuel_buy_t = 0.0;
            double rate = -std::numeric_limits<double>::infinity();
        } plan;
        // At the ore end every trip length is weighed; the empty leg back flies the same.
        const std::vector<double> options =
            here == nail ? prm.leg_days : std::vector<double>{round_days};
        for (const double days : options) {
            const CoursePreview p = plot_cheapest_within(c, w, ship, dest, prm.accel_g, days_f(days));
            if (p.status != CourseStatus::ok && p.status != CourseStatus::insufficient_reaction_mass) {
                continue;
            }
            Credits instalment = 0;
            if (const Loan* loan = player_loan(w); loan != nullptr && loan->next_due < p.arrival) {
                instalment = instalment_outstanding(*loan);
                if (cash(w) - instalment < prm.min_trade_cash) {
                    instalment = 0; // can't spare it: trade now, pay from the proceeds or take a strike
                }
            }
            const double per_t = p.reaction_mass_needed_t / m0; // extra reaction mass per tonne aboard
            const double back_t = p.reaction_mass_needed_t;     // the leg back: about as long, lighter
            auto fuel_short = [&](double cargo_t) {
                return std::max(0.0, p.reaction_mass_needed_t + per_t * cargo_t + back_t +
                                         prm.fuel_margin_t - s.reaction_mass_t);
            };
            const double free = static_cast<double>(cash(w) - instalment - 100);
            const double space = cls.cargo_capacity_t - cargo_mass_t(s);
            TradeIdea t;
            if (here == nail) {
                t = best_trade_of(c, w, here, dest, ore, free - fuel_short(0.0) * price, space, per_t * price);
            } else if (here != sakai) {
                t = best_trade(c, w, here, dest, free - fuel_short(0.0) * price, space, per_t * price);
            }
            const double fuel_cost = (p.reaction_mass_needed_t + back_t) * price;
            const double fees = static_cast<double>(c.table<StationDef>()[dest].docking_fee +
                                                    c.table<StationDef>()[here].docking_fee);
            const double rate = (t.profit - fuel_cost - fees) / (2.0 * days + 1.0);
            if (rate > plan.rate) {
                plan = {days, t, instalment, fuel_short(t.tonnes), rate};
            }
        }
        if (plan.days <= 0.0) {
            rec.advance(w.now() + sim::days(1));
            continue;
        }
        if (here == nail) {
            round_days = plan.days;
        }

        // 3. Execute: instalment, reaction mass, cargo, go.
        if (plan.instalment > 0) {
            (void)pay_loan(c, w, player_loan_id(w), plan.instalment);
        }
        const double tank_space = cls.reaction_mass_capacity_t - w.ships.at(ship).reaction_mass_t;
        const double affordable = static_cast<double>(cash(w) - 50) / std::max(price, 1.0);
        const double fuel = std::floor(std::min({plan.fuel_buy_t + 1.0, tank_space, affordable}));
        if (plan.fuel_buy_t > 0.0 && fuel >= 1.0) {
            (void)eco::refuel(c, w, ship, fuel);
        }
        if (plan.trade.tonnes >= 1.0) {
            const double t = std::min(plan.trade.tonnes,
                                      std::floor(static_cast<double>(cash(w) - 50) /
                                                 eco::quote(c, w, here, plan.trade.commodity)->ask));
            if (t >= 1.0 && eco::buy(c, w, ship, plan.trade.commodity, t).ok()) {
                sell_at = dest;
            }
        }
        const CoursePreview p = plot_cheapest_within(c, w, ship, dest, prm.accel_g, days_f(plan.days));
        DepartResult d = depart(c, w, ship, dest, {prm.accel_g, p.delta_v_km_s * (1.0 + 1e-9) + 1e-6});
        if (!d.ok()) {
            // Short of reaction mass for the load: take the slowest burn the drive offers.
            d = depart(c, w, ship, dest, {0.05});
        }
        if (!d.ok()) {
            run.log.push_back(std::format("stuck at {}: {}", c.table<StationDef>()[here].name, d.reason));
            rec.advance(w.now() + sim::days(1));
            continue;
        }
        ++run.legs;
        if (tracing()) {
            run.log.push_back(std::format(
                "day {:5.1f} {} -> {}: {:.0f} t {} (est. {:.0f} cr, {:.0f}/day), {:.1f} d, dv {:.0f} km/s, "
                "fuel {:.1f} t, cash {} tank {:.0f}",
                (w.now() - rec.start()).to_seconds_f() / 86400.0, c.table<StationDef>()[here].name,
                c.table<StationDef>()[dest].name, plan.trade.tonnes,
                plan.trade.tonnes >= 1.0 ? c.table<CommodityDef>().key(plan.trade.commodity) : "-",
                plan.trade.profit, plan.rate, d.preview.duration.to_seconds_f() / 86400.0,
                d.preview.delta_v_km_s, d.preview.reaction_mass_needed_t, cash(w),
                w.ships.at(ship).reaction_mass_t));
        }
        rec.advance(d.preview.arrival + sim::minutes(1));
    }
    rec.advance(end);
    for (const Message& m : w.messages) {
        run.missed_payments += m.text.find("MISSED LOAN PAYMENT") != std::string::npos ? 1 : 0;
        if (tracing() && (m.kind == MessageKind::market || m.kind == MessageKind::finance)) {
            run.log.push_back(std::format("  [{:.1f}] {}", (m.time - rec.start()).to_seconds_f() / 86400.0, m.text));
        }
    }
    return run;
}

// ---- The careful contractor --------------------------------------------------------------------
// The careful trader's habits (coasting courses, fuel bought before cargo, early instalments),
// but free to roam the Ceres cluster and to take jobs from the boards. At each port it weighs
// every other cluster station: the contracts bound there it can take (best-paying first, as long
// as hold, berths and the deposit fit), plus the best own-account trade in the space left, less
// reaction mass and the destination's docking fee, per day of the leg. The leg must make the
// earliest deadline among the jobs taken (with a margin); it never takes a job it can't make.

BotRun run_contractor(const Content& c, std::uint64_t seed, int weeks, const CarefulParams& prm = {}) {
    BotRun run;
    run.name = "contractor";
    World w = start_game(c, seed);
    const ShipId ship = player_ship(w);
    const ShipClassDef& cls = c.table<ShipClassDef>()[w.ships.at(ship).ship_class];
    const std::vector<StationId> ports = cluster(c);
    Recorder rec(c, w, run);
    const sim::Time end = rec.start() + sim::days(7 * weeks) + sim::hours(2);
    std::optional<StationId> sell_at;
    const sim::Duration margin = sim::hours(6);

    while (w.now() < end && !w.game_over) {
        const StationId here = std::get<Docked>(w.ships.at(ship).location).station;
        if (sell_at == here) {
            for (auto [cid, def] : c.table<CommodityDef>()) {
                (void)def;
                const double spare = aboard(w.ships.at(ship), cid) - 4.0 * provision_floor(c, cid, 35.0);
                if (spare > 0.01 && eco::market_entry(c, here, cid)) {
                    (void)eco::sell(c, w, ship, cid, spare);
                }
            }
            sell_at.reset();
        }
        keep_provisions(c, w, ship, 4.0); // room for a full passenger list

        const double price = water_ask(c, w, here);
        struct Plan {
            StationId dest;
            std::vector<ContractId> jobs;
            double days = 0.0;
            TradeIdea trade;
            Credits instalment = 0;
            double rate = -std::numeric_limits<double>::infinity();
        } plan;
        const auto board = contracts::board_at(w, here);
        for (const StationId dest : ports) {
            if (dest == here) {
                continue;
            }
            // Jobs to `dest`, best reward first, while they fit.
            std::vector<ContractId> offers;
            for (const ContractId id : board) {
                if (w.contracts.at(id).destination == dest && contracts::can_accept(c, w, ship, id).ok) {
                    offers.push_back(id);
                }
            }
            std::sort(offers.begin(), offers.end(), [&](ContractId a, ContractId b) {
                return std::tie(w.contracts.at(b).reward, b.index) < std::tie(w.contracts.at(a).reward, a.index);
            });
            const Ship& s = w.ships.at(ship);
            double space = cls.cargo_capacity_t - cargo_mass_t(s);
            auto berths = static_cast<int>(cls.crew_berths) - static_cast<int>(crew::aboard(w, ship).size()) -
                          static_cast<int>(contracts::passengers_aboard(w, ship));
            Credits deposits = 0;
            Credits rewards = 0;
            double job_t = 0.0;
            sim::Time due = sim::Time{std::numeric_limits<std::int64_t>::max()};
            std::vector<ContractId> jobs;
            for (const ContractId id : offers) {
                const Contract& k = w.contracts.at(id);
                const auto terms = contracts::terms_for(c, w, k, w.player);
                const bool fits = k.kind == ContractKind::cargo ? k.tonnes <= space : k.passengers <= berths;
                // Keep enough cash for the leg's reaction mass after deposits.
                if (!fits || cash(w) - deposits - terms.deposit < 600) {
                    continue;
                }
                jobs.push_back(id);
                deposits += terms.deposit;
                rewards += terms.reward;
                space -= k.kind == ContractKind::cargo ? k.tonnes : 0.0;
                job_t += k.kind == ContractKind::cargo ? k.tonnes : 0.0;
                berths -= k.kind == ContractKind::passengers ? k.passengers : 0;
                due = std::min(due, w.now() + k.time_allowed);
            }
            const double m0 = cls.dry_mass_t + s.reaction_mass_t + cargo_mass_t(s) + job_t;
            for (const double days : prm.leg_days) {
                if (!jobs.empty() && w.now() + days_f(days) + margin > due) {
                    continue;
                }
                const CoursePreview p = plot_cheapest_within(c, w, ship, dest, prm.accel_g, days_f(days));
                if (p.status != CourseStatus::ok && p.status != CourseStatus::insufficient_reaction_mass) {
                    continue;
                }
                Credits instalment = 0;
                if (const Loan* loan = player_loan(w); loan != nullptr && loan->next_due < p.arrival) {
                    instalment = instalment_outstanding(*loan);
                    if (cash(w) - deposits - instalment < prm.min_trade_cash) {
                        instalment = 0;
                    }
                }
                const double leg_t = p.reaction_mass_needed_t * (m0 / std::max(1.0, p.wet_mass_t));
                const double per_t = leg_t / m0;
                const double fuel_short = std::max(0.0, 2.0 * leg_t + prm.fuel_margin_t - s.reaction_mass_t);
                const double free = static_cast<double>(cash(w) - deposits - instalment - 100) - fuel_short * price;
                const TradeIdea t = best_trade(c, w, here, dest, free, space, per_t * price);
                const double cost = leg_t * price + static_cast<double>(c.table<StationDef>()[dest].docking_fee);
                const double rate = (static_cast<double>(rewards) + t.profit - cost) / (days + 0.5);
                if (rate > plan.rate) {
                    plan = {dest, jobs, days, t, instalment, rate};
                }
            }
        }
        if (plan.days <= 0.0) {
            rec.advance(w.now() + sim::days(1));
            continue;
        }

        if (plan.instalment > 0) {
            (void)pay_loan(c, w, player_loan_id(w), plan.instalment);
        }
        for (const ContractId id : plan.jobs) {
            (void)contracts::accept(c, w, ship, id);
        }
        // Reaction mass for this leg (with the jobs aboard and the trade to come) and the next.
        {
            const CoursePreview p = plot_cheapest_within(c, w, ship, plan.dest, prm.accel_g, days_f(plan.days));
            const double m_now = p.wet_mass_t;
            const double need = p.reaction_mass_needed_t * (m_now + plan.trade.tonnes) / std::max(1.0, m_now);
            const double want = 2.0 * need + prm.fuel_margin_t - w.ships.at(ship).reaction_mass_t;
            const double tank_space = cls.reaction_mass_capacity_t - w.ships.at(ship).reaction_mass_t;
            const double affordable = static_cast<double>(cash(w) - 50) / std::max(price, 1.0);
            const double fuel = std::floor(std::min({want + 1.0, tank_space, affordable}));
            if (want > 0.0 && fuel >= 1.0) {
                (void)eco::refuel(c, w, ship, fuel);
            }
        }
        if (plan.trade.tonnes >= 1.0) {
            const double t = std::min({plan.trade.tonnes,
                                       cls.cargo_capacity_t - cargo_mass_t(w.ships.at(ship)),
                                       std::floor(static_cast<double>(cash(w) - 50) /
                                                  eco::quote(c, w, here, plan.trade.commodity)->ask)});
            if (t >= 1.0 && eco::buy(c, w, ship, plan.trade.commodity, t).ok()) {
                sell_at = plan.dest;
            }
        }
        const CoursePreview p = plot_cheapest_within(c, w, ship, plan.dest, prm.accel_g, days_f(plan.days));
        DepartResult d = depart(c, w, ship, plan.dest, {prm.accel_g, p.delta_v_km_s * (1.0 + 1e-9) + 1e-6});
        if (!d.ok()) {
            d = depart(c, w, ship, plan.dest, {0.05});
        }
        if (!d.ok()) {
            run.log.push_back(std::format("stuck at {}: {}", c.table<StationDef>()[here].name, d.reason));
            rec.advance(w.now() + sim::days(1));
            continue;
        }
        ++run.legs;
        if (tracing()) {
            Credits rewards = 0;
            for (const ContractId id : plan.jobs) {
                rewards += w.contracts.at(id).reward;
            }
            run.log.push_back(std::format(
                "day {:5.1f} {} -> {}: {} jobs ({} cr), trade {:.0f} t {} ({:.0f} cr), {:.1f} d, fuel {:.1f} t, "
                "cash {} tank {:.0f}",
                (w.now() - rec.start()).to_seconds_f() / 86400.0, c.table<StationDef>()[here].name,
                c.table<StationDef>()[plan.dest].name, plan.jobs.size(), rewards, plan.trade.tonnes,
                plan.trade.tonnes >= 1.0 ? c.table<CommodityDef>().key(plan.trade.commodity) : "-",
                plan.trade.profit, d.preview.duration.to_seconds_f() / 86400.0,
                d.preview.reaction_mass_needed_t, cash(w), w.ships.at(ship).reaction_mass_t));
        }
        rec.advance(d.preview.arrival + sim::minutes(1));
    }
    rec.advance(end);
    for (const Message& m : w.messages) {
        run.missed_payments += m.text.find("MISSED LOAN PAYMENT") != std::string::npos ? 1 : 0;
        if (tracing() && (m.kind == MessageKind::warning)) {
            run.log.push_back(std::format("  [{:.1f}] {}", (m.time - rec.start()).to_seconds_f() / 86400.0, m.text));
        }
    }
    // Closed contracts are forgotten after a month, so count from the books and the journal.
    for (const LedgerEntry& e : w.ledger) {
        if (e.company == w.player && e.category == LedgerCategory::contract && e.amount > 0 &&
            e.description.starts_with("contract #")) {
            run.contract_income += e.amount;
            ++run.contracts_delivered;
        }
    }
    for (const Message& m : w.messages) {
        run.contracts_failed += m.text.starts_with("Contract #") && m.text.find(" failed (") != std::string::npos;
    }
    return run;
}

// ---- The greedy-but-careless trader -------------------------------------------------------------
// Same ore route, but flies flip-and-burn (fastest course) at `accel_g`, spends every credit on
// cargo, refuels only when the drive refuses, and pays the loan only when the lender takes it.

BotRun run_careless(const Content& c, std::uint64_t seed, int weeks, double accel_g) {
    BotRun run;
    run.name = std::format("careless ({} g flip-and-burn)", accel_g);
    World w = start_game(c, seed);
    const ShipId ship = player_ship(w);
    const StationId nail = c.find<StationDef>("hollow_nail");
    const StationId sakai = c.find<StationDef>("sakai_drift");
    const CommodityId ore = c.find<CommodityDef>("ore");
    Recorder rec(c, w, run);
    const sim::Time end = rec.start() + sim::days(7 * weeks) + sim::hours(2);
    const CourseOptions flat_out{accel_g};

    while (w.now() < end && !w.game_over) {
        const StationId here = std::get<Docked>(w.ships.at(ship).location).station;
        if (aboard(w.ships.at(ship), ore) > 0.0 && eco::market_entry(c, here, ore)) {
            (void)eco::sell(c, w, ship, ore, aboard(w.ships.at(ship), ore));
        }
        keep_provisions(c, w, ship);
        const StationId dest = here == nail ? sakai : nail;
        if (here == nail) {
            const auto q = eco::quote(c, w, here, ore);
            const double t = std::floor(static_cast<double>(cash(w)) / (q->ask * 1.05));
            if (t >= 1.0) {
                (void)eco::buy(c, w, ship, ore, t);
            }
        }
        DepartResult d = depart(c, w, ship, dest, flat_out);
        if (d.status == CourseStatus::insufficient_reaction_mass) {
            (void)eco::refuel(c, w, ship, std::nullopt);
            d = depart(c, w, ship, dest, flat_out);
        }
        if (!d.ok()) {
            rec.advance(w.now() + sim::days(1));
            continue;
        }
        ++run.legs;
        rec.advance(d.preview.arrival + sim::minutes(1));
    }
    rec.advance(end);
    for (const Message& m : w.messages) {
        run.missed_payments += m.text.find("MISSED LOAN PAYMENT") != std::string::npos ? 1 : 0;
    }
    return run;
}

BotRun run_idle(const Content& c, std::uint64_t seed, int weeks) {
    BotRun run;
    run.name = "idle";
    World w = start_game(c, seed);
    Recorder rec(c, w, run);
    rec.advance(rec.start() + sim::days(7 * weeks) + sim::hours(2));
    return run;
}

// The seed whose full trajectory trace() prints (BELTER_BALANCE_TRACE_SEED, default 1).
std::uint64_t traced_seed() {
    const char* v = std::getenv("BELTER_BALANCE_TRACE_SEED");
    return v != nullptr && *v != '\0' ? static_cast<std::uint64_t>(std::atoll(v)) : 1;
}

void trace(const BotRun& run) {
    if (!tracing()) {
        return;
    }
    std::cout << std::format("--- {} bot: {} legs, {} missed payments{}, contracts {} delivered / {} "
                             "failed, {} cr contract pay\n",
                             run.name, run.legs, run.missed_payments, run.repossessed ? ", REPOSSESSED" : "",
                             run.contracts_delivered, run.contracts_failed, run.contract_income);
    for (const std::string& line : run.log) {
        std::cout << line << '\n';
    }
    std::cout << "week       cash       loan      cargo  net worth  missed  tank t\n";
    for (const WeekRow& r : run.weeks) {
        if (run.repossessed && r.loan == 0) {
            std::cout << std::format("     (repossessed on day {:.0f})\n",
                                     (*run.repossessed - run.start).to_seconds_f() / 86400.0);
            break;
        }
        std::cout << std::format("{:>4} {:>10} {:>10} {:>10} {:>10} {:>7} {:>7.0f}\n", r.week, r.cash,
                                 r.loan, r.cargo, r.net_worth, static_cast<int>(r.missed), r.tank_t);
    }
}

const WeekRow& week(const BotRun& run, int k) {
    REQUIRE(static_cast<std::size_t>(k) < run.weeks.size());
    return run.weeks[static_cast<std::size_t>(k)];
}

Credits liquid(const WeekRow& r) { return r.cash + r.cargo; }

Credits regular_instalment(const Content& c) {
    return c.table<ScenarioDef>()[c.find<ScenarioDef>("secondhand")].loan_weekly_payment;
}

} // namespace

TEST_CASE("balance: a captain who does nothing is repossessed") {
    // Ceres' 150 cr docking fee eats the 1,850 cr in under two weeks. The interest-only
    // instalments are small, but three missed in a row still cost the ship: first instalment
    // paid (day 7), strikes on days 14, 21 and 28.
    const Content& c = game_content();
    const BotRun idle = run_idle(c, 1, 12);
    trace(idle);
    REQUIRE(idle.repossessed);
    const sim::Time start = idle.weeks.empty() ? sim::Time{} : new_game(c, "secondhand", 1).now();
    CHECK(*idle.repossessed == start + sim::days(28));
}

TEST_CASE("balance: a careful trader survives the Secondhand start but feels the squeeze") {
    // Different seeds give different market noise and supply disruptions; the careful captain
    // must make it through all of them. BELTER_BALANCE_SEEDS widens the survey when tuning.
    const Content& c = game_content();
    std::uint64_t seeds = 16;
    if (const char* n = std::getenv("BELTER_BALANCE_SEEDS"); n != nullptr && *n != '\0') {
        seeds = static_cast<std::uint64_t>(std::atoll(n));
    }
    const Credits instalment = regular_instalment(c);
    if (tracing()) {
        std::cout << "seed  liquid@w4  liquid@w8  cash@w8  min liquid w8-12  networth@w26   gain  loan@w26  missed\n";
    }
    std::vector<Credits> liquid8;
    std::vector<Credits> gains;
    int lost = 0;
    int missed = 0;
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        CAPTURE(seed);
        const BotRun r = run_careful(c, seed, 26);
        if (seed == traced_seed()) {
            trace(r);
        }
        REQUIRE(r.weeks.size() >= 27);
        // Never repossessed; the squeeze may cost a strike or two, never three in a row.
        CHECK_FALSE(r.repossessed);
        CHECK(r.missed_payments <= 2);
        // Richer at the half-year than at the start (net of the loan).
        CHECK(week(r, 26).net_worth > week(r, 0).net_worth);
        // When the interest-only period ends the captain can cover the first full instalment.
        CHECK(liquid(week(r, 8)) > instalment);

        Credits min_liquid = std::numeric_limits<Credits>::max();
        for (int k = 8; k <= 12; ++k) {
            min_liquid = std::min(min_liquid, liquid(week(r, k)));
        }
        liquid8.push_back(liquid(week(r, 8)));
        gains.push_back(week(r, 26).net_worth - week(r, 0).net_worth);
        lost += r.repossessed ? 1 : 0;
        missed += r.missed_payments;
        if (tracing()) {
            std::cout << std::format("{:>4} {:>10} {:>10} {:>8} {:>17} {:>13} {:>6} {:>9} {:>7}\n", seed,
                                     liquid(week(r, 4)), liquid8.back(), week(r, 8).cash, min_liquid,
                                     week(r, 26).net_worth, gains.back(), week(r, 26).loan,
                                     r.missed_payments);
        }
    }
    std::sort(liquid8.begin(), liquid8.end());
    std::sort(gains.begin(), gains.end());
    if (tracing()) {
        std::cout << std::format("SUMMARY lost {}/{}  missed {}  liquid@w8 min {} median {} max {}  "
                                 "gain@w26 min {} median {}\n",
                                 lost, seeds, missed, liquid8.front(), liquid8[liquid8.size() / 2],
                                 liquid8.back(), gains.front(), gains[gains.size() / 2]);
    }
    // The squeeze: for the typical captain, week 8 finds only a few weeks' instalments in hand.
    CHECK(liquid8[liquid8.size() / 2] < 10 * instalment);
}

TEST_CASE("balance: a greedy careless trader does clearly worse than a careful one") {
    const Content& c = game_content();
    for (const std::uint64_t seed : {1u, 2u, 3u}) {
        CAPTURE(seed);
        const WeekRow careful = run_careful(c, seed, 26).weeks.back();
        // Flat out at 1 g: the tank is dry within two round trips and every credit is in cargo.
        const BotRun reckless = run_careless(c, seed, 26, 1.0);
        // Flip-and-burn at 0.3 g, the shell's default: survives longer, still bleeds fuel.
        const BotRun hasty = run_careless(c, seed, 26, 0.3);
        if (seed == traced_seed()) {
            trace(reckless);
            trace(hasty);
        }
        CHECK(reckless.repossessed);
        CHECK((hasty.repossessed || hasty.weeks.back().net_worth + 10000 < careful.net_worth));
    }
}

TEST_CASE("balance: contracts make a careful captain noticeably better off") {
    // Contracts are the broke captain's way in: paid work without capital. Over the first half
    // year the contractor should clearly beat the pure ore trader, yet the start stays tight.
    const Content& c = game_content();
    std::uint64_t seeds = 8;
    if (const char* n = std::getenv("BELTER_BALANCE_SEEDS"); n != nullptr && *n != '\0') {
        seeds = static_cast<std::uint64_t>(std::atoll(n));
    }
    const Credits instalment = regular_instalment(c);
    if (tracing()) {
        std::cout << "seed  trader@w26  contractor@w4 @w8 @w26  jobs done/failed  contract pay  missed\n";
    }
    std::vector<Credits> trader_gain;
    std::vector<Credits> contractor_gain;
    std::vector<Credits> liquid4;
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        CAPTURE(seed);
        const BotRun trader = run_careful(c, seed, 26);
        const BotRun contractor = run_contractor(c, seed, 26);
        if (seed == traced_seed()) {
            trace(contractor);
        }
        REQUIRE(contractor.weeks.size() >= 27);
        CHECK_FALSE(contractor.repossessed);
        CHECK(contractor.missed_payments <= 1);
        CHECK(contractor.contracts_delivered > 10);
        CHECK(contractor.contracts_failed <= 1);
        trader_gain.push_back(week(trader, 26).net_worth - week(trader, 0).net_worth);
        contractor_gain.push_back(week(contractor, 26).net_worth - week(contractor, 0).net_worth);
        liquid4.push_back(liquid(week(contractor, 4)));
        if (tracing()) {
            std::cout << std::format("{:>4} {:>11} {:>14} {:>6} {:>6} {:>10}/{:<6} {:>12} {:>7}\n", seed,
                                     trader_gain.back(), liquid(week(contractor, 4)),
                                     liquid(week(contractor, 8)), contractor_gain.back(),
                                     contractor.contracts_delivered, contractor.contracts_failed,
                                     contractor.contract_income, contractor.missed_payments);
        }
    }
    std::sort(trader_gain.begin(), trader_gain.end());
    std::sort(contractor_gain.begin(), contractor_gain.end());
    std::sort(liquid4.begin(), liquid4.end());
    const Credits trader_median = trader_gain[trader_gain.size() / 2];
    const Credits contractor_median = contractor_gain[contractor_gain.size() / 2];
    if (tracing()) {
        std::cout << std::format("SUMMARY gain@w26 median: trader {} contractor {}; contractor liquid@w4 median {}\n",
                                 trader_median, contractor_median, liquid4[liquid4.size() / 2]);
    }
    // Noticeably better: at least half as much again as the trader's gain...
    CHECK(contractor_median > trader_median + trader_median / 2);
    // ...but not a money printer, and the first month is still lean.
    CHECK(contractor_median < 5 * trader_median);
    CHECK(liquid4[liquid4.size() / 2] < 8 * instalment);
}

TEST_CASE("balance: bots are deterministic") {
    const Content& c = game_content();
    const BotRun a = run_careful(c, 3, 10);
    const BotRun b = run_careful(c, 3, 10);
    REQUIRE(a.weeks.size() == b.weeks.size());
    for (std::size_t i = 0; i < a.weeks.size(); ++i) {
        CHECK(a.weeks[i].cash == b.weeks[i].cash);
        CHECK(a.weeks[i].loan == b.weeks[i].loan);
        CHECK(a.weeks[i].cargo == b.weeks[i].cargo);
    }
}
