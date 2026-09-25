#include <doctest/doctest.h>

#include "expanse/content.hpp"
#include "expanse/crew.hpp"
#include "expanse/scenario.hpp"
#include "expanse/simulation.hpp"
#include "expanse/stat_ids.hpp"
#include "expanse/world.hpp"

#include <memory>
#include <string>
#include <string_view>

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

StationId station(std::string_view key) { return game_content().find<StationDef>(key); }

ShipId player_ship(const World& w) {
    for (auto [id, s] : w.ships) {
        if (s.owner == w.player) {
            return id;
        }
    }
    return {};
}

Credits& cash(World& w) { return w.companies.at(w.player).cash; }

CrewId add_seeker(World& w, StationId at, CrewRole role, std::uint8_t skill, Credits wage,
                  double morale = crew::neutral_morale) {
    CrewMember m;
    m.name = "Test Hand";
    m.role = role;
    m.skill = skill;
    m.wage_per_week = wage;
    m.morale = morale;
    m.station = at;
    return w.crew.insert(std::move(m));
}

CrewId captain_of(const World& w, ShipId ship) {
    for (CrewId id : crew::aboard(w, ship)) {
        if (w.crew.at(id).role == CrewRole::captain) {
            return id;
        }
    }
    return {};
}

void set_underway(World& w, ShipId ship) {
    Ship& s = w.ships.at(ship);
    const StationId from = std::get<Docked>(s.location).station;
    Underway u;
    u.origin = from;
    u.destination = station("tycho_station");
    u.departure = w.now();
    u.arrival = w.now() + sim::days(10);
    s.location = u;
}

void set_docked(World& w, ShipId ship, StationId at) { w.ships.at(ship).location = Docked{at}; }

bool has_message(const World& w, std::string_view needle, bool urgent_only = false) {
    for (const Message& m : w.messages) {
        if (m.text.find(needle) != std::string::npos && (!urgent_only || m.urgent)) {
            return true;
        }
    }
    return false;
}

double stock(const World& w, ShipId ship, std::string_view key) {
    const CommodityId c = game_content().find<CommodityDef>(key);
    double t = 0.0;
    for (const CargoLot& lot : w.ships.at(ship).cargo) {
        if (lot.commodity == c) {
            t += lot.tonnes;
        }
    }
    return t;
}

void set_stock(World& w, ShipId ship, std::string_view key, double tonnes) {
    const CommodityId c = game_content().find<CommodityDef>(key);
    auto& cargo = w.ships.at(ship).cargo;
    std::erase_if(cargo, [&](const CargoLot& lot) { return lot.commodity == c; });
    if (tonnes > 0.0) {
        cargo.push_back(CargoLot{c, tonnes, 0});
    }
}

std::string all_names(const World& w) {
    std::string s;
    for (auto [id, m] : w.crew) {
        (void)id;
        s += m.name + "|";
    }
    return s;
}

} // namespace

TEST_CASE("new game puts the captain aboard with provisions") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 42);
    const ShipId ship = player_ship(w);
    const auto crew_aboard = crew::aboard(w, ship);
    REQUIRE(crew_aboard.size() == 1);
    const CrewMember& captain = w.crew.at(crew_aboard.front());
    CHECK(captain.role == CrewRole::captain);
    CHECK(captain.wage_per_week == 0);
    CHECK_FALSE(captain.name.empty());
    CHECK(crew::weekly_payroll(w, ship) == 0);
    const crew::Provisions p = crew::stores(c, w.ships.at(ship));
    CHECK(p.food_t == doctest::Approx(21 * crew::life_support_per_person.food_t));
    CHECK(p.water_t == doctest::Approx(21 * crew::life_support_per_person.water_t));
    CHECK(p.oxygen_t > 21 * crew::life_support_per_person.oxygen_t); // plus hull leakage
}

TEST_CASE("hiring pool is deterministic by seed") {
    const Content& c = game_content();
    World a = new_game(c, "secondhand", 5);
    World b = new_game(c, "secondhand", 5);
    World other = new_game(c, "secondhand", 6);
    CHECK(all_names(a) == all_names(b));
    CHECK(world_hash(a) == world_hash(b));
    CHECK(all_names(a) != all_names(other));
    for (int week = 0; week < 5; ++week) {
        systems::weekly_crew(c, a);
        systems::weekly_crew(c, b);
    }
    CHECK(all_names(a) == all_names(b));
    CHECK(world_hash(a) == world_hash(b));
}

