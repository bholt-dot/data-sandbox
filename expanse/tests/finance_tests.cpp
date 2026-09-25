#include <doctest/doctest.h>

#include "expanse/content.hpp"
#include "expanse/finance.hpp"
#include "expanse/ships.hpp"
#include "expanse/scenario.hpp"
#include "expanse/simulation.hpp"
#include "expanse/world.hpp"

#include <map>
#include <memory>

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

LoanId player_loan(const World& w) {
    for (auto [id, loan] : w.loans) {
        if (loan.borrower == w.player) {
            return id;
        }
    }
    return {};
}

Credits cash(const World& w) { return w.companies.at(w.player).cash; }

StationId ceres(const Content& c) { return c.find<StationDef>("ceres_station"); }

bool any_pending_loan_event(const World& w) {
    for (const auto& e : w.scheduler.pending_events()) {
        if (std::holds_alternative<LoanPaymentDue>(e.payload)) {
            return true;
        }
    }
    return false;
}

std::size_t count_entries(const World& w, LedgerCategory cat) {
    std::size_t n = 0;
    for (const LedgerEntry& e : w.ledger) {
        n += e.category == cat ? 1u : 0u;
    }
    return n;
}

// Replays each company's ledger from its opening cash and checks it lands on today's cash.
void check_books_reconcile(const World& w, const std::map<std::uint32_t, Credits>& opening) {
    std::map<std::uint32_t, Credits> running = opening;
    sim::Time last{};
    bool first = true;
    for (const LedgerEntry& e : w.ledger) {
        CHECK(e.amount != 0);
        if (!first) {
            CHECK(e.time >= last);
        }
        first = false;
        last = e.time;
        Credits& bal = running[e.company.index];
        bal += e.amount;
        CHECK(e.balance_after == bal);
        CHECK(e.balance_after >= 0);
    }
    for (auto [id, company] : w.companies) {
        CHECK(running[id.index] == company.cash);
    }
}

std::map<std::uint32_t, Credits> opening_cash(const World& w) {
    std::map<std::uint32_t, Credits> m;
    for (auto [id, company] : w.companies) {
        m[id.index] = company.cash;
    }
    return m;
}

} // namespace

TEST_CASE("ledger records every transaction and balance after is consistent") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const auto opening = opening_cash(w);
    const sim::Time start = w.now();

    CHECK(transact(w, w.player, 500, LedgerCategory::trade, "sold water"));
    CHECK(transact(w, w.player, -300, LedgerCategory::fuel, "reaction mass"));
    CHECK_FALSE(transact(w, w.player, -100000, LedgerCategory::repairs, "new drive"));
    CHECK(transact(w, w.player, 0, LedgerCategory::other, "nothing"));
    REQUIRE(w.ledger.size() == 2);
    CHECK(w.ledger[0].balance_after == 2350);
    CHECK(w.ledger[1].balance_after == 2050);
    CHECK(w.ledger[1].category == LedgerCategory::fuel);
    CHECK(w.ledger[1].description == "reaction mass");
    CHECK(cash(w) == 2050);

    const LedgerSummary s = summarize_ledger(w, w.player, start, start + sim::days(1));
    CHECK(s.opening_balance == 1850);
    CHECK(s.closing_balance == 2050);
    CHECK(s.income_of(LedgerCategory::trade) == 500);
    CHECK(s.expense_of(LedgerCategory::fuel) == 300);
    CHECK(s.net() == 200);
    CHECK(s.entries == 2);

    // Systems post through the ledger too; the books still reconcile after a month.
    advance_to(c, w, start + sim::days(30), false);
    check_books_reconcile(w, opening);
    const LedgerSummary month = summarize_ledger(w, w.player, start, w.now());
    CHECK(month.opening_balance == 1850);
    CHECK(month.closing_balance == cash(w));
    CHECK(month.opening_balance + month.net() == month.closing_balance);
    // A later window opens where the earlier one closed.
    const LedgerSummary tail = summarize_ledger(w, w.player, start + sim::days(3), w.now());
    const LedgerSummary head = summarize_ledger(w, w.player, start, start + sim::days(3));
    CHECK(tail.opening_balance == head.closing_balance);
    CHECK(ledger_between(w, start, w.now()).size() == w.ledger.size());
    CHECK(ledger_between(w, w.now(), start).empty());

    CHECK(to_string(LedgerCategory::docking) == "docking");
    CHECK(format_credits(1234567) == "1,234,567 cr");
    CHECK(format_credits(-150) == "-150 cr");
    CHECK(format_credits(0) == "0 cr");
}

