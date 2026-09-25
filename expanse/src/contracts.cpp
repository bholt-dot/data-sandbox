#include "expanse/contracts.hpp"

#include "expanse/calendar.hpp"
#include "expanse/crew.hpp"
#include "expanse/economy.hpp"
#include "expanse/finance.hpp"
#include "expanse/ships.hpp"
#include "expanse/simulation.hpp"
#include "expanse/transit.hpp"
#include "expanse/units.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <span>
#include <variant>

namespace expanse::contracts {

namespace {

using namespace tuning;

constexpr std::uint8_t warned_at_risk = 1;

const StationDef& station_def(const Content& content, StationId id) {
    return content.table<StationDef>()[id];
}

bool is_active(const World& world, CompanyId company) {
    const Company* c = world.companies.get(company);
    return c != nullptr && c->status == CompanyStatus::active;
}

Credits round10(double v) { return static_cast<Credits>(std::llround(std::max(0.0, v) / 10.0)) * 10; }

double days_of(sim::Duration d) { return d.to_seconds_f() / units::day_s; }

std::string short_days(sim::Duration d) { return format_trip(d); }

orbit::BodyId sun_body(const Content& content) {
    for (auto [id, body] : content.table<BodyDef>()) {
        if (body.kind == BodyKind::star) {
            return content.orbit_of(id);
        }
    }
    return orbit::no_body;
}

double separation_au(const Content& content, StationId a, StationId b, sim::Time t) {
    const auto& orbits = content.orbits();
    return units::to_au(distance(orbits.world_position(content.orbit_of(a), t),
                                 orbits.world_position(content.orbit_of(b), t)));
}

double proximity(double au) {
    const double x = au / proximity_au;
    return 1.0 / (1.0 + x * x);
}

double category_risk(CommodityCategory c) {
    switch (c) {
    case CommodityCategory::medical: return 1.3;
    case CommodityCategory::luxury: return 1.2;
    case CommodityCategory::food: return 1.1;
    case CommodityCategory::volatiles:
    case CommodityCategory::industrial: return 1.0;
    }
    return 1.0;
}

double standing_reward_factor(std::int32_t s) {
    return 1.0 + reward_per_standing * static_cast<double>(std::clamp(s, -standing_limit, standing_limit));
}

double standing_deposit_factor(std::int32_t s) {
    return std::clamp(1.0 - deposit_per_standing * static_cast<double>(s), 0.4, 1.6);
}

void adjust_standing(const Content& content, World& world, CompanyId company, Faction faction,
                     std::int32_t delta) {
    (void)content;
    for (Standing& s : world.standings) {
        if (s.company == company && s.faction == faction) {
            s.value = std::clamp(s.value + delta, -standing_limit, standing_limit);
            return;
        }
    }
    world.standings.push_back(Standing{company, faction, std::clamp(delta, -standing_limit, standing_limit)});
}

std::string_view faction_name(Faction f) {
    for (const auto& [name, value] : enum_names(Faction{})) {
        if (value == f) {
            return name;
        }
    }
    return "?";
}

// Removes the contract's consignment from its ship and marks it off the ship.
void unload(World& world, Contract& c, ContractId id) {
    if (Ship* ship = world.ships.get(c.ship)) {
        std::erase_if(ship->consignments, [&](const Consignment& k) { return k.contract == id; });
    }
    c.aboard = false;
}

bool players(const World& world, const Contract& c) { return c.holder == world.player; }

void fail(const Content& content, World& world, ContractId id, std::string why) {
    Contract& c = world.contracts.at(id);
    c.status = ContractStatus::failed;
    c.closed_at = world.now();
    adjust_standing(content, world, c.holder, station_def(content, c.origin).faction, -fail_loss);
    const Ship* ship = world.ships.get(c.ship);
    if (ship == nullptr || std::holds_alternative<Docked>(ship->location)) {
        unload(world, c, id);
    }
    if (players(world, c)) {
        post(world, MessageKind::warning,
             std::format("Contract #{} failed ({}): {}. {}Standing with {} -{}.", id.index,
                         describe(content, c), why,
                         c.deposit > 0 ? std::format("The {} deposit is forfeit. ", format_credits(c.deposit))
                                       : std::string{},
                         faction_name(station_def(content, c.origin).faction), fail_loss),
             true);
    }
}

// ---- Generation ---------------------------------------------------------------------------------

struct Draft {
    ContractKind kind = ContractKind::cargo;
    StationId destination;
    CommodityId commodity;
    double tonnes = 0.0;
    std::uint8_t passengers = 0;
};

bool supplies(const MarketEntryDef& e, double stock) {
    return e.production > e.consumption || stock > 1.2 * economy::target_stock(e);
}

std::optional<Draft> draft_cargo(const Content& content, World& world, sim::Rng& rng, StationId origin) {
    const StationDef& from = station_def(content, origin);
    const StationState& here = world.stations[index_of(origin)];
    struct Candidate {
        StationId to;
        const MarketEntryDef* want;
        const MarketEntryDef* have;
    };
    std::vector<Candidate> candidates;
    std::vector<double> weights;
    for (const MarketEntryDef& have : from.market) {
        const double stock = here.stock[index_of(have.commodity)];
        if (!supplies(have, stock) || stock < 2.0) {
            continue;
        }
        for (auto [to, def] : content.table<StationDef>()) {
            if (to == origin) {
                continue;
            }
            for (const MarketEntryDef& want : def.market) {
                if (want.commodity != have.commodity || want.consumption <= want.production) {
                    continue;
                }
                const StationState& there = world.stations[index_of(to)];
                const std::size_t k = index_of(want.commodity);
                const double need = economy::price_multiplier(there.stock[k], economy::target_stock(want)) *
                                    (there.unmet[k] > 0.0 ? 2.0 : 1.0);
                candidates.push_back({to, &want, &have});
                weights.push_back(need * proximity(separation_au(content, origin, to, world.now())));
            }
        }
    }
    if (candidates.empty()) {
        return std::nullopt;
    }
    const Candidate& pick = candidates[rng.pick_weighted(std::span<const double>{weights})];
    const double stock = here.stock[index_of(pick.have->commodity)];
    double t = pick.want->consumption * rng.uniform_real(3.0, 12.0);
    t = std::min({t, max_job_tonnes, 0.5 * stock});
    t = t >= 20.0 ? 5.0 * std::floor(t / 5.0) : std::floor(t * 10.0) / 10.0;
    if (t < 1.0) {
        return std::nullopt;
    }
    return Draft{ContractKind::cargo, pick.to, pick.have->commodity, t, 0};
}

std::optional<Draft> draft_passengers(const Content& content, World& world, sim::Rng& rng, StationId origin) {
    const StationDef& from = station_def(content, origin);
    std::vector<StationId> candidates;
    std::vector<double> weights;
    for (auto [to, def] : content.table<StationDef>()) {
        if (to == origin || def.population == 0) {
            continue;
        }
        candidates.push_back(to);
        weights.push_back(proximity(separation_au(content, origin, to, world.now())) *
                          std::log10(1.0 + static_cast<double>(def.population)));
    }
    if (candidates.empty()) {
        return std::nullopt;
    }
    const StationId to = candidates[rng.pick_weighted(std::span<const double>{weights})];
    const double smaller = static_cast<double>(std::min(from.population, station_def(content, to).population));
    const auto most = static_cast<std::uint8_t>(std::clamp(std::lround(std::log10(smaller) - 2.0), 1L, 6L));
    const auto n = rng.uniform_int<std::uint8_t>(1, most);
    return Draft{ContractKind::passengers, to, {}, 0.0, n};
}

// Reward and deposit before standing (standing is applied when shown and when accepted).
void price(const Content& content, Contract& c, double transit_days, double slack) {
    const StationDef& from = station_def(content, c.origin);
    const StationDef& to = station_def(content, c.destination);
    const double urgency = 1.0 + urgency_bonus * (max_slack - slack) / (max_slack - min_slack);
    const double premium = c.min_standing > 0 ? premium_reward : 1.0;
    const double t = transit_days;
    double raw = 0.0;
    if (c.kind == ContractKind::cargo) {
        const CommodityDef& what = content.table<CommodityDef>()[c.commodity];
        const double value = c.tonnes * static_cast<double>(what.base_price);
        const double risk = category_risk(what.category) * (from.faction != to.faction ? cross_faction_risk : 1.0);
        raw = (base_fee + day_rate * t + tonne_day_rate * c.tonnes * t + value_share * value) * risk;
        c.deposit = value < free_deposit_value ? 0 : round10(deposit_fraction * value * (c.min_standing > 0 ? 0.5 : 1.0));
    } else {
        raw = static_cast<double>(c.passengers) * (fare + fare_per_day * t);
        c.deposit = 0;
    }
    c.reward = round10(raw * urgency * premium);
}

void post_offer(const Content& content, World& world, sim::Rng& rng, StationId origin) {
    const bool want_passengers = rng.chance(passenger_share);
    const bool premium = rng.chance(premium_chance);
    const double slack = rng.uniform_real(min_slack, max_slack);
    const auto lifetime = rng.uniform_int<std::int64_t>(offer_min_days * 24, offer_max_days * 24);
    std::optional<Draft> d = want_passengers ? draft_passengers(content, world, rng, origin)
                                             : draft_cargo(content, world, rng, origin);
    if (!d && !want_passengers) {
        d = draft_passengers(content, world, rng, origin);
    }
    if (!d) {
        return;
    }
    const auto transit = reference_transit(content, origin, d->destination, world.now());
    if (!transit) {
        return;
    }
    Contract c;
    c.kind = d->kind;
    c.origin = origin;
    c.destination = d->destination;
    c.commodity = d->commodity;
    c.tonnes = d->tonnes;
    c.passengers = d->passengers;
    c.min_standing = premium ? premium_standing : std::int8_t{0};
    const double allowed_s = transit->to_seconds_f() * slack + static_cast<double>(handling_hours) * 3600.0;
    c.time_allowed = sim::hours(static_cast<std::int64_t>(std::ceil(allowed_s / 3600.0)));
    c.posted_at = world.now();
    c.expires_at = world.now() + sim::hours(lifetime);
    price(content, c, days_of(*transit), slack);
    world.contracts.insert(std::move(c));
}

std::size_t offers_at(const World& world, StationId station) {
    std::size_t n = 0;
    for (auto [id, c] : world.contracts) {
        (void)id;
        n += c.status == ContractStatus::offered && c.origin == station ? 1u : 0u;
    }
    return n;
}

void fill_boards(const Content& content, World& world, double chance) {
    sim::Rng& rng = world.rng("contracts");
    for (auto [station, def] : content.table<StationDef>()) {
        const std::size_t cap = board_capacity(def.population);
        const std::size_t have = offers_at(world, station);
        for (std::size_t i = have; i < cap; ++i) {
            if (chance >= 1.0 || rng.chance(chance)) {
                post_offer(content, world, rng, station);
            }
        }
    }
}

std::vector<ContractId> sorted(std::vector<ContractId> ids, const World& world) {
    std::sort(ids.begin(), ids.end(), [&](ContractId a, ContractId b) {
        const Contract& ca = world.contracts.at(a);
        const Contract& cb = world.contracts.at(b);
        return std::tie(ca.posted_at, a.index) < std::tie(cb.posted_at, b.index);
    });
    return ids;
}

void deliver(const Content& content, World& world, ContractId id) {
    Contract& c = world.contracts.at(id);
    const sim::Duration late = world.now() - c.deadline;
    const Faction faction = station_def(content, c.origin).faction;
    if (late > grace(c)) {
        fail(content, world, id, "delivered too late; the client refused it");
        return;
    }
    double share = 1.0;
    if (late > sim::Duration{}) {
        share = 1.0 - max_late_cut * late.to_seconds_f() / grace(c).to_seconds_f();
    }
    const Credits pay = static_cast<Credits>(std::llround(static_cast<double>(c.reward) * share));
    const std::string what = describe(content, c);
    if (c.deposit > 0) {
        transact(world, c.holder, c.deposit, LedgerCategory::contract,
                 std::format("deposit back: contract #{}", id.index));
    }
    transact(world, c.holder, pay, LedgerCategory::contract, std::format("contract #{}: {}", id.index, what));
    if (c.kind == ContractKind::cargo) {
        if (economy::market_entry(content, c.destination, c.commodity) != nullptr) {
            world.stations[index_of(c.destination)].stock[index_of(c.commodity)] += c.tonnes;
        }
    }
    unload(world, c, id);
    c.status = ContractStatus::delivered;
    c.closed_at = world.now();
    c.paid = pay;
    const std::int32_t delta = late > sim::Duration{} ? -late_loss : on_time_gain;
    adjust_standing(content, world, c.holder, faction, delta);
    if (players(world, c)) {
        post(world, MessageKind::finance,
             late > sim::Duration{}
                 ? std::format("Contract #{} delivered {} late ({}): paid {} of {}{}. Standing with {} {}.",
                               id.index, short_days(late), what, format_credits(pay), format_credits(c.reward),
                               c.deposit > 0 ? std::format(", deposit {} back", format_credits(c.deposit)) : "",
                               faction_name(faction), delta)
                 : std::format("Contract #{} delivered on time ({}): paid {}{}. Standing with {} +{}.", id.index,
                               what, format_credits(pay),
                               c.deposit > 0 ? std::format(", deposit {} back", format_credits(c.deposit)) : "",
                               faction_name(faction), delta));
    }
}

void warn_deadlines(const Content& content, World& world) {
    for (auto [id, c] : world.contracts) {
        if (c.status != ContractStatus::accepted || (c.warned & warned_at_risk) || !players(world, c)) {
            continue;
        }
        const Ship* ship = world.ships.get(c.ship);
        if (ship == nullptr) {
            continue;
        }
        const auto* u = std::get_if<Underway>(&ship->location);
        const std::string& dest = station_def(content, c.destination).name;
        if (u != nullptr && u->destination == c.destination) {
            if (u->arrival > c.deadline) {
                const sim::Duration late = u->arrival - c.deadline;
                c.warned |= warned_at_risk;
                post(world, MessageKind::warning,
                     late > grace(c)
                         ? std::format("Contract #{} ({}): ETA {} is {} past the deadline, beyond the "
                                       "client's patience. The job will fail.",
                                       id.index, describe(content, c), calendar::format_datetime(u->arrival),
                                       short_days(late))
                         : std::format("Contract #{} ({}): ETA {} is {} past the deadline; expect "
                                       "about {:.0f}% of the pay.",
                                       id.index, describe(content, c), calendar::format_datetime(u->arrival),
                                       short_days(late),
                                       100.0 * (1.0 - max_late_cut * late.to_seconds_f() / grace(c).to_seconds_f())),
                     true);
            }
        } else if (c.deadline - world.now() <= sim::days(1)) {
            c.warned |= warned_at_risk;
            post(world, MessageKind::warning,
                 std::format("Contract #{} ({}) is due at {} by {} and the {} isn't on the way.", id.index,
                             describe(content, c), dest, calendar::format_datetime(c.deadline), ship->name),
                 true);
        }
    }
}

} // namespace

// ---- Queries ------------------------------------------------------------------------------------

std::size_t board_capacity(std::uint32_t population) {
    if (population == 0) {
        return 0;
    }
    // ~3 on a 1,200-soul rock, 5 at 15k, 9 on Ceres: busier ports post more work.
    const double n = 1.5 * std::log10(static_cast<double>(population)) - 1.5;
    return static_cast<std::size_t>(std::clamp(std::lround(n), 2L, 10L));
}

std::vector<ContractId> board_at(const World& world, StationId station) {
    std::vector<ContractId> out;
    for (auto [id, c] : world.contracts) {
        if (c.status == ContractStatus::offered && c.origin == station) {
            out.push_back(id);
        }
    }
    return sorted(std::move(out), world);
}

std::vector<ContractId> held_by(const World& world, CompanyId company) {
    std::vector<ContractId> out;
    for (auto [id, c] : world.contracts) {
        if (c.status == ContractStatus::accepted && c.holder == company) {
            out.push_back(id);
        }
    }
    return sorted(std::move(out), world);
}

std::uint32_t passengers_aboard(const World& world, ShipId ship) {
    std::uint32_t n = 0;
    for (auto [id, c] : world.contracts) {
        (void)id;
        if (c.aboard && c.ship == ship && c.kind == ContractKind::passengers) {
            n += c.passengers;
        }
    }
    return n;
}

std::int32_t standing(const World& world, CompanyId company, Faction faction) {
    for (const Standing& s : world.standings) {
        if (s.company == company && s.faction == faction) {
            return s.value;
        }
    }
    return 0;
}

sim::Duration grace(const Contract& c) {
    const auto quarter = static_cast<std::int64_t>(grace_fraction * c.time_allowed.to_seconds_f());
    return std::max(sim::days(1), sim::seconds(quarter));
}

std::optional<sim::Duration> reference_transit(const Content& content, StationId from, StationId to,
                                               sim::Time t) {
    transit::PlotRequest r;
    r.origin = transit::Origin::at_body(content.orbit_of(from));
    r.destination = content.orbit_of(to);
    r.departure = t;
    r.accel = units::gees(reference_accel_g);
    // Timing only: a notional ship with a bottomless tank.
    r.ship.dry_mass = units::tonnes(1000.0);
    r.ship.reaction_mass = units::tonnes(1.0e9);
    r.avoid = sun_body(content);
    r.avoid_radius = r.avoid.valid() ? units::au(sun_keep_out_au) : 0.0;
    r.horizon = sim::days(365);
    const transit::Plot p = transit::plot_transit(content.orbits(), r);
    if (p.status != transit::PlotStatus::ok) {
        return std::nullopt;
    }
    return p.duration;
}

std::string describe(const Content& content, const Contract& c) {
    const std::string& to = station_def(content, c.destination).name;
    if (c.kind == ContractKind::passengers) {
        return std::format("{} passenger{} -> {}", c.passengers, c.passengers == 1 ? "" : "s", to);
    }
    const std::string& what = content.table<CommodityDef>()[c.commodity].name;
    return c.tonnes >= 10.0 ? std::format("{:.0f} t {} -> {}", c.tonnes, what, to)
                            : std::format("{:.1f} t {} -> {}", c.tonnes, what, to);
}

Terms terms_for(const Content& content, const World& world, const Contract& c, CompanyId company) {
    if (c.status != ContractStatus::offered) {
        return {c.reward, c.deposit}; // fixed on acceptance
    }
    const std::int32_t s = standing(world, company, station_def(content, c.origin).faction);
    return {round10(static_cast<double>(c.reward) * standing_reward_factor(s)),
            round10(static_cast<double>(c.deposit) * standing_deposit_factor(s))};
}

Check can_accept(const Content& content, const World& world, ShipId ship_id, ContractId id) {
    const Contract* c = world.contracts.get(id);
    if (c == nullptr || c->status != ContractStatus::offered || world.now() >= c->expires_at) {
        return {false, "That offer is no longer on the board."};
    }
    const Ship* ship = world.ships.get(ship_id);
    if (ship == nullptr || !is_active(world, ship->owner)) {
        return {false, "No ship to carry it."};
    }
    const auto* docked = std::get_if<Docked>(&ship->location);
    if (docked == nullptr || docked->station != c->origin) {
        return {false, std::format("Jobs are taken in person: the {} must be docked at {}.", ship->name,
                                   station_def(content, c->origin).name)};
    }
    const Faction faction = station_def(content, c->origin).faction;
    const std::int32_t s = standing(world, ship->owner, faction);
    if (s <= refuse_standing) {
        return {false, std::format("Your name is mud with the {} brokers (standing {}).", faction_name(faction), s)};
    }
    if (s < c->min_standing) {
        return {false, std::format("Reserved for captains in good standing ({}+ with {}; you have {}).",
                                   c->min_standing, faction_name(faction), s)};
    }
    if (held_by(world, ship->owner).size() >= max_active) {
        return {false, std::format("You already hold {} contracts.", max_active)};
    }
    const ShipClassDef& cls = content.table<ShipClassDef>()[ship->ship_class];
    if (c->kind == ContractKind::cargo) {
        const double space = cls.cargo_capacity_t - cargo_mass_t(*ship);
        if (space + 1e-9 < c->tonnes) {
            return {false, std::format("Needs {:.1f} t of hold; {:.1f} t free.", c->tonnes, std::max(0.0, space))};
        }
    } else {
        const std::size_t used = crew::aboard(world, ship_id).size() + passengers_aboard(world, ship_id);
        const std::size_t free = cls.crew_berths > used ? cls.crew_berths - used : 0;
        if (free < c->passengers) {
            return {false, std::format("Needs {} berth{}; {} free.", c->passengers, c->passengers == 1 ? "" : "s", free)};
        }
    }
    const Terms t = terms_for(content, world, *c, ship->owner);
    if (world.companies.at(ship->owner).cash < t.deposit) {
        return {false, std::format("Needs a {} deposit.", format_credits(t.deposit))};
    }
    return {true, {}};
}

// ---- Actions ------------------------------------------------------------------------------------

Result accept(const Content& content, World& world, ShipId ship_id, ContractId id) {
    if (const Check check = can_accept(content, world, ship_id, id); !check.ok) {
        return {false, check.reason};
    }
    Contract& c = world.contracts.at(id);
    Ship& ship = world.ships.at(ship_id);
    const Terms t = terms_for(content, world, c, ship.owner);
    if (t.deposit > 0 && !transact(world, ship.owner, -t.deposit, LedgerCategory::contract,
                                   std::format("deposit: contract #{}", id.index))) {
        return {false, std::format("Needs a {} deposit.", format_credits(t.deposit))};
    }
    c.reward = t.reward;
    c.deposit = t.deposit;
    c.status = ContractStatus::accepted;
    c.holder = ship.owner;
    c.ship = ship_id;
    c.accepted_at = world.now();
    c.deadline = world.now() + c.time_allowed;
    c.aboard = true;
    if (c.kind == ContractKind::cargo) {
        ship.consignments.push_back(Consignment{id, c.commodity, c.tonnes});
        if (economy::market_entry(content, c.origin, c.commodity) != nullptr) {
            double& stock = world.stations[index_of(c.origin)].stock[index_of(c.commodity)];
            stock = std::max(0.0, stock - c.tonnes);
        }
    }
    std::string text = std::format("Took contract #{}: {}. Pays {} on delivery by {}{}.", id.index,
                                   describe(content, c), format_credits(c.reward),
                                   calendar::format_datetime(c.deadline),
                                   c.deposit > 0 ? std::format("; {} deposit held", format_credits(c.deposit)) : "");
    if (ship.owner == world.player) {
        post(world, c.kind == ContractKind::cargo ? MessageKind::ship : MessageKind::crew, text);
    }
    return {true, std::move(text)};
}

Result abandon(const Content& content, World& world, CompanyId company, ContractId id) {
    Contract* c = world.contracts.get(id);
    if (c == nullptr || c->status != ContractStatus::accepted || c->holder != company) {
        return {false, "You hold no such contract."};
    }
    Credits fee = 0;
    if (c->deposit == 0) {
        fee = round10(abandon_fee_fraction * static_cast<double>(c->reward));
        if (fee > 0 && !transact(world, company, -fee, LedgerCategory::contract,
                                 std::format("cancellation fee: contract #{}", id.index))) {
            fee = 0;
        }
    }
    const Faction faction = station_def(content, c->origin).faction;
    c->status = ContractStatus::failed;
    c->closed_at = world.now();
    adjust_standing(content, world, company, faction, -abandon_loss);
    const Ship* ship = world.ships.get(c->ship);
    const bool docked = ship == nullptr || std::holds_alternative<Docked>(ship->location);
    if (docked) {
        unload(world, *c, id);
    }
    std::string text = std::format(
        "Abandoned contract #{} ({}). {}{}Standing with {} -{}.", id.index, describe(content, *c),
        c->deposit > 0 ? std::format("The {} deposit is forfeit. ", format_credits(c->deposit)) : "",
        fee > 0 ? std::format("Paid a {} cancellation fee. ", format_credits(fee)) : "", faction_name(faction),
        abandon_loss);
    if (!docked) {
        text += c->kind == ContractKind::cargo ? " The cargo comes off at your next port."
                                               : " The passengers get off at your next port.";
    }
    if (company == world.player) {
        post(world, MessageKind::warning, text);
    }
    return {true, std::move(text)};
}

// ---- Hooks --------------------------------------------------------------------------------------

void start_new_game(const Content& content, World& world) { fill_boards(content, world, 1.0); }

void on_docked(const Content& content, World& world, ShipId ship_id) {
    const Ship* ship = world.ships.get(ship_id);
    if (ship == nullptr) {
        return;
    }
    const StationId here = std::get<Docked>(ship->location).station;
    std::vector<ContractId> due;
    std::vector<ContractId> leftovers;
    for (auto [id, c] : world.contracts) {
        if (!c.aboard || c.ship != ship_id) {
            continue;
        }
        if (c.status == ContractStatus::accepted && c.destination == here) {
            due.push_back(id);
        } else if (c.status != ContractStatus::accepted) {
            leftovers.push_back(id);
        }
    }
    for (ContractId id : sorted(std::move(due), world)) {
        deliver(content, world, id);
    }
    for (ContractId id : sorted(std::move(leftovers), world)) {
        Contract& c = world.contracts.at(id);
        unload(world, c, id);
        if (players(world, c)) {
            post(world, MessageKind::ship,
                 std::format("Put off what was left of contract #{} ({}) at {}.", id.index, describe(content, c),
                             station_def(content, here).name));
        }
    }
}

} // namespace expanse::contracts

namespace expanse::systems {

void daily_contracts(const Content& content, World& world) {
    using namespace contracts;
    const sim::Time now = world.now();
    // Withdraw stale offers; forget closed jobs after a month.
    world.contracts.erase_if([&](ContractId, const Contract& c) {
        return (c.status == ContractStatus::offered && now >= c.expires_at) ||
               (c.status != ContractStatus::offered && c.status != ContractStatus::accepted && !c.aboard &&
                now - c.closed_at >= sim::days(tuning::keep_closed_days));
    });
    // Jobs past their grace period, or whose ship is gone, fail.
    std::vector<std::pair<ContractId, std::string>> failing;
    for (auto [id, c] : world.contracts) {
        if (c.status != ContractStatus::accepted) {
            continue;
        }
        if (!world.ships.contains(c.ship)) {
            failing.emplace_back(id, "the ship is gone");
        } else if (now > c.deadline + grace(c)) {
            failing.emplace_back(id, "the deadline passed");
        }
    }
    std::sort(failing.begin(), failing.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [id, why] : failing) {
        contracts::fail(content, world, id, std::move(why));
    }
    contracts::warn_deadlines(content, world);
    contracts::fill_boards(content, world, tuning::post_chance);
}

} // namespace expanse::systems
