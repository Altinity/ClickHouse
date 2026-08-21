---
description: 'Implementation plan for scoping a content-addressed FREEZE snapshot to the server root that created it'
sidebar_label: 'CAS shadow namespace server root'
sidebar_position: 1
slug: /superpowers/plans/cas-shadow-namespace-server-root
title: 'FREEZE shadow namespace under server_root_id — implementation plan'
doc_type: 'guide'
---

# FREEZE shadow namespace under `server_root_id` — Implementation Plan {#cas-shadow-namespace-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a `FREEZE` snapshot on a content-addressed disk private to the server root that created it, so `UNFREEZE` on one replica can no longer destroy another replica's backup.

**Architecture:** `shadowNamespace` gains the `serverPrefix` prefix, exactly as `liveNamespace` already does, so a frozen part's pool namespace becomes `<server_root_id>/shadow/<backup>/...`. The disk path stays `shadow/...`; only the pool-namespace derivation changes. Because a mirrored object key is built as `<pool>/roots/<namespace path>`, prefixing the namespace moves its mirrored files with it — the write side needs no change, and every *read* scope that enumerates the shadow subtree needs the same prefix.

**Tech Stack:** C++ (ClickHouse fork), stateless `.sh` tests, `clickhouse-local` for output shaping, gtest.

**Spec:** `docs/superpowers/cas/BACKLOG.md` `{#issue-2212-shadow-namespace}` (corrected in commit `6309169135f`). Read it first: it is the authority, it enumerates the six production sites, and it explains why one of them is a silent-leak risk.

## Global Constraints {#global-constraints}

- Branch `cas-gc-rebuild`. No rebase, no amend — add new commits.
- **Pre-release no-compat policy: no migration, no alias, no fallback for the old namespace layout.** Existing shadow data under the unprefixed namespace is not read, not moved, not detected.
- The disk path `shadow/...` does not change. `Cas::PartPathParser` and `ContentAddressedMetadataStorage::route` parse the disk path and must not be touched.
- No `LOGICAL_ERROR` anywhere in this work. This change adds no new failure mode.
- Allman braces (opening brace on its own line) — enforced by the style check.
- Comments must not cite this plan, the BACKLOG, a task number, or an issue number. Keep the reason, drop the provenance.
- **The controller owns builds, test runs and commits.** Do not run `ninja`, `clickhouse-test`, or `python3 -m ci.praktika`; do not commit. Report when the tree is ready.
- This checkout is shared and has foreign uncommitted files. Never `git add -A`, `git add .`, or `git commit -a`; do not stage anything.

---

## Task 1: Pin the cross-root destruction with a failing test {#task-1}

**Files:**
- Modify: `tests/queries/0_stateless/05024_cas_freeze_two_roots.sh` (already created by `add-test`, currently a stub)
- Modify: `tests/queries/0_stateless/05024_cas_freeze_two_roots.reference` (created empty by `add-test`)

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: the test Task 2 must turn green. The number `05024` was assigned by `./tests/queries/0_stateless/add-test cas_freeze_two_roots.sh`; do not renumber it, and do not `chmod` — `add-test` already did both.

### Why the obvious test does not work {#task-1-why-not-obvious}

Two ordinary tables on two disks do **not** collide. `FREEZE` builds the snapshot path as `shadow/<backup>/<relative_data_path>` (`MergeTreeData.cpp`, the freeze callback), and in an Atomic database `relative_data_path` embeds the table UUID. Two tables have two UUIDs, so their shadow namespaces differ *before* any fix and an unfreeze of one could never have touched the other. A test built that way passes on broken code and proves nothing.

The collision needs **one `relative_data_path` reachable from two server roots**. On a single server that means reusing one explicit table UUID sequentially: create on disk A, freeze, drop, then create with the *same* UUID on disk B. Both freezes then derive the identical shadow path, which is exactly the two-replicas-one-table shape from the issue.

Explicit UUIDs are available in stateless tests — `02990_rmt_replica_path_uuid.sql` uses `CREATE TABLE x UUID '...'`.

Two further traps this test avoids:

