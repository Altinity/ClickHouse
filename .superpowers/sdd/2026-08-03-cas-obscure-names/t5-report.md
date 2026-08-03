# Task 5 — per-disk settings and test names: `dedup_*`, `gc_snap_*`

## Enumeration

`git ls-files | xargs grep -l 'dedup_cache_bytes|dedup_head_first_min_bytes|gc_snap_generations_to_keep'`
minus `docs/superpowers/`, `.superpowers/`, `contrib/`: **29 files** — the settings declaration and its
consumers in `src/`, seven gtests, `docs/en/operations/storing-data.md`, one tracked stand config
(`tmp/test_stand_ca_storage.xml`), and eleven `utils/ca-soak` configs / compose files / cards / docs.
All 29 renamed.

The plan also names `tests/config/config.d/cas*_storage_policy_*.xml` and
`tests/integration/test_cas_*/configs/*.xml` as expected sites — **neither sets any of the three keys**,
so there was nothing to change there.

## Applied

- `deduplication_cache_bytes`, `deduplication_head_first_min_bytes`, `gc_snapshot_generations_to_keep`
  (config key and C++ symbol share the spelling, so one sed covers both). No compat alias.
- Test renames via `git mv`, both extensions each:
  `04285_cas_dedup_window_inline_disk` → `04285_cas_deduplication_window_inline_disk`,
  `05006_cas_dedup_blob_insert` → `05006_cas_deduplication_blob_insert`,
  `05008_cas_gc_snap_prune` → `05008_cas_gc_snapshot_prune`.
  Nothing anywhere else in the tree referenced the old basenames.
- Inline identifiers inside those tests: tables `t_ca_dedup_blob` → `t_cas_deduplicated_blob`,
  `t_cas_dedup` → `t_cas_deduplication`; the inline disk name / pool path `04285_cas_dedup*` →
  `04285_cas_deduplication*`; the `05008` disk `05008_ca_gc_snap_prune` →
  `05008_cas_gc_snapshot_prune`; and the "dedup"/"deduped" prose in their comments. No `.reference`
  file needed changing (none of them prints a name).

## Extra finding fixed in place: `storing-data.md` described the threshold backwards

The line being edited for the abbreviation also stated the setting's behaviour inversely: "Minimum
object size **below which** dedup reads the whole head of the object first". The gate in
`CasPartWriteTxn` is `source.size >= cfg.deduplication_head_first_min_bytes`, i.e. the `HEAD` is sent
at or *above* the threshold, and it is a `HEAD` request, not a read of the object's head. Since the
sentence was being rewritten anyway, it now reads: "Minimum blob size at which a `HEAD` is sent before
the body, so that an upload of already-present content can be skipped. `0` disables it."

## Verification

- `git ls-files | xargs grep -nw` for the three old keys: only one hit, in a dated
  `.superpowers/sdd/…/t6b-report.md` run record (excluded by grep hygiene).
- `ninja -C build clickhouse unit_tests_dbms` → `NINJA_EXIT=0` (`build/obscure_t5_build.log`).
- `unit_tests_dbms --gtest_filter='CAS*Settings*:CAS*Dedup*:CAS*Deduplication*:CAS*Snap*'` →
  **134 tests from 41 suites, PASSED**, `TEST_EXIT=0` (`build/obscure_t5_test.log`).
- `python3 -m pytest tests scenarios/tests` in `utils/ca-soak`: **336 passed** (the ca-soak configs and
  `test_render_tuned_config.py` are in the changed set).

## Survivors (deliberate, internal-only)

- File names still carrying the abbreviation: `src/Disks/tests/gtest_ca_dedup_cache.cpp`,
  `utils/ca-soak/docker-compose-small_dedup_cache.yml`,
  `utils/ca-soak/configs/storage_conf_small_dedup_cache_ch{1,2}.xml`. The plan lists the last three as
  *modify*, not rename; none is a user-facing name.