TEST_CASE("docking fees accrue daily") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const sim::Time start = w.now();
    const Credits fee = c.table<StationDef>()[ceres(c)].docking_fee;
    REQUIRE(fee > 0);
    advance_to(c, w, start + sim::days(5), false); // nightly ticks on days 0..4
    CHECK(count_entries(w, LedgerCategory::docking) == 5);
    CHECK(cash(w) == 1850 - 5 * fee);
    const LedgerSummary s = summarize_ledger(w, w.player, start, w.now());
    CHECK(s.expense_of(LedgerCategory::docking) == 5 * fee);
    CHECK(can_depart(c, w, player_ship(w)).allowed);
}

TEST_CASE("broke captains run a dock tab that blocks departure") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const sim::Time start = w.now();
    const ShipId ship = player_ship(w);
    REQUIRE(transact(w, w.player, -cash(w), LedgerCategory::other, "drank it"));

    advance_to(c, w, start + sim::days(1), false);
    CHECK(dock_tab(w, w.player, ceres(c)) == 150);
    CHECK(w.messages.back().urgent);
    CHECK(can_depart(c, w, ship).allowed); // within the three-day allowance

    advance_to(c, w, start + sim::days(4), false);
    CHECK(dock_tab(w, w.player, ceres(c)) == 600);
    CHECK(total_dock_tabs(w, w.player) == 600);
    const DepartureCheck blocked = can_depart(c, w, ship);
    CHECK_FALSE(blocked.allowed);
    CHECK(blocked.reason.find("dockmaster") != std::string::npos);
    CHECK(cash(w) == 0);
    // Ship operations enforce it: the ship can't undock.
    const DepartResult d = depart(c, w, ship, c.find<StationDef>("vesta_dock"), {});
    CHECK(d.status == CourseStatus::dock_fees_owed);
    CHECK(d.reason == blocked.reason);
    CHECK(std::holds_alternative<Docked>(w.ships.at(ship).location));

    // Income arrives; paying the tab restores clearance.
    REQUIRE(transact(w, w.player, 1000, LedgerCategory::contract, "odd job"));
    CHECK(pay_dock_tab(c, w, w.player, ceres(c)) == 600);
    CHECK(dock_tab(w, w.player, ceres(c)) == 0);
    CHECK(w.dock_tabs.empty());
    CHECK(can_depart(c, w, ship).allowed);
    CHECK(cash(w) == 400);

    // Partial income: the dockmaster collects what there is at the next tick.
    REQUIRE(transact(w, w.player, -400, LedgerCategory::other, "drank it again"));
    advance_to(c, w, w.now() + sim::days(2), false); // tab 300
    REQUIRE(transact(w, w.player, 100, LedgerCategory::trade, "scrap"));
    advance_to(c, w, w.now() + sim::days(1), false); // collects 100, then tonight's fee
    CHECK(dock_tab(w, w.player, ceres(c)) == 350);
    CHECK(cash(w) == 0);
    check_books_reconcile(w, {{w.player.index, 1850}});
}

TEST_CASE("loan instalment is debited when cash is available") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const sim::Time start = w.now();
    REQUIRE(transact(w, w.player, 100000, LedgerCategory::contract, "windfall"));
    const LoanId id = player_loan(w);
    REQUIRE(w.loans.contains(id));
    CHECK(w.scheduler.is_pending(w.loans.at(id).due_event));

    // The due event is a player event: an interruptible advance stops on it.
    const AdvanceReport r = advance_to(c, w, start + sim::days(30), true);
    CHECK(r.stopped_early);
    CHECK(w.now() == start + sim::days(7));
    const Loan& loan = w.loans.at(id);
    CHECK(loan.balance == 380000 + 2280 - 4200);
    CHECK(loan.interest_charged == 2280);
    CHECK(loan.missed_payments == 0);
    CHECK(loan.next_due == start + sim::days(14));
    CHECK(w.scheduler.is_pending(loan.due_event));
    CHECK(w.ledger.back().category == LedgerCategory::loan);
    CHECK(w.ledger.back().amount == -4200);
    CHECK(w.messages.back().text.find("2,280 cr interest, 1,920 cr principal") != std::string::npos);
}

