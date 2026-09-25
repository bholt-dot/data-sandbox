#include <doctest/doctest.h>

#include "expanse/contracts.hpp"
#include "expanse/crew.hpp"
#include "expanse/economy.hpp"
#include "expanse/finance.hpp"
#include "expanse/scenario.hpp"
#include "expanse/shell.hpp"
#include "expanse/ships.hpp"
#include "expanse/simulation.hpp"
#include "expanse/world.hpp"

#include "simcore/hash.hpp"
#include "simcore/snapshot.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <sstream>
#include <string>

using namespace expanse;

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

ShipId player_ship(const World& w) {
    for (auto [id, ship] : w.ships) {
        if (ship.owner == w.player) {
            return id;
        }
    }
    return {};
}

Credits cash(const World& w) { return w.companies.at(w.player).cash; }

StationId station(const char* key) {
    const StationId id = game_content().find<StationDef>(key);
    REQUIRE(id);
    return id;
}

CommodityId commodity(const char* key) {
    const CommodityId id = game_content().find<CommodityDef>(key);
    REQUIRE(id);
    return id;
}

// A hand-made offer with known terms, posted now.
ContractId post(World& w, ContractKind kind, const char* from, const char* to, double tonnes_or_people,
                Credits reward, Credits deposit, sim::Duration allowed = sim::days(8)) {
    Contract k;
    k.kind = kind;
    k.origin = station(from);
    k.destination = station(to);
    if (kind == ContractKind::cargo) {
        k.commodity = commodity("water");
        k.tonnes = tonnes_or_people;
    } else {
        k.passengers = static_cast<std::uint8_t>(tonnes_or_people);
    }
    k.reward = reward;
    k.deposit = deposit;
    k.time_allowed = allowed;
    k.posted_at = w.now();
    k.expires_at = w.now() + sim::days(5);
    return w.contracts.insert(std::move(k));
}

// Flies the player's ship to `to` on the cheapest course arriving within `within`; returns the
// arrival time. Tops up the tank first so tests don't depend on the scenario's fuel.
sim::Time fly(World& w, const char* to, sim::Duration within = sim::days(5)) {
    const Content& c = game_content();
    const ShipId ship = player_ship(w);
    w.ships.at(ship).reaction_mass_t = 400.0;
    const CoursePreview p = plot_cheapest_within(c, w, ship, station(to), 0.3, within);
    REQUIRE(p.feasible());
    const DepartResult d = depart(c, w, ship, station(to), {0.3, p.delta_v_km_s * (1.0 + 1e-9) + 1e-6});
    REQUIRE(d.ok());
    return d.preview.arrival;
}

std::size_t ledger_count(const World& w, LedgerCategory cat) {
    std::size_t n = 0;
    for (const LedgerEntry& e : w.ledger) {
        n += e.category == cat && e.company == w.player ? 1u : 0u;
    }
    return n;
}

double stock(const World& w, const char* st, const char* com) {
    return w.stations[index_of(station(st))].stock[index_of(commodity(com))];
}

} // namespace

TEST_CASE("contract boards fill at the start and are deterministic by seed") {
    const Content& c = game_content();
    World a = new_game(c, "secondhand", 11);
    World b = new_game(c, "secondhand", 11);
    World other = new_game(c, "secondhand", 12);
    for (auto [id, st] : c.table<StationDef>()) {
        const auto board = contracts::board_at(a, id);
        CHECK(board.size() <= contracts::board_capacity(st.population));
        if (st.population > 0) {
            CHECK_FALSE(board.empty());
        }
        for (const ContractId k : board) {
            const Contract& ct = a.contracts.at(k);
            CHECK(ct.reward > 0);
            CHECK(ct.destination != id);
            CHECK(ct.time_allowed > sim::Duration{});
            CHECK(ct.expires_at > a.now());
        }
    }
    advance_to(c, a, a.now() + sim::days(20), false);
    advance_to(c, b, b.now() + sim::days(20), false);
    advance_to(c, other, other.now() + sim::days(20), false);
    CHECK(world_hash(a) == world_hash(b));
    CHECK(sim::hash_state(a.contracts) == sim::hash_state(b.contracts));
    CHECK(sim::hash_state(a.contracts) != sim::hash_state(other.contracts));
}

