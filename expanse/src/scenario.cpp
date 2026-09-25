#include "expanse/scenario.hpp"

#include "expanse/calendar.hpp"
#include "expanse/crew.hpp"
#include "expanse/finance.hpp"

#include <stdexcept>
#include <string>

namespace expanse {

World new_game(const Content& content, std::string_view scenario_key, std::uint64_t seed) {
    const auto scenario_id = content.find<ScenarioDef>(scenario_key);
    if (!scenario_id) {
        throw std::invalid_argument("unknown scenario '" + std::string(scenario_key) + "'");
    }
    const ScenarioDef& sc = content.table<ScenarioDef>()[scenario_id];
    const sim::Time start = calendar::to_time(*calendar::parse_date(sc.start_date));

    World w;
    w.content_fingerprint = content.fingerprint();
    w.seed = seed;
    w.scheduler = sim::Scheduler<Event>(start);

    // Station markets start from their defined stock.
    const auto& commodities = content.table<CommodityDef>();
    for (auto [id, st] : content.table<StationDef>()) {
        (void)id;
        StationState state;
        state.stock.assign(commodities.size(), 0.0);
        for (const MarketEntryDef& m : st.market) {
            state.stock[index_of(m.commodity)] = m.stock;
        }
        w.stations.push_back(std::move(state));
    }

    // The player: one secondhand ship, almost no money, and a loan.
    w.player = w.companies.insert(Company{sc.company_name, sc.cash, true});

    const ShipClassDef& cls = content.table<ShipClassDef>()[sc.ship_class];
    Ship ship;
    ship.name = sc.ship_name;
    ship.ship_class = sc.ship_class;
    ship.owner = w.player;
    ship.location = Docked{sc.start_station};
    ship.reaction_mass_t = cls.reaction_mass_capacity_t * sc.reaction_mass_fraction;
    ship.hull_condition = sc.hull_condition;
    const ShipId ship_id = w.ships.insert(std::move(ship));

    if (sc.loan_principal > 0) {
        open_loan(w, w.player, sc.start_station, sc.loan_principal, sc.loan_weekly_payment,
                  sc.loan_missed_payment_limit, start + sim::days(7));
    }

    // The captain, provisions aboard, and job-seekers on every dock.
    crew::start_new_game(content, w, sc, ship_id);

    // Recurring systems, on a grid anchored at midnight of the epoch.
    w.scheduler.add_periodic(sim::days(1), DailyTick{}, {sim::Time{}, priority_daily, 0});
    w.scheduler.add_periodic(sim::days(7), WeeklyTick{}, {sim::Time{}, priority_weekly, 0});

    return w;
}

} // namespace expanse
