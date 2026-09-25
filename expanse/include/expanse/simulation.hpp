#pragma once

// Advancing the game clock. The scheduler fires events in deterministic order; dispatch() routes
// each one to the system hooks below. Every system lives in its own translation unit
// (economy.cpp, crew.cpp, finance.cpp, ships.cpp) so systems can evolve independently.

#include "expanse/content.hpp"
#include "expanse/world.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace expanse {

// Appends to the world's journal (time-stamped with world.now()).
void post(World& world, MessageKind kind, std::string text, bool urgent = false);

struct AdvanceReport {
    std::uint64_t occurrences = 0;
    std::size_t first_new_message = 0; // index into world.messages
    bool stopped_early = false;        // an urgent message / player event interrupted the advance
};

// Runs the world forward to `until`. With `stop_on_urgent`, stops right after the first
// occurrence that posted an urgent message or carried the player_event flag.
AdvanceReport advance_to(const Content& content, World& world, sim::Time until, bool stop_on_urgent);

// Routes one event to its system. Exposed for tests; normally called by advance_to.
void dispatch(const Content& content, World& world, sim::Scheduler<Event>& scheduler, const Event& event);

// ---- System hooks (each defined in its own .cpp) ----------------------------------------------
namespace systems {

// economy.cpp: station production/consumption, prices drift.
void daily_economy(const Content& content, World& world);
// finance.cpp: docking fees etc.
void daily_finance(const Content& content, World& world);
// crew.cpp: wages, morale, hiring pool turnover.
void weekly_crew(const Content& content, World& world);
// crew.cpp: life support consumption, deprivation, crew walking off at port.
void daily_crew(const Content& content, World& world);
// finance.cpp: a scheduled loan instalment.
void loan_payment_due(const Content& content, World& world, sim::Scheduler<Event>& scheduler, LoanId loan);
// contracts.cpp: withdraw stale offers, fail overdue jobs, deadline warnings, post new offers.
void daily_contracts(const Content& content, World& world);
// ships.cpp: a transit completes (and contracts bound for the port are delivered).
void ship_arrives(const Content& content, World& world, sim::Scheduler<Event>& scheduler, ShipId ship);

} // namespace systems

} // namespace expanse
