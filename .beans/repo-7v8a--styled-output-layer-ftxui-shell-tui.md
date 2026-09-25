---
# repo-7v8a
title: Styled output layer + FTXUI shell TUI
status: completed
type: feature
priority: normal
created_at: 2026-09-25T01:08:58Z
updated_at: 2026-09-25T01:38:35Z
parent: repo-89dz
---

Commands emit semantic output (heading, money, warning, table...) through a core output layer rendered as plain text (scripts/tests/pipes), ANSI colour, or FTXUI panels. TUI: always-visible status banner, scrolling output, journal panel with pinned urgent items, input line with completion and history. Full-screen only on an interactive TTY; --plain/--script keep current behaviour. Subsumes the shell colour bean.

## Summary of Changes

**Output layer (core, no dependencies)** — `simcore/doc.hpp`, `src/doc.cpp`:
- `sim::Doc`: lines of styled `Span`s plus `TextTable`s (columns with header + left/right
  alignment, auto-sized, indent/gap, last column wraps under a width limit). Stream-like writing
  (`out << "cash " << styled(Style::money, "5 cr") << "\n"`), `heading()`, `table()`; arithmetic
  `operator<<` is deleted so numbers can't silently become chars. Blocks live in a deque so a table
  reference stays valid while writing on.
- Semantic styles: plain, heading, emphasis, dim, key, money, positive, negative, good, bad,
  warning, urgent.
- `layout(doc, width)` → rows of spans (tables padded, lines word-wrapped with hanging indent; 0 =
  unlimited). All three renderers sit on it: plain text (`to_text`, byte-stable), ANSI
  (`render(doc, os, Ansi::color|mono)`), and the FTXUI view in apps/.
- `ansi_for_terminal(is_tty)`: none off a TTY or for TERM=dumb, `mono` (bold/dim/inverse only)
  when NO_COLOR is non-empty, 16-colour SGR otherwise (the user's terminal palette applies).
- `CommandBus` handlers now take `sim::Doc&`; `submit_line/apply_pending/execute_line` too.
  `run_repl` renders each line's Doc (new `ReplOptions::ansi`). `help` uses a table and key styling;
  plain help text is unchanged. Log/replay/script behaviour unchanged; the 20-year determinism
  CTest passes.

**Game** — every command in `expanse/src/shell.cpp` migrated: tables for market, stations,
station, routes, crew, books (category + ledger); money/money_delta helpers (gains green, losses
red), resource levels (RM/hull/morale/tank) warn/negative by threshold, GO/NO GO good/bad, urgent
journal lines and missed payments, keys you can type in cyan. New game-side queries for any front
end: `status_banner(const Session&)` (plain data: date, cash, earliest loan instalment +
countdown + missed/limit, ship docked/destination+ETA, RM %, hull %, crew/berths, game over),
`banner_segments()` (styled segments) and `journal_line(msg, compact)`.

**TUI (apps/, FTXUI v7.0.3 pinned, SYSTEM, option SIM_TUI)** — `apps/tui/doc_view.*` (generic:
styled rows → FTXUI elements, colour or mono; `plain_text(Screen)` for frame snapshots) and
`apps/belter/tui.*` (`belter::Tui`):
- banner packed into as many rows as the width needs; output scrollback laid out at pane width,
  cached per width, only visible rows built (PgUp/PgDn, mouse wheel; hint line shows how far back);
  journal panel on the right (width/3, 30..56 cols) with unacknowledged urgent entries pinned at the
  top for 7 game days or until Esc on an empty line; below 100 columns the panel collapses and
  pinned entries show as a strip above the output; input line (FTXUI Input) with Up/Down history
  and Tab completion via `CommandRegistry::complete()` (unique → completes + space; ambiguous →
  longest common prefix, candidates on the hint line).
- Ctrl+C (handled by us via `ForceHandleCtrlC(false)`, so exit is a normal return, code 0), Ctrl+D
  on an empty line, or `quit` end the loop; FTXUI restores the terminal (checked in a pty).
- `Tui::run()` blocks whichever thread calls it; the bus and session are touched only from inside
  run(), so the future SDL viewer can own the main thread.

**Mode selection** (`apps/belter/main.cpp`): `--script` → plain text; otherwise full-screen TUI
when stdin and stdout are TTYs, TERM is not dumb and `--plain` is not given; `--plain` (or a
dumb terminal) → isocline REPL with ANSI colour/mono; piped input → plain text REPL.

**Tests**: `core/tests/doc_tests.cpp` (plain text, tables, wrapping, ANSI colour/mono codes,
NO_COLOR/TERM detection), command tests on Doc, shell tests for styled/tabular output, status
banner and journal lines, `apps/tests/tui_tests.cpp` rendering frames with `ftxui::Screen`
(banner content, journal, narrow collapse, pin/acknowledge, completion, history, paging, quit).
dev (ASan/UBSan), release and clang builds warning-free and green.

**Research**: Rich's design (semantic theme names; renderables resolved to lines of Segments at
a width via `render_lines`) → the Doc/layout split; no-color.org (only non-empty NO_COLOR counts;
bold/underline remain allowed → the mono theme; flags/config may override); FTXUI docs and
discussions (Renderer+CatchEvent around an Input is the standard pattern; `App::PostEvent` is
thread-safe (fixed in 7.0.2) for redraw requests from another thread; manual slicing instead of a
yframe for large logs; FTXUI 7.0.1 itself degrades colours under NO_COLOR, we still pick an
explicit mono theme so urgent stays visible via inverse); MUD client layouts (TinTin++ #split:
fixed status rows the scrollback never touches, PgUp/PgDn scrollback, input at the bottom).

Deferred: `--color always|never` flag (NO_COLOR precedence rule), reflowing scenario descriptions
(hard line breaks in data wrap unevenly in narrow panes), wrapping plain/ANSI REPL output at the
terminal width, a thread-safe `Tui::post()`/redraw hook for the SDL viewer integration, mouse
text selection (mouse tracking is on for the wheel; Shift+drag selects in most terminals).
