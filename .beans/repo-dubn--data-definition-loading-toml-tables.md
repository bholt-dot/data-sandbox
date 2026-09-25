---
# repo-dubn
title: Data definition loading (TOML -> tables)
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:14:26Z
parent: repo-zclh
---

Bodies, stations, commodities, ship classes defined in data/.

## Summary of Changes

Generic data-definition layer in `core/` (namespace `sim`), no game concepts.

- `simcore/data_node.hpp` + `src/data_node.cpp`: `SourceLoc`, `Diagnostic(s)` (compiler-style `file:line:col: error: ...`, sorted by load order/line/col), `DataNode` (parser-agnostic view; toml++ is included only in `src/data_node.cpp`, never in public headers), OSA edit distance + `closest_match` / `did_you_mean`.
- `simcore/defs.hpp` + `src/defs.cpp`: `DefId<Row>` (= `Handle<Row>`), `Schema<Row>` field descriptors (`field` / `optional` / `optional(..., default)`, `.min/.max/.range/.non_empty/.check`, row-level `check`), `ValueCodec<T>` extension point (bool, integers with fit check, finite floats, string, enums via ADL `enum_names(E)`, definition refs, `std::vector`, `std::optional`, nested `describe()` structs and arrays of tables), `DefTable<Row>` (key -> id, key(id), source(id), key-ordered iteration), `DefRegistry` (`define<Row>(section)`, `load(sources)`, `load_directory(dir)`), `read_data_directory` (recursive, sorted by relative path).
- TOML layout: one `[section.key]` table per definition. Loading is two-pass (read all rows, then assign ids in key order and resolve references), collects all errors, rejects unknown sections/fields with did-you-mean, duplicate keys across files and invalid keys; atomic (a failed load keeps previous contents). Ids depend only on the key set, not on file layout/order.
- Mod-override hook: documented at the duplicate check in `DefRegistry::load`.
- toml++ v3.4.0 pinned in `cmake/Dependencies.cmake` (SYSTEM), linked PRIVATE to simcore; `TOML_EXCEPTIONS=0`. Warning-free under GCC 13 (dev) and Clang 18.
- Tests: `core/tests/defs_tests.cpp` (happy path with example data, missing field, wrong type, integer fit, out of range, unknown keys + suggestions, enums, unresolved refs, duplicates, syntax errors, row checks, atomic reload, directory order, deterministic ids, edit distance).

Research checked: toml++ `parse_result` / `parse_error` / `source_region` (1-based line/col, path stored per node; the parser stops at the first syntax error, so syntax errors are per file); serde `deny_unknown_fields` + did-you-mean practice (small edit-distance threshold); rustc `find_best_match_for_name` (limit len/3, case-insensitive) -> made suggestions case-insensitive; RimWorld `DirectXmlCrossRefLoader` (cross-refs resolved after all defs load) and Factorio data stages -> confirms two-pass resolution and a later override/patch stage (left as a hook).

Deferred: mod override/patch layers; per-element range validation for arrays; unicode-aware columns (toml++ counts codepoints). Note: pre-existing GCC Release `-Wnull-dereference` failures in table_tests/scheduler_tests (not from this change).
