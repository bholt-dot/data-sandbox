#include "expanse/crew.hpp"

#include "expanse/finance.hpp"
#include "expanse/simulation.hpp"
#include "expanse/stat_ids.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <span>
#include <variant>

// Life-support figures (NASA Life Support Baseline Values and Assumptions Document, BVAD
// NASA/TP-2015-218570, and ISS ECLSS performance):
//   oxygen consumed    0.84 kg/person-day
//   food, as packaged  ~1.8 kg/person-day (0.62 kg dry mass; BVAD planning value 1.82 kg)
//   water              ~3.5 kg drinking + food prep, +0.4 kg austere hygiene = ~3.9 kg gross
// A Belter hauler recycles hard, so only the make-up is drawn from stores:
//   water   90% loop closure (ISS reached 98% with the brine processor; a worn secondhand plant
//           does worse)                                            -> 0.39 kg, ~0.4 kg/day
//   oxygen  ~50% recovered from CO2 (Sabatier-class, roughly ISS)  -> 0.42 kg/day
//   food    no hydroponics aboard, nothing recovered               -> 1.8 kg/day
// plus cabin leakage that grows as the hull wears (hull_leak_oxygen_t_per_day). Docked ships
// breathe station air (covered by the docking fee), so oxygen is drawn only underway.
// Water here is drinking/hygiene water; reaction mass (Ship::reaction_mass_t) is never touched.

