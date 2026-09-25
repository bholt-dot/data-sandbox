#pragma once

// Dynamic game state. Everything here is saved, loaded and hashed (each type lists its fields
// once); static definitions live in Content and are referenced by DefId.
//
// Conventions:
//   * Money is integral Credits. Masses are tonnes (double) at this level; convert to kg only
//     when calling the physics in transit.hpp.
//   * Per-station and per-commodity arrays are indexed by the definition's dense index.
//   * Randomness comes from World::rng(key), never from ad-hoc generators.

#include "expanse/content.hpp"
#include "expanse/transit.hpp"
#include "expanse/vec3.hpp"
#include "simcore/rng.hpp"
#include "simcore/scheduler.hpp"
#include "simcore/serialize.hpp"
#include "simcore/stats.hpp"
#include "simcore/table.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace expanse {

struct CompanyTag;
struct ShipTag;
struct CrewTag;
struct LoanTag;
using CompanyId = sim::Handle<CompanyTag>;
using ShipId = sim::Handle<ShipTag>;
using CrewId = sim::Handle<CrewTag>;
using LoanId = sim::Handle<LoanTag>;

using StationId = sim::DefId<StationDef>;
using CommodityId = sim::DefId<CommodityDef>;

// ---- Companies --------------------------------------------------------------------------------

struct Company {
    std::string name;
    Credits cash = 0;
    bool is_player = false;

    static constexpr auto fields(auto& self) { return std::tie(self.name, self.cash, self.is_player); }
};

// ---- Ships ------------------------------------------------------------------------------------

struct CargoLot {
    CommodityId commodity;
    double tonnes = 0.0;
    Credits cost_basis = 0; // what was paid for the lot, for profit reporting

    static constexpr auto fields(auto& self) { return std::tie(self.commodity, self.tonnes, self.cost_basis); }
};

struct Docked {
    StationId station;

    static constexpr auto fields(auto& self) { return std::tie(self.station); }
};

// A committed straight-line transit (see transit.hpp). Position at time t is
// start + dir * state_along(profile, t - departure).distance.
struct Underway {
    StationId origin;
    StationId destination;
    sim::Time departure;
    sim::Time arrival;
    Vec3 start; // heliocentric [m]
    Vec3 end;   // heliocentric [m]
    transit::BurnProfile profile;
    sim::EventId arrival_event;

    static constexpr auto fields(auto& self) {
        return std::tie(self.origin, self.destination, self.departure, self.arrival, self.start,
                        self.end, self.profile, self.arrival_event);
    }
};

using ShipLocation = std::variant<Docked, Underway>;

struct Ship {
    std::string name;
    sim::DefId<ShipClassDef> ship_class;
    CompanyId owner;
    ShipLocation location;
    double reaction_mass_t = 0.0;
    std::vector<CargoLot> cargo;
    double hull_condition = 1.0; // 0..1

    static constexpr auto fields(auto& self) {
        return std::tie(self.name, self.ship_class, self.owner, self.location, self.reaction_mass_t,
                        self.cargo, self.hull_condition);
    }
};

// ---- Crew -------------------------------------------------------------------------------------

enum class CrewRole : std::uint8_t { captain, pilot, engineer, medic, deckhand };

struct CrewMember {
    std::string name;
    CrewRole role = CrewRole::deckhand;
    std::uint8_t skill = 0;  // 0..100
    double morale = 0.5;     // 0..1
    Credits wage_per_week = 0;
    ShipId ship;             // null = ashore looking for work
    StationId station;       // where they are when ashore

    static constexpr auto fields(auto& self) {
        return std::tie(self.name, self.role, self.skill, self.morale, self.wage_per_week, self.ship,
                        self.station);
    }
};

// ---- Finance ----------------------------------------------------------------------------------

struct Loan {
    CompanyId borrower;
    StationId lender; // the station whose lender holds the paper
    Credits balance = 0;
    Credits weekly_payment = 0;
    std::uint8_t missed_payments = 0;
    std::uint8_t missed_payment_limit = 3;
    sim::Time next_due;

    static constexpr auto fields(auto& self) {
        return std::tie(self.borrower, self.lender, self.balance, self.weekly_payment,
                        self.missed_payments, self.missed_payment_limit, self.next_due);
    }
};

// ---- Stations ---------------------------------------------------------------------------------

struct StationState {
    std::vector<double> stock; // t, by commodity index

    static constexpr auto fields(auto& self) { return std::tie(self.stock); }
};

// ---- Player-facing messages -------------------------------------------------------------------

enum class MessageKind : std::uint8_t { info, ship, market, crew, finance, warning };

struct Message {
    sim::Time time;
    MessageKind kind = MessageKind::info;
    bool urgent = false; // stops `advance until event`
    std::string text;

    static constexpr auto fields(auto& self) { return std::tie(self.time, self.kind, self.urgent, self.text); }
};

// ---- Scheduled events -------------------------------------------------------------------------
// Events are plain data; the game's dispatch switches on the alternative.

struct ShipArrives {
    ShipId ship;
    static constexpr auto fields(auto& self) { return std::tie(self.ship); }
};
struct LoanPaymentDue {
    LoanId loan;
    static constexpr auto fields(auto& self) { return std::tie(self.loan); }
};
struct DailyTick {
    static constexpr auto fields(auto&) { return std::tie(); }
};
struct WeeklyTick {
    static constexpr auto fields(auto&) { return std::tie(); }
};

using Event = std::variant<ShipArrives, LoanPaymentDue, DailyTick, WeeklyTick>;

// Scheduler flag: the event matters to the player (stops `advance until event`).
inline constexpr sim::EventFlags player_event = 1u;

// ---- World ------------------------------------------------------------------------------------

struct World {
    std::uint64_t content_fingerprint = 0;
    std::uint64_t seed = 0;
    sim::Scheduler<Event> scheduler;
    std::map<std::string, sim::Rng> rngs; // lazily-created named streams

    sim::Table<CompanyTag, Company> companies;
    sim::Table<ShipTag, Ship> ships;
    sim::Table<CrewTag, CrewMember> crew;
    sim::Table<LoanTag, Loan> loans;
    std::vector<StationState> stations; // by station index
    sim::StatPipeline stats;
    std::vector<Message> messages; // append-only journal; the shell shows what's new

    CompanyId player;

    sim::Time now() const { return scheduler.now(); }

    // The persistent random stream for `key` (e.g. "crew", "market"), created on first use from
    // the world seed. Streams are independent: adding one never perturbs another.
    sim::Rng& rng(const std::string& key);

    static constexpr auto fields(auto& self) {
        return std::tie(self.content_fingerprint, self.seed, self.scheduler, self.rngs,
                        self.companies, self.ships, self.crew, self.loans, self.stations,
                        self.stats, self.messages, self.player);
    }
};

// ---- Queries ----------------------------------------------------------------------------------

// Heliocentric position of a ship at the world's current time.
Vec3 ship_position(const Content& content, const World& world, const Ship& ship);
Vec3 ship_position(const Content& content, const Ship& ship, sim::Time t);

// Total cargo mass aboard [t].
double cargo_mass_t(const Ship& ship);

// ---- Save / load / hash -----------------------------------------------------------------------

std::vector<std::uint8_t> save_world(const World& world);
// Throws sim::SerializeError on corrupt data or a content fingerprint mismatch.
World load_world(std::span<const std::uint8_t> bytes, const Content& content);
std::uint64_t world_hash(const World& world);

inline constexpr std::uint32_t world_schema_version = 1;

} // namespace expanse