TEST_CASE("loan interest math") {
    Loan loan;
    loan.balance = 380000;
    loan.weekly_payment = 4200;
    loan.weekly_interest_bp = 60;
    CHECK(weekly_interest(loan) == 2280);
    loan.balance = 84; // 0.504 cr rounds up
    CHECK(weekly_interest(loan) == 1);
    loan.balance = 83; // 0.498 cr rounds down
    CHECK(weekly_interest(loan) == 0);
    CHECK(effective_apr(60) == doctest::Approx(0.3649).epsilon(0.001));
    CHECK(effective_apr(0) == 0.0);
    loan.balance = 1000;
    CHECK(instalment_outstanding(loan) == 1006); // final instalment: balance plus interest
    loan.balance = 380000;
    loan.paid_since_due = 3000;
    CHECK(instalment_outstanding(loan) == 1200);

    // Paying on schedule amortises the Secondhand loan in ~2.5 years.
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const sim::Time start = w.now();
    REQUIRE(transact(w, w.player, 900000, LedgerCategory::contract, "rich uncle"));
    advance_to(c, w, start + sim::days(7 * 130), false);
    REQUIRE(w.loans.size() == 1);
    advance_to(c, w, start + sim::days(7 * 132), false);
    CHECK(w.loans.empty());
    CHECK_FALSE(any_pending_loan_event(w));
    const LedgerSummary s = summarize_ledger(w, w.player, start, w.now());
    CHECK(s.expense_of(LedgerCategory::loan) > 540000);
    CHECK(s.expense_of(LedgerCategory::loan) < 560000);
    CHECK_FALSE(w.game_over);
}

TEST_CASE("missed payments escalate to repossession on schedule") {
    // Doing nothing with the Secondhand numbers: dock fees drain the cash, the first three
    // instalments are missed, and the ship is seized on day 21.
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const sim::Time start = w.now();
    const LoanId id = player_loan(w);
    const ShipId ship = player_ship(w);

    advance_to(c, w, start + sim::days(7) + sim::hours(1), false);
    REQUIRE(w.loans.contains(id));
    CHECK(w.loans.at(id).missed_payments == 1);
    CHECK(w.loans.at(id).balance == 380000 + 2280 + 420);
    CHECK(cash(w) == 1850 - 8 * 150);
    bool warned = false, missed_urgent = false;
    for (const Message& m : w.messages) {
        warned = warned || m.text.find("due tomorrow") != std::string::npos;
        missed_urgent = missed_urgent || (m.urgent && m.text.find("MISSED") != std::string::npos);
    }
    CHECK(warned);
    CHECK(missed_urgent);

    advance_to(c, w, start + sim::days(14) + sim::hours(1), false);
    REQUIRE(w.loans.contains(id));
    CHECK(w.loans.at(id).missed_payments == 2);
    CHECK(w.messages.back().text.find("take the ship") != std::string::npos);
    CHECK_FALSE(w.game_over);
    CHECK(w.ships.at(ship).owner == w.player);

    advance_to(c, w, start + sim::days(21), false);
    CHECK_FALSE(w.game_over); // the third instalment falls due at exactly day 21
    advance_to(c, w, start + sim::days(21) + sim::hours(1), false);
    REQUIRE(w.game_over);
    CHECK(w.game_over->time == start + sim::days(21));
    CHECK(w.game_over->reason.find("repossessed") != std::string::npos);
    CHECK(w.companies.at(w.player).status == CompanyStatus::repossessed);
    CHECK(w.ships.at(ship).owner.is_null());
    CHECK(w.loans.empty());
    CHECK_FALSE(any_pending_loan_event(w));
    CHECK_FALSE(can_depart(c, w, ship).allowed);
    CHECK(w.messages.back().urgent);

    // The world keeps turning without charging the seized ship.
    const std::size_t ledger_size = w.ledger.size();
    advance_to(c, w, start + sim::days(60), false);
    CHECK(w.ledger.size() == ledger_size);

    // A made payment resets the strike count.
    World w2 = new_game(c, "secondhand", 1);
    advance_to(c, w2, start + sim::days(14) + sim::hours(1), false);
    REQUIRE(w2.loans.at(player_loan(w2)).missed_payments == 2);
    REQUIRE(transact(w2, w2.player, 10000, LedgerCategory::contract, "last-minute job"));
    advance_to(c, w2, start + sim::days(21) + sim::hours(1), false);
    CHECK_FALSE(w2.game_over);
    CHECK(w2.loans.at(player_loan(w2)).missed_payments == 0);
}