- `ALTER … UNFREEZE` returns rows only under `alter_partition_verbose_result = 1`. The setting defaults to `false` and `MergeTreeData` returns an empty result without it, so any reference file expecting rows without the setting is unreachable.
- The `PARTITION` clause is omitted deliberately: `UNFREEZE WITH NAME` alone removes the backup of all partitions, so the statement does not have to resolve a partition expression against a table whose live data was dropped. That choice is also why the reported `command_type` is `UNFREEZE ALL` rather than `UNFREEZE PARTITION` — the reference below matches the command actually built.

- [ ] **Step 1: Write the failing test**

Replace the body of `tests/queries/0_stateless/05024_cas_freeze_two_roots.sh` (keep the `add-test` header lines it generated):

```bash
#!/usr/bin/env bash
# Tags: no-fasttest
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.

# A FREEZE snapshot on a content-addressed disk belongs to the server root that made it. UNFREEZE is
# local and destructive, so releasing one root's freeze must not touch another root's.
#
# Two server roots sharing one pool is how two replicas of one table look from the pool's side. The
# collision needs ONE table path reachable from both roots, and the shadow path embeds the table UUID
# in an Atomic database -- so the UUID is reused sequentially rather than creating two tables, which
# would have two UUIDs, two namespaces, and no collision to test.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

TABLE_UUID='05024aaa-0000-4000-8000-000000000001'
BACKUP='shared_05024'
UNFREEZE_STRUCTURE='command_type String, partition_id String, part_name String, backup_name String, backup_path String, part_backup_path String'

# `ALTER ... UNFREEZE` returns rows only under alter_partition_verbose_result=1; the default is off.
# backup_path and part_backup_path are absolute, so only the stable columns are printed.
unfreeze_and_print() {
    ${CLICKHOUSE_CLIENT} --query "ALTER TABLE $1 UNFREEZE WITH NAME '${BACKUP}' SETTINGS alter_partition_verbose_result = 1;" \
        | ${CLICKHOUSE_LOCAL} --structure "$UNFREEZE_STRUCTURE" \
            --query "SELECT command_type, partition_id, backup_name FROM table ORDER BY partition_id FORMAT TSVWithNames"
}

create_on_root() {
    # $1 = table name, $2 = server_root_id, $3 = disk name. One pool, two roots.
    ${CLICKHOUSE_CLIENT} --query "
    CREATE TABLE $1 UUID '${TABLE_UUID}' (k UInt32, v String)
    ENGINE = MergeTree ORDER BY k PARTITION BY k
    SETTINGS disk = disk(
        type = object_storage,
        object_storage_type = local,
        metadata_type = cas,
        server_root_id = '$2',
        name = '$3',
        path = '05024_cas_freeze_pool/');"
}

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS t_cas_freeze_a;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS t_cas_freeze_b;"

# Root A freezes, then releases the UUID. Its freeze must outlive the table -- an independent GC root.
create_on_root t_cas_freeze_a 05024_root_a 05024_cas_freeze_a
${CLICKHOUSE_CLIENT} --query "INSERT INTO t_cas_freeze_a VALUES (1, 'a');"
${CLICKHOUSE_CLIENT} --query "ALTER TABLE t_cas_freeze_a FREEZE PARTITION 1 WITH NAME '${BACKUP}';"
${CLICKHOUSE_CLIENT} --query "DROP TABLE t_cas_freeze_a;"

# Root B, SAME table UUID and SAME backup name: identical shadow path, so the pre-fix namespace
# derivation gives both roots one pool namespace.
create_on_root t_cas_freeze_b 05024_root_b 05024_cas_freeze_b
${CLICKHOUSE_CLIENT} --query "INSERT INTO t_cas_freeze_b VALUES (1, 'b');"
${CLICKHOUSE_CLIENT} --query "ALTER TABLE t_cas_freeze_b FREEZE PARTITION 1 WITH NAME '${BACKUP}';"

# (1) B releases its OWN freeze. This must succeed -- it is also the self-release half: a change that
# made unfreeze search the wrong subtree would print nothing here.
echo 'unfreeze_b'
unfreeze_and_print t_cas_freeze_b

# A collection round with A's table already dropped: A's freeze is an independent root and must
# survive it. The round is started through B, which is still alive, so this does not depend on an
# inline disk outliving the only table that referenced it. A round folds the whole pool, so starting
# it on either disk covers A's namespaces. Its result row is suppressed: the command returns a row
# with dynamic values, and only the side effect is wanted here.
${CLICKHOUSE_CLIENT} --query "SYSTEM CAS COLLECT GARBAGE ON DISK '05024_cas_freeze_b';" >/dev/null

${CLICKHOUSE_CLIENT} --query "DROP TABLE t_cas_freeze_b;"

# (2) A's freeze must still be there. Recreate A's table on root A with the same UUID -- the freeze is
# addressed by path, so the recreated table reaches its predecessor's snapshot -- and release it.
# Pre-fix this prints nothing, because B's unfreeze above already dropped the shared namespace.
create_on_root t_cas_freeze_a 05024_root_a 05024_cas_freeze_a
echo 'unfreeze_a'
unfreeze_and_print t_cas_freeze_a

${CLICKHOUSE_CLIENT} --query "DROP TABLE t_cas_freeze_a;"
${CLICKHOUSE_CLIENT} --query "SELECT 'dropped_ok';"
```

