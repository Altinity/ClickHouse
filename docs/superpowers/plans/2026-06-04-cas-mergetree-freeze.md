# CAS MergeTree FREEZE / UNFREEZE Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) tracking. Build to a log (`ninja -C build clickhouse > build/<log> 2>&1`, NO `-j`/`nproc`); summarize via a subagent. Bounded foreground tests (`timeout` ≤ 900), non-empty `--test`, never `clickhouse local`.

**Goal:** Support `ALTER TABLE … FREEZE [PARTITION]` and `… UNFREEZE [PARTITION]` on a content-addressed (CA) disk by publishing each frozen part as its own ref in a GC-rooted `shadow/` namespace that shares the live part's blobs zero-copy.

**Architecture:** A CA freeze already runs through the whole-part clone transaction (`DataPartStorageOnDiskBase::freeze`, B21). Three gaps remain: the freeze ref currently collides with the live ref (`parsePartFilePath` ignores the `shadow/<backup>/` prefix); the frozen blobs are not GC roots (the scan only walks `store/`); and the `shadow/` path is not routed for read/list/remove. Fix by (1) parsing the `shadow/<backup>/` prefix into a `backup_name`, (2) redirecting the commit's ref/sidecar to a `shadow/<backup>/<server>/<uuid>/refs/<part>` namespace, (3) adding `shadow/` to the GC roots, (4) routing `shadow/` reads/lists/removes to the frozen ref-set.

**Spec:** `docs/superpowers/specs/2026-06-04-cas-mergetree-freeze-design.md`.
**Branch:** `cas-mergetree-poc` (never master). Trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Seams (verified):**
- Gate: `MergeTreeData.cpp:~6583` — `supported_commands` in the `MetadataStorageType::ContentAddressed` branch of `checkAlterPartitionIsPossible` (currently DROP/DROP_DETACHED/ATTACH/REPLACE/MOVE/FETCH). The four FREEZE enum values are `PartitionCommand::{FREEZE_PARTITION, FREEZE_ALL_PARTITIONS, UNFREEZE_PARTITION, UNFREEZE_ALL_PARTITIONS}` (`PartitionCommands.h:30-33`; dispatched at `MergeTreeData.cpp:6999-7024`).
- Freeze write path: `MergeTreeData::freezePartitionsByMatcher` (`MergeTreeData.cpp:9654`) → `data_part_storage->freeze(shadow/<backup>/<relative_data_path>, <part>, …)`. On CA `relative_data_path` is `store/<uuid[:3]>/<uuid>/`, so the disk-relative target is `shadow/<backup>/store/<uuid[:3]>/<uuid>/<part>/<file>`. `DataPartStorageOnDiskBase::freeze` (`DataPartStorageOnDiskBase.cpp:504`) opens a self-owned whole-part transaction for CA (B21) and commits it.
- Unfreeze: `MergeTreeData::unfreezePartitionsByMatcher` (`MergeTreeData.cpp:9789`) lists `shadow/<backup>/<relative_data_path>` and removes via `Unfreezer::unfreezePartitionsFromTableDirectory`.
- Transaction target capture: `ContentAddressedTransaction::rememberTarget` (`ContentAddressedTransaction.cpp:164`) sets `table_uuid`/`part_name` from `parsePartFilePath`, ignoring any `shadow/` prefix today.
- Commit ref publish: `ContentAddressedTransaction::commit` (`ContentAddressedTransaction.cpp:1018`); ref key built at line 1155 via `refKey(key_prefix, server_id, table_uuid, part_name)`; sidecar at 1145; the detached-RMW branch (per-part shared `detached` ref) at 1073/1119 — does NOT apply to per-part shadow refs.
- GC roots: `listLivePartIds` (`ContentAddressedGC.cpp:88`) walks `listKeysUnder(refsRootPrefix(key_prefix))` keeping keys with a `/refs/` segment, skipping `.meta`.
- Path helpers: `PoolPaths.h` (`refsRootPrefix`/`refsPrefix`/`refKey`/`refMetaKey`/`refMutableFileKey`, the `PartFilePath` struct at line 133, `parsePartFilePath` at 142); bodies in `PoolPaths.cpp` (`refsRootPrefix` at 49, `parsePartFilePath` at 283).
- Metadata routing: `ContentAddressedMetadataStorage.cpp` — `existsDirectory`/`isDirectoryEmpty`/`listDirectory`/`getStorageObjects`/`removeRecursive` route `detached`/projection namespaces by inspecting `parsePartFilePath` (e.g. `existsDirectory` detached branch ~line 425). `readRefPartId(table_uuid, part_name)` (line ~uses `refKey`) reads a live ref.
- gtests: `src/Disks/tests/gtest_content_addressed_metadata.cpp`, `src/Disks/tests/gtest_content_addressed.cpp`. Run `build/src/unit_tests_dbms --gtest_filter='ContentAddressed*'`.

