# Task 6 report — integration tests `test_content_addressed_*` -> `test_cas_*`

## Step 1 — directories and the missing `__init__.py`
Five dirs renamed via `git mv`: `drop_pool_member`, `gc_s3`, `ref_snaplog`, `s3`, `shared_pool` — matching
the plan exactly. `test_cas_ref_snaplog` was indeed the only sibling without `__init__.py`; created and added.
All ten `test_cas_*` dirs now have one.

## Step 2 — order mattered, and it was followed
**The legacy-key DELETION ran first**, before any renaming sed, so the keys were removed rather than renamed:
`<content_addressed_allow_shared_pool>` / `<content_addressed_gc_grace_sec>` deleted from **8** tracked
`storage_conf.xml` files. Zero remain anywhere tracked. This closes the Task-3 dependency: those configs would
otherwise have made the server refuse to start, since Task 3 removed both keys from `non_cas_keys` and an
unrecognised key reaches `impl->set`, which throws.

`test_cas_replicated_relink/configs/storage_conf-preprocessed.xml` was untracked (stale artifact) and removed,
as the plan directs.

The one prose comment naming a key mid-sentence
(`test_cas_replicated_relink/configs/storage_conf_other_pool.xml`) was reworded by hand — the clause naming
the now-rejected key was dropped, keeping the durable reason ("only one server mounts it").

Then the seds, gc-log table first, across **28** files.

### Wire-protocol assertions match Task 3
`test_cas_replicated_relink/test.py` asserts `cas_pool_uuid`, `cas_relink=part_manifest_v2`,
`cas_source_token` — the literals set in `DataPartsExchange.cpp`. `cas_confirm` / `cas_confirm_answer` are
defined in the source but were never asserted by this test, at HEAD either, so nothing was lost in the rename.
All 13 `<metadata_type>` values across the integration configs are now `cas`.

## Step 3 — verification
- `grep -rn 'content_addressed' tests/integration/ --include='*.py' --include='*.xml'` (minus `_instances`):
  **no output**.
- All 20 `test_cas_*/test.py` and `__init__.py` parse (`ast.parse`). `python3 -m py_compile` could not be used
  as the plan writes it: the pre-existing `__pycache__` dirs are root-owned from earlier container runs and
  raise `Permission denied` on write. Parsing avoids emitting bytecode and checks the same syntax property.
- Every XML under `tests/integration/test_cas_*/configs/` and `utils/ca-soak/configs/` parses.

## Two Task-3 defects found here and fixed (outside this task's nominal scope)
Both are fallout from Task 3's sweeps that only surfaced when auditing config keys and table names as a set.

1. **`utils/ca-soak/configs/ca_gc_log.xml` declared a log section that does not exist.** Task 3's ca-soak
   config sweep used the naive `s/content_addressed/cas/g`, without the "gc-log table first" rule that its own
   Step 3 applied to `src/`. The result was `<cas_garbage_collection_log>` with `<table>cas_garbage_collection_log</table>`,
   but the section name is derived from the `SystemLog.h` macro member, which is `cas_gc_log`. The soak's GC
   log would simply not have been configured. Corrected; the section and table names in every ca-soak config
   now match the `SystemLog.h` members exactly (`cas_log`, `cas_gc_log`), checked by comparing the two sets.

2. **51 stale `system.content_addressed_*` queries in live soak tooling.** Task 3 swept `src/` and the ca-soak
   *configs*, but not the ca-soak scripts/scenarios, so 10 live files still queried
   `system.content_addressed_log` / `_garbage_collection_log` / `_mounts` — tables that no longer exist.
   Fixed in all 10. `utils/ca-soak/scenarios/BACKLOG.md` keeps the old names: dated history, per the ruling
   recorded in `t1-report.md`.

Also corrected two comments in `utils/ca-soak/soak/{cluster,checker}.py` that stated
`content_addressed_gc_grace_sec` "is inert (not read by the core)". That was true while the key was
skip-listed and is now false — setting it makes the server refuse to start. Per the comment policy the
citation was deleted and the durable reason kept ("there is NO core retire-grace throttle").

After the fixes: 96 ca-soak `.py` files parse, the three touched shell scripts pass `bash -n`.

## Not run
No integration execution — the spec excludes it from this effort. What this task *can* claim is that the
configs no longer carry keys the server rejects and the tests no longer name tables that do not exist; it
cannot claim the suites pass, and does not.

## Noted for Task 8 (lead ruling)
`ci/praktika/settings.py:87`, `ci/praktika/native_jobs.py:227`, `ci/defs/altinity_jobs.py:65` use
"content-addressed" in the GENERIC sense (CI-cache content-addressed hashing), unrelated to the CAS feature.
Reviewed and deliberately kept.