Write `tests/queries/0_stateless/05024_cas_freeze_two_roots.reference`:

```
unfreeze_b
command_type	partition_id	backup_name
UNFREEZE ALL	1	shared_05024
unfreeze_a
command_type	partition_id	backup_name
UNFREEZE ALL	1	shared_05024
dropped_ok
```

`UNFREEZE ALL`, not `UNFREEZE PARTITION`: omitting the `PARTITION` clause builds the
all-partitions command, and that is the string it reports.

- [ ] **Step 2: Report for a run and state the expected failure precisely**

Expected on today's code: the `unfreeze_b` block prints its row, and the `unfreeze_a` block prints **only the header** — B's unfreeze already dropped the namespace A's freeze shared. The single production line responsible is `ContentAddressedMetadataStorage::shadowNamespace`, which derives the namespace from the shadow path alone with no server-root prefix.

**Do not proceed to Task 2 until the controller confirms it fails that way.** Two failure modes mean a broken test rather than a pinned bug, and each needs fixing here rather than in production: `SYSTEM CAS COLLECT GARBAGE ON DISK` rejecting that disk name, or the recreated table failing because the UUID or the inline disk is still held. The stateless configuration sets `database_atomic_wait_for_drop_and_detach_synchronously`, so the drop before the reuse needs no explicit `SYNC`.

- [ ] **Step 3: Commit (controller only, after the failing run is confirmed)**

```bash
git add tests/queries/0_stateless/05024_cas_freeze_two_roots.sh \
        tests/queries/0_stateless/05024_cas_freeze_two_roots.reference
git commit -m 'Pin the cross-root FREEZE destruction on a content-addressed disk'
```

---

## Task 2: Prefix the shadow namespace, every scope, and every caller {#task-2}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp`
- Modify: `src/Disks/tests/gtest_ca_wiring.cpp` (two static call sites)
- Modify: `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp` (an ownership expectation that changes meaning)

**Interfaces:**
- Consumes: the test from Task 1.
- Produces:
  - `Cas::RootNamespace ContentAddressedMetadataStorage::shadowNamespace(const std::string & shadow_table_dir) const` — no longer `static`.
  - `std::string ContentAddressedMetadataStorage::shadowScope(const std::string & path) const` — the server-root-scoped enumeration prefix for a shadow directory, always ending in `/`.

Everything here lands together. The `static` drop breaks compilation at four call sites — two in production, two in gtests — which is the point: the compiler enumerates them, where a grep scoped to the production tree missed the test ones.

- [ ] **Step 1: Change the derivation and add the scope helper**

In `ContentAddressedMetadataStorage.h`, replace the `shadowNamespace` declaration with:

```cpp
    /// Canonicalizes a literal shadow-table directory into the pool namespace used by freeze and
    /// unfreeze paths, rooted at this server's own `server_root_id` exactly as `liveNamespace` is. A
    /// trailing slash is ignored.
    ///
    /// A freeze belongs to the server root that made it. UNFREEZE is local and destructive, so a
    /// pool-global namespace let one replica's unfreeze release another's backup, and the blobs went
    /// with the next collection round. Cross-replica READABILITY of a freeze goes away with it; shared
    /// access to a backup belongs to the BACKUP machinery, not to UNFREEZE.
    Cas::RootNamespace shadowNamespace(const std::string & shadow_table_dir) const;

    /// The namespace-enumeration prefix for a shadow directory: the same server-root scoping
    /// `shadowNamespace` applies, for callers that enumerate a subtree instead of naming one
    /// namespace. Always ends in '/'. An empty `path` means the disk root's whole shadow subtree.
    ///
    /// One helper on purpose: three callers need this prefix, and a shadow namespace enumerated with a
    /// differently-built prefix is invisible — which on the unfreeze path means it silently stops
    /// releasing anything.
    std::string shadowScope(const std::string & path) const;
```

In `ContentAddressedMetadataStorage.cpp`, replace the body:

```cpp
Cas::RootNamespace ContentAddressedMetadataStorage::shadowNamespace(const std::string & shadow_table_dir) const
{
    /// The LITERAL shadow table dir (shadow/<backup>/store/<u3>/<uuid> or .../data/<db>/<tbl>) is
    /// bijective with the disk path for both layouts, and the disk path itself is unchanged by this
    /// prefix. Canonicalize because the unfreezer can hand the directory a trailing slash.
    return Cas::RootNamespace{serverPrefix() + "/" + canonicalDiskPath(shadow_table_dir)};
}

std::string ContentAddressedMetadataStorage::shadowScope(const std::string & path) const
{
    const std::string canonical = canonicalDiskPath(path);
    return serverPrefix() + "/" + (canonical.empty() ? "shadow/" : canonical + "/");
}
```

- [ ] **Step 2: Point both metadata-storage enumeration scopes at the helper**

At the intermediate-shadow-dir existence probe, replace:

```cpp
            const std::string canonical = canonicalDiskPath(path);
            const std::string scope = canonical.empty() ? "shadow/" : canonical + "/";
            const Cas::NamespaceListing listing = store()->listNamespaces(scope);
```

with:

```cpp
            const Cas::NamespaceListing listing = store()->listNamespaces(shadowScope(path));
```

At the listing counterpart, replace:

```cpp
            const std::string canonical = canonicalDiskPath(path);
            const std::string scope = canonical.empty() ? "shadow/" : canonical + "/";
            std::unordered_set<std::string> result;
            for (const auto & child : store()->listMirroredChildren(scope))
```

with:

```cpp
            std::unordered_set<std::string> result;
            for (const auto & child : store()->listMirroredChildren(shadowScope(path)))
```

The second needs the prefix for the same reason as the first even though it queries the mirrored tree rather than the catalog: a mirrored object key is built as `<pool>/roots/<namespace path>`, so moving the namespace moves its mirrored files with it.

- [ ] **Step 3: Fix the two production call sites and the bulk-drop scope**

In `ContentAddressedTransaction.cpp`, the two calls become member calls through the storage reference the transaction already holds — change only the namespace expression, not the surrounding statements:

```cpp
            const auto ns = metadata_storage.shadowNamespace(p->shadow_table_dir);
```

```cpp
            metadata_storage.partAccess()->dropNamespace(metadata_storage.shadowNamespace(path));
```

Then the bulk drop behind `UNFREEZE WITH NAME`. Replace:

```cpp
        const Cas::NamespaceListing listing = metadata_storage.store()->listNamespaces(prefix + "/");
```

with:

```cpp
        const Cas::NamespaceListing listing
            = metadata_storage.store()->listNamespaces(metadata_storage.shadowScope(prefix));
```

**This is the site the original adjudication missed, and the one worth checking twice.** Prefix the derivation without prefixing this, and `UNFREEZE WITH NAME` enumerates the unprefixed subtree, finds nothing, and stops releasing its own freeze — cross-replica destruction traded for a permanent leak of every frozen ref. The `prefix` local is already stripped of trailing slashes and `shadowScope` canonicalizes anyway, so pass it directly; do not append a slash.