TEST_CASE("hiring pool size scales with station population") {
    CHECK(crew::pool_target(0) == 0);
    CHECK(crew::pool_target(100) >= 1);
    CHECK(crew::pool_target(15000) < crew::pool_target(250000));
    CHECK(crew::pool_target(250000) < crew::pool_target(6000000));
    CHECK(crew::pool_target(4000000000u) <= 12);

    const Content& c = game_content();
    World w = new_game(c, "secondhand", 3);
    for (auto [id, st] : c.table<StationDef>()) {
        CHECK(crew::pool_at(w, id).size() == crew::pool_target(st.population));
    }
    CHECK(crew::pool_at(w, station("ceres_station")).size() >
          crew::pool_at(w, station("tycho_station")).size());

    // Weekly turnover: some leave, newcomers top the pool back up.
    const auto before = crew::pool_at(w, station("ceres_station"));
    for (int week = 0; week < 4; ++week) {
        systems::weekly_crew(c, w);
    }
    const auto after = crew::pool_at(w, station("ceres_station"));
    CHECK(after.size() == before.size());
    CHECK(after != before);
}

TEST_CASE("job seekers have sane skills and origin priced wages") {
    const Content& c = game_content();
    const CrewOriginDef& belter = c.table<CrewOriginDef>()[c.find<CrewOriginDef>("belter")];
    const CrewOriginDef& martian = c.table<CrewOriginDef>()[c.find<CrewOriginDef>("martian")];
    CHECK(crew::expected_wage(CrewRole::pilot, 60, belter.wage_multiplier) <
          crew::expected_wage(CrewRole::pilot, 60, martian.wage_multiplier));
    CHECK(crew::expected_wage(CrewRole::engineer, 20, 1.0) < crew::expected_wage(CrewRole::engineer, 80, 1.0));
    CHECK(crew::expected_wage(CrewRole::deckhand, 50, 1.0) < crew::expected_wage(CrewRole::pilot, 50, 1.0));

    int at_mars = 0;
    int martians_at_mars = 0;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        World w = new_game(c, "secondhand", seed);
        for (auto [id, m] : w.crew) {
            (void)id;
            if (m.role == CrewRole::captain) {
                continue;
            }
            CHECK_FALSE(m.ship);
            CHECK(m.skill >= 5);
            CHECK(m.skill <= 95);
            const CrewOriginDef& o = c.table<CrewOriginDef>()[m.origin];
            const auto fair = static_cast<double>(crew::expected_wage(m.role, m.skill, o.wage_multiplier));
            CHECK(static_cast<double>(m.wage_per_week) >= fair * 0.9 - 10.0);
            CHECK(static_cast<double>(m.wage_per_week) <= fair * 1.1 + 10.0);
            if (m.station == station("mars_highport")) {
                ++at_mars;
                martians_at_mars += m.origin == c.find<CrewOriginDef>("martian") ? 1 : 0;
            }
        }
    }
    REQUIRE(at_mars > 0);
    CHECK(martians_at_mars * 2 > at_mars);
}

TEST_CASE("hire signs a seeker on and charges a signing bonus") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 8);
    const ShipId ship = player_ship(w);
    const CrewId seeker = crew::pool_at(w, station("ceres_station")).front();
    cash(w) = 100000;
    const Credits wage = w.crew.at(seeker).wage_per_week;

    const crew::HireResult r = crew::hire(c, w, ship, seeker);
    REQUIRE(r.ok());
    CHECK(r.reason.empty());
    CHECK(r.signing_bonus == wage);
    CHECK(cash(w) == 100000 - wage);
    CHECK(w.crew.at(seeker).ship == ship);
    CHECK(crew::aboard(w, ship).size() == 2);
    CHECK(crew::weekly_payroll(w, ship) == wage);
    CHECK(has_message(w, "signed on"));
}