TEST_CASE("voluntary payments count toward the instalment and paying off stops events") {
    const Content& c = game_content();
    World w = new_game(c, "secondhand", 1);
    const sim::Time start = w.now();
    const LoanId id = player_loan(w);
    REQUIRE(transact(w, w.player, 500000, LedgerCategory::contract, "salvage claim"));

    // Refusals.
    CHECK_FALSE(pay_loan(c, w, id, 0).ok);
    CHECK_FALSE(pay_loan(c, w, LoanId{}, 100).ok);

    // A partial payment early in the week: only the remainder is debited at due time, and
    // the lower balance saves interest.
    advance_to(c, w, start + sim::days(2), false);
    const PaymentResult part = pay_loan(c, w, id, 3000);
    CHECK(part.ok);
    CHECK(part.paid == 3000);
    CHECK(w.loans.at(id).balance == 377000);
    CHECK(instalment_outstanding(w.loans.at(id)) == 1200);
    advance_to(c, w, start + sim::days(7) + sim::hours(1), false);
    CHECK(w.ledger.back().amount == -1200);
    CHECK(w.loans.at(id).balance == 377000 + 2262 - 1200);
    CHECK(w.loans.at(id).paid_since_due == 0);

    // Overpaying is capped at the balance (no negative debt).
    const Credits balance = w.loans.at(id).balance;
    const Credits before = cash(w);
    const PaymentResult off = pay_loan(c, w, id, balance + 50000);
    CHECK(off.ok);
    CHECK(off.paid_off);
    CHECK(off.paid == balance);
    CHECK(cash(w) == before - balance);
    CHECK_FALSE(w.loans.contains(id));
    CHECK_FALSE(any_pending_loan_event(w));

    const std::size_t loan_entries = count_entries(w, LedgerCategory::loan);
    advance_to(c, w, start + sim::days(60), false);
    CHECK(count_entries(w, LedgerCategory::loan) == loan_entries);
    CHECK_FALSE(w.game_over);

    // A stale due event for a closed loan is a no-op.
    systems::loan_payment_due(c, w, w.scheduler, id);
    CHECK(count_entries(w, LedgerCategory::loan) == loan_entries);

    // Can't pay with money you don't have.
    World poor = new_game(c, "secondhand", 1);
    const PaymentResult r = pay_loan(c, poor, player_loan(poor), 5000);
    CHECK_FALSE(r.ok);
    CHECK(r.message.find("only") != std::string::npos);
    CHECK(cash(poor) == 1850);
}

TEST_CASE("finance is deterministic and save load mid run equals uninterrupted") {
    const Content& c = game_content();
    auto setup = [&](World& w) {
        REQUIRE(transact(w, w.player, 9000, LedgerCategory::contract, "short haul"));
    };
    World a = new_game(c, "secondhand", 5);
    World b = new_game(c, "secondhand", 5);
    setup(a);
    setup(b);
    const sim::Time start = a.now();

    advance_to(c, a, start + sim::days(10), false);
    (void)pay_loan(c, a, player_loan(a), 2500);
    advance_to(c, a, start + sim::days(45), false);

    advance_to(c, b, start + sim::days(10), false);
    World b2 = load_world(save_world(b), c);
    (void)pay_loan(c, b2, player_loan(b2), 2500);
    advance_to(c, b2, start + sim::days(16), false);
    World b3 = load_world(save_world(b2), c); // mid-run, with a dock tab open
    advance_to(c, b3, start + sim::days(45), false);

    CHECK(a.game_over.has_value()); // this captain still didn't find work
    CHECK(world_hash(a) == world_hash(b3));
    CHECK(a.ledger.size() == b3.ledger.size());
    CHECK(save_world(a) == save_world(b3));
}