---

## Phase 1 — shadow namespace + parser + gate lift; empirical probe

### Task 1: add the `shadow/` namespace helpers and parse the `shadow/<backup>/` prefix

**Files:** Modify `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h`, `…/PoolPaths.cpp`

- [ ] **Step 1: add a `backup_name` field to `PartFilePath`.** In `PoolPaths.h`, extend the struct (after `file`):

```cpp
struct PartFilePath
{
    std::string table_uuid;
    std::string part_name;
    std::string file; /// empty when the path is a part directory
    /// Set to the backup name when the path is a FREEZE target shadow/<backup_name>/…/<part>[/<file>].
    /// Empty for a normal live-part path. The frozen ref then lives in the shadow/ namespace
    /// (shadowRefKey) rather than the live store/.../refs/ location, so a freeze never clobbers the
    /// live part's ref (the shadow ref is also an independent GC root).
    std::string backup_name;
};
```

- [ ] **Step 2: declare the shadow namespace helpers** in `PoolPaths.h`, next to the `refs*` declarations (after `refMutableFileKey`, ~line 29):

```cpp
// FREEZE namespace. A frozen part is published as its OWN ref under shadow/<backup>/<server>/<uuid>/refs/
// (one ref per frozen part, unlike the shared "detached" ref). shadowRefsRootPrefix is an additional GC
// root the reachability scan walks alongside refsRootPrefix, so a frozen snapshot's blobs stay reachable
// even after the live part is merged/dropped — the whole point of FREEZE.
std::string shadowRefsRootPrefix(const std::string & key_prefix);
std::string shadowRefsPrefix(const std::string & key_prefix, const std::string & backup_name, const std::string & server_id, const std::string & table_uuid);
RefObjectKey shadowRefKey(const std::string & key_prefix, const std::string & backup_name, const std::string & server_id, const std::string & table_uuid, const std::string & part_name);
RefMetaObjectKey shadowRefMetaKey(const std::string & key_prefix, const std::string & backup_name, const std::string & server_id, const std::string & table_uuid, const std::string & part_name);
RefMetaObjectKey shadowRefMutableFileKey(const std::string & key_prefix, const std::string & backup_name, const std::string & server_id, const std::string & table_uuid, const std::string & part_name, const std::string & file);

// The literal first path component reserved for FREEZE snapshots (mirrors kDetachedDirName).
inline constexpr std::string_view kShadowDirName = "shadow";
```

- [ ] **Step 3: define the shadow helpers** in `PoolPaths.cpp`, right after `refMutableFileKey`'s definition (the `refsPrefix`/`refKey` block ends ~line 73). Mirror `refsPrefix`/`refKey` exactly but insert the `shadow/<backup>/` prefix:

```cpp
std::string shadowRefsRootPrefix(const std::string & key_prefix)
{
    return withPrefix(key_prefix, "shadow/");
}

std::string shadowRefsPrefix(const std::string & key_prefix, const std::string & backup_name, const std::string & server_id, const std::string & table_uuid)
{
    return withPrefix(key_prefix, "shadow/" + backup_name + "/" + server_id + "/" + table_uuid + "/refs/");
}

RefObjectKey shadowRefKey(const std::string & key_prefix, const std::string & backup_name, const std::string & server_id, const std::string & table_uuid, const std::string & part_name)
{
    return RefObjectKey(shadowRefsPrefix(key_prefix, backup_name, server_id, table_uuid) + part_name);
}

RefMetaObjectKey shadowRefMetaKey(const std::string & key_prefix, const std::string & backup_name, const std::string & server_id, const std::string & table_uuid, const std::string & part_name)
{
    return RefMetaObjectKey(shadowRefsPrefix(key_prefix, backup_name, server_id, table_uuid) + part_name + std::string(kRefMetaSuffix));
}

RefMetaObjectKey shadowRefMutableFileKey(const std::string & key_prefix, const std::string & backup_name, const std::string & server_id, const std::string & table_uuid, const std::string & part_name, const std::string & file)
{
    return RefMetaObjectKey(shadowRefsPrefix(key_prefix, backup_name, server_id, table_uuid) + part_name + "." + file + std::string(kRefMetaSuffix));
}
```