namespace expanse::crew {

namespace {

struct RoleInfo {
    CrewRole role;
    std::string_view name;
    Credits base_wage;         // weekly, skill 50-ish Belter pays ~1.25x this x 0.8
    std::uint64_t pool_weight; // how common among job-seekers
    double skill_mean;
};

constexpr std::array role_table{
    RoleInfo{CrewRole::captain, "captain", 0, 0, 40.0},
    RoleInfo{CrewRole::pilot, "pilot", 560, 20, 45.0},
    RoleInfo{CrewRole::engineer, "engineer", 520, 25, 45.0},
    RoleInfo{CrewRole::medic, "medic", 600, 15, 45.0},
    RoleInfo{CrewRole::deckhand, "deckhand", 260, 40, 30.0},
};

const RoleInfo& info(CrewRole role) { return role_table[static_cast<std::size_t>(role)]; }

constexpr double skill_stddev = 18.0;
constexpr double pool_departure_chance = 0.35; // per seeker per week
constexpr double faction_affinity = 8.0;       // weight of home-faction origins vs. 1 for others
constexpr double health_recovery_per_day = 0.02;
constexpr double low_stores_days = 3.0;

// Per day of total shortage (scaled by the unmet fraction).
struct Deprivation {
    double morale;
    double health;
};
constexpr Deprivation water_short{0.10, 0.20};
constexpr Deprivation food_short{0.05, 0.05};
constexpr Deprivation oxygen_short{0.30, 0.50};

// Wage-type payments go through the finance ledger. Never overdraws.
bool pay(World& world, CompanyId company, Credits amount, std::string description) {
    return amount >= 0 && transact(world, company, -amount, LedgerCategory::wages, std::move(description));
}

bool is_player_ship(const World& world, ShipId id) {
    const Ship* s = world.ships.get(id);
    return s != nullptr && s->owner == world.player;
}

std::string credits(Credits c) { return std::format("{} cr", c); }

std::string_view station_name(const Content& content, StationId id) {
    const StationDef* st = content.table<StationDef>().get(id);
    return st != nullptr ? std::string_view{st->name} : std::string_view{"port"};
}

void adjust_morale(World& world, CrewMember& m, double delta) {
    const double before = m.morale;
    m.morale = std::clamp(m.morale + delta, 0.0, 1.0);
    if (m.role != CrewRole::captain && before >= warn_morale && m.morale < warn_morale &&
        is_player_ship(world, m.ship)) {
        post(world, MessageKind::crew,
             std::format("{} ({}) is grumbling about conditions aboard the {}. Morale is low.",
                         m.name, role_name(m.role), world.ships.at(m.ship).name));
    }
}

// Puts a crew member ashore at `station` as a fresh job-seeker.
void put_ashore(CrewMember& m, StationId station) {
    m.ship = ShipId::null();
    m.station = station;
    m.morale = neutral_morale;
    m.wages_owed = 0;
    m.unpaid_weeks = 0;
}

template <typename T>
const T& pick(sim::Rng& rng, const std::vector<T>& items) {
    return items[static_cast<std::size_t>(rng.below(items.size()))];
}

sim::DefId<CrewOriginDef> pick_origin(const Content& content, sim::Rng& rng, StationId station) {
    const auto& origins = content.table<CrewOriginDef>();
    if (origins.empty()) {
        return {};
    }
    const StationDef* st = content.table<StationDef>().get(station);
    std::vector<double> weights;
    weights.reserve(origins.size());
    for (auto [id, o] : origins) {
        (void)id;
        weights.push_back(st != nullptr && o.faction == st->faction ? faction_affinity : 1.0);
    }
    return origins.table().handle_at(rng.pick_weighted(std::span<const double>{weights}));
}

Credits asking_wage(CrewRole role, std::uint8_t skill, double origin_multiplier, double haggle) {
    const double skill_factor = 0.5 + 1.5 * static_cast<double>(skill) / 100.0;
    const double raw = static_cast<double>(info(role).base_wage) * skill_factor * origin_multiplier * haggle;
    return std::max<Credits>(10, static_cast<Credits>(std::llround(raw / 10.0)) * 10);
}

CrewMember generate(const Content& content, sim::Rng& rng, StationId station, CrewRole role) {
    CrewMember m;
    m.role = role;
    m.station = station;
    m.origin = pick_origin(content, rng, station);
    double wage_multiplier = 1.0;
    if (const CrewOriginDef* o = content.table<CrewOriginDef>().get(m.origin)) {
        m.name = pick(rng, o->given_names) + " " + pick(rng, o->family_names);
        if (!o->backgrounds.empty()) {
            m.background = pick(rng, o->backgrounds);
        }
        wage_multiplier = o->wage_multiplier;
    } else {
        m.name = std::format("Drifter #{}", rng.uniform_int(100, 999));
    }
    const double skill = std::clamp(rng.normal(info(role).skill_mean, skill_stddev), 5.0, 95.0);
    m.skill = static_cast<std::uint8_t>(std::lround(skill));
    m.morale = rng.uniform_real(0.5, 0.7);
    if (role != CrewRole::captain) {
        m.wage_per_week = asking_wage(role, m.skill, wage_multiplier, rng.uniform_real(0.9, 1.1));
    }
    return m;
}

CrewMember generate_seeker(const Content& content, sim::Rng& rng, StationId station) {
    std::array<std::uint64_t, role_table.size()> weights{};
    for (std::size_t i = 0; i < role_table.size(); ++i) {
        weights[i] = role_table[i].pool_weight;
    }
    const CrewRole role = role_table[rng.pick_weighted(std::span<const std::uint64_t>{weights})].role;
    return generate(content, rng, station, role);
}

void fill_pools(const Content& content, World& world) {
    sim::Rng& rng = world.rng("crew");
    for (auto [station, st] : content.table<StationDef>()) {
        const std::size_t target = pool_target(st.population);
        for (std::size_t n = pool_at(world, station).size(); n < target; ++n) {
            world.crew.insert(generate_seeker(content, rng, station));
        }
    }
}

void turnover_pools(const Content& content, World& world) {
    sim::Rng& rng = world.rng("crew");
    std::vector<CrewId> leaving;
    for (auto [id, m] : world.crew) {
        if (!m.ship && m.role != CrewRole::captain && rng.chance(pool_departure_chance)) {
            leaving.push_back(id);
        }
    }
    for (CrewId id : leaving) {
        world.crew.erase(id);
    }
    fill_pools(content, world);
}

void pay_wages(World& world) {
    struct Payroll {
        Credits paid = 0;
        int paid_count = 0;
        std::vector<std::string> unpaid;
        Credits owed = 0;
    };
    std::map<ShipId, Payroll> by_ship; // ordered: messages come out in ship-handle order

    for (auto [id, m] : world.crew) {
        (void)id;
        const Ship* ship = world.ships.get(m.ship);
        if (ship == nullptr || m.role == CrewRole::captain) {
            continue;
        }
        Payroll& p = by_ship[m.ship];
        const Credits due = m.wage_per_week + m.wages_owed;
        if (pay(world, ship->owner, due, std::format("wages: {}", m.name))) {
            p.paid += due;
            ++p.paid_count;
            const bool cleared_arrears = m.wages_owed > 0;
            m.wages_owed = 0;
            m.unpaid_weeks = 0;
            m.morale += (neutral_morale - m.morale) * 0.25;
            if (cleared_arrears) {
                adjust_morale(world, m, 0.05);
            }
        } else {
            m.wages_owed += m.wage_per_week;
            m.unpaid_weeks = static_cast<std::uint8_t>(std::min(255, m.unpaid_weeks + 1));
            adjust_morale(world, m, -(0.1 + 0.1 * static_cast<double>(m.unpaid_weeks)));
            p.unpaid.push_back(m.name);
            p.owed += m.wages_owed;
        }
    }

    for (const auto& [ship_id, p] : by_ship) {
        if (!is_player_ship(world, ship_id)) {
            continue;
        }
        const std::string& ship_name = world.ships.at(ship_id).name;
        if (p.paid_count > 0) {
            post(world, MessageKind::crew,
                 std::format("Payroll for the {}: paid {} to {} crew.", ship_name, credits(p.paid),
                             p.paid_count));
        }
        if (!p.unpaid.empty()) {
            std::string names;
            for (const std::string& n : p.unpaid) {
                names += names.empty() ? n : ", " + n;
            }
            post(world, MessageKind::crew,
                 std::format("Missed payroll on the {}: {} went unpaid ({} owed). Morale is falling; "
                             "unhappy crew walk off at the next port.",
                             ship_name, names, credits(p.owed)),
                 true);
        }
    }
}

// Removes up to `tonnes` of a commodity from the hold, oldest lots first; returns what it got.
double draw(Ship& ship, CommodityId commodity, double tonnes) {
    double remaining = tonnes;
    for (CargoLot& lot : ship.cargo) {
        if (remaining <= 0.0) {
            break;
        }
        if (lot.commodity != commodity || lot.tonnes <= 0.0) {
            continue;
        }
        const double take = std::min(lot.tonnes, remaining);
        lot.cost_basis -= static_cast<Credits>(
            std::llround(static_cast<double>(lot.cost_basis) * take / lot.tonnes));
        lot.tonnes -= take;
        remaining -= take;
    }
    std::erase_if(ship.cargo, [&](const CargoLot& lot) {
        return lot.commodity == commodity && lot.tonnes <= 1e-9;
    });
    return tonnes - remaining;
}

double tonnes_of(const Ship& ship, CommodityId commodity) {
    double t = 0.0;
    for (const CargoLot& lot : ship.cargo) {
        if (lot.commodity == commodity) {
            t += lot.tonnes;
        }
    }
    return t;
}

struct SupplyKind {
    std::string_view key;
    std::string_view label;
    double Provisions::*member;
    Deprivation effect;
};
constexpr std::array supplies{
    SupplyKind{"oxygen", "oxygen", &Provisions::oxygen_t, oxygen_short},
    SupplyKind{"water", "water", &Provisions::water_t, water_short},
    SupplyKind{"food", "food", &Provisions::food_t, food_short},
};

void life_support(const Content& content, World& world, ShipId ship_id) {
    const std::vector<CrewId> people = aboard(world, ship_id);
    if (people.empty()) {
        return;
    }
    const Provisions need = daily_need(content, world, ship_id);
    Ship& ship = world.ships.at(ship_id);
    const bool player = ship.owner == world.player;

    double morale_hit = 0.0;
    double health_hit = 0.0;
    std::vector<std::string_view> out;
    std::vector<std::string> running_low;
    for (const SupplyKind& s : supplies) {
        const double wanted = need.*s.member;
        const CommodityId commodity = content.find<CommodityDef>(s.key);
        if (wanted <= 0.0 || !commodity) {
            continue;
        }
        const double before = tonnes_of(ship, commodity);
        const double got = draw(ship, commodity, wanted);
        const double shortfall = std::clamp(1.0 - got / wanted, 0.0, 1.0);
        if (shortfall > 1e-6) {
            morale_hit += s.effect.morale * shortfall;
            health_hit += s.effect.health * shortfall;
            out.push_back(s.label);
        } else {
            const double after_days = (before - got) / wanted;
            if (before / wanted >= low_stores_days && after_days < low_stores_days) {
                running_low.push_back(std::format("{} ({:.1f} days left)", s.label, after_days));
            }
        }
    }

    if (player && !out.empty()) {
        std::string list;
        for (std::string_view s : out) {
            list += list.empty() ? std::string{s} : std::format(", {}", s);
        }
        post(world, MessageKind::crew,
             std::format("The {} is out of {}! The crew is suffering; resupply now.", ship.name, list),
             true);
    }
    if (player && !running_low.empty()) {
        std::string list;
        for (const std::string& s : running_low) {
            list += list.empty() ? s : ", " + s;
        }
        post(world, MessageKind::crew, std::format("The {} is running low on {}.", ship.name, list),
             true);
    }

    const double health_mult = world.stats.get(ship_key(ship_id), stat::health_loss);
    for (CrewId id : people) {
        CrewMember& m = world.crew.at(id);
        if (health_hit > 0.0) {
            adjust_morale(world, m, -morale_hit);
            const double before = m.health;
            m.health = std::max(0.0, m.health - health_hit * std::max(0.0, health_mult));
            if (before > 0.0 && m.health <= 0.0) {
                on_health_depleted(world, id);
            }
        } else {
            m.health = std::min(1.0, m.health + health_recovery_per_day);
        }
    }
}

// Crew too unhappy to stay walk off when the ship is in port.
void walk_offs(const Content& content, World& world, ShipId ship_id) {
    const Ship& ship = world.ships.at(ship_id);
    const auto* docked = std::get_if<Docked>(&ship.location);
    if (docked == nullptr) {
        return;
    }
    const StationId station = docked->station;
    bool changed = false;
    for (CrewId id : aboard(world, ship_id)) {
        CrewMember& m = world.crew.at(id);
        m.station = station; // last port, where they end up if the ship is lost
        if (m.role == CrewRole::captain || m.morale >= quit_morale) {
            continue;
        }
        const Credits owed = m.wages_owed;
        if (ship.owner == world.player) {
            post(world, MessageKind::crew,
                 owed > 0 ? std::format("{} ({}) walked off the {} at {}, still owed {}.", m.name,
                                        role_name(m.role), ship.name, station_name(content, station),
                                        credits(owed))
                          : std::format("{} ({}) walked off the {} at {}.", m.name, role_name(m.role),
                                        ship.name, station_name(content, station)),
                 true);
        }
        put_ashore(m, station);
        changed = true;
    }
    if (changed) {
        refresh_modifiers(world, ship_id);
    }
}

std::vector<ShipId> ship_ids(const World& world) {
    std::vector<ShipId> ids;
    ids.reserve(world.ships.size());
    for (auto [id, s] : world.ships) {
        (void)s;
        ids.push_back(id);
    }
    return ids;
}

constexpr std::array crew_stats{stat::accel_tolerance, stat::reaction_mass_use, stat::hull_wear,
                                stat::life_support_use, stat::health_loss};

} // namespace

// ---- Queries ----------------------------------------------------------------------------------

std::string_view role_name(CrewRole role) { return info(role).name; }

std::size_t pool_target(std::uint32_t population) {
    if (population == 0) {
        return 0;
    }
    // ~4 on a 15k outpost, ~6 at 250k, ~8 on Ceres (6M): a big port has more hands, not 400x more.
    const double n = 1.5 * std::log10(static_cast<double>(population)) - 2.0;
    return static_cast<std::size_t>(std::clamp(std::lround(n), 1L, 12L));
}

std::vector<CrewId> pool_at(const World& world, StationId station) {
    std::vector<CrewId> out;
    for (auto [id, m] : world.crew) {
        if (!m.ship && m.station == station) {
            out.push_back(id);
        }
    }
    return out;
}

std::vector<CrewId> aboard(const World& world, ShipId ship) {
    std::vector<CrewId> out;
    for (auto [id, m] : world.crew) {
        if (m.ship && m.ship == ship) {
            out.push_back(id);
        }
    }
    return out;
}

Credits weekly_payroll(const World& world, ShipId ship) {
    Credits total = 0;
    for (CrewId id : aboard(world, ship)) {
        total += world.crew.at(id).wage_per_week;
    }
    return total;
}

Credits signing_bonus(const CrewMember& member) { return member.wage_per_week; }

Credits expected_wage(CrewRole role, std::uint8_t skill, double origin_multiplier) {
    return role == CrewRole::captain ? 0 : asking_wage(role, skill, origin_multiplier, 1.0);
}

Provisions daily_need(const Content& content, World& world, ShipId ship_id) {
    (void)content;
    const Ship* ship = world.ships.get(ship_id);
    if (ship == nullptr) {
        return {};
    }
    const auto people = static_cast<double>(aboard(world, ship_id).size());
    if (people == 0.0) {
        return {};
    }
    const double recycling = std::max(0.0, world.stats.get(ship_key(ship_id), stat::life_support_use));
    Provisions p;
    p.water_t = people * life_support_per_person.water_t * recycling;
    p.food_t = people * life_support_per_person.food_t;
    if (std::holds_alternative<Underway>(ship->location)) {
        const double leak = hull_leak_oxygen_t_per_day * std::clamp(1.0 - ship->hull_condition, 0.0, 1.0);
        p.oxygen_t = (people * life_support_per_person.oxygen_t + leak) * recycling;
    }
    return p;
}

Provisions stores(const Content& content, const Ship& ship) {
    Provisions p;
    for (const SupplyKind& s : supplies) {
        if (const CommodityId c = content.find<CommodityDef>(s.key)) {
            p.*s.member = tonnes_of(ship, c);
        }
    }
    return p;
}

double skill_effect(sim::StatId stat, std::uint8_t skill) {
    const double s = static_cast<double>(skill) / 100.0;
    if (stat == stat::accel_tolerance) {
        return 0.20 * s;
    }
    if (stat == stat::reaction_mass_use) {
        return -0.10 * s;
    }
    if (stat == stat::hull_wear) {
        return -0.40 * s;
    }
    if (stat == stat::life_support_use) {
        return -0.25 * s;
    }
    if (stat == stat::health_loss) {
        return -0.50 * s;
    }
    return 0.0;
}

// ---- Actions ----------------------------------------------------------------------------------

HireResult hire(const Content& content, World& world, ShipId ship_id, CrewId member_id) {
    auto fail = [](HireStatus status, std::string reason) { return HireResult{status, std::move(reason), 0}; };

    const Ship* ship = world.ships.get(ship_id);
    if (ship == nullptr) {
        return fail(HireStatus::no_such_ship, "No such ship.");
    }
    CrewMember* m = world.crew.get(member_id);
    if (m == nullptr) {
        return fail(HireStatus::no_such_crew, "That person isn't around any more.");
    }
    if (m->ship) {
        return fail(HireStatus::not_looking, std::format("{} already has a berth.", m->name));
    }
    const auto* docked = std::get_if<Docked>(&ship->location);
    if (docked == nullptr) {
        return fail(HireStatus::not_docked,
                    std::format("The {} is underway; you can only sign crew on in port.", ship->name));
    }
    if (docked->station != m->station) {
        return fail(HireStatus::wrong_station,
                    std::format("{} is looking for work on {}, but the {} is docked at {}.", m->name,
                                station_name(content, m->station), ship->name,
                                station_name(content, docked->station)));
    }
    const ShipClassDef& cls = content.table<ShipClassDef>()[ship->ship_class];
    if (aboard(world, ship_id).size() >= cls.crew_berths) {
        return fail(HireStatus::no_berth,
                    std::format("All {} berths on the {} are taken.", cls.crew_berths, ship->name));
    }
    const Credits bonus = signing_bonus(*m);
    if (!pay(world, ship->owner, bonus, std::format("signing bonus: {}", m->name))) {
        return fail(HireStatus::cannot_afford,
                    std::format("{} wants {} up front to sign on; you can't cover it.", m->name,
                                credits(bonus)));
    }
    m->ship = ship_id;
    m->wages_owed = 0;
    m->unpaid_weeks = 0;
    if (ship->owner == world.player) {
        post(world, MessageKind::crew,
             std::format("{} ({}, skill {}) signed on to the {} for {}/week. Paid a {} signing bonus.",
                         m->name, role_name(m->role), m->skill, ship->name, credits(m->wage_per_week),
                         credits(bonus)));
    }
    refresh_modifiers(world, ship_id);
    return HireResult{HireStatus::ok, {}, bonus};
}

FireResult fire(World& world, CrewId member_id) {
    auto fail = [](FireStatus status, std::string reason) { return FireResult{status, std::move(reason), 0}; };

    CrewMember* m = world.crew.get(member_id);
    if (m == nullptr) {
        return fail(FireStatus::no_such_crew, "That person isn't around any more.");
    }
    const Ship* ship = world.ships.get(m->ship);
    if (ship == nullptr) {
        return fail(FireStatus::not_aboard, std::format("{} isn't aboard any ship.", m->name));
    }
    if (m->role == CrewRole::captain) {
        return fail(FireStatus::captain, "You can't fire the captain. You are the captain.");
    }
    const auto* docked = std::get_if<Docked>(&ship->location);
    if (docked == nullptr) {
        return fail(FireStatus::underway,
                    std::format("The {} is underway; {} can only be put off in port.", ship->name, m->name));
    }
    const ShipId ship_id = m->ship;
    const Credits owed = m->wages_owed;
    Credits settled = 0;
    if (owed > 0 && pay(world, ship->owner, owed, std::format("back pay: {}", m->name))) {
        settled = owed;
    }
    if (ship->owner == world.player) {
        std::string text = std::format("{} ({}) signed off the {}.", m->name, role_name(m->role), ship->name);
        if (settled > 0) {
            text += std::format(" Settled {} in back pay.", credits(settled));
        } else if (owed > 0) {
            text += std::format(" Left without the {} you owe.", credits(owed));
        }
        post(world, MessageKind::crew, std::move(text));
    }
    put_ashore(*m, docked->station);
    refresh_modifiers(world, ship_id);
    return FireResult{FireStatus::ok, {}, settled};
}

// ---- Setup and upkeep -------------------------------------------------------------------------

void start_new_game(const Content& content, World& world, const ScenarioDef& scenario, ShipId player_ship) {
    for (sim::StatId s : crew_stats) {
        world.stats.set_base(sim::global_scope, s, 1.0);
    }

    CrewMember captain = generate(content, world.rng("crew"), scenario.start_station, CrewRole::captain);
    captain.ship = player_ship;
    captain.morale = neutral_morale;
    captain.background = "Owner-operator. Signed the loan papers.";
    world.crew.insert(std::move(captain));
    refresh_modifiers(world, player_ship);

    // Provisions for the captain alone, sized as if underway (oxygen included).
    Ship& ship = world.ships.at(player_ship);
    const double leak = hull_leak_oxygen_t_per_day * std::clamp(1.0 - ship.hull_condition, 0.0, 1.0);
    const Provisions per_day{life_support_per_person.water_t, life_support_per_person.food_t,
                             life_support_per_person.oxygen_t + leak};
    for (const SupplyKind& s : supplies) {
        const CommodityId c = content.find<CommodityDef>(s.key);
        const double tonnes = scenario.provision_days * (per_day.*s.member);
        if (c && tonnes > 0.0) {
            const Credits basis = static_cast<Credits>(
                std::llround(tonnes * static_cast<double>(content.table<CommodityDef>()[c].base_price)));
            ship.cargo.push_back(CargoLot{c, tonnes, basis});
        }
    }

    fill_pools(content, world);
}

void refresh_modifiers(World& world, ShipId ship_id) {
    const sim::EntityKey source = crew_complement_key(ship_id);
    world.stats.remove_source(source);
    if (!world.ships.contains(ship_id)) {
        return;
    }
    std::uint8_t pilot = 0;
    std::uint8_t engineer = 0;
    std::uint8_t medic = 0;
    for (CrewId id : aboard(world, ship_id)) {
        const CrewMember& m = world.crew.at(id);
        switch (m.role) {
        case CrewRole::captain:
        case CrewRole::pilot: pilot = std::max(pilot, m.skill); break;
        case CrewRole::engineer: engineer = std::max(engineer, m.skill); break;
        case CrewRole::medic: medic = std::max(medic, m.skill); break;
        case CrewRole::deckhand: break;
        }
    }
    const sim::EntityKey target = ship_key(ship_id);
    auto add = [&](sim::StatId stat, std::uint8_t skill) {
        const double v = skill_effect(stat, skill);
        if (v != 0.0) {
            world.stats.add(sim::Modifier{source, target, stat, sim::ModOp::increased, v, sim::never});
        }
    };
    add(stat::accel_tolerance, pilot);
    add(stat::reaction_mass_use, pilot);
    add(stat::hull_wear, engineer);
    add(stat::life_support_use, engineer);
    add(stat::health_loss, medic);
}

void on_health_depleted(World& world, CrewId member) {
    const CrewMember& m = world.crew.at(member);
    if (is_player_ship(world, m.ship)) {
        post(world, MessageKind::crew,
             std::format("{} has collapsed and needs a medical bay now.", m.name), true);
    }
}

} // namespace expanse::crew

namespace expanse::systems {

void weekly_crew(const Content& content, World& world) {
    crew::pay_wages(world);
    crew::turnover_pools(content, world);
}

void daily_crew(const Content& content, World& world) {
    // A ship that no longer exists (sold, repossessed) leaves its crew on the last dock it saw.
    for (auto [id, m] : world.crew) {
        (void)id;
        if (m.ship && !world.ships.contains(m.ship)) {
            crew::put_ashore(m, m.station);
        }
    }
    for (ShipId id : crew::ship_ids(world)) {
        crew::life_support(content, world, id);
        crew::walk_offs(content, world, id);
    }
}

} // namespace expanse::systems
