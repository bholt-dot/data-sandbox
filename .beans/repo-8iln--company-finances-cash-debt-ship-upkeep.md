---
# repo-8iln
title: 'Company finances: cash, debt, ship upkeep'
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:39:36Z
parent: repo-tydy
---

Ledger, loan interest, docking fees, maintenance.

## Summary of Changes

New `expanse/include/expanse/finance.hpp` + `expanse/src/finance.cpp` (the `daily_finance` and
`loan_payment_due` hooks), tests in `expanse/tests/finance_tests.cpp`.

- **Ledger**: `transact(world, company, amount, LedgerCategory, description)` is the one writer of
  `Company::cash`; it appends a `LedgerEntry {time, company, amount, category, balance_after,
  description}` to `World::ledger` (append-only, time-ordered). Debits larger than cash are refused
  (returns false, nothing changes). Categories: trade, fuel, docking, wages, loan, repairs, contract,
  supplies, other. Queries for a future `books` command: `ledger_between`, `summarize_ledger`
  (opening/closing balance, income/expense per category), `format_credits`.
- **Docking fees**: each daily tick charges every docked ship's owner its station's fee. Unpaid fees
  go on a `DockTab` (per company and station). The dockmaster collects tabs first, fully or partly,
  from any cash at the next tick. A ship may leave owing up to 3 days' fees. Above that,
  `can_depart(content, world, ship)` refuses and gives a reason. The allowance means a broke
  captain has a few days to leave instead of being soft-locked.
- **Loans**: `open_loan` (scenario now uses it; stores `due_event`). 60 bp/week compounding weekly
  (31.2% nominal, 36.5% effective APR). The Secondhand loan pays 2,280 interest / 1,920 principal
  in week 1 and is paid off in ~131 weeks (~550k repaid). At a due date: interest is added, then the
  instalment is debited in full or missed. Voluntary payments made since the last due date count
  toward it. A missed instalment adds a 10% late fee and a strike; a made one resets the strikes.
  At `missed_payment_limit` the loan is called: `repossess` seizes the ships (owner set to null),
  sets `CompanyStatus::repossessed` and `World::game_over`. `pay_loan` works at any time. It is
  capped at the balance, so there is no negative debt. Paying the balance closes the loan, cancels
  its event and erases the row; a stale due event does nothing. The lender's messages go from
  friendly to menacing, and missed payments and repossession are urgent.
- **Do-nothing timeline (Secondhand)**: 150/day at Ceres. Day 7: 650 cash, instalment missed (1/3).
  Day 12: cash runs out and the dock tab starts. Day 14: missed (2/3), final warning. Day 16: tab
  over allowance, departure blocked. Day 21: third miss, ship repossessed, game over (3 weeks).
- World additions (only added, nothing changed): `Company::status`, Loan fields (`weekly_interest_bp`,
  `paid_since_due`, `interest_charged`, `due_event`), `LedgerEntry`, `DockTab`, `GameOver`,
  `World::{ledger, dock_tabs, game_over}`.

Research (before commit): ledger design guides (Modern Treasury, Fintechly, Temporal) recommend
append-only entries, corrections as new entries, one atomic posting path and balance-after to check
the books. The ledger was designed that way, and a test replays it. Taipan!'s Elder Brother Wu
(10%/month) had an overpayment bug that created "negative debt" that earned interest, so
`pay_loan` caps at the balance. In Patrician III, the lender takes property in proportion to the
debt, and that was the model for repossession ending the debt. Game-design discussions of debt
spirals (Cliff Empire, "Debt Spiral", MMO XP debt) say debt feels unfair when interest outruns
payments or when there is no warning. So the instalment always exceeds the interest, early payments
reduce interest, there is a reminder the day before an unaffordable instalment, the warnings get
stronger, and the dock-tab allowance prevents soft-locks.

Deferred: ship maintenance/wear costs (hull repairs) and "upkeep"; reputation effects of
defaults; a loan collateral field (currently every ship of the borrower is seized); an opening-
balance ledger entry; the shell `books`/`pay` commands (REPL bean).