- [ ] **Step 4: parse the `shadow/<backup>/` prefix** in `parsePartFilePath` (`PoolPaths.cpp:283`). The freeze path is `shadow/<backup>/store/<uuid[:3]>/<uuid>/<part>/<file>`; `findPartDirComponent` already anchors on the inner `store/<uuid>` pair, so `table_uuid`/`part_name`/`file` parse correctly today — we ONLY add the backup capture. After the existing anchor logic builds `r` (just before `return r;`):

```cpp
    PartFilePath r;
    r.table_uuid = joinTableId(p, anchor->table_start, anchor->part_idx);
    r.part_name = p[anchor->part_idx];
    if (anchor->part_idx + 1 < p.size())
    {
        std::string file = p[anchor->part_idx + 1];
        for (size_t i = anchor->part_idx + 2; i < p.size(); ++i)
            file += "/" + p[i];
        r.file = file;
    }
    // FREEZE target: shadow/<backup_name>/…/<part>. The frozen ref lives in the shadow/ namespace, so
    // capture the backup name (the component right after the reserved "shadow" root) for the commit /
    // read routing. The inner store/<uuid>/<part> anchor above is unaffected by the prefix.
    if (p.size() >= 2 && p[0] == std::string(kShadowDirName))
        r.backup_name = p[1];
    return r;
```

- [ ] **Step 5: build** `ninja -C build clickhouse > build/freeze_t1_build.log 2>&1; echo $?; grep -cE "error:|FAILED:" build/freeze_t1_build.log` → 0 errors. (Summarize via a subagent.)

- [ ] **Step 6: commit** `git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.cpp && git commit` subject `CAS FREEZE: shadow/ namespace helpers + parse the shadow/<backup>/ prefix` + trailer.

### Task 2: lift the FREEZE/UNFREEZE gate

**Files:** Modify `src/Storages/MergeTree/MergeTreeData.cpp`

- [ ] **Step 1: add the four FREEZE enum values** to the ContentAddressed `supported_commands` set (`~line 6583`):

```cpp
                    const static auto supported_commands = {
                        PartitionCommand::DROP_PARTITION,
                        PartitionCommand::DROP_DETACHED_PARTITION,
                        PartitionCommand::ATTACH_PARTITION,
                        PartitionCommand::REPLACE_PARTITION,
                        PartitionCommand::MOVE_PARTITION,
                        PartitionCommand::FETCH_PARTITION,
                        PartitionCommand::FREEZE_PARTITION,
                        PartitionCommand::FREEZE_ALL_PARTITIONS,
                        PartitionCommand::UNFREEZE_PARTITION,
                        PartitionCommand::UNFREEZE_ALL_PARTITIONS,
                    };
```

Update the adjacent comment: FREEZE/UNFREEZE are now SUPPORTED on CA — a freeze publishes each part as its own ref in the `shadow/` namespace (a GC root sharing the live blobs zero-copy); UNFREEZE removes the backup's refs.

- [ ] **Step 2: build** `ninja -C build clickhouse > build/freeze_t2_build.log 2>&1; echo $?; grep -cE "error:|FAILED:" build/freeze_t2_build.log` → 0 errors. (Summarize via a subagent.)

- [ ] **Step 3: commit** `git add src/Storages/MergeTree/MergeTreeData.cpp && git commit` subject `CAS FREEZE: lift the FREEZE/UNFREEZE gate` + trailer.

### Task 3: empirical probe — un-gate the 7 FREEZE tests and run

**Files:** Modify the 7 gated FREEZE tests under `tests/queries/0_stateless/`.