TEST_CASE("hire refuses with a reason and changes nothing") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 8);
    const ShipId ship = player_ship(w);
    cash(w) = 100000;
    const CrewId local = add_seeker(w, station("ceres_station"), CrewRole::deckhand, 30, 200);
    const CrewId faraway = add_seeker(w, station("tycho_station"), CrewRole::pilot, 50, 500);

    auto refused = [&](crew::HireResult r, crew::HireStatus expected) {
        CHECK(r.status == expected);
        CHECK_FALSE(r.ok());
        CHECK_FALSE(r.reason.empty());
    };

    refused(crew::hire(c, w, ShipId::null(), local), crew::HireStatus::no_such_ship);
    refused(crew::hire(c, w, ship, CrewId::null()), crew::HireStatus::no_such_crew);
    refused(crew::hire(c, w, ship, captain_of(w, ship)), crew::HireStatus::not_looking);
    refused(crew::hire(c, w, ship, faraway), crew::HireStatus::wrong_station);
    CHECK(crew::hire(c, w, ship, faraway).reason.find("Tycho Station") != std::string::npos);

    set_underway(w, ship);
    refused(crew::hire(c, w, ship, local), crew::HireStatus::not_docked);
    set_docked(w, ship, station("ceres_station"));

    cash(w) = 199;
    refused(crew::hire(c, w, ship, local), crew::HireStatus::cannot_afford);
    CHECK(cash(w) == 199);
    CHECK_FALSE(w.crew.at(local).ship);

    // Berth limit: the Mule has 4 berths, the captain takes one.
    cash(w) = 100000;
    const ShipClassDef& cls = c.table<ShipClassDef>()[w.ships.at(ship).ship_class];
    REQUIRE(cls.crew_berths == 4);
    for (int i = 0; i < 3; ++i) {
        const CrewId hand = add_seeker(w, station("ceres_station"), CrewRole::deckhand, 20, 150);
        REQUIRE(crew::hire(c, w, ship, hand).ok());
    }
    const Credits before = cash(w);
    refused(crew::hire(c, w, ship, local), crew::HireStatus::no_berth);
    CHECK(cash(w) == before);
    CHECK(crew::aboard(w, ship).size() == 4);
}

TEST_CASE("fire puts crew ashore and refuses bad requests") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 9);
    const ShipId ship = player_ship(w);
    cash(w) = 100000;
    const CrewId hand = add_seeker(w, station("ceres_station"), CrewRole::engineer, 60, 400);
    REQUIRE(crew::hire(c, w, ship, hand).ok());

    CHECK(crew::fire(w, CrewId::null()).status == crew::FireStatus::no_such_crew);
    CHECK(crew::fire(w, captain_of(w, ship)).status == crew::FireStatus::captain);
    set_underway(w, ship);
    const crew::FireResult underway = crew::fire(w, hand);
    CHECK(underway.status == crew::FireStatus::underway);
    CHECK_FALSE(underway.reason.empty());
    CHECK(w.crew.at(hand).ship == ship);

    set_docked(w, ship, station("vesta_dock"));
    w.crew.at(hand).wages_owed = 800;
    const Credits before = cash(w);
    const crew::FireResult r = crew::fire(w, hand);
    REQUIRE(r.ok());
    CHECK(r.wages_settled == 800);
    CHECK(cash(w) == before - 800);
    const CrewMember& m = w.crew.at(hand);
    CHECK_FALSE(m.ship);
    CHECK(m.station == station("vesta_dock"));
    CHECK(m.wages_owed == 0);
    const auto pool = crew::pool_at(w, station("vesta_dock"));
    CHECK(std::find(pool.begin(), pool.end(), hand) != pool.end());
    CHECK(crew::fire(w, hand).status == crew::FireStatus::not_aboard);
}

TEST_CASE("weekly wages are paid from the owner cash") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 10);
    const ShipId ship = player_ship(w);
    cash(w) = 5000;
    const CrewId a = add_seeker(w, station("ceres_station"), CrewRole::pilot, 50, 600);
    const CrewId b = add_seeker(w, station("ceres_station"), CrewRole::deckhand, 20, 150);
    REQUIRE(crew::hire(c, w, ship, a).ok());
    REQUIRE(crew::hire(c, w, ship, b).ok());
    CHECK(cash(w) == 5000 - 750);

    systems::weekly_crew(c, w);
    CHECK(cash(w) == 5000 - 750 - 750);
    CHECK(w.crew.at(a).wages_owed == 0);
    CHECK(has_message(w, "paid 750 cr to 2 crew"));
}

TEST_CASE("unpaid wages crush morale until crew walk off at port") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 11);
    const ShipId ship = player_ship(w);
    cash(w) = 600;
    const CrewId hand = add_seeker(w, station("ceres_station"), CrewRole::engineer, 55, 600);
    REQUIRE(crew::hire(c, w, ship, hand).ok());
    REQUIRE(cash(w) == 0);
    REQUIRE(w.stats.get(ship_key(ship), stat::hull_wear) < 1.0);

    systems::weekly_crew(c, w);
    const CrewMember& m = w.crew.at(hand);
    CHECK(m.unpaid_weeks == 1);
    CHECK(m.wages_owed == 600);
    CHECK(m.morale == doctest::Approx(crew::neutral_morale - 0.2));
    CHECK(has_message(w, "Missed payroll", true));
    CHECK(has_message(w, "grumbling"));

    // Underway nobody can leave, however unhappy.
    set_underway(w, ship);
    systems::weekly_crew(c, w);
    CHECK(w.crew.at(hand).morale < crew::quit_morale);
    CHECK(w.crew.at(hand).wages_owed == 1200);
    systems::daily_crew(c, w);
    CHECK(w.crew.at(hand).ship == ship);

    // Next port: they walk.
    set_docked(w, ship, station("tycho_station"));
    systems::daily_crew(c, w);
    CHECK_FALSE(w.crew.at(hand).ship);
    CHECK(w.crew.at(hand).station == station("tycho_station"));
    CHECK(has_message(w, "walked off", true));
    CHECK(w.stats.get(ship_key(ship), stat::hull_wear) == 1.0);
    CHECK(crew::aboard(w, ship).size() == 1); // the captain never leaves
}