TEST_CASE("the Ceres cluster always has local work") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 3);
    const std::array cluster{station("ceres_station"), station("hollow_nail"), station("dagu_rock"),
                             station("sakai_drift")};
    int days_without = 0;
    for (int day = 0; day < 90; ++day) {
        for (const StationId here : cluster) {
            bool local = false;
            for (const ContractId id : contracts::board_at(w, here)) {
                const Contract& k = w.contracts.at(id);
                local = local || (k.min_standing == 0 &&
                                  std::find(cluster.begin(), cluster.end(), k.destination) != cluster.end());
            }
            days_without += local ? 0 : 1;
        }
        advance_to(c, w, w.now() + sim::days(1), false);
    }
    // Four boards x 90 days: an empty local board is a rare bad day, not the norm.
    CHECK(days_without < 36);
}

TEST_CASE("deadlines are feasible at modest acceleration") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 5);
    for (auto [id, k] : w.contracts) {
        const auto t = contracts::reference_transit(c, k.origin, k.destination, k.posted_at);
        REQUIRE(t.has_value());
        CHECK(k.time_allowed.to_seconds_f() >= contracts::tuning::min_slack * t->to_seconds_f());
        CHECK(k.time_allowed.to_seconds_f() <=
              contracts::tuning::max_slack * t->to_seconds_f() + 86400.0);
        (void)id;
    }
}

TEST_CASE("untaken offers expire and are withdrawn") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ContractId id = post(w, ContractKind::cargo, "ceres_station", "hollow_nail", 10.0, 300, 0);
    const sim::Time expires = w.contracts.at(id).expires_at;
    advance_to(c, w, expires - sim::hours(1), false);
    CHECK(w.contracts.contains(id));
    advance_to(c, w, expires + sim::days(1), false);
    CHECK_FALSE(w.contracts.contains(id));
    const auto board = contracts::board_at(w, station("ceres_station"));
    CHECK(std::find(board.begin(), board.end(), id) == board.end());
    CHECK_FALSE(contracts::accept(c, w, player_ship(w), id).ok);
}

TEST_CASE("accept checks location hold berths deposit and standing") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ShipId ship = player_ship(w);

    const ContractId elsewhere = post(w, ContractKind::cargo, "dagu_rock", "hollow_nail", 10.0, 300, 0);
    CHECK_FALSE(contracts::can_accept(c, w, ship, elsewhere).ok);
    CHECK(contracts::can_accept(c, w, ship, elsewhere).reason.find("docked at Dagu Rock") != std::string::npos);

    const ContractId huge = post(w, ContractKind::cargo, "ceres_station", "hollow_nail", 700.0, 300, 0);
    CHECK(contracts::can_accept(c, w, ship, huge).reason.find("hold") != std::string::npos);

    const ContractId crowd = post(w, ContractKind::passengers, "ceres_station", "hollow_nail", 4.0, 300, 0);
    CHECK(contracts::can_accept(c, w, ship, crowd).reason.find("berths") != std::string::npos);
    const ContractId three = post(w, ContractKind::passengers, "ceres_station", "hollow_nail", 3.0, 300, 0);
    CHECK(contracts::can_accept(c, w, ship, three).ok);

    const ContractId dear = post(w, ContractKind::cargo, "ceres_station", "hollow_nail", 50.0, 900, 5000);
    CHECK(contracts::can_accept(c, w, ship, dear).reason.find("deposit") != std::string::npos);

    const ContractId premium = post(w, ContractKind::cargo, "ceres_station", "hollow_nail", 5.0, 300, 0);
    w.contracts.at(premium).min_standing = 3;
    CHECK(contracts::can_accept(c, w, ship, premium).reason.find("good standing") != std::string::npos);

    // A refused accept changes nothing.
    const Credits before = cash(w);
    CHECK_FALSE(contracts::accept(c, w, ship, dear).ok);
    CHECK(cash(w) == before);
    CHECK(w.contracts.at(dear).status == ContractStatus::offered);
    CHECK(w.ships.at(ship).consignments.empty());

    // Passengers take berths: a full ship can't hire.
    REQUIRE(contracts::accept(c, w, ship, three).ok);
    CHECK(contracts::passengers_aboard(w, ship) == 3);
    const auto pool = crew::pool_at(w, station("ceres_station"));
    REQUIRE_FALSE(pool.empty());
    CHECK(crew::hire(c, w, ship, pool.front()).status == crew::HireStatus::no_berth);
    const ContractId one_more = post(w, ContractKind::passengers, "ceres_station", "dagu_rock", 1.0, 100, 0);
    CHECK(contracts::can_accept(c, w, ship, one_more).reason.find("berth") != std::string::npos);
}