- [ ] **Step 1: un-tag all 7** (remove `no-content-addressed-storage` + any reason comment; preserve other tags): `00952_part_frozen_info.sql`, `01325_freeze_mutation_stuck.sql`, `01414_freeze_does_not_prevent_alters.sql`, `01417_freeze_partition_verbose.sh`, `01417_freeze_partition_verbose_zookeeper.sh`, `03611_freeze_partition_parallel_verbose.sh`, `03612_freeze_partition_parallel_verbose_zookeeper.sh`.

- [ ] **Step 2: run on CA-default**
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
sel="00952_part_frozen_info 01325_freeze_mutation_stuck 01414_freeze_does_not_prevent_alters 01417_freeze_partition_verbose 01417_freeze_partition_verbose_zookeeper 03611_freeze_partition_parallel_verbose 03612_freeze_partition_parallel_verbose_zookeeper"
[ -n "$(echo "$sel"|tr -d ' ')" ] || { echo ABORT; exit 1; }
timeout 900 python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" --test "$sel" > build/freeze_t3_run.log 2>&1
echo "exit $?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+, Skipped: [0-9]+" build/freeze_t3_run.log | tail -1
grep -A20 -E "0095|0132|0141|0361" ci/tmp/test_result.txt | head -80
```

- [ ] **Step 3: classify (drives Phase 2/3).** Record each test's outcome and exact error. Expected, before the commit redirect: a freeze publishes the ref at the LIVE ref key (collision) or the read/list of `shadow/` fails → a `FILE_DOESNT_EXIST` / empty `system.parts` frozen info / "not found in shadow" symptom. Note whether the failure is the **commit redirect** gap (ref lands at live key, Phase 2) or the **routing** gap (UNFREEZE/`shadow` list/read, Phase 3) or **orthogonal** (verbose output format, `is_frozen` column plumbing — re-gate with a reason in Task 8). Do NOT commit a failing un-gate yet.

---

## Phase 2 — commit redirect + GC root

### Task 4: redirect the freeze commit to the `shadow/` ref namespace

**Files:** Modify `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h`, `…/ContentAddressedTransaction.cpp`

- [ ] **Step 1: add a `frozen_backup_name` member.** In `ContentAddressedTransaction.h`, next to `table_uuid`/`part_name`:

```cpp
    /// Non-empty iff this transaction writes a FREEZE target (shadow/<backup_name>/…). The commit then
    /// publishes the ref + sidecar in the shadow/ namespace (shadowRefKey) instead of the live
    /// store/.../refs/ location, so a freeze publishes an independent, GC-rooted snapshot ref rather than
    /// clobbering the live part's ref.
    std::string frozen_backup_name;
```

- [ ] **Step 2: capture the backup name in `rememberTarget`** (`ContentAddressedTransaction.cpp:164`). Replace the body's capture block so the backup name is captured + consistency-checked alongside table/part:

```cpp
    /// All files of one commit must belong to the same (table_uuid, part_name).
    if (table_uuid.empty() && part_name.empty())
    {
        table_uuid = p->table_uuid;
        part_name = p->part_name;
        frozen_backup_name = p->backup_name; /// empty for a live part; the FREEZE backup name otherwise
    }
    else if (table_uuid != p->table_uuid || part_name != p->part_name || frozen_backup_name != p->backup_name)
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "ContentAddressed: a single transaction must write one part, got {}/{} (backup '{}') and {}/{} (backup '{}')",
            table_uuid, part_name, frozen_backup_name, p->table_uuid, p->part_name, p->backup_name);
    }