TEST_CASE("paid crew morale drifts back to neutral") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 12);
    const ShipId ship = player_ship(w);
    cash(w) = 100000;
    const CrewId happy = add_seeker(w, station("ceres_station"), CrewRole::medic, 50, 500, 0.95);
    const CrewId sulky = add_seeker(w, station("ceres_station"), CrewRole::deckhand, 50, 200, 0.3);
    REQUIRE(crew::hire(c, w, ship, happy).ok());
    REQUIRE(crew::hire(c, w, ship, sulky).ok());
    for (int week = 0; week < 12; ++week) {
        systems::weekly_crew(c, w);
    }
    CHECK(w.crew.at(happy).morale == doctest::Approx(crew::neutral_morale).epsilon(0.02));
    CHECK(w.crew.at(sulky).morale == doctest::Approx(crew::neutral_morale).epsilon(0.02));
}

TEST_CASE("life support draws daily rations and warns before running out") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 13);
    const ShipId ship = player_ship(w);
    const crew::Provisions& rate = crew::life_support_per_person;
    const double water0 = stock(w, ship, "water");
    const double food0 = stock(w, ship, "food");
    const double oxygen0 = stock(w, ship, "oxygen");
    const double reaction_mass = w.ships.at(ship).reaction_mass_t;

    // Docked: station air, ship water and food.
    systems::daily_crew(c, w);
    CHECK(stock(w, ship, "water") == doctest::Approx(water0 - rate.water_t));
    CHECK(stock(w, ship, "food") == doctest::Approx(food0 - rate.food_t));
    CHECK(stock(w, ship, "oxygen") == doctest::Approx(oxygen0));
    CHECK(w.ships.at(ship).reaction_mass_t == reaction_mass);

    // Underway: oxygen too, including leakage through a worn hull.
    set_underway(w, ship);
    const double leak = crew::hull_leak_oxygen_t_per_day * (1.0 - w.ships.at(ship).hull_condition);
    systems::daily_crew(c, w);
    CHECK(stock(w, ship, "oxygen") == doctest::Approx(oxygen0 - rate.oxygen_t - leak));
    CHECK(crew::daily_need(c, w, ship).oxygen_t == doctest::Approx(rate.oxygen_t + leak));

    // A second mouth doubles the draw.
    set_docked(w, ship, station("ceres_station"));
    cash(w) = 10000;
    REQUIRE(crew::hire(c, w, ship, add_seeker(w, station("ceres_station"), CrewRole::deckhand, 10, 100)).ok());
    CHECK(crew::daily_need(c, w, ship).food_t == doctest::Approx(2 * rate.food_t));

    // Crossing three days of water left warns once.
    set_stock(w, ship, "water", 3.5 * 2 * rate.water_t);
    const std::size_t before = w.messages.size();
    systems::daily_crew(c, w);
    CHECK(has_message(w, "running low on water", true));
    CHECK(w.messages.size() == before + 1);
    systems::daily_crew(c, w);
    CHECK(w.messages.size() == before + 1);
}

TEST_CASE("running out of provisions hurts morale and health") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 14);
    const ShipId ship = player_ship(w);
    cash(w) = 10000;
    const CrewId hand = add_seeker(w, station("ceres_station"), CrewRole::deckhand, 10, 100);
    REQUIRE(crew::hire(c, w, ship, hand).ok());
    set_stock(w, ship, "food", 0.0);

    systems::daily_crew(c, w);
    CHECK(has_message(w, "out of food", true));
    CHECK(w.crew.at(hand).morale < crew::neutral_morale);
    CHECK(w.crew.at(hand).health < 1.0);
    CHECK(stock(w, ship, "water") > 0.0);

    // No air, no water, no food: collapse within days (the hook for a future injury system).
    set_underway(w, ship);
    set_stock(w, ship, "water", 0.0);
    set_stock(w, ship, "oxygen", 0.0);
    for (int day = 0; day < 3; ++day) {
        systems::daily_crew(c, w);
    }
    CHECK(w.crew.at(hand).health == 0.0);
    CHECK(has_message(w, "collapsed", true));
}

