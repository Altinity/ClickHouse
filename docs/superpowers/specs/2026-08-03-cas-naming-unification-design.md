# CAS naming unification — design

Date: 2026-08-03
Branch: cas-gc-rebuild

## Goal

Unify every user-facing spelling of the content-addressed-storage feature name
(`Cas`, `CA`, `content_addressed`, `content-addressed`, `CONTENT ADDRESSED`, …)
into one canonical form, in the least invasive way:

- **`CAS`** in CamelCase identifiers, SQL keywords, grants, and prose.
- **`cas`** in snake_case contexts (system table names, settings, config keys,
  test file names, test tags).
- In docs prose, the first mention may spell it out as
  "content-addressed storage (CAS)"; every later mention uses `CAS`.

## Scope — what is renamed

1. **Metrics (ProfileEvents / CurrentMetrics).** Prefix `Cas*` → `CAS*`
   (`CasBlobGet` → `CASBlobGet`, `CasGcHeadMiss` → `CASGcHeadMiss`, …).
   Where the trailing `Cas` means *compare-and-swap*, rename that part to
   `CompareSwap` to remove the abbreviation collision:
   `CasBlobCas` → `CASBlobCompareSwap`, `CasBlobCasConflict` →
   `CASBlobCompareSwapConflict`, `CasGcCas` → `CASGcCompareSwap`,
   `CasGcCasConflict` → `CASGcCompareSwapConflict`.
2. **SQL commands and grants.** `SYSTEM CONTENT ADDRESSED GC RUN` →
   `SYSTEM CAS GC RUN`; likewise GC REBUILD, GC START/STOP, FSCK,
   DROP POOL MEMBER, FORGET. Access types `CONTENT_ADDRESSED_*` → `CAS_*`.
   No parser aliases for the old syntax — the branch is unreleased.
3. **System tables.** Registered names only (C++ classes stay):
   `content_addressed_log` → `cas_log`,
   `content_addressed_garbage_collection_log` → `cas_garbage_collection_log`,
   `content_addressed_mounts` → `cas_mounts`.
4. **Settings and config.** `content_addressed_blob_upload_pool_size` →
   `cas_blob_upload_pool_size`, `content_addressed_condemned_upload_memory_bytes`
   → `cas_condemned_upload_memory_bytes`; metadata storage type string
   `"content_addressed"` → `"cas"`. (There is no `<content_addressed>` settings
   block in the code — per-disk keys are read flat off the disk element; the
   XML occurrences are disk/policy *names*, renamed with the tests.)
   The old type string IS persisted (table `.sql` ATTACH queries, backup
   metadata). Decision (2026-08-03): NO compat alias anywhere — `"cas"` is the
   only accepted spelling. Pre-rename data (old ATTACH files, backups, soak
   configs) is expected to need a manual edit or recreation.
5. **Tests.** Stateless `*content_addressed*` → `*cas*` (with `.reference`
   files); integration `test_content_addressed_*` → `test_cas_*`; tag
   `no-content-addressed-storage` → `no-cas-storage` (test headers, runner tag
   parsing, praktika job, skill/doc mentions); gtest suite prefix `Cas*` →
   `CAS*` together with the suite-prefix gate generator.
6. **Documentation.** `docs/en/operations/system-tables/content_addressed_*.md`
   → `cas_*.md`; prose variants `Cas` / `CA` / `content_addressed` → `CAS`.
7. **Replication wire protocol.** The part-fetch HTTP parameter/cookie names in
   `DataPartsExchange.cpp` (`content_addressed_pool_uuid`,
   `content_addressed_relink`, `content_addressed_confirm`,
   `content_addressed_source_token`, `content_addressed_confirm_answer`) →
   `cas_*`. This is external API too; the branch is unreleased, so no
   compatibility fallback. Mixed-build clusters (soak) must be fully rebuilt —
   a version-skewed fetch silently degrades to the non-CAS full-copy path.

## Out of scope

- C++ class, file, and namespace names (`ContentAddressedLog`,
  `StorageSystemContentAddressedMounts`, …) stay as they are.
- Internal comments, except where they quote a renamed user-facing string.
- TLA+ models and internal `docs/superpowers/` artifacts (worklogs, reports,
  historical specs).

## Execution approach

Surface-by-surface commits, one commit per surface in the order listed above.
Each commit also updates every `.reference` file and doc that embeds the
renamed strings (metric introspection outputs, `show_privileges`, table names
in query outputs).

Chosen over a single mechanical sweep because `cas` is a substring of `cast`,
`case`, `cascade`, `replicas`, and old names are embedded in reference
outputs — small reviewable steps with per-step checks are the least risky
option. A compatibility/alias layer was rejected: nothing released depends on
the old names.

## Verification (deliberately minimal)

- **Build** after the changes.
- **Unit tests only** as the test run; no stateless/integration runs in this
  effort.
- **Aggressive grep discipline** as the primary control: before each surface,
  inventory occurrences with strict patterns (excluding
  `cast|case|cascad|replicas`); after each surface, re-run the sweep and
  manually re-check every remaining suspicious hit. Final gate: zero remaining
  non-canonical variants across user-facing surfaces.

## Risks

- Old names embedded in many `.reference` files; they must change in the same
  commit as the code, and correctness is checked by grep review rather than by
  running the stateless suite.
- Grep false positives/negatives around `cast`/`case`/`cascade`/`replicas`.
- Possible persistence of the metadata storage type string (see Scope §4).
- Tag rename must stay in sync across tests, runner, CI job, and skills.
