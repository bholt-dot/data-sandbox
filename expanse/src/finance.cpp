#include "expanse/finance.hpp"

#include "expanse/calendar.hpp"
#include "expanse/scenario.hpp"
#include "expanse/simulation.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace expanse {

namespace {

const StationDef& station_def(const Content& content, StationId station) {
    return content.table<StationDef>()[station];
}

DockTab* find_tab(World& world, CompanyId company, StationId station) {
    for (DockTab& tab : world.dock_tabs) {
        if (tab.company == company && tab.station == station) {
            return &tab;
        }
    }
    return nullptr;
}

Credits dock_tab_allowance(const Content& content, StationId station) {
    return station_def(content, station).docking_fee * dock_tab_allowance_days;
}

bool is_active(const World& world, CompanyId company) {
    const Company* c = world.companies.get(company);
    return c != nullptr && c->status == CompanyStatus::active;
}

// Cash of `company` just before time `t`, reconstructed from the ledger.
Credits balance_before(const World& world, CompanyId company, sim::Time t) {
    const auto& ledger = world.ledger;
    const auto split = std::lower_bound(ledger.begin(), ledger.end(), t,
                                        [](const LedgerEntry& e, sim::Time x) { return e.time < x; });
    for (auto it = split; it != ledger.begin();) {
        --it;
        if (it->company == company) {
            return it->balance_after;
        }
    }
    for (auto it = split; it != ledger.end(); ++it) {
        if (it->company == company) {
            return it->balance_after - it->amount;
        }
    }
    const Company* c = world.companies.get(company);
    return c != nullptr ? c->cash : 0;
}

void schedule_due(World& world, sim::Scheduler<Event>& scheduler, LoanId id) {
    Loan& loan = world.loans.at(id);
    loan.due_event = scheduler.schedule_at(loan.next_due, LoanPaymentDue{id},
                                           {priority_finance, player_event});
}

void close_loan(const Content& content, World& world, sim::Scheduler<Event>& scheduler, LoanId id) {
    const Loan loan = world.loans.at(id);
    scheduler.cancel(loan.due_event);
    world.loans.erase(id);
    post(world, MessageKind::finance,
         std::format("{}: \"Paid in full. Always a pleasure, captain. You know where to find us "
                     "when you need us again.\" The loan is closed; lifetime interest paid {}.",
                     lender_name(content, loan), format_credits(loan.interest_charged)));
}

std::string date_of(sim::Time t) { return calendar::format_date(t); }

} // namespace

// ---- Ledger -----------------------------------------------------------------------------------

bool transact(World& world, CompanyId company, Credits amount, LedgerCategory category,
              std::string description) {
    Company& c = world.companies.at(company);
    if (amount == 0) {
        return true;
    }
    if (amount < 0 && c.cash + amount < 0) {
        return false;
    }
    c.cash += amount;
    world.ledger.push_back(
        LedgerEntry{world.now(), company, amount, category, c.cash, std::move(description)});
    return true;
}

std::string_view to_string(LedgerCategory category) {
    for (const auto& [name, value] : enum_names(LedgerCategory{})) {
        if (value == category) {
            return name;
        }
    }
    return "?";
}

Credits LedgerSummary::total_income() const {
    Credits sum = 0;
    for (const Credits v : income) {
        sum += v;
    }
    return sum;
}

Credits LedgerSummary::total_expense() const {
    Credits sum = 0;
    for (const Credits v : expense) {
        sum += v;
    }
    return sum;
}

std::span<const LedgerEntry> ledger_between(const World& world, sim::Time from, sim::Time to) {
    const auto& ledger = world.ledger;
    auto by_time = [](const LedgerEntry& e, sim::Time x) { return e.time < x; };
    const auto first = std::lower_bound(ledger.begin(), ledger.end(), from, by_time);
    const auto last = std::lower_bound(first, ledger.end(), std::max(from, to), by_time);
    return {first, last};
}

LedgerSummary summarize_ledger(const World& world, CompanyId company, sim::Time from, sim::Time to) {
    LedgerSummary s;
    s.opening_balance = balance_before(world, company, from);
    s.closing_balance = s.opening_balance;
    for (const LedgerEntry& e : ledger_between(world, from, to)) {
        if (e.company != company) {
            continue;
        }
        const auto cat = static_cast<std::size_t>(e.category);
        if (e.amount > 0) {
            s.income[cat] += e.amount;
        } else {
            s.expense[cat] -= e.amount;
        }
        s.closing_balance = e.balance_after;
        ++s.entries;
    }
    return s;
}

