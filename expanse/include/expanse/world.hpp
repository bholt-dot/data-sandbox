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
#include <optional>
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

enum class CompanyStatus : std::uint8_t { active, repossessed };

struct Company {
    std::string name;
    Credits cash = 0; // change only through transact() (finance.hpp), which keeps the ledger
    bool is_player = false;
    CompanyStatus status = CompanyStatus::active;

    static constexpr auto fields(auto& self) {
        return std::tie(self.name, self.cash, self.is_player, self.status);
    }
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
    // Crew system state (crew.hpp).
    sim::DefId<CrewOriginDef> origin; // null for the scenario captain if no origins are defined
    std::string background;
    Credits wages_owed = 0;
    std::uint8_t unpaid_weeks = 0; // consecutive paydays missed
    double health = 1.0;           // 0..1; falls when life support runs out

    static constexpr auto fields(auto& self) {
        return std::tie(self.name, self.role, self.skill, self.morale, self.wage_per_week, self.ship,
                        self.station, self.origin, self.background, self.wages_owed,
                        self.unpaid_weeks, self.health);
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
    std::uint32_t weekly_interest_bp = 60; // basis points of balance per week (see finance.hpp)
    Credits paid_since_due = 0;            // voluntary payments counted toward the next instalment
    Credits interest_charged = 0;          // lifetime, for the books
    sim::EventId due_event;                // pending LoanPaymentDue

    static constexpr auto fields(auto& self) {
        return std::tie(self.borrower, self.lender, self.balance, self.weekly_payment,
                        self.missed_payments, self.missed_payment_limit, self.next_due,
                        self.weekly_interest_bp, self.paid_since_due, self.interest_charged,
                        self.due_event);
    }
};

// Every change to a company's cash, in posting order (time is non-decreasing). Append-only:
// corrections are new entries, never edits. Written only by transact() (finance.hpp).
enum class LedgerCategory : std::uint8_t {
    trade, fuel, docking, wages, loan, repairs, contract, supplies, other
};

constexpr auto enum_names(LedgerCategory) {
    using L = LedgerCategory;
    return std::array{std::pair{std::string_view{"trade"}, L::trade},
                      std::pair{std::string_view{"fuel"}, L::fuel},
                      std::pair{std::string_view{"docking"}, L::docking},
                      std::pair{std::string_view{"wages"}, L::wages},
                      std::pair{std::string_view{"loan"}, L::loan},
                      std::pair{std::string_view{"repairs"}, L::repairs},
                      std::pair{std::string_view{"contract"}, L::contract},
                      std::pair{std::string_view{"supplies"}, L::supplies},
                      std::pair{std::string_view{"other"}, L::other}};
}

struct LedgerEntry {
    sim::Time time;
    CompanyId company;
    Credits amount = 0; // + income, - expense
    LedgerCategory category = LedgerCategory::other;
    Credits balance_after = 0;
    std::string description;

    static constexpr auto fields(auto& self) {
        return std::tie(self.time, self.company, self.amount, self.category, self.balance_after,
                        self.description);
    }
};

// Docking fees a company could not pay at a station (see finance.hpp: dock tabs).
struct DockTab {
    CompanyId company;
    StationId station;
    Credits owed = 0;

    static constexpr auto fields(auto& self) { return std::tie(self.company, self.station, self.owed); }
};

// Set when the player's company is lost; the shell stops offering play.
struct GameOver {
    sim::Time time;
    std::string reason;

    static constexpr auto fields(auto& self) { return std::tie(self.time, self.reason); }
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
    std::vector<LedgerEntry> ledger; // append-only; see finance.hpp
    std::vector<DockTab> dock_tabs;  // unpaid docking fees, by (company, station)
    std::optional<GameOver> game_over;

    CompanyId player;

    sim::Time now() const { return scheduler.now(); }

    // The persistent random stream for `key` (e.g. "crew", "market"), created on first use from
    // the world seed. Streams are independent: adding one never perturbs another.
    sim::Rng& rng(const std::string& key);

    static constexpr auto fields(auto& self) {
        return std::tie(self.content_fingerprint, self.seed, self.scheduler, self.rngs,
                        self.companies, self.ships, self.crew, self.loans, self.stations,
                        self.stats, self.messages, self.player, self.ledger, self.dock_tabs,
                        self.game_over);
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
