# Task 3 — `system.cas_gc_log`: `srid`→`server_root_id`, `phase_duration_us`→`phase_duration_microseconds`

## Enumeration

`git ls-files | xargs grep -lwE 'srid|phase_duration_us'` minus `docs/superpowers/`, `.superpowers/`,
`contrib/` and the unrelated iceberg spatial-reference-id tests: **46 files**. Classified into
user-facing string/SQL/prose (changed) versus internal C++/Python identifiers and `<srid>`
object-key path placeholders in comments (left).

Every occurrence of the *quoted* literal `"srid"` in `src/` + `programs/` was enumerated first — nine
hits, all of them either the `cas_gc_log` column name or a `CasEvent::detail` map key (plus two gtest
consumers of those keys). `gtest_cas_layout.cpp` uses `srid` inside a namespace *path* literal
(`"srid/store/ab/…"`), not as a key, so it is untouched.

## Deviations (both widening)

1. **`detail["srid"]` in `system.cas_log` was renamed too.** The plan scopes `srid` to the
   `cas_gc_log` column, but the same abbreviation is a user-facing `detail` map key emitted at five
   sites (`CasGc`, `CasServerRoot`, `CasDecommission` ×3). The tree was already inconsistent:
   `CasPool` emits the *same fact* under the key `server_root_id`, and `system.cas_mounts` /
   `SYSTEM CAS DROP POOL MEMBER`'s result set already use `server_root_id`. All five now use
   `server_root_id`; the two gtests asserting `detail.at("srid")` were updated with them, so a revert
   of any emitter fails `CASHeartbeat*` / `CASGcAckFloor*`.

2. **The `phase_metrics` keys `txns_unapplied` / `txns_opened` were renamed** to
   `transactions_unapplied` / `transactions_opened`. `Txns` is on the decided-violations list and these
   are user-facing map keys of this very table (they are also named in the `phase_metrics` column
   description). Consumers updated: two gtests, `utils/ca-soak/soak/{metrics,signals,run}.py` and three
   of its pytest files.

The soak's internal dict aliases `total_us` / `max_us` in `signals.py` are NOT user-facing (they never
leave the Python) and were left; only the column referenced in the SQL changed.

## Also applied

- `clickhouse-disks cas-drop-member` stdout `srid=` → `server_root_id=` and its `--read-only` error text.
  Nothing in the tree parses that output for a `srid=` key (grep for `srid=`: empty).
- `docs/en/operations/system-tables/cas_gc_log.md`: column entry, and the three example queries
  (`phase_duration_us` plus the `p50_us`/`p99_us`/`total_us` aliases → `*_microseconds`).
- `docs/en/sql-reference/statements/system.md` `SYSTEM CAS DROP POOL MEMBER`: the
  "(`server_root_id`, or `srid`)" parenthetical collapsed, the syntax-block placeholder
  `'srid'` → `'server_root_id'`, and the three prose mentions.
- Both column descriptions lost their now-redundant opener ("`server_root_id` of the mount whose…" →
  "Identifies the mount whose…").

## Verification

- `grep -nwE 'srid|phase_duration_us|txns_unapplied|txns_opened'` over
  `src/Interpreters/`, `tests/queries/`, `utils/ca-soak/`, `docs/en/`, `programs/`: no strings, no SQL,
  no docs remain — only internal identifiers (`String srid;`, `report.srid`, the Python parameter
  `srid`) and `<srid>` path placeholders in comments.
- `ninja -C build clickhouse unit_tests_dbms` → `NINJA_EXIT=0` (`build/obscure_t3_build.log`).
- `unit_tests_dbms --gtest_filter='CAS*'` → **2004 tests from 278 suites, PASSED**, `TEST_EXIT=0`
  (`build/obscure_t3_test.log`).
- `python3 -m pytest tests scenarios/tests` in `utils/ca-soak`: **336 passed** (baseline before the
  rename: 290 passed for `tests` alone — same set, re-run after).