std::string format_credits(Credits amount) {
    const bool negative = amount < 0;
    // Work in unsigned magnitude so INT64_MIN is representable.
    std::uint64_t mag = negative ? 0 - static_cast<std::uint64_t>(amount) : static_cast<std::uint64_t>(amount);
    const std::string digits = std::to_string(mag);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            out += ',';
        }
        out += digits[i];
    }
    return (negative ? "-" : "") + out + " cr";
}

// ---- Docking ----------------------------------------------------------------------------------

Credits dock_tab(const World& world, CompanyId company, StationId station) {
    for (const DockTab& tab : world.dock_tabs) {
        if (tab.company == company && tab.station == station) {
            return tab.owed;
        }
    }
    return 0;
}

Credits total_dock_tabs(const World& world, CompanyId company) {
    Credits sum = 0;
    for (const DockTab& tab : world.dock_tabs) {
        if (tab.company == company) {
            sum += tab.owed;
        }
    }
    return sum;
}

Credits pay_dock_tab(const Content& content, World& world, CompanyId company, StationId station) {
    DockTab* tab = find_tab(world, company, station);
    const Company* c = world.companies.get(company);
    if (tab == nullptr || c == nullptr || tab->owed <= 0 || c->cash <= 0) {
        return 0;
    }
    const Credits paid = std::min(tab->owed, c->cash);
    const std::string& name = station_def(content, station).name;
    const bool ok = transact(world, company, -paid, LedgerCategory::docking,
                             std::format("dock tab at {}", name));
    if (!ok) {
        return 0;
    }
    tab->owed -= paid;
    if (tab->owed == 0) {
        post(world, MessageKind::finance,
             std::format("Dock tab at {} settled ({}). Departure clearance restored.", name,
                         format_credits(paid)));
        std::erase_if(world.dock_tabs, [](const DockTab& t) { return t.owed == 0; });
    }
    return paid;
}

DepartureCheck can_depart(const Content& content, const World& world, ShipId ship_id) {
    const Ship* ship = world.ships.get(ship_id);
    if (ship == nullptr) {
        return {false, "no such ship"};
    }
    const auto* docked = std::get_if<Docked>(&ship->location);
    if (docked == nullptr) {
        return {false, std::format("{} is not docked", ship->name)};
    }
    const Company* owner = world.companies.get(ship->owner);
    if (owner == nullptr) {
        return {false, std::format("{} has been impounded and has no owner", ship->name)};
    }
    if (owner->status != CompanyStatus::active) {
        return {false, std::format("{} is in receivership; nothing leaves the dock", owner->name)};
    }
    const Credits owed = dock_tab(world, ship->owner, docked->station);
    const Credits allowance = dock_tab_allowance(content, docked->station);
    if (owed > allowance) {
        return {false, std::format("the dockmaster at {} holds departure clearance until the dock "
                                   "tab is paid down to {} (owed {})",
                                   station_def(content, docked->station).name,
                                   format_credits(allowance), format_credits(owed))};
    }
    return {true, {}};
}

// ---- Loans ------------------------------------------------------------------------------------

LoanId open_loan(World& world, CompanyId borrower, StationId lender, Credits principal,
                 Credits weekly_payment, std::uint8_t missed_payment_limit, sim::Time first_due,
                 std::uint32_t weekly_interest_bp) {
    Loan loan;
    loan.borrower = borrower;
    loan.lender = lender;
    loan.balance = principal;
    loan.weekly_payment = weekly_payment;
    loan.missed_payment_limit = missed_payment_limit;
    loan.next_due = first_due;
    loan.weekly_interest_bp = weekly_interest_bp;
    const LoanId id = world.loans.insert(loan);
    schedule_due(world, world.scheduler, id);
    return id;
}

Credits weekly_interest(const Loan& loan) {
    // Round half up; balances are far below the int64 overflow threshold (~9e14 cr at 10000 bp).
    return (loan.balance * static_cast<Credits>(loan.weekly_interest_bp) + 5000) / 10000;
}

Credits instalment_outstanding(const Loan& loan) {
    const Credits due = std::max<Credits>(0, loan.weekly_payment - loan.paid_since_due);
    return std::min(due, loan.balance + weekly_interest(loan));
}