TEST_CASE("cargo is loaded on accept and delivered with pay and deposit back on docking") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ShipId ship = player_ship(w);
    const ContractId id = post(w, ContractKind::cargo, "ceres_station", "hollow_nail", 60.0, 800, 500);
    const double cargo_before = cargo_mass_t(w.ships.at(ship));
    const double origin_before = stock(w, "ceres_station", "water");
    const Credits cash_before = cash(w);

    const contracts::Result r = contracts::accept(c, w, ship, id);
    REQUIRE(r.ok);
    const Contract& k = w.contracts.at(id);
    CHECK(k.status == ContractStatus::accepted);
    CHECK(k.holder == w.player);
    CHECK(k.deadline == w.now() + sim::days(8));
    CHECK(cash(w) == cash_before - 500);
    CHECK(cargo_mass_t(w.ships.at(ship)) == doctest::Approx(cargo_before + 60.0));
    REQUIRE(w.ships.at(ship).consignments.size() == 1);
    CHECK(stock(w, "ceres_station", "water") == doctest::Approx(origin_before - 60.0));
    // Contract cargo is not the captain's to sell.
    CHECK_FALSE(economy::sell(c, w, ship, commodity("water"), 60.0).ok());
    CHECK(contracts::held_by(w, w.player) == std::vector<ContractId>{id});

    const sim::Time arrival = fly(w, "hollow_nail");
    REQUIRE(arrival < w.contracts.at(id).deadline);
    const Credits cash_departed = cash(w);
    const double dest_before = stock(w, "hollow_nail", "water");
    advance_to(c, w, arrival + sim::minutes(1), false);
    const Contract& done = w.contracts.at(id);
    CHECK(done.status == ContractStatus::delivered);
    CHECK(done.paid == 800);
    CHECK_FALSE(done.aboard);
    CHECK(w.ships.at(ship).consignments.empty());
    CHECK(cash(w) >= cash_departed + 1300 - 200); // pay + deposit, less at most a night's fees
    CHECK(stock(w, "hollow_nail", "water") >= dest_before + 60.0 - 10.0);
    CHECK(contracts::standing(w, w.player, Faction::belt) == 1);
    // Deposit out, deposit back, reward: all in the contract column of the books.
    CHECK(ledger_count(w, LedgerCategory::contract) == 3);
    const LedgerSummary books = summarize_ledger(w, w.player, sim::Time{}, w.now() + sim::seconds(1));
    CHECK(books.income_of(LedgerCategory::contract) == 1300);
    CHECK(books.expense_of(LedgerCategory::contract) == 500);
}

TEST_CASE("late delivery pays less and too late fails and forfeits the deposit") {
    const Content& c = game_content();
    SUBCASE("a little late") {
        World w = new_game(c, "secondhand", 1);
        const ShipId ship = player_ship(w);
        // Allowed 4 days (grace 1 day); the trip takes ~4.5.
        const ContractId id = post(w, ContractKind::cargo, "ceres_station", "hollow_nail", 20.0, 1000, 0, sim::days(4));
        REQUIRE(contracts::accept(c, w, ship, id).ok);
        const sim::Time arrival = fly(w, "hollow_nail", sim::days(4) + sim::hours(12));
        const sim::Time deadline = w.contracts.at(id).deadline;
        REQUIRE(arrival > deadline);
        REQUIRE(arrival <= deadline + contracts::grace(w.contracts.at(id)));
        advance_to(c, w, arrival + sim::minutes(1), false);
        const Contract& k = w.contracts.at(id);
        CHECK(k.status == ContractStatus::delivered);
        CHECK(k.paid < 1000);
        CHECK(k.paid >= 500);
        CHECK(contracts::standing(w, w.player, Faction::belt) == -contracts::tuning::late_loss);
    }
    SUBCASE("far too late") {
        World w = new_game(c, "secondhand", 1);
        const ShipId ship = player_ship(w);
        REQUIRE(transact(w, w.player, 5000, LedgerCategory::other, "windfall"));
        const ContractId id = post(w, ContractKind::cargo, "ceres_station", "hollow_nail", 100.0, 1000, 700, sim::days(2));
        REQUIRE(contracts::accept(c, w, ship, id).ok);
        const Credits after_deposit = cash(w);
        // Sit at the dock: the at-risk warning is urgent, then the job fails after the grace day.
        const AdvanceReport r = advance_to(c, w, w.now() + sim::days(2), true);
        CHECK(r.stopped_early);
        advance_to(c, w, w.now() + sim::days(4), false);
        const Contract& k = w.contracts.at(id);
        CHECK(k.status == ContractStatus::failed);
        CHECK_FALSE(k.aboard); // docked: put off at once
        CHECK(w.ships.at(ship).consignments.empty());
        CHECK(contracts::standing(w, w.player, Faction::belt) == -contracts::tuning::fail_loss);
        // No refund: only docking fees moved the cash since.
        CHECK(cash(w) < after_deposit);
        CHECK(ledger_count(w, LedgerCategory::contract) == 1);
    }
}