- [ ] **Step 4: Fix the two gtest call sites**

`gtest_ca_wiring.cpp` calls the derivation statically in two tests. Both become member calls on the storage the test already has:

```cpp
    publishWiredPart(*storage, storage->shadowNamespace("shadow/bk1/store/a11/a11a11a1-1111-4111-8111-111111111111"), "all_1_1_0");
```

While there, assert the prefix rather than leaving it implied. In whichever of the two tests reads back the namespace, add:

```cpp
    EXPECT_EQ(storage->shadowNamespace("shadow/bk1/store/a11/a11a11a1-1111-4111-8111-111111111111").string(),
              storage->serverRootId() + "/shadow/bk1/store/a11/a11a11a1-1111-4111-8111-111111111111");
```

`string` is the accessor — `RootNamespace` exposes its underlying string only that way — and `serverRootId` is already public, so no new test seam is needed.

- [ ] **Step 5: Update the ownership expectation that changes meaning**

`gtest_cas_confirm_exact_ref.cpp` asserts that a shadow namespace is owned by nobody:

```cpp
        EXPECT_FALSE(storage->ownsNamespace("srv1", "shadow/backup/store/abc/abcdef")) << phase;
```

Keep it — an *unprefixed* shadow path is still owned by nobody, and after this change nothing writes one. Add the case that now exists, because "shadow is ordinarily owned" is the behavioural consequence of the fix and nothing else asserts it:

```cpp
        /// A prefixed shadow namespace is ordinary owned content: the freeze belongs to the root
        /// that made it, so relink routing treats it exactly like a live namespace.
        EXPECT_TRUE(storage->ownsNamespace("srv1", "srv1/shadow/backup/store/abc/abcdef")) << phase;
        EXPECT_FALSE(storage->ownsNamespace("srv10", "srv1/shadow/backup/store/abc/abcdef")) << phase;
```

Update the comment above the kept assertion so it no longer calls the FREEZE tree pool-global.

- [ ] **Step 6: Report for a build and a run**

Report the sites touched, and say which claims you verified by reading versus which only a run can settle. Expected:

- `05024_cas_freeze_two_roots.sh` — PASSES.
- `05003_cas_freeze.sh` — STILL PASSES. It is the leak guard: it asserts `UNFREEZE WITH NAME` finds and removes its own snapshot on a single root, so a missed Step 3 fails it.
- `CASWiringRead*`, `CASWiringOps*`, `CASConfirmExactRef*` — pass with the updated expectations.
- The full `CAS*:Cas*:CA*` gate — otherwise unchanged.

- [ ] **Step 7: Commit (controller only, after a green run)**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp \
        src/Disks/tests/gtest_ca_wiring.cpp \
        src/Disks/tests/gtest_cas_confirm_exact_ref.cpp