double effective_apr(std::uint32_t weekly_interest_bp) {
    double growth = 1.0;
    const double weekly = static_cast<double>(weekly_interest_bp) / 10000.0;
    for (int week = 0; week < 52; ++week) {
        growth *= 1.0 + weekly;
    }
    return growth - 1.0;
}

std::string lender_name(const Content& content, const Loan& loan) {
    return station_def(content, loan.lender).name + " Credit";
}

PaymentResult pay_loan(const Content& content, World& world, LoanId id, Credits amount) {
    PaymentResult r;
    Loan* loan = world.loans.get(id);
    if (loan == nullptr) {
        r.message = "no such loan (already paid off?)";
        return r;
    }
    if (amount <= 0) {
        r.message = "payment must be positive";
        return r;
    }
    if (!is_active(world, loan->borrower)) {
        r.message = "the company is in receivership";
        return r;
    }
    amount = std::min(amount, loan->balance);
    const Credits cash = world.companies.at(loan->borrower).cash;
    if (!transact(world, loan->borrower, -amount, LedgerCategory::loan,
                  std::format("payment to {}", lender_name(content, *loan)))) {
        r.message = std::format("can't pay {}: only {} on hand", format_credits(amount),
                                format_credits(cash));
        return r;
    }
    loan->balance -= amount;
    loan->paid_since_due += amount;
    r.ok = true;
    r.paid = amount;
    if (loan->balance == 0) {
        r.paid_off = true;
        r.message = std::format("paid {}; the loan is paid off", format_credits(amount));
        close_loan(content, world, world.scheduler, id);
        return r;
    }
    r.message = std::format("paid {}; balance {}; {} still due {}", format_credits(amount),
                            format_credits(loan->balance),
                            format_credits(instalment_outstanding(*loan)), date_of(loan->next_due));
    post(world, MessageKind::finance,
         std::format("Payment of {} to {} received. Balance {}.", format_credits(amount),
                     lender_name(content, *loan), format_credits(loan->balance)));
    return r;
}

void repossess(const Content& content, World& world, CompanyId company, std::string reason) {
    Company& c = world.companies.at(company);
    if (c.status != CompanyStatus::active) {
        return;
    }
    c.status = CompanyStatus::repossessed;
    for (auto [ship_id, ship] : world.ships) {
        (void)ship_id;
        if (ship.owner == company) {
            ship.owner = CompanyId{};
            post(world, MessageKind::finance,
                 std::format("The {} has been seized. The locks are changed and your codes no "
                             "longer open the airlock.",
                             ship.name),
                 true);
        }
    }
    // The seizure extinguishes the debt.
    std::vector<LoanId> loans;
    for (auto [loan_id, loan] : world.loans) {
        if (loan.borrower == company) {
            loans.push_back(loan_id);
        }
    }
    for (const LoanId id : loans) {
        world.scheduler.cancel(world.loans.at(id).due_event);
        world.loans.erase(id);
    }
    (void)content;
    if (c.is_player && !world.game_over) {
        world.game_over = GameOver{world.now(), reason};
        post(world, MessageKind::warning, std::format("GAME OVER: {}", reason), true);
    }
}

// ---- Systems ----------------------------------------------------------------------------------

