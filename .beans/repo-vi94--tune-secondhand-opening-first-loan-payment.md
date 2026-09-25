---
# repo-vi94
title: 'Tune Secondhand opening: first loan payment'
status: completed
type: task
priority: normal
created_at: 2026-09-25T00:45:36Z
updated_at: 2026-09-25T01:39:28Z
parent: repo-tydy
---

Playtest: the first 4,200 cr instalment is due 7 days in with 1,850 cr cash and an empty hold; even a perfect first trade may not cover it, so the scenario can be unwinnable. Once the economy lands, measure the best achievable week-1 income and tune (grace period for the first instalment, smaller first payment, or starting cargo contract).



## Summary of Changes

**Mechanism (general, data-driven).**
- `ScenarioDef` gets `loan_weekly_interest_bp` (default 60, was hard-wired) and `loan_interest_only_weeks` (default 0). Content validation rejects a scenario whose `loan_weekly_payment` doesn't exceed the weekly interest on the principal, so a steady payer always makes progress once the interest-only period ends.
- `Loan::interest_only_until` (saved and hashed, `world_schema_version` 2). Instalments due before that date are interest only: the scheduled instalment is exactly the interest charged that week (same rounding), so the balance stays flat. After it, `weekly_payment` as before. New helpers in finance.hpp: `interest_only()`, `first_regular_due()`, `scheduled_instalment()`; `instalment_outstanding()` uses them. `open_loan()` takes an optional `interest_only_until`.
- Early payments still count toward the next instalment (an early payment of at least the interest covers an interest-only week). The late fee is 10% of the *scheduled* instalment, so 36 cr during the interest-only weeks. Strikes and repossession work as before. Everything stays deterministic.
- Messages: every interest-only debit or miss, and the last one before the step-up, ends with "From 2350-05-09 your instalment rises to 1,000 cr." The research on payment shock says to announce the step-up early and more than once.

**Numbers (data/scenarios.toml).** 120,000 cr at 30 bp/week (16.9 % effective APR, was 380,000 at 60 bp = 36.5 %), interest only for 8 weeks (360 cr/week), then **1,000 cr/week** (640 cr of principal at first, paid off ~149 weeks after that, ~151,500 cr repaid in total). Starting cash, tank and hull are unchanged, and no station market numbers changed. The scenario description now states the terms.

