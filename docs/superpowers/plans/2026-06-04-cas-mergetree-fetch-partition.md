# CAS MergeTree FETCH PARTITION/PART Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) tracking. Build to a log (`ninja -C build <target> > build/<log> 2>&1`, NO `-j`/`nproc`); summarize. Bounded foreground tests (`timeout` ≤ 900), non-empty `--test`, never `clickhouse local`.

**Goal:** Support `ALTER TABLE … FETCH PARTITION/PART … FROM '<zk_path>'` on a content-addressed (CA) disk by lifting the gate and landing the byte-fetched part in the CA `detached/` namespace.

**Architecture:** FETCH is a `ReplicatedMergeTree` op (now supported on CA). Lift the `checkAlterPartitionIsPossible` gate; force the byte-fetch path for `to_detached` (don't advertise the relink capability when fetching into detached); ensure the fresh detached-staging writes content-address + commit a usable `detached` ref and the `tmp-fetch_<part>` → `<part>` rename re-keys. Relink-into-detached is deferred.

**Spec:** `docs/superpowers/specs/2026-06-04-cas-mergetree-fetch-partition-design.md`.
**Branch:** `cas-mergetree-poc` (never master). Trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Seams (verified):**
- Gate: `src/Storages/MergeTree/MergeTreeData.cpp:~6578` — the `const static auto supported_commands = {DROP_PARTITION, DROP_DETACHED_PARTITION, ATTACH_PARTITION, REPLACE_PARTITION, MOVE_PARTITION};` in the `MetadataStorageType::ContentAddressed` branch of `checkAlterPartitionIsPossible`, with the throw just below it. `PartitionCommand::FETCH_PARTITION` is the enum (`src/Storages/PartitionCommands.h:29`).
- Relink-capability advertise: `src/Storages/MergeTree/DataPartsExchange.cpp` `Fetcher::fetchSelectedPart` — the receiver adds `CA_POOL_UUID_PARAM` (the relink capability) to the request URI, guarded by `if (try_zero_copy)` (~line 512-535). The relink-cookie receive branch is ~line 700-725 (`relinkPartToDisk`). `to_detached` is a `fetchSelectedPart` parameter; the staging path is `getRelativeDataPath() + DETACHED_DIR_NAME` (~line 917).
- Detached CA machinery (templates for Phase 2): `ContentAddressedTransaction::republishCommittedPartIntoDetached` (publish a `detached` ref) and `rekeyDetachedPartDir` (re-key a detached part dir, e.g. `attaching_<part>` staging).

---

## Phase 1 — gate lift + force byte-fetch for `to_detached`; empirical probe

### Task 1: lift the FETCH gate and disable relink for `to_detached`

**Files:** Modify `src/Storages/MergeTree/MergeTreeData.cpp`; Modify `src/Storages/MergeTree/DataPartsExchange.cpp`

- [ ] **Step 1: add `FETCH_PARTITION` to the CA `supported_commands`.** In `checkAlterPartitionIsPossible`'s ContentAddressed branch, change the set to include FETCH and update the comment:

```cpp
                    const static auto supported_commands = {
                        PartitionCommand::DROP_PARTITION,
                        PartitionCommand::DROP_DETACHED_PARTITION,
                        PartitionCommand::ATTACH_PARTITION,
                        PartitionCommand::REPLACE_PARTITION,
                        PartitionCommand::MOVE_PARTITION,
                        PartitionCommand::FETCH_PARTITION,
                    };
```
Update the adjacent comment: FETCH is now SUPPORTED — it is a ReplicatedMergeTree op (supported on CA), and on CA a `to_detached` fetch takes the byte-fetch path (the downloaded files content-address into the `detached/` namespace; relink-into-detached is deferred). FREEZE/UNFREEZE stay gated (B4). (FETCH PART, if it reaches this check, is also `FETCH_PARTITION`-classed; confirm `command.type` for `ALTER … FETCH PART` — if it is a distinct value, add it too.)

- [ ] **Step 2: do NOT advertise the relink capability for a `to_detached` fetch.** In `Fetcher::fetchSelectedPart`, find the `if (try_zero_copy)` block that adds `CA_POOL_UUID_PARAM` to the request URI and tighten the guard to `if (try_zero_copy && !to_detached)`. Rationale: the relink path (`relinkPartToDisk`) stages at the ACTIVE path and ignores `to_detached`; not advertising the capability for a detached fetch makes the sender stream bytes (which `downloadPartToDisk` writes into `detached/`). Add a one-line comment. (The active-fetch relink path — replication — is unchanged because `to_detached=false` there.)

- [ ] **Step 3: build the server** `ninja -C build clickhouse > build/fetch_t1_build.log 2>&1; echo $?; grep -cE "error:|FAILED:" build/fetch_t1_build.log` → 0 errors. (Summarize via a subagent.)

- [ ] **Step 4: commit** `git add src/Storages/MergeTree/MergeTreeData.cpp src/Storages/MergeTree/DataPartsExchange.cpp && git commit -m "CAS FETCH: lift the gate + force byte-fetch for to_detached" …trailer`

### Task 2: empirical probe — un-gate the 2 FETCH tests and run

**Files:** Modify `tests/queries/0_stateless/01650_fetch_patition_with_macro_in_zk_path_long.sql`, `…/03350_alter_table_fetch_partition_thread_pool.sql`

- [ ] **Step 1: un-tag both** (remove `no-content-addressed-storage` + any reason comment; preserve other tags).
- [ ] **Step 2: run on CA-default**
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
sel="01650_fetch_patition_with_macro_in_zk_path_long 03350_alter_table_fetch_partition_thread_pool"
[ -n "$(echo "$sel"|tr -d ' ')" ] || { echo ABORT; exit 1; }
timeout 900 python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" --test "$sel" > build/fetch_t2_run.log 2>&1
echo "exit $?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+, Skipped: [0-9]+" build/fetch_t2_run.log | tail -1
grep -A15 -E "01650|03350" ci/tmp/test_result.txt | head -50
```
- [ ] **Step 3: classify.** If BOTH pass → the detached landing already works; commit the un-gate and SKIP to Phase 3. If either FAILS, read the error and record it (this drives Task 3): a `no ref`/`FILE_DOESNT_EXIST` on a `detached/…tmp-fetch…` path → the fresh-detached-write commit or the staging rename is the gap (Task 3a/3b); a different error (e.g. the test needs a topology the stateless server lacks) → triage as orthogonal. Do NOT commit a failing un-gate yet.

---

## Phase 2 — fix the detached landing (only if Task 2 showed a gap)

### Task 3: make a fresh byte-fetched part land usably in the CA `detached/` namespace

**Files:** Modify `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` (and/or `ContentAddressedMetadataStorage.cpp`); Test: a new oracle (below).

This task is reproduction-driven — its exact shape depends on Task 2's failure. The two known sub-gaps and their fixes:

- [ ] **Step 1 (3a): fresh detached-staging write commits a `detached` ref.** A byte-fetch writes fresh files into `detached/tmp-fetch_<part>/<file>` through the CA whole-part transaction. Confirm (via the Task-2 failure + reading `ContentAddressedTransaction::commit`) whether the commit publishes a `detached` ref for these in-transaction `recorded` blobs (keyed `tmp-fetch_<part>/<file>` under the shared `detached` ref). DETACH publishes its detached ref by re-keying a SOURCE manifest (`republishCommittedPartIntoDetached`); a fresh fetch has the files in the transaction's `recorded` map instead. If commit does not already handle a `part_name == detached` target by folding `recorded` into the shared `detached` ref, add that path: on commit, when the part being written is in the detached namespace, build/merge the `detached` ref's manifest from `recorded` (keys already `<staging>/<file>`) + sidecar, mirroring `republishCommittedPartIntoDetached`'s ref/sidecar publish but sourcing from `recorded`/`recorded_mutable` rather than a source manifest. Build, re-run the failing test.

- [ ] **Step 2 (3b): the `detached/tmp-fetch_<part>` → `detached/<part>` rename re-keys.** `fetchPartition` renames the staging dir to the final detached name → CA `moveDirectory(from=detached/tmp-fetch_<part>, to=detached/<part>)`. Confirm `rekeyDetachedPartDir` is invoked for this detached→detached re-key and that it handles the `tmp-fetch_` staging prefix (it already re-keys `attaching_<part>`). If the `moveDirectory` dispatch doesn't route a `tmp-fetch_`-staging detached→detached rename to `rekeyDetachedPartDir`, add/extend the branch (same operation: re-key `<old_dir>/`→`<new_dir>/` in the shared detached ref's manifest + sidecar, update the per-file mutable keys). Build, re-run.

- [ ] **Step 3: add the inline-CA oracle.** Create `tests/queries/0_stateless/<NNNNN>_content_addressed_fetch_partition.sql` (via `add-test`). A Replicated CA table; insert; `ALTER TABLE … FETCH PARTITION … FROM` its own zk path into `detached`; assert `system.detached_parts` shows the fetched part; `ATTACH PARTITION`; `SELECT` reads the data back equal to the source. Mirror an existing FETCH stateless test for the inline-CA-disk + zk-path setup (use `disk = disk(type=object_storage, object_storage_type=local, metadata_type=content_addressed, …, content_addressed_allow_shared_pool=1)`). Hand-verify the reference. Run on CA-default → `Passed: 1`.

- [ ] **Step 4: re-run the 2 un-gated tests** on CA-default → both pass. Build/gtests: `build/src/unit_tests_dbms --gtest_filter='ContentAddressed*'` all pass (if you changed CA transaction code).

- [ ] **Step 5: commit** the fix + the oracle. Subject: `CAS FETCH: land a byte-fetched part in the CA detached namespace`.

---

## Phase 3 — regression, finalize, push

### Task 4: non-CA regression + finalize un-gate + backlog

- [ ] **Step 1: non-CA regression** — run 2 normal FETCH PARTITION tests on the PLAIN job to confirm no regression from the `!to_detached` advertise gate + the gate-set change:
```bash
timeout 900 python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test "01650_fetch_patition_with_macro_in_zk_path_long 03350_alter_table_fetch_partition_thread_pool" > build/fetch_t4_run.log 2>&1
echo "exit $?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+" build/fetch_t4_run.log | tail -1
```
Expected: both pass on plain (no regression).

- [ ] **Step 2: confirm the 2 tests are un-gated + passing on CA** (final state): both `[ OK ]` on the CA-default job; no `no-content-addressed-storage` tag remains on them. If either still fails for an orthogonal reason (e.g. a topology the stateless server can't provide), re-gate THAT one with a precise reason + note it (do not leave it failing un-gated).

- [ ] **Step 3: backlog** — update `docs/superpowers/deferred_backlog/cas-mergetree-integration.md`: FETCH PARTITION/PART now supported on CA (byte-fetch into detached); the FETCH entries removed from the gated set; **relink-into-detached deferred** as a new backlog item (the same-pool optimization: extend `relinkPartToDisk`/`relinkExistingPart` to publish into the detached namespace so a same-pool FETCH moves no bytes) with the plug-in point.

- [ ] **Step 4: commit + push** `git push filimonov cas-mergetree-poc`.

---

## Done criteria
- `01650`/`03350` un-gated and passing on the CA-default job (or one re-gated for a documented orthogonal reason, not a CA-FETCH bug).
- A FETCH PARTITION FROM into `detached` on an inline CA disk lands a usable part (the oracle: ATTACH + SELECT read back correct data).
- No non-CA regression (the `!to_detached` advertise gate only affects the CA relink branch).
- Backlog: FETCH supported; relink-into-detached deferred with a plug-in point.