namespace systems {

void daily_finance(const Content& content, World& world) {
    // 1. The dockmaster collects old tabs first, from whatever cash there is.
    std::vector<std::pair<CompanyId, StationId>> tabs;
    for (const DockTab& tab : world.dock_tabs) {
        tabs.emplace_back(tab.company, tab.station);
    }
    for (const auto& [company, station] : tabs) {
        if (is_active(world, company)) {
            pay_dock_tab(content, world, company, station);
        }
    }

    // 2. Tonight's fees for every docked ship.
    for (auto [ship_id, ship] : world.ships) {
        (void)ship_id;
        const auto* docked = std::get_if<Docked>(&ship.location);
        if (docked == nullptr || !is_active(world, ship.owner)) {
            continue;
        }
        const StationDef& st = station_def(content, docked->station);
        if (st.docking_fee <= 0) {
            continue;
        }
        if (transact(world, ship.owner, -st.docking_fee, LedgerCategory::docking,
                     std::format("docking {} at {}", ship.name, st.name))) {
            continue;
        }
        DockTab* tab = find_tab(world, ship.owner, docked->station);
        if (tab == nullptr) {
            world.dock_tabs.push_back(DockTab{ship.owner, docked->station, 0});
            tab = &world.dock_tabs.back();
        }
        const Credits before = tab->owed;
        tab->owed += st.docking_fee;
        const Credits allowance = dock_tab_allowance(content, docked->station);
        if (before == 0) {
            post(world, MessageKind::finance,
                 std::format("Can't cover tonight's docking fee at {} ({}). The dockmaster puts it "
                             "on your tab. Owe more than {} and the {} doesn't leave.",
                             st.name, format_credits(st.docking_fee), format_credits(allowance),
                             ship.name),
                 true);
        } else if (before <= allowance && tab->owed > allowance) {
            post(world, MessageKind::finance,
                 std::format("Dock tab at {} is {}. Departure clearance for the {} is withheld "
                             "until you pay it down.",
                             st.name, format_credits(tab->owed), ship.name),
                 true);
        }
    }

    // 3. Heads-up the day before an instalment the borrower can't cover.
    for (auto [loan_id, loan] : world.loans) {
        (void)loan_id;
        if (loan.next_due <= world.now() || loan.next_due - world.now() > sim::days(1)) {
            continue;
        }
        const Company* c = world.companies.get(loan.borrower);
        const Credits due = instalment_outstanding(loan);
        if (c != nullptr && c->status == CompanyStatus::active && c->cash < due) {
            post(world, MessageKind::finance,
                 std::format("{} instalment of {} is due tomorrow. You have {}.",
                             lender_name(content, loan), format_credits(due),
                             format_credits(c->cash)));
        }
    }
}

void loan_payment_due(const Content& content, World& world, sim::Scheduler<Event>& scheduler,
                      LoanId id) {
    if (!world.loans.contains(id)) {
        return; // paid off or seized since this was scheduled
    }
    Loan& loan = world.loans.at(id);
    if (!is_active(world, loan.borrower)) {
        return;
    }
    const std::string lender = lender_name(content, loan);

    const Credits interest = weekly_interest(loan);
    loan.balance += interest;
    loan.interest_charged += interest;

    const Credits due = std::min(std::max<Credits>(0, loan.weekly_payment - loan.paid_since_due),
                                 loan.balance);
    Credits paid_this_week = loan.paid_since_due;
    loan.paid_since_due = 0;

    bool made = due == 0;
    if (!made) {
        made = transact(world, loan.borrower, -due, LedgerCategory::loan,
                        std::format("weekly instalment to {}", lender));
        if (made) {
            loan.balance -= due;
            paid_this_week += due;
        }
    }

    if (made) {
        loan.missed_payments = 0;
        if (loan.balance == 0) {
            close_loan(content, world, scheduler, id);
            return;
        }
        const Credits to_interest = std::min(paid_this_week, interest);
        post(world, MessageKind::finance,
             std::format("{} debited this week's instalment ({} interest, {} principal). Balance "
                         "{}. \"Pleasure doing business, captain.\"",
                         lender, format_credits(to_interest),
                         format_credits(paid_this_week - to_interest), format_credits(loan.balance)));
    } else {
        const Credits late_fee = loan.weekly_payment * late_fee_percent / 100;
        loan.balance += late_fee;
        ++loan.missed_payments;
        const Company& borrower = world.companies.at(loan.borrower);
        if (loan.missed_payments >= loan.missed_payment_limit) {
            post(world, MessageKind::finance,
                 std::format("{}: \"We did warn you, captain.\" {} missed payments; the loan is "
                             "called and the collateral repossessed.",
                             lender, loan.missed_payments),
                 true);
            repossess(content, world, loan.borrower,
                      std::format("{} repossessed by {} after {} missed loan payments",
                                  borrower.name, lender, loan.missed_payment_limit));
            return;
        }
        const int left = loan.missed_payment_limit - loan.missed_payments;
        const std::string threat =
            left == 1 ? "\"That's the last one we let slide. Miss the next and we take the ship. "
                        "We know where you dock.\""
                      : "\"Happens to everyone once, captain. Don't make it a habit.\"";
        post(world, MessageKind::finance,
             std::format("MISSED LOAN PAYMENT ({} of {}). {} couldn't collect {} (you have {}). "
                         "Late fee {} added; balance {}. {} Next instalment due {}.",
                         loan.missed_payments, loan.missed_payment_limit, lender,
                         format_credits(due), format_credits(borrower.cash),
                         format_credits(late_fee), format_credits(loan.balance), threat,
                         date_of(loan.next_due + sim::days(7))),
             true);
    }

    loan.next_due = loan.next_due + sim::days(7);
    schedule_due(world, scheduler, id);
}

} // namespace systems

} // namespace expanse