```

- [ ] **Step 3: redirect the ref / sidecar / mutable-file keys in `commit`.** In `ContentAddressedTransaction::commit`, the shadow target is per-part (one ref per frozen part), so it must SKIP the shared-`detached` RMW branches and use the shadow key builders. Make three edits:

  (a) The detached-RMW manifest merge (line ~1073) and the sidecar merge (line ~1119) are guarded by `if (part_name == ContentAddressed::kDetachedDirName)`. A freeze target has a real `part_name` (never `"detached"`), so these are already skipped — no change needed, but add a one-line comment at the top of the manifest block noting frozen targets are per-part and never enter the shared-ref merge.

  (b) The mutable per-file key (line ~1133) and sidecar meta key (line ~1145): replace the key builders with a frozen-aware selection. Define a small local lambda just before the `if (!merged_mutable.empty())` block:

```cpp
    /// FREEZE publishes into the shadow/<backup>/ namespace (one ref per frozen part); a live part uses
    /// the store/.../refs/ location. Select the ref-family keys accordingly.
    const bool is_frozen = !frozen_backup_name.empty();
    auto mutable_file_key = [&](const std::string & file)
    {
        return is_frozen
            ? ContentAddressed::shadowRefMutableFileKey(key_prefix, frozen_backup_name, metadata_storage.server_id, table_uuid, part_name, file).string()
            : ContentAddressed::refMutableFileKey(key_prefix, metadata_storage.server_id, table_uuid, part_name, file).string();
    };
    const std::string meta_key = is_frozen
        ? ContentAddressed::shadowRefMetaKey(key_prefix, frozen_backup_name, metadata_storage.server_id, table_uuid, part_name).string()
        : ContentAddressed::refMetaKey(key_prefix, metadata_storage.server_id, table_uuid, part_name).string();
```

  Then in the `merged_mutable` loop use `mutable_file_key(file)` instead of the inline `refMutableFileKey(...)`, and use the `meta_key` local instead of recomputing it inline (remove the inline `const std::string meta_key = … refMetaKey(...)`).

  (c) The ref publish (line ~1155): select the ref key the same way:

```cpp
    const std::string ref_key = is_frozen
        ? ContentAddressed::shadowRefKey(key_prefix, frozen_backup_name, metadata_storage.server_id, table_uuid, part_name).string()
        : ContentAddressed::refKey(key_prefix, metadata_storage.server_id, table_uuid, part_name).string();
```

- [ ] **Step 4: build** `ninja -C build clickhouse > build/freeze_t4_build.log 2>&1; echo $?; grep -cE "error:|FAILED:" build/freeze_t4_build.log` → 0 errors. (Summarize via a subagent.)

- [ ] **Step 5: add a metadata-layer gtest** in `src/Disks/tests/gtest_content_addressed_metadata.cpp` (mirror an existing freeze/detached test): write a part live + commit; then write the same files to a `shadow/<backup>/store/<uuid>/<part>/…` path through a fresh transaction + commit; assert (a) the live ref at `refKey(...)` is unchanged (its part_id intact — no clobber), (b) a ref exists at `shadowRefKey(...,backup,...)` resolving to the SAME part_id (content-only, B6), (c) no shadow object was written under the live `refsPrefix`. Run `build/src/unit_tests_dbms --gtest_filter='ContentAddressed*'` → all pass.

- [ ] **Step 6: commit** the transaction redirect + gtest. Subject `CAS FREEZE: publish the frozen ref in the shadow/ namespace (no live-ref clobber)`.

### Task 5: make `shadow/` refs a GC root

**Files:** Modify `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.cpp`

- [ ] **Step 1: walk the shadow root in `listLivePartIds`** (`ContentAddressedGC.cpp:88`). The frozen refs live under `shadowRefsRootPrefix` and have the same `/refs/` shape, so add a second pass over that root reusing the same filter:

```cpp
    static const std::string refs_segment = "/refs/";
    std::set<PartId> live;
    auto scan_root = [&](const std::string & root)
    {
        for (const auto & key : listKeysUnder(object_storage, root))
        {
            if (key.find(refs_segment) == std::string::npos)
                continue;
            if (isRefMetaKey(key))
                continue;
            live.insert(partIdFromRefPayload(readSmallObject(object_storage, key)));
        }
    };
    scan_root(refsRootPrefix(key_prefix));        /// live active parts (store/.../refs/)
    scan_root(shadowRefsRootPrefix(key_prefix));  /// FREEZE snapshots (shadow/<backup>/.../refs/) — keep
                                                  /// frozen blobs reachable even after the live part is gone
    return live;
