---
# repo-exl2
title: REPL with command bus
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:29:18Z
parent: repo-9zts
---

Commands parsed into command structs applied at tick start. History, completion.


## Summary of Changes

Generic command layer in core (`namespace sim`, no game concepts, no dependencies) plus a thin
interactive front-end in `apps/belter`.

**core/include/simcore/command.hpp + core/src/command.cpp**
- `tokenize()`: splits on whitespace. Handles `"double"` quotes (with `\"` `\\` escapes), `'single'`
  (literal) quotes, joined `pre"quoted"post` words, and `#` comments that start at a word start.
  `Token::option_syntax` marks unquoted `--x`, so `"--x"` is a plain value and a lone `--` ends
  options. An unterminated quote gives an error with its column. `quote_token()` is the inverse.
- Typed values (`ArgType` / `ArgValue` variant): integer, number, string, duration (`30d`, `6h`,
  `90m`, `45s`, `2w`, `1d12h`, `1.5h`; exact integer arithmetic), acceleration (`0.3g`,
  `2.5m/s2`), flag. Errors look like "expected duration like 30d, 6h or 90m, got 'abc'".
  `format_value()` gives canonical text that parses back to the identical value (doubles use the
  shortest round-trip form; accelerations are written in g only when that is bit-exact).
- `CommandSpec`/`ArgSpec`: name, aliases, summary/details, positionals (required, optional, or
  defaulted; a string `rest` positional collects the remaining words), `--options`, `choices`,
  and a `completer` callback for runtime candidates such as station names.
- `CommandRegistry`: `add` (checks specs), `parse`/`bind` produce an `Invocation` (plain data: the
  canonical name plus typed args; defaults are filled in so logs don't depend on later default
  changes), `format` (canonical script line), `suggest` (prefix matches, then optimal-string-
  alignment edit distance; never auto-executes a prefix), `usage`/`write_help`, `complete` (command
  names, `--options`, option/positional values, `--opt=` values, open quotes). Also
  `parse_script` (errors as "line N: ...").
- `Invocation`/`NamedArg`/`LoggedCommand` have `fields()`, so the command log can go into
  snapshots.

**core/include/simcore/command_bus.hpp**: `CommandBus<Context>`
- `add_query(spec, void(const Context&, const Invocation&, std::ostream&))` runs immediately and is
  never logged.
- `add_action(spec, void(Context&, const Invocation&, std::ostream&))` is queued (`submit_line` /
  `submit`) and applied in FIFO order by `apply_pending` at tick start, then appended to the log.
  `execute_line` = submit + apply, the unit of work for REPL and script lines alike.
- Failed actions are logged with their error, so a replay stays faithful even if a handler changed
  state before throwing. `write_script` adds a `# failed: ...` comment to those lines.
- Built-ins: `help [command]` (completes command names), `quit`/`exit`, `history` (the log as a
  replayable script).
- Handlers only write to the given `std::ostream`, never `std::cout`.

**core/include/simcore/repl.hpp**
- `LineReader` interface and `StreamLineReader` (getline, optional `file:line` location, optional
  echo for piped transcripts).
- `run_repl(bus, ctx, reader, out, {prompt, stop_on_error})` returns 0, or 1 when stopped by an
  error.
- Batch mode (repo-96k2) is `StreamLineReader(ifstream, path)` + `stop_on_error`. The table
  inspector (repo-mou8) can use `rest` args plus `completer` for table and column names.

**apps/lineedit (target `sim_lineedit`) + apps/belter**
- `make_terminal_reader()` returns an isocline-backed reader (in-memory history, Tab completion
  through `CommandRegistry::complete`). It returns nullptr when stdin or stdout is not a TTY, and
  the app then uses a plain `StreamLineReader` that echoes each line: no escape codes.
  `-DSIM_LINE_EDITING=OFF` drops isocline completely.
- `belter` is now a REPL with placeholder commands: `date` (query), `advance <span>` (action on a
  `sim::Scheduler` with a daily periodic tick counter) and `save-script <file>` (writes the replay
  script).

**Build**
- `cmake/Dependencies.cmake` pins isocline `v1.1.0` via FetchContent (SYSTEM). Its own CMakeLists
  also builds a shared library and samples, so it is skipped with `SOURCE_SUBDIR` and the single
  `src/isocline.c` TU is built as a static C target.
- `core/CMakeLists.txt` adds one source and one test file.
- CLAUDE.md now says that player input changing state must be a CommandBus action.

**Tests** (`core/tests/command_tests.cpp`, 23 cases)
- Tokenizer: quotes, escapes, comments, option syntax, errors.
- `quote_token` round trip; duration, acceleration and number parsing, with error messages.
- `format_value` round trip; binding, defaults, flags and aliases; argument error messages.
- Rest args; did-you-mean; spec validation; help output.
- Queue semantics: queries run immediately, actions wait for apply, and failed actions are logged.
- Log → script → `parse_script` gives identical records, and replaying the script into a fresh
  world gives identical state and log.
- Log snapshot encode/decode; `run_repl` output capture, including stop-on-error with location;
  completion.
- Checked: dev preset (GCC 13 + ASan/UBSan) and clang 18 Debug, both warning-free and green.
  Clang Release and `SIM_LINE_EDITING=OFF` also build. Smoke-tested by piping input, and
  interactively under a pty (Tab completion and history work).

**Research**
- Command pattern for deterministic replay: serialize commands as data, record them by tick/order
  (not wall time), apply them at tick start in arrival order, and never apply I/O mid-tick. Here
  time only moves through actions such as `advance`, so position in the log fixes when each
  command applies.
- Line-editing libraries, with activity checked through git clone:
  - replxx (BSD): last master commit 2021, stale.
  - linenoise (BSD-2): active in 2026, but POSIX-only and without word-level completion helpers.
  - isocline (MIT, daanx): v1.1.0 released 2026-04, pure C in a single TU, Windows + POSIX,
    completion with arbitrary `delete_before` replacement, falls back to plain reading when not a
    TTY. It builds warning-free with default flags; with `-Wall -Wextra` the only warnings are
    unused static functions.
  - Chose isocline.
  - Inline hints are disabled because they assume a candidate extends the typed bytes verbatim,
    which is false for case-insensitive or quoted candidates.

**Deferred**
- Persistent history file.
- The isocline prompt is bbcode-interpreted, so a prompt containing `[` must be escaped.
- Scheduling actions at future sim times (`at 30d buy ...`).
- Per-arg numeric ranges.
- Short `-x` options and subcommands.
- Pre-existing and not touched here: a GCC Release `-Wnull-dereference` error in `table_tests.cpp`
  / `scheduler_tests.cpp`. CI only builds GCC with the dev preset.