**Tuning by simulation: `expanse/tests/balance_tests.cpp`.** Scripted captains play Secondhand through the real APIs (`economy::buy/sell/refuel`, `plot_cheapest_within`/`depart`, `pay_loan`, `advance_to`) and never hire:
- *idle*: does nothing at Ceres. It pays the first instalment (day 7), misses days 14/21/28 and is **repossessed on day 28**. I chose this deliberately: Ceres' 150 cr/night docking fee is what kills it. The 8-12 week bound in the brief was an upper limit.
- *careful*: runs the Hollow Nail -> Sakai Drift ore shuttle (the opening leg from Ceres carries water). For each round it picks the trip length (2.5-5 d, cheapest coasting course) with the best credits per day net of reaction mass bought at the local price. It buys fuel for the leg out and back before any cargo, and pays an instalment falling due in flight before undocking unless that would leave < 1,000 cr to trade.
- *careless*: same route, flip-and-burn at 1 g (and a 0.3 g "hasty" variant, the shell's default course), every credit spent on ore, refuels only when the drive refuses.
- Assertions: idle repossessed on day 28. Careful, on each of 16 seeds: never repossessed, at most 2 missed payments, net worth (cash + cargo at best cluster bid - loan - dock tabs) higher at week 26 than at the start, and liquid (cash + cargo) at week 8 above one regular instalment. The median week-8 liquid is below 10 instalments. Careless: repossessed at 1 g, and at 0.3 g either repossessed or 10k+ behind the careful bot (seeds 1-3). The bots are deterministic.
- Tuning aids (env vars): `BELTER_BALANCE_TRACE`, `_TRACE_SEED`, `_SEEDS`, `_TERMS=principal,bp,io_weeks,payment`, `_CASH`.

**Survey results** (careful bot, 64 seeds, 26 weeks: ships lost / missed payments / week-8 liquid median):
| terms | lost | missed | liquid@w8 median |
|---|---|---|---|
| old: 380k, 60 bp, no IO, 4,200 | 64/64 | 256 | 686 |
| 120k, 30 bp, no IO, 1,000 | 56/64 | 304 | 847 |
| 120k, 30 bp, 8 wk IO, 1,200 | 5/64 | 20 | 8,088 |
| 120k, 30 bp, 8 wk IO, 1,100 | 2/32 | 4 | 8,169 |
| **120k, 30 bp, 8 wk IO, 1,000** | **1/64** | 6 | 8,288 |
| 120k, 30 bp, 10 wk IO, 1,200 | 0/32 | 2 | 8,909 |

The interest-only period is what makes the start survivable, and the regular payment is the knife edge. 1,200 cr (the first proposal) sinks the careful bot on 5 of 64 seeds. At 1,000 cr it survives all 16 test seeds; 3 of 64 take strikes, and seed 56 (dear ore from a supply disruption) is lost. That is the "bad luck can still sink a rigid captain" margin; a human who diversifies or buys fuel at Ceres/Dagu does better than the bot.

**Bot trajectories, seed 1** (weekly, just after the instalment; cash / loan balance / cargo at bid / net worth):
```
careful: 49 legs, 0 missed
wk    cash    loan  cargo  net worth       wk    cash    loan  cargo  net worth
 0    1850  120000     27  -118123         14    8382  115450     37  -107031
 1     135  119999   2749  -117115         16    7885  114134     33  -106216
 2     124  119998   3364  -116510         18    9597  112810     42  -103171
 4    4018  119996     17  -115961         20   11298  111478     22  -100158
 6    4828  119994     29  -115137         22    7459  110138     31  -102648
 8    8218  119350     37  -111095         24    8415  108790     39  -100336
10   12586  118058     31  -105441         26    9451  107434     22   -97961
12      36  116758  11218  -105504
idle:            wk1 290/120000, wk2 0/120396 (1 missed), wk3 0/120793 (2), repossessed day 28
careless 1 g:    wk1 0/120396 (tank 42 t), wk2 18/120793 (tank 2 t), repossessed day 21
careless 0.3 g:  wk1 136/120396, wk2 0/120793, repossessed day 21
```
Liquid assets sit on an ~8-10k treadmill from week 8: ore profit minus fuel just covers the 1,000 cr instalment, and growth comes slowly through principal repaid (net worth +20k by week 26, median across seeds). That is the dirt-poor squeeze. Getting out of it takes diversifying (water/oxygen from Dagu, cheaper fuel at Ceres), which the bot does not do.

**Tests updated to the new mechanism (intent kept):** finance_tests.cpp:
- instalment debit (interest-only, then the first regular 360/640 split, plus the step-up message);
- amortisation (paid off at week ~157, ~151.5k total);
- repossession timeline (paid day 7, strikes 14/21, seized day 28, strike reset);
- voluntary payments (an early payment covers an interest-only week; partial payment in the regular period);
- determinism fixture (smaller windfall so a dock tab is still open at the mid-run save).

New finance tests: interest-only maths, and scenario terms wiring/validation. shell_tests.cpp: the no-income repossession now happens at Vesta around day 49, so `advance 60d`.

**Research.**
- Taipan! starts you with a debt that compounds; there is no fixed schedule and the pressure comes from interest growth.
- Recettear ramps its weekly payments (10k, 30k, 80k...) so the first week is gentle and the squeeze grows with the player's means.
- Elite: Dangerous repays its rebuy loan from a share of income.
- Patrician III's hard start is 1,000 gold and one ship, with credit only after a few trades.
- Offworld Trading Company prices debt by the borrower's rating.
- On the finance side: interest-only loans end in "payment shock", and default risk spikes at the reset (HELOC studies). That is why the messages name the step-up date and amount from the first instalment on, and why the regular payment was tuned against what a week-8 captain can actually earn.
- Balance-testing practice: scripted bots on the game's own APIs as regression tests, swept over many seeds to find where balance breaks (boundary discovery), not a single run.

**Deferred / follow-ups.**
- The shell's `status` still prints `loan.weekly_payment` as "due" ("1,000 cr due 2350-03-21" when 360 cr is due). It should print `instalment_outstanding(loan)` and, while `interest_only(loan)`, the step-up date. shell.cpp belongs to the TUI rewrite, so this is left for that work.
- The shell's default course (0.3 g flip-and-burn) is the "hasty" careless bot, which loses the ship by day 21. A newcomer typing `go` gets the ruinous option. Consider defaulting `go` to an economical course or showing the fuel cost more prominently.
- There are no repairs yet: hull wear is cosmetic. The bot's economy doesn't pay for repairs, so the treadmill will tighten once repairs exist; re-run the survey then.