```

Update the function's doc comment to mention the shadow root as the second GC-root family.

- [ ] **Step 2: build** `ninja -C build clickhouse > build/freeze_t5_build.log 2>&1; echo $?; grep -cE "error:|FAILED:" build/freeze_t5_build.log` → 0 errors. (Summarize via a subagent.)

- [ ] **Step 3: extend the gtest** (`gtest_content_addressed_metadata.cpp` or `gtest_content_addressed.cpp`, wherever the GC reachability tests live — grep `listLivePartIds`/`markReachable`): freeze a part, DROP the live ref (simulate the live part merging away by removing the live ref object), assert the frozen part's blobs are STILL in the reachable set (named by the shadow ref); then remove the shadow ref and assert they are no longer reachable. Run `--gtest_filter='ContentAddressed*'` → all pass.

- [ ] **Step 4: commit** subject `CAS FREEZE: shadow/ refs are GC roots (frozen blobs survive the live part)`.

---

## Phase 3 — shadow path routing + oracle

### Task 6: route `shadow/` path reads / lists / removes to the frozen ref-set

**Files:** Modify `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (and `.h` if a private helper is added)

This task is reproduction-driven by Task 3's failures — implement exactly the routing the un-gated tests + the oracle exercise. The methods to extend (each already routes the `detached`/projection namespaces by inspecting `parsePartFilePath`; add a `p->backup_name` non-empty branch using the shadow key family):

- [ ] **Step 1: a private `readShadowRefPartId` + `readShadowRefSidecarIfExists` helper.** Mirror `readRefPartId`/`readRefSidecarIfExists` but key via `shadowRefKey`/`shadowRefMetaKey` with the backup name. Declare in the `.h` private section; define in the `.cpp` next to `readRefPartId`.

- [ ] **Step 2: `getStorageObjects` / `readFile` / `existsFile` / `getFileSize`** — when `parsePartFilePath(path)` returns a `p` with non-empty `p->backup_name` and non-empty `p->file`, resolve the blob(s) via the shadow ref's manifest (`readShadowRefPartId` → `loadPartManifestOrThrow` → the same `resolveBlobEntry` the live path uses; mutable files via the shadow sidecar). This makes ATTACH-from-shadow and any read of a frozen file work.

- [ ] **Step 3: `existsDirectory` / `isDirectoryEmpty` / `listDirectory`** — when `p->backup_name` is non-empty: a frozen PART dir (`p->file.empty()`) exists iff `readShadowRefPartId(backup, uuid, part)` is set; listing the backup's table dir (`shadow/<backup>/store/<uuid>/`, which arrives as a `parseTableUuid`-style path with a `shadow` leading component) enumerates the parts present under `shadowRefsPrefix(key_prefix, backup, server_id, uuid)` (list keys, strip `.meta`, return the part names). This is what `Unfreezer::unfreezePartitionsFromTableDirectory` walks. NOTE: a bare `shadow/<backup>/store/<uuid>/` table-dir path has no part-dir anchor, so it parses via `parseTableUuid` not `parsePartFilePath` — add a sibling branch that detects the leading `shadow` component for the table-dir listing (grep how the live table-dir listing at `existsDirectory ~line 408` obtains its `uuid`, and add the shadow-prefixed analogue keyed by `shadowRefsPrefix`).

