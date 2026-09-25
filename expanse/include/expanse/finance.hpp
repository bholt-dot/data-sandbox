#pragma once

// Company finances: the ledger, docking fees, and loans.
//
// THE RULE: every change to Company::cash goes through transact(). It is the only writer of
// Company::cash and World::ledger, so the books always reconcile:
//     entry.balance_after == previous entry's balance_after (same company) + entry.amount.
// Systems that buy, sell, pay or get paid (economy, crew, ship ops, contracts) call transact()
// with a category and a short human-readable description, e.g.
//     if (!transact(world, ship.owner, -cost, LedgerCategory::fuel, "reaction mass 40 t")) { ... }
//     transact(world, ship.owner, +revenue, LedgerCategory::trade, "sold 120 t water");
// transact() never overdraws: a debit larger than the cash on hand is refused (returns false,
// changes nothing). Callers decide what refusal means (can't buy, goes on a tab, missed payment).
//
// Docking: each daily tick, every docked ship's owner is charged its station's docking_fee
// ("per night"). If the owner can't pay, the fee goes on a dock tab owed to that station. Tabs are
// collected automatically (fully or partly) from any cash the company has at the next daily tick,
// before new fees — the dockmaster is senior to every other creditor. The dockmaster lets a ship
// run a tab of up to dock_tab_allowance_days days' fees; beyond that, can_depart() refuses
// departure from that station until the tab is paid down (pay_dock_tab or income). The allowance
// is what keeps a broke captain from being soft-locked: you get a few days to haul something out.
//
// Loans: simple weekly-compounding interest. At each instalment date:
//   1. interest = round(balance * weekly_interest_bp / 10000) is added to the balance;
//   2. the instalment (weekly_payment, less any voluntary payments made since the last due date)
//      is debited if the borrower has the cash — all or nothing; the lender takes no partial
//      instalments at due time, the borrower can make partial payments any time with pay_loan();
//   3. otherwise the instalment is missed: a late fee (late_fee_fraction of the instalment) is
//      added to the balance and missed_payments counts up. A made instalment resets the count
//      (the counter tracks consecutive misses: "three strikes").
//   4. At missed_payment_limit consecutive misses the lender repossesses every ship of the
//      borrower; the company's status becomes repossessed and, for the player, World::game_over
//      is set. The debt is extinguished by the seizure.
// Payments cover the week's interest first, then principal. Voluntary payments reduce the balance
// immediately, so paying early saves interest. A payment can never exceed the balance (no
// "negative debt" — Taipan!'s famous overpayment bug). When the balance reaches zero the loan is
// closed, its due event cancelled and the Loan row erased; a stale LoanPaymentDue is a no-op.
//
// Default rate 60 bp/week: nominal 31.2 %/yr, effective APR (1.006^52 - 1) = 36.5 %. The Secondhand
// loan (380,000 cr at 4,200 cr/week) starts at 2,280 cr interest / 1,920 cr principal per week and
// amortises in ~131 weeks (~2.5 years), repaying ~550,000 cr in total. Predatory, but the
// instalment always exceeds the interest, so steady payment always makes progress.

#include "expanse/content.hpp"
#include "expanse/world.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace expanse {

inline constexpr std::uint32_t default_weekly_interest_bp = 60;
inline constexpr std::int64_t late_fee_percent = 10;      // of the weekly instalment
inline constexpr std::int64_t dock_tab_allowance_days = 3; // days of fees a ship may leave owing

// ---- Ledger -----------------------------------------------------------------------------------

// Posts `amount` (+ income, - expense) to `company`'s cash and appends a LedgerEntry. A debit
// larger than the cash on hand is refused: returns false and changes nothing. A zero amount is a
// no-op that returns true. Throws std::out_of_range for an unknown company.
bool transact(World& world, CompanyId company, Credits amount, LedgerCategory category,
              std::string description);

inline constexpr std::size_t ledger_category_count = enum_names(LedgerCategory{}).size();

std::string_view to_string(LedgerCategory category);

struct LedgerSummary {
    Credits opening_balance = 0; // cash at `from`
    Credits closing_balance = 0; // cash at `to`
    std::array<Credits, ledger_category_count> income{};  // >= 0, by category index
    std::array<Credits, ledger_category_count> expense{}; // >= 0 (magnitude), by category index
    std::size_t entries = 0;

    Credits total_income() const;
    Credits total_expense() const;
    Credits net() const { return total_income() - total_expense(); }
    Credits income_of(LedgerCategory c) const { return income[static_cast<std::size_t>(c)]; }
    Credits expense_of(LedgerCategory c) const { return expense[static_cast<std::size_t>(c)]; }
};

// Entries of all companies with from <= time < to (the ledger is time-ordered).
std::span<const LedgerEntry> ledger_between(const World& world, sim::Time from, sim::Time to);

// Income/expense by category for `company` over [from, to).
LedgerSummary summarize_ledger(const World& world, CompanyId company, sim::Time from, sim::Time to);

// "12,345 cr" / "-150 cr".
std::string format_credits(Credits amount);

// ---- Docking ----------------------------------------------------------------------------------

// What `company` owes the dockmaster at `station` (0 if nothing).
Credits dock_tab(const World& world, CompanyId company, StationId station);
Credits total_dock_tabs(const World& world, CompanyId company);

// Pays as much of the tab at `station` as cash allows (all of it if possible). Returns the amount
// paid.
Credits pay_dock_tab(const Content& content, World& world, CompanyId company, StationId station);

struct DepartureCheck {
    bool allowed = false;
    std::string reason; // empty when allowed; otherwise player-facing
};

// Whether `ship` may undock now. Refused when: the ship doesn't exist, isn't docked, has no owner
// (repossessed), the owner is not active, or the owner's tab at this station exceeds
// dock_tab_allowance_days days of fees. Ship ops call this before committing a transit.
DepartureCheck can_depart(const Content& content, const World& world, ShipId ship);

// ---- Loans ------------------------------------------------------------------------------------

// Creates a loan and schedules its first instalment (a player_event) at `first_due`.
LoanId open_loan(World& world, CompanyId borrower, StationId lender, Credits principal,
                 Credits weekly_payment, std::uint8_t missed_payment_limit, sim::Time first_due,
                 std::uint32_t weekly_interest_bp = default_weekly_interest_bp);

// Interest the next instalment will add at the current balance.
Credits weekly_interest(const Loan& loan);
// What will be debited at the next due date (instalment less voluntary payments, capped at the
// balance plus the coming interest).
Credits instalment_outstanding(const Loan& loan);
// Effective annual rate, e.g. 0.365 for 60 bp/week.
double effective_apr(std::uint32_t weekly_interest_bp);

struct PaymentResult {
    bool ok = false;
    Credits paid = 0;
    bool paid_off = false;
    std::string message; // player-facing, success or reason for refusal
};

// A voluntary payment toward `loan` at any time. `amount` is capped at the balance (pay the whole
// balance to pay off early). Refused if the borrower lacks the cash or isn't active. Counts toward
// the next instalment. Paying the balance closes the loan and cancels its schedule.
PaymentResult pay_loan(const Content& content, World& world, LoanId loan, Credits amount);

// The name the lender goes by in messages, e.g. "Ceres Station Credit".
std::string lender_name(const Content& content, const Loan& loan);

// Seizes every ship of `company`, marks it repossessed and, for the player, ends the game.
void repossess(const Content& content, World& world, CompanyId company, std::string reason);

} // namespace expanse