TEST_CASE("an eta past the deadline posts an urgent warning") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ShipId ship = player_ship(w);
    const ContractId id = post(w, ContractKind::passengers, "ceres_station", "hollow_nail", 1.0, 400, 0, sim::days(3));
    REQUIRE(contracts::accept(c, w, ship, id).ok);
    fly(w, "hollow_nail", sim::days(5));
    const std::size_t seen = w.messages.size();
    advance_to(c, w, w.now() + sim::days(1), false);
    bool warned = false;
    for (std::size_t i = seen; i < w.messages.size(); ++i) {
        warned = warned || (w.messages[i].urgent && w.messages[i].text.find("past the deadline") != std::string::npos);
    }
    CHECK(warned);
}

TEST_CASE("abandoning costs the deposit or a fee and standing") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ShipId ship = player_ship(w);
    const ContractId cargo = post(w, ContractKind::cargo, "ceres_station", "hollow_nail", 30.0, 600, 400);
    const ContractId pax = post(w, ContractKind::passengers, "ceres_station", "dagu_rock", 2.0, 500, 0);
    REQUIRE(contracts::accept(c, w, ship, cargo).ok);
    REQUIRE(contracts::accept(c, w, ship, pax).ok);
    const Credits before = cash(w);
    CHECK(contracts::abandon(c, w, w.player, cargo).ok);
    CHECK(cash(w) == before); // the deposit simply isn't returned
    CHECK(w.ships.at(ship).consignments.empty());
    CHECK(contracts::abandon(c, w, w.player, pax).ok);
    CHECK(cash(w) == before - 50); // 10% cancellation fee stands in for a deposit
    CHECK(contracts::passengers_aboard(w, ship) == 0);
    CHECK(contracts::standing(w, w.player, Faction::belt) == -2 * contracts::tuning::abandon_loss);
    CHECK_FALSE(contracts::abandon(c, w, w.player, pax).ok);
}

TEST_CASE("passengers consume life support like crew") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ShipId ship = player_ship(w);
    const crew::Provisions alone = crew::daily_need(c, w, ship);
    const ContractId id = post(w, ContractKind::passengers, "ceres_station", "hollow_nail", 2.0, 500, 0);
    REQUIRE(contracts::accept(c, w, ship, id).ok);
    const crew::Provisions with = crew::daily_need(c, w, ship);
    CHECK(with.food_t == doctest::Approx(3.0 * alone.food_t));
    CHECK(with.water_t == doctest::Approx(3.0 * alone.water_t));
    advance_to(c, w, w.now() + sim::hours(1), false);
    const double food_before = crew::stores(c, w.ships.at(ship)).food_t;
    advance_to(c, w, w.now() + sim::days(1), false);
    CHECK(crew::stores(c, w.ships.at(ship)).food_t == doctest::Approx(food_before - with.food_t));
    // Delivered passengers leave the berths.
    advance_to(c, w, fly(w, "hollow_nail") + sim::minutes(1), false);
    CHECK(w.contracts.at(id).status == ContractStatus::delivered);
    CHECK(contracts::passengers_aboard(w, ship) == 0);
}

TEST_CASE("standing changes the terms offered") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ContractId id = post(w, ContractKind::cargo, "ceres_station", "hollow_nail", 100.0, 1000, 800);
    const auto neutral = contracts::terms_for(c, w, w.contracts.at(id), w.player);
    CHECK(neutral.reward == 1000);
    CHECK(neutral.deposit == 800);
    w.standings.push_back(Standing{w.player, Faction::belt, 5});
    const auto good = contracts::terms_for(c, w, w.contracts.at(id), w.player);
    CHECK(good.reward > neutral.reward);
    CHECK(good.deposit < neutral.deposit);
    w.standings.back().value = contracts::tuning::refuse_standing;
    CHECK(contracts::can_accept(c, w, player_ship(w), id).reason.find("mud") != std::string::npos);
}