TEST_CASE("crew skills apply and remove ship stat modifiers") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 15);
    const ShipId ship = player_ship(w);
    const sim::EntityKey key = ship_key(ship);
    const std::uint8_t captain_skill = w.crew.at(captain_of(w, ship)).skill;

    // The captain flies; nobody maintains or doctors.
    CHECK(w.stats.get(key, stat::accel_tolerance) == doctest::Approx(1.0 + 0.2 * captain_skill / 100.0));
    CHECK(w.stats.get(key, stat::hull_wear) == 1.0);
    CHECK(w.stats.get(key, stat::life_support_use) == 1.0);
    CHECK(w.stats.get(key, stat::health_loss) == 1.0);

    cash(w) = 100000;
    const CrewId engineer = add_seeker(w, station("ceres_station"), CrewRole::engineer, 80, 900);
    const CrewId pilot = add_seeker(w, station("ceres_station"), CrewRole::pilot, 100, 1200);
    const CrewId medic = add_seeker(w, station("ceres_station"), CrewRole::medic, 50, 700);
    REQUIRE(crew::hire(c, w, ship, engineer).ok());
    REQUIRE(crew::hire(c, w, ship, pilot).ok());
    REQUIRE(crew::hire(c, w, ship, medic).ok());
    CHECK(w.stats.get(key, stat::hull_wear) == doctest::Approx(1.0 - 0.4 * 0.8));
    CHECK(w.stats.get(key, stat::life_support_use) == doctest::Approx(1.0 - 0.25 * 0.8));
    CHECK(w.stats.get(key, stat::accel_tolerance) == doctest::Approx(1.2)); // best pilot only
    CHECK(w.stats.get(key, stat::reaction_mass_use) == doctest::Approx(0.9));
    CHECK(w.stats.get(key, stat::health_loss) == doctest::Approx(0.75));
    // The engineer's recycler work shows up in the water draw.
    CHECK(crew::daily_need(c, w, ship).water_t ==
          doctest::Approx(4 * crew::life_support_per_person.water_t * 0.8));

    REQUIRE(crew::fire(w, engineer).ok());
    REQUIRE(crew::fire(w, pilot).ok());
    CHECK(w.stats.get(key, stat::hull_wear) == 1.0);
    CHECK(w.stats.get(key, stat::accel_tolerance) == doctest::Approx(1.0 + 0.2 * captain_skill / 100.0));
    CHECK(w.stats.get(key, stat::health_loss) == doctest::Approx(0.75));
}

TEST_CASE("crew state survives save and load mid run") {
    const Content& c = game_content();
    auto setup = [&] {
        World w = new_game(c, "secondhand", 16);
        cash(w) = 3000;
        const ShipId ship = player_ship(w);
        REQUIRE(crew::hire(c, w, ship, crew::pool_at(w, station("ceres_station")).front()).ok());
        return w;
    };
    World a = setup();
    World b = setup();
    const sim::Time start = a.now();
    advance_to(c, a, start + sim::days(60), false);

    advance_to(c, b, start + sim::days(24), false);
    World b2 = load_world(save_world(b), c);
    CHECK(world_hash(b2) == world_hash(b));
    advance_to(c, b2, start + sim::days(60), false);
    CHECK(world_hash(a) == world_hash(b2));
    CHECK(all_names(a) == all_names(b2));
    // Two months in, the crew has eaten through the stores and missed pay.
    CHECK(has_message(a, "out of"));
}

TEST_CASE("crew of a lost ship end up ashore at the last port") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 17);
    const ShipId ship = player_ship(w);
    const CrewId captain = captain_of(w, ship);
    set_docked(w, ship, station("pallas_refinery"));
    systems::daily_crew(c, w);
    w.ships.erase(ship);
    systems::daily_crew(c, w);
    CHECK_FALSE(w.crew.at(captain).ship);
    CHECK(w.crew.at(captain).station == station("pallas_refinery"));
}

TEST_CASE("crew origin content is validated") {
    sim::Diagnostics diags;
    const sim::DataSource src{"inline.toml", R"(
[body.sun]
name = "Sun"
kind = "star"
radius_km = 1
[crew_origin.nobody]
name = "Nobody"
faction = "belt"
given_names = []
family_names = ["X"]
)"};
    CHECK_FALSE(Content::load(std::span{&src, 1}, diags));
    CHECK(diags.contains("must not be empty"));
}