git commit -m 'Scope a content-addressed FREEZE snapshot to the server root that made it'
```

---

## Task 3: Retire the pool-global shadow model from the prose {#task-3}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h` (the namespace-mapping table; the `FREEZE shadow` row)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (the `ownsNamespace` contract clause; the lifecycle grouping list)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h` (the `listMirroredChildren` contract)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h` (the namespace example)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h` (the namespace example)
- Modify: `src/Disks/tests/gtest_ca_wiring.cpp` (a comment calling the shadow publish pool-global)

**Interfaces:**
- Consumes: Task 2's landed change.
- Produces: nothing later tasks depend on.

Task 2 removes a special case rather than moving a prefix: shadow used to be a second relative root beside the server root, and becomes ordinary server-relative content. Six places still describe the old model, and two of them are contracts rather than asides.

- [ ] **Step 1: Correct the two contracts**

`CasPool.h`'s `listMirroredChildren` contract says `prefix` is "a server-relative or shadow-relative path ending in '/'". Shadow is no longer a separate relative root: say `prefix` is a server-relative path ending in `/`, and say nothing about shadow — needing no mention is the point.

`ownsNamespace`'s comment says the predicate "deliberately does not match a pool-global shadow" tree. That clause is now wrong in its reason: a shadow namespace IS matched, because it is rooted at a server root like everything else. Replace it with the current truth — ownership is exactly "rooted at MY server root", and the freeze tree is no exception.

- [ ] **Step 2: Correct the namespace map and the two examples**

The mapping table's `FREEZE shadow` row still reads "the LITERAL shadow table dir" and says the tree enumerates from `Pool::listNamespaces("shadow/...")`. Make it `SERVER_ID/shadow/BACKUP/...` with the same `ref = PART_DIR`, and say the enumeration is server-root-scoped.

`CasTypes.h` and `CasLayout.h` each carry an example string of the form `"shadow/<backup>/<table_uuid>"` beside a `"srv1/<table_uuid>"` one. Update the shadow example to `"srv1/shadow/<backup>/<table_uuid>"` so the two examples stop implying two different rooting schemes.

- [ ] **Step 3: Correct the remaining comments**

Add `shadowScope` to the lifecycle grouping list beside `shadowNamespace`. That list claims "pure path computation, no pool I/O", which stays true — `serverPrefix` returns a member set in the constructor — so verify that rather than weakening the claim.

In `gtest_ca_wiring.cpp`, the comment "the staged shadow part publishes at commit (pool-global - any replica reads the backup)" is now false in both halves. State what is true: the staged shadow part publishes at commit under this server root.

- [ ] **Step 4: Report for a build**

These are comment-only changes, but they sit in `.cpp`/`.h` files, so the controller rebuilds and re-runs the `CAS*:Cas*:CA*` gate. Say explicitly in your report that you changed no code — and if you found yourself needing to change code to make a comment true, stop and say so instead.

- [ ] **Step 5: Commit (controller only)**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h \
        src/Disks/tests/gtest_ca_wiring.cpp
git commit -m 'Shadow content is ordinary server-relative content now'
```

---

## Optional simplification, only after Task 2 lands clean {#optional-simplification}

Not part of the fix. A separate decision for the controller; do not fold it into Task 2.

After Task 2, `liveNamespace` and `shadowNamespace` have the same shape — `serverPrefix() + "/" + <path>` — differing only in how the path is derived. A single `serverScoped(path)` helper used by both would make "everything this server owns is rooted at its own id" explicit in code instead of by convention repeated twice, and give the next namespace kind one obvious place to hook into.

Against it: two callers is a thin basis for an abstraction, and the derivations are otherwise unrelated. Take it only if the invariant is judged worth naming.

## Self-review {#self-review}

**Spec coverage.** The spec's six production sites map to Task 2 Steps 1-3. Spec items (3) parser/route untouched and (4) no migration are Global Constraints. Item (5) the test is Task 1, including the third assertion the spec calls non-redundant — here it is the `unfreeze_b` block, which is both the foreign-unfreeze setup and the self-release proof. Item (6) nothing asserts cross-replica visibility is why Task 3 deletes the concept instead of preserving it.

**Beyond the spec.** The spec's site list covered production only. Task 2 Steps 4-5 add two test files the `static` drop breaks or invalidates, and Task 3 adds four prose sites the spec did not enumerate. The spec should be treated as complete for production and incomplete for tests and prose.

**Placeholders.** None. Every code step carries actual before/after text; the test and its reference are given in full.

**Type consistency.** `shadowNamespace` is a const member returning `Cas::RootNamespace`, used that way in both production callers and both gtests. `shadowScope` is a const member returning `std::string`, used at all three enumeration sites. The transaction reaches both through `metadata_storage`, held as `ContentAddressedMetadataStorage &`.

**Risks this plan cannot remove.** Task 1's reference predicts the exact `command_type` string and relies on an inline disk plus a table UUID being reusable after `DROP TABLE`. If any of those is wrong the first run says so, and each is a test fix rather than a production finding — Step 2 names all three so they are not mistaken for one. Task 2 Step 4's prefix assertion uses the existing public `serverRootId` accessor and `RootNamespace::string`; both were checked against the headers rather than assumed.
