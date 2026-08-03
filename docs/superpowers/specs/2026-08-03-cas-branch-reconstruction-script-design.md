# CAS branch reconstruction script — design

**Status:** APPROVED (2026-08-03). **Branch:** `cas-gc-rebuild`.
**Deliverable:** `utils/cas-carve/carve.sh` — a rerunnable script that rebuilds the essential
fragment of the CAS branch as a fresh, logically-ordered commit series on top of
`altinity/antalya-26.6`, discarding everything inessential.
**Companions:** `docs/superpowers/cas/upstream.md` (thematic fix split),
`docs/superpowers/cas/upstream-patch-inventory.md` (hunk-by-hunk upstream classification),
`docs/superpowers/specs/2026-07-28-cas-merge-layout-preparation-design.md` (§7 layer order, §8
mechanics — this script implements §8 step 2–3 mechanically).

---

## 1. Goal and non-goals

**Goal.** From the current working branch (3000+ commits, ~1516 files, +567k lines against
merge-base `4359a07f088`), produce a new branch containing only the feature: upstream fixes,
the CAS subsystem, its integration, tests, CI wiring, and user-facing docs — as a readable
series of grouped commits. The script will be rerun often as the working branch evolves, so it
must be cheap to update and must fail loudly rather than silently drop new files.

**Non-goals.**

- Per-commit buildability. Commits are a logical grouping for reading and review, not bisect
  points. Whole files are taken as-is from the source ref; a file whose main theme is an
  upstream fix may carry CAS integration hunks — the commit message says so. The only hard
  guarantee is about the **tip**: it equals the source tree minus the excluded junk.
- Hunk-level splitting of mixed files.
- Rewriting or preserving the existing history.
- Pushing, PR creation, or building.

## 2. Interface

```bash
utils/cas-carve/carve.sh [--src <ref>] [--base <ref>] [--branch <name>] [--check] [--force]
```

- `--src` — source ref to carve from (default `HEAD`).
- `--base` — base ref for the new branch (default `altinity/antalya-26.6`).
- `--branch` — name of the branch to create (default `cas-carved`).
- `--check` — validate the manifest against the current diff (coverage, unknown files, empty
  groups) and print the classification report; create nothing.
- `--force` — delete and recreate the target branch if it exists; without it an existing
  branch is a hard error.

The script itself lives only on the working branch; it is in its own exclude list and never
appears on the carved branch.

## 3. Mechanics

1. Compute `MB = git merge-base $BASE $SRC` and the full file list
   `git diff --name-status $MB..$SRC`.
2. Classify every file: first match wins, in order — EXCLUDES first, then the ordered GROUPS
   (each a list of pathspecs: directory globs preferred, explicit files where necessary).
   Any file matching nothing → **hard fail** with the full unmatched list. A group that
   matches zero files → warning (likely manifest rot).
3. Create a **temporary worktree** (`tmp/carve-worktree`) on the new branch at `$MB`. The
   primary checkout is shared with live sessions and is never switched.
4. For each group, in manifest order: for each of its files, `git checkout $SRC -- <file>`
   (or `git rm -q <file>` when the file is deleted in `$SRC` — status `D` from step 1), then
   `git commit -F <message>`. Empty groups are skipped.
5. **Completeness invariant** (the point of the exercise):
   `git diff <branch>..$SRC --name-only` restricted by `':!<exclude>'` pathspecs **must be
   empty**; otherwise fail with the list of lost files. The unrestricted diff is printed as
   the "discarded" report, summarized per exclude category.
6. Print the summary table: commit sha → group name → file count; then the discard summary.
7. Remove the temporary worktree; the branch stays.

## 4. Data model

Data at the top of the script, engine below; routine regeneration touches only the data.

- **EXCLUDES** — pathspec prefixes of the inessential: `docs/superpowers/`, `.superpowers/`,
  `utils/ca-soak/`, `utils/cas-gate/`, `utils/cas-carve/`, `tmp/`, `.gitignore`,
  `tests/broken_tests.yaml`.
- **GROUPS** — ordered records: `name | commit message (heredoc) | pathspec list`.

Directory globs (e.g. `src/.../ContentAddressed/Gc/**`, `tests/integration/test_cas_*/**`)
absorb new files automatically as the branch grows; anything genuinely new outside known
territory trips the unmatched-file failure and forces a deliberate manifest decision.

One classification needs `--name-status`, not just globs: within `tests/queries/0_stateless/`,
**added** files are new CAS tests (phase 4) while **modified** files are tag edits to old
tests (phase 5). The driver supports an `added-only` / `modified-only` flag on a pathspec for
this case.

## 5. Commit order (the manifest)

### Phase 1 — upstream fixes (~10 commits, per `upstream.md`)

Whole files, thematic grouping; mixed files go where their main theme is, with a
"includes CAS integration hunks, wired later" note in the message.

1. `ReadBufferFromFileView` position fix (B115) + gtest (+ `ReadBufferFromMemory` and its
   gtest).
2. `ReadBufferFromS3` retry-cancel after `KILL QUERY` (B117).
3. `ThreadGroup` parent-lifetime fix (B90): `ThreadStatusExt.cpp`, `ThreadStatus.h`.
4. `MergeTreeDeduplicationLog` null-writer fail-closed (B37) + gtest.
5. S3 conditional-write core: 412 no-retry policy + `isPreconditionFailed`, `Client.*`,
   `S3Common.*`, `WriteBufferFromS3.*`, `copyS3File.*` (also carries the
   `message_format_string` fix), `Requests.h`, `S3ObjectStorage.*`, `diskSettings.cpp`,
   `S3Defines.h`, `S3AuthSettings.cpp`, `gtest_aws_s3_client.cpp`, `gtest_writebuffer_s3.cpp`.