TEST_CASE("save and load mid contract matches an uninterrupted run") {
    const Content& c = game_content();
    World a = new_game(c, "secondhand", 4);
    const ContractId id = post(a, ContractKind::cargo, "ceres_station", "sakai_drift", 40.0, 700, 300);
    REQUIRE(contracts::accept(c, a, player_ship(a), id).ok);
    const sim::Time arrival = fly(a, "sakai_drift", sim::days(6));
    advance_to(c, a, a.now() + sim::days(2), false);
    REQUIRE(a.contracts.at(id).aboard);

    World b = load_world(save_world(a), c);
    CHECK(world_hash(a) == world_hash(b));
    advance_to(c, a, arrival + sim::days(10), false);
    advance_to(c, b, arrival + sim::days(10), false);
    CHECK(a.contracts.at(id).status == ContractStatus::delivered);
    CHECK(world_hash(a) == world_hash(b));
}

TEST_CASE("closed contracts are forgotten after a month") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const ContractId id = post(w, ContractKind::passengers, "ceres_station", "hollow_nail", 1.0, 300, 0);
    REQUIRE(contracts::accept(c, w, player_ship(w), id).ok);
    REQUIRE(contracts::abandon(c, w, w.player, id).ok);
    advance_to(c, w, w.now() + sim::days(contracts::tuning::keep_closed_days + 1), false);
    CHECK_FALSE(w.contracts.contains(id));
    // Boards stay bounded however long the game runs.
    advance_to(c, w, w.now() + sim::days(365), false);
    std::size_t capacity = 0;
    for (auto [sid, st] : c.table<StationDef>()) {
        (void)sid;
        capacity += contracts::board_capacity(st.population);
    }
    CHECK(w.contracts.size() <= capacity);
}

namespace {

struct Shell {
    std::shared_ptr<const Content> content;
    Session session;
    ShellBus bus;

    Shell() {
        sim::Diagnostics diags;
        content = Content::load(EXPANSE_DATA_DIR, diags);
        REQUIRE(content);
        session.content = content;
        register_game_commands(bus, content);
    }

    std::string run(std::string_view line) {
        sim::Doc out;
        const sim::LineResult r = bus.execute_line(line, session, out);
        if (!r.ok()) {
            out << "error: " << r.error << "\n";
        }
        return sim::to_text(out);
    }
};

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("shell jobs accept contracts and abandon") {
    Shell sh;
    sh.run("new secondhand --seed 2");
    World& w = *sh.session.world;
    const ContractId id = post(w, ContractKind::cargo, "ceres_station", "hollow_nail", 25.0, 450, 0);
    const std::string jobs = sh.run("jobs");
    CHECK(contains(jobs, "Jobs at Ceres Station"));
    CHECK(contains(jobs, std::format("#{}", id.index)));
    CHECK(contains(jobs, "25 t Water ice -> Hollow Nail"));
    CHECK(contains(jobs, "450 cr"));
    CHECK(contains(sh.run("contracts"), "(none"));
    CHECK(contains(sh.run(std::format("accept {}", id.index)), "Took contract"));
    CHECK(contains(sh.run(std::format("accept {}", id.index)), "error"));
    CHECK(contains(sh.run("status"), "(contract #"));
    const std::string held = sh.run("contracts");
    CHECK(contains(held, "Water ice -> Hollow Nail"));
    CHECK(contains(held, "not en route"));
    sh.run("go hollow_nail --within 5d");
    CHECK(contains(sh.run("contracts"), "on time"));
    CHECK(contains(sh.run("accept 999999"), "no contract"));
    CHECK(contains(sh.run(std::format("abandon {}", id.index)), "Abandoned"));
    CHECK(contains(sh.run("contracts"), "belt -2"));
    CHECK(contains(sh.run("jobs"), "not docked"));
}

TEST_CASE("shell session with contracts replays to the same hash") {
    Shell a;
    a.run("new secondhand --seed 6");
    const auto board = contracts::board_at(*a.session.world, station("ceres_station"));
    REQUIRE_FALSE(board.empty());
    for (const std::string& line : {std::format("accept {}", board.front().index), std::string("advance 10d --force")}) {
        a.run(line);
    }
    std::ostringstream script;
    a.bus.write_script(script);
    Shell b;
    std::istringstream lines(script.str());
    for (std::string line; std::getline(lines, line);) {
        b.run(line);
    }
    CHECK(world_hash(*a.session.world) == world_hash(*b.session.world));
}