- [ ] **Step 4: `removeRecursive`** — when the path is under `shadow/<backup>/…`, delete the matching shadow refs + their `.meta` sidecars + per-file mutable objects under the resolved `shadowRefsPrefix` (for a single frozen part: that part's ref/sidecar; for the backup/table subtree: all parts under it). This is what UNFREEZE invokes; after it the frozen blobs become GC-eligible if no other ref names their part. Mirror the live-ref removeRecursive ref-scoped deletion (grep the existing `removeRecursive` detached/ref branch).

- [ ] **Step 5: build** `ninja -C build clickhouse > build/freeze_t6_build.log 2>&1; echo $?; grep -cE "error:|FAILED:" build/freeze_t6_build.log` → 0 errors. (Summarize via a subagent.)

- [ ] **Step 6: re-run the un-gated FREEZE tests** (Task 3's `sel`) on CA-default → record pass/fail; iterate Steps 2-4 until the non-orthogonal failures pass. `--gtest_filter='ContentAddressed*'` still green.

- [ ] **Step 7: commit** subject `CAS FREEZE: route shadow/ reads/lists/removes to the frozen ref-set (UNFREEZE + ATTACH-from-shadow)`.

### Task 7: inline-CA oracle

**Files:** Create `tests/queries/0_stateless/<NNNNN>_content_addressed_freeze.sql` (via `./tests/queries/0_stateless/add-test <name>`)

- [ ] **Step 1: write the oracle.** A CA table on an inline content-addressed disk (`disk = disk(type=object_storage, object_storage_type=local, metadata_type=content_addressed, …, content_addressed_allow_shared_pool=1)`); insert deterministic rows across ≥2 partitions; `ALTER TABLE … FREEZE PARTITION <p> WITH NAME 'b1'`; `ALTER TABLE … DROP PARTITION <p>` (the live part is gone); assert the table no longer returns those rows; `ALTER TABLE … ATTACH PARTITION <p>` is not directly possible from a named freeze, so instead assert durability via the frozen snapshot being intact — `SELECT` after re-`ATTACH` of the detached copy, OR (simpler and within the engine) assert `system.parts`/the frozen part remains readable and a fresh `ATTACH PARTITION <p> FROM` path. Keep the oracle to what FREEZE/UNFREEZE actually expose on a single server: FREEZE → the part is frozen (`is_frozen`/`system.parts` reflects it or `shadow` is populated) → `UNFREEZE PARTITION <p> WITH NAME 'b1'` succeeds and the backup is gone. Mirror `00952`/`01414` for the exact surface. Hand-verify the reference.

- [ ] **Step 2: run on CA-default + plain** → `Passed: 1` on both:
```bash
timeout 900 python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" --test "<NNNNN>_content_addressed_freeze" > build/freeze_t7_ca.log 2>&1; echo "exit $?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+" build/freeze_t7_ca.log | tail -1
timeout 900 python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test "<NNNNN>_content_addressed_freeze" > build/freeze_t7_plain.log 2>&1; echo "exit $?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+" build/freeze_t7_plain.log | tail -1
```

- [ ] **Step 3: commit** subject `CAS FREEZE: inline-CA oracle (freeze → drop live → snapshot survives → unfreeze)`.

---

## Phase 4 — regression, finalize, push

### Task 8: non-CA regression + finalize un-gate + backlog

- [ ] **Step 1: non-CA regression** — run a couple of FREEZE tests on the PLAIN job to confirm no regression from the gate-set change + the commit/GC/routing branches (all `isContentAddressed`/`backup_name`-gated):
```bash
timeout 900 python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test "01414_freeze_does_not_prevent_alters 00952_part_frozen_info" > build/freeze_t8_plain.log 2>&1
echo "exit $?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+" build/freeze_t8_plain.log | tail -1
```
Expected: both pass on plain.

- [ ] **Step 2: finalize the un-gate** — confirm each of the 7 tests is either un-gated + passing on CA, OR re-gated with a PRECISE reason if it fails for an orthogonal cause (verbose-output format the stateless server can't match, `is_frozen` cross-session plumbing, a topology the server lacks) — not a CA-FREEZE bug. Do not leave a test failing un-gated. (`01417_freeze_partition_verbose_zookeeper` was in the original gated set; include it in the final accounting — un-gate if it passes, else re-gate with a reason.)

- [ ] **Step 3: backlog** — update `docs/superpowers/deferred_backlog/cas-mergetree-integration.md`: B4 FREEZE/UNFREEZE now supported on CA (per-part frozen ref-set in the GC-rooted `shadow/` namespace, zero-copy blob sharing); the FREEZE tests removed from the gated set (list any re-gated with their reason); note `freezeRemote` / cross-disk freeze deferred with the plug-in point.

- [ ] **Step 4: commit + push** `git push filimonov cas-mergetree-poc`.

---

## Done criteria
- The 7 FREEZE tests un-gated and passing on the CA-default job (or individually re-gated for a documented orthogonal reason, not a CA-FREEZE bug).
- A `FREEZE PARTITION` on a CA disk publishes a per-part ref in `shadow/` that survives the live partition being dropped (the oracle), and `UNFREEZE` removes it.
- Frozen blobs are GC roots (the gtest: reachable after the live ref is gone; unreachable after the shadow ref is removed).
- No live-ref clobber (the gtest: live ref intact, shadow ref resolves to the same part_id).
- No non-CA regression (every new branch is `isContentAddressed`/`backup_name`-gated).
- Backlog: B4 FREEZE/UNFREEZE supported; `freezeRemote`/cross-disk deferred.