6. `Expect: 100-continue`: `PocoHTTPClient.*`, `PocoHTTPClientFactory.cpp`.
7. GCS conditional-write support: `GOOG4Signer.*`, `GCSConditionalDialect.*` + gtests.
8. `LocalObjectStorage` robustness: TOCTOU vanished-entry handling, snapshot-semantics
   listing, NUL-byte guard.
9. Disk-transaction contract (workstream A3): `IDiskTransaction.h`,
   `DiskObjectStorageTransaction.*`, `DataPartStorageOnDiskFull.cpp`,
   `DataPartStorageOnDiskBase.*`, `IDataPartStorage.h`, `IMergeTreeDataPart.cpp`,
   `MergeTask.*`, `MutateTask.cpp`, `MergeProjectionPartsTask.cpp`,
   `MergeTreeDataWriter.cpp`, `MergeTreeData.*`, `gtest_projection_borrowed_transaction.cpp`.
   This is the most mixed group — the message states explicitly that these files also carry
   the CA capability/read-your-writes surface.
10. SYSTEM-on-proxy fix (`unwrapTableProxy`): `StorageProxy.h`, `StorageTableProxy.h`.

### Phase 2 — CAS subsystem, bottom-up (spec §7; one commit per layer)

`Primitives` → `Formats` → `Backend` → `Pool` → `Parts` → `Gc` → `Tools` → `benchmarks`,
each as `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/<Layer>/**` plus the
top-level `ContentAddressed/*.{h,cpp}` files with the final layer they belong to (the
metadata storage / transaction / exchange files go to phase 3 instead).

### Phase 3 — integration and wiring (~4 commits)

1. Disk surface: `ContentAddressedMetadataStorage.*`, `ContentAddressedTransaction.*` (and
   the rest of the top-level `ContentAddressed/` glue), `MetadataStorageFactory.cpp` (the
   registration line), `RegisterDiskObjectStorage.cpp`, `DiskObjectStorage.*`,
   `DiskObjectStorageCache.cpp`, `MetadataStorageFromCacheObjectStorage.*`,
   `IMetadataStorage.h`, `IObjectStorage.h`, `DiskType.*`, `IDisk.h`,
   `ReadOnlyDiskWrapper.h`, `ReadPipeline.*`, `WriteBufferFromFileBase.h`,
   `WriteBufferFromFileDecorator.h`, `WriteSettings.h`, `StorageMergeTree.cpp`,
   `StorageReplicatedMergeTree.cpp`, `VersionMetadataOnDisk.cpp`.
2. System logs and introspection: `ContentAddressedLog.*`,
   `ContentAddressedGarbageCollectionLog.*`, `SystemLog.*`, `SystemLogBase.*`, `Context.*`,
   `ServerAsynchronousMetrics.cpp`, `src/Storages/System/**` (CA system tables).
3. SYSTEM commands: `ASTSystemQuery.*`, `ParserSystemQuery.cpp`, `gtest_Parser.cpp`,
   `InterpreterSystemQuery.*`.
4. Registries and entry points: `ProfileEvents.cpp`, `CurrentMetrics.cpp`, `AccessType.h`,
   `FailPoint.cpp`, `ServerSettings.cpp`, `setThreadName.h`, `src/CMakeLists.txt`,
   `contrib/**` (if any), `DataPartsExchange.*` (fetch-by-relink),
   `programs/disks/**`, `programs/server/Server.cpp`, `programs/server/config.xml`,
   `programs/local/LocalServer.cpp`.

### Phase 4 — tests (3 commits)

1. Unit: `src/Disks/tests/**`.
2. Stateless: `tests/queries/0_stateless/**` — **added-only**.
3. Integration: `tests/integration/test_cas_*/**`, `tests/integration/test_content_addressed_*/**`,
   `tests/integration/test_disks_app_func/**`, `tests/integration/helpers/**`,
   `tests/integration/compose/**`.

### Phase 5 — CI/CD and old-test tags (1–2 commits)

`ci/**`, `.github/**`, `tests/config/**`, `tests/clickhouse-test`,
`tests/queries/0_stateless/**` — **modified-only** (tag edits to pre-existing tests).

### Phase 6 — documentation (1 commit)

`docs/en/operations/storing-data.md`, `docs/en/sql-reference/statements/system.md`,
`docs/en/operations/system-tables/content_addressed_*.md`.

## 6. Excluded by decision

Recorded here so the exclusions are deliberate, not accidental: all of `docs/superpowers/`
(worklogs, models, reports, specs — including this one), `.superpowers/`, `utils/ca-soak/`
and its scenarios, `utils/cas-gate/` (gate infrastructure, not the feature),
`utils/cas-carve/` (this script), `tmp/`, `.gitignore` (artifact entries),
`tests/broken_tests.yaml`.

## 7. Verification

- `--check` mode is the fast loop while maintaining the manifest.
- A full run must end with the completeness invariant holding (§3.5); the script's exit code
  is the gate.
- Spot-check after each regeneration: `git diff <branch>..$SRC --stat` should show only
  excluded paths.

## 8. Risks

- **Manifest rot as the branch evolves.** Mitigated by the unmatched-file hard fail, the
  empty-group warning, and directory globs doing most of the matching.
- **A junk file lands inside an included glob** (e.g. a stray artifact committed under
  `src/`). The classifier cannot tell; hygiene stays a review concern on the working branch.
- **File moves between areas** (e.g. a gtest moved into a layer directory) silently change
  which commit carries them. Acceptable: grouping is presentational.
