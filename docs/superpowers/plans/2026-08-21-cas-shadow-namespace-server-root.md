# FREEZE shadow namespace under `server_root_id` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a `FREEZE` snapshot on a content-addressed disk private to the server root that created it, so `UNFREEZE` on one replica can no longer destroy another replica's backup.

**Architecture:** `shadowNamespace` gains the `serverPrefix()` prefix, exactly as `liveNamespace` already does, so a frozen part's pool namespace becomes `<server_root_id>/shadow/<backup>/...`. The disk path stays `shadow/...`; only the pool-namespace derivation changes. Because mirrored object keys are built as `<pool>/roots/<namespace path>`, prefixing the namespace moves its mirrored files with it — so the write side needs no change and every *read* scope that enumerates the shadow subtree needs the same prefix.

**Tech Stack:** C++ (ClickHouse fork), stateless `.sh` tests, `clickhouse-local` for output shaping.

**Spec:** `docs/superpowers/cas/BACKLOG.md` `{#issue-2212-shadow-namespace}` (corrected in commit `6309169135f`). Read it first: it is the authority, it enumerates all six sites, and it explains why one of them is a silent-leak risk.

## Global Constraints

- Branch `cas-gc-rebuild`. No rebase, no amend — add new commits.
- **Pre-release no-compat policy: no migration, no alias, no fallback for the old namespace layout.** Existing shadow data under the unprefixed namespace is not read, not moved, not detected.
- The disk path `shadow/...` does not change. `Cas::PartPathParser` and `ContentAddressedMetadataStorage::route` parse the disk path and must not be touched.
- No `LOGICAL_ERROR` anywhere in this work. Nothing here is input-reachable failure territory; this change adds no new failure mode.
- Allman braces (opening brace on its own line) — enforced by the style check.
- Comments must not cite this plan, the BACKLOG, a task number, or an issue number. Keep the reason, drop the provenance.
- **The controller owns builds, test runs and commits.** Do not run `ninja`, do not run `clickhouse-test`, do not run `python3 -m ci.praktika`, do not commit. Report when the tree is ready.
- This checkout is shared and has foreign uncommitted files. Never `git add -A`, `git add .`, or `git commit -a`; do not stage anything.

---

### Task 1: Pin the bug with a failing two-root isolation test

**Files:**
- Create: `tests/queries/0_stateless/05011_cas_freeze_two_roots.sh`
- Create: `tests/queries/0_stateless/05011_cas_freeze_two_roots.reference`

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: the test that Task 2 must turn green. Its name and path are fixed here so Task 2 can run it.

The test expresses the issue on ONE server, which is what a stateless test gives us. `SYSTEM UNFREEZE WITH NAME` is server-wide and would legitimately release both roots' freezes, so it cannot express "a foreign unfreeze"; the table-scoped form can:

```sql
ALTER TABLE tbl UNFREEZE PARTITION p WITH NAME 'backup'
```

Two CAS disks share one pool (same `path`), differing only in `server_root_id`. That is the stateless stand-in for two replicas on one pool.

Three assertions, and the third is the one that looks redundant and is not:

1. Root B's table unfreezing its own freeze must NOT release root A's freeze under the same backup name.
2. `DROP TABLE` on B plus a GC round must leave A's freeze intact.
3. Each root's own unfreeze still finds and removes its own freeze. Without this, the test stays green under a change that makes unfreeze find nothing at all.

- [ ] **Step 1: Write the failing test**

Create `tests/queries/0_stateless/05011_cas_freeze_two_roots.sh`:

```bash
#!/usr/bin/env bash
# Tags: no-fasttest
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.

# A FREEZE snapshot on a content-addressed disk belongs to the server root that made it. Two server
# roots sharing one pool is how two replicas of one table look from the pool's side, and UNFREEZE is a
# local, destructive statement: releasing one root's freeze must not touch the other's.
#
# Both freezes deliberately use the SAME backup name, because that is the case that collides: the
# pool namespace used to be derived from the shadow path alone, so both roots wrote the same
# namespace and either one's unfreeze released it for both.

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

UNFREEZE_STRUCTURE='command_type String, partition_id String, part_name String, backup_name String, backup_path String, part_backup_path String'

# Filter UNFREEZE output to the deterministic columns: backup_path and part_backup_path are absolute.
unfreeze_rows() {
    ${CLICKHOUSE_LOCAL} --structure "$UNFREEZE_STRUCTURE" \
        --query "SELECT command_type, partition_id, backup_name FROM table ORDER BY partition_id FORMAT TSVWithNames"
}

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS t_cas_freeze_root_a;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS t_cas_freeze_root_b;"

# Two disks, ONE pool (same path), two server roots.
${CLICKHOUSE_CLIENT} --query "
CREATE TABLE t_cas_freeze_root_a (k UInt32, v String)
ENGINE = MergeTree ORDER BY k PARTITION BY k
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    server_root_id = '05011_root_a',
    name = '05011_cas_freeze_a',
    path = '05011_cas_freeze_pool/');"

${CLICKHOUSE_CLIENT} --query "
CREATE TABLE t_cas_freeze_root_b (k UInt32, v String)
ENGINE = MergeTree ORDER BY k PARTITION BY k
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    server_root_id = '05011_root_b',
    name = '05011_cas_freeze_b',
    path = '05011_cas_freeze_pool/');"

${CLICKHOUSE_CLIENT} --query "INSERT INTO t_cas_freeze_root_a VALUES (1, 'a');"
${CLICKHOUSE_CLIENT} --query "INSERT INTO t_cas_freeze_root_b VALUES (1, 'b');"

# Same backup name from both roots: the colliding case.
${CLICKHOUSE_CLIENT} --query "ALTER TABLE t_cas_freeze_root_a FREEZE PARTITION 1 WITH NAME 'shared_05011';"
${CLICKHOUSE_CLIENT} --query "ALTER TABLE t_cas_freeze_root_b FREEZE PARTITION 1 WITH NAME 'shared_05011';"

${CLICKHOUSE_CLIENT} --query "SELECT 'frozen_a', count() FROM system.parts
WHERE database = currentDatabase() AND table = 't_cas_freeze_root_a' AND is_frozen AND active;"
${CLICKHOUSE_CLIENT} --query "SELECT 'frozen_b', count() FROM system.parts
WHERE database = currentDatabase() AND table = 't_cas_freeze_root_b' AND is_frozen AND active;"

# (1) B releases its own freeze. A's must be untouched.
echo 'unfreeze_b'
${CLICKHOUSE_CLIENT} --query "ALTER TABLE t_cas_freeze_root_b UNFREEZE PARTITION 1 WITH NAME 'shared_05011';" | unfreeze_rows

# (2) Drop B's table and run a GC round: A's freeze must survive both.
${CLICKHOUSE_CLIENT} --query "DROP TABLE t_cas_freeze_root_b;"
${CLICKHOUSE_CLIENT} --query "SYSTEM CAS COLLECT GARBAGE ON DISK '05011_cas_freeze_a';"

# (3) A's own unfreeze still finds and removes A's freeze. This is the assertion that fails if the
# fix makes unfreeze search the wrong subtree and find nothing: without it, a total leak reads as a
# clean pass, because "A's freeze was not destroyed" is also true when nothing can ever release it.
echo 'unfreeze_a'
${CLICKHOUSE_CLIENT} --query "ALTER TABLE t_cas_freeze_root_a UNFREEZE PARTITION 1 WITH NAME 'shared_05011';" | unfreeze_rows

${CLICKHOUSE_CLIENT} --query "DROP TABLE t_cas_freeze_root_a;"
${CLICKHOUSE_CLIENT} --query "SELECT 'dropped_ok';"
```

Create `tests/queries/0_stateless/05011_cas_freeze_two_roots.reference`:

```
frozen_a	1
frozen_b	1
unfreeze_b
command_type	partition_id	backup_name
UNFREEZE PARTITION	1	shared_05011
unfreeze_a
command_type	partition_id	backup_name
UNFREEZE PARTITION	1	shared_05011
dropped_ok
```

- [ ] **Step 2: Make the test executable**

```bash
chmod +x tests/queries/0_stateless/05011_cas_freeze_two_roots.sh
```

- [ ] **Step 3: Report for a run, and state the expected failure precisely**

Report to the controller. Expected on today's code: the `unfreeze_a` block comes back EMPTY (only the header, or nothing), because B's unfreeze already dropped the shared pool namespace that A's freeze also used. The single line of production code responsible is `ContentAddressedMetadataStorage::shadowNamespace`, which derives the namespace from the shadow path alone with no server-root prefix.

**Do not proceed to Task 2 until the controller confirms the test fails for that reason.** A test that fails for a different reason — the disk name in `SYSTEM CAS COLLECT GARBAGE` being wrong, `ALTER … UNFREEZE` not accepting this spelling, the pool refusing two roots — is a broken test, not a pinned bug, and fixing production against it proves nothing.

- [ ] **Step 4: Commit (controller only, after the failing run is confirmed)**

```bash
git add tests/queries/0_stateless/05011_cas_freeze_two_roots.sh \
        tests/queries/0_stateless/05011_cas_freeze_two_roots.reference
git commit -m 'Pin the cross-root FREEZE destruction on a content-addressed disk'
```

---

### Task 2: Prefix the shadow namespace and every scope that enumerates it

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h` (the `shadowNamespace` declaration; add the scope helper)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (the derivation; two enumeration scopes)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` (two call sites; one enumeration scope)

**Interfaces:**
- Consumes: the test from Task 1.
- Produces:
  - `Cas::RootNamespace ContentAddressedMetadataStorage::shadowNamespace(const std::string & shadow_table_dir) const` — no longer `static`.
  - `std::string ContentAddressedMetadataStorage::shadowScope(const std::string & path) const` — the server-root-scoped enumeration prefix for a shadow directory, ending in `/`.

All six sites change together. The `static` drop breaks compilation at the two call sites, which is deliberate: it makes the compiler enumerate them rather than trusting a grep.

- [ ] **Step 1: Change the derivation and add the scope helper**

In `ContentAddressedMetadataStorage.h`, replace the `shadowNamespace` declaration:

```cpp
    /// Canonicalizes a literal shadow-table directory into the pool namespace used by freeze and
    /// unfreeze paths, rooted at this server's own `server_root_id` exactly as `liveNamespace` is. A
    /// trailing slash is ignored.
    ///
    /// A freeze belongs to the server root that made it. UNFREEZE is local and destructive, so a
    /// pool-global namespace let one replica's unfreeze release another's backup and the blobs then
    /// went with the next collection round. Cross-replica READABILITY of a freeze goes away with it;
    /// shared access to a backup belongs to the BACKUP machinery, not to UNFREEZE.
    Cas::RootNamespace shadowNamespace(const std::string & shadow_table_dir) const;

    /// The namespace-enumeration prefix for a shadow directory: the same server-root scoping
    /// `shadowNamespace` applies, for callers that enumerate a subtree instead of naming one
    /// namespace. Always ends in '/'. `path` empty means the disk root's whole shadow subtree.
    ///
    /// One helper on purpose: three callers need this prefix, and a shadow namespace that is
    /// enumerated with a differently-built prefix is invisible — which for the unfreeze path means it
    /// silently stops releasing anything.
    std::string shadowScope(const std::string & path) const;
```

In `ContentAddressedMetadataStorage.cpp`, replace the body:

```cpp
Cas::RootNamespace ContentAddressedMetadataStorage::shadowNamespace(const std::string & shadow_table_dir) const
{
    /// The LITERAL shadow table dir (shadow/<backup>/store/<u3>/<uuid> or .../data/<db>/<tbl>) is
    /// bijective with the disk path for both layouts; the disk path itself is unchanged by the server
    /// root prefix. Canonicalize because the unfreezer can hand the directory a trailing slash.
    return Cas::RootNamespace{serverPrefix() + "/" + canonicalDiskPath(shadow_table_dir)};
}

std::string ContentAddressedMetadataStorage::shadowScope(const std::string & path) const
{
    const std::string canonical = canonicalDiskPath(path);
    return serverPrefix() + "/" + (canonical.empty() ? "shadow/" : canonical + "/");
}
```

- [ ] **Step 2: Point the two enumeration scopes in the metadata storage at the helper**

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

This one needs the prefix for the same reason as the namespace scope even though it queries the mirrored tree rather than the catalog: a mirrored object key is built as `<pool>/roots/<namespace path>`, so moving the namespace moves its mirrored files with it.

- [ ] **Step 3: Fix the two call sites and the bulk-drop scope in the transaction**

The two calls become member calls through the storage reference the transaction already holds:

```cpp
            const auto ns = metadata_storage.shadowNamespace(p->shadow_table_dir);
```

```cpp
            metadata_storage.dropNamespace(metadata_storage.shadowNamespace(path));
```

For the second, keep the existing call shape and change only the namespace expression — the surrounding statement is `metadata_storage.partAccess()->dropNamespace(...)`; do not restructure it.

Then the bulk drop behind `UNFREEZE WITH NAME`. Replace:

```cpp
        const Cas::NamespaceListing listing = metadata_storage.store()->listNamespaces(prefix + "/");
```

with:

```cpp
        const Cas::NamespaceListing listing
            = metadata_storage.store()->listNamespaces(metadata_storage.shadowScope(prefix));
```

**This is the site the original adjudication missed, and the one worth checking twice.** Prefix the derivation without prefixing this, and `UNFREEZE WITH NAME` enumerates the unprefixed subtree, finds nothing, and stops releasing its own freeze — the cross-replica destruction is gone and every frozen ref leaks forever instead. The `prefix` local here is already stripped of trailing slashes, and `shadowScope` canonicalizes anyway, so passing it directly is correct; do not append a slash.

- [ ] **Step 4: Report for a build and a run**

Report to the controller with the list of sites touched. Expected:

- `tests/queries/0_stateless/05011_cas_freeze_two_roots.sh` — PASSES.
- `tests/queries/0_stateless/05003_cas_freeze.sh` — STILL PASSES. This one is the leak guard: it asserts that `UNFREEZE WITH NAME` finds and removes its own snapshot on a single root, so if Step 3's bulk-drop scope were missed, 05003 fails.
- The full `CAS*:Cas*:CA*` gate — unchanged.

State plainly which of these you verified by reading and which only the run can settle. You cannot run any of them.

- [ ] **Step 5: Commit (controller only, after a green run)**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
git commit -m 'Scope a content-addressed FREEZE snapshot to the server root that made it'
```

---

### Task 3: Retire the shadow-is-a-separate-root concept from the prose

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (the lifecycle grouping comment listing `shadowNamespace` among pure computations)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h` (the `listMirroredChildren` contract)

**Interfaces:**
- Consumes: Task 2's landed change.
- Produces: nothing later tasks depend on.

Task 2 does not merely move a prefix; it removes a special case. Shadow used to be a second relative root alongside the server root, and now it is ordinary server-relative content. Two comments still describe the old model, and one of them is an API contract.

- [ ] **Step 1: Correct the `listMirroredChildren` contract**

It currently says `prefix` is "a server-relative or shadow-relative path ending in '/'". Shadow is no longer a separate relative root, so replace that clause with: `prefix` is a server-relative path ending in `/`. Say nothing about shadow — that is the point: it needs no mention because it is no longer special.

- [ ] **Step 2: Keep the lifecycle grouping comment true**

The grouping comment lists `serverPrefix/liveNamespace/shadowNamespace/route/classifyDirectory` as "pure path computation, no pool I/O". That stays TRUE after Task 2 — `serverPrefix()` returns a member set in the constructor and does no pool I/O — so the only change needed is to add `shadowScope` to the same list, beside `shadowNamespace`. Do not weaken the "no pool I/O" claim; verify it rather than assuming, by checking that `serverPrefix()` still just returns the member.

- [ ] **Step 3: Report for a build**

Comment-only changes cannot alter behaviour, but they are in a `.cpp` and a `.h`, so the controller rebuilds and re-runs the `CAS*:Cas*:CA*` gate to confirm the tree is still green. Say explicitly in your report that you changed no code.

- [ ] **Step 4: Commit (controller only)**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
git commit -m 'Shadow content is ordinary server-relative content now'
```

---

## Optional simplification, only if Task 2 landed clean {#optional-simplification}

Not part of the fix. Offer it to the controller as a separate decision; do not fold it into Task 2.

After Task 2, `liveNamespace` and `shadowNamespace` have the same shape — `serverPrefix() + "/" + <a mirrored path>` — differing only in how the path is derived. A single `serverScoped(path)` helper used by both would make "everything this server owns is rooted at its own id" explicit in code instead of by convention repeated twice, and would give the next namespace kind one obvious place to hook into.

The argument against is equally real: two callers is a thin basis for an abstraction, and the two derivations are not otherwise related. Take it only if the controller judges the invariant worth naming.

---

## Self-review

**Spec coverage.** The spec's six sites map to Task 2 Steps 1-3: the derivation (Step 1), the two metadata-storage enumeration scopes (Step 2), the two transaction call sites and the bulk-drop scope (Step 3). Spec item (3) — parser and `route` untouched — is a Global Constraint. Item (4), no migration, is a Global Constraint. Item (5), the test, is Task 1, including the third assertion the spec calls out as non-redundant. Item (6), that nothing asserts cross-replica visibility, needs no task; it is why Task 3 can delete the concept rather than preserve it.

**Placeholders.** None. Every code step carries the actual before/after text; the test and its reference file are given in full.

**Type consistency.** `shadowNamespace` is declared and used as a const member returning `Cas::RootNamespace` in both files that call it. `shadowScope` is declared and used as a const member returning `std::string` at all three enumeration sites. The transaction reaches both through `metadata_storage`, which it holds as `ContentAddressedMetadataStorage &`.

**One risk the plan cannot remove.** Task 1's reference file predicts the exact `UNFREEZE PARTITION` output shape. If the real `command_type` string differs from `UNFREEZE PARTITION`, the reference is wrong and the first run will say so — that is a reference fix, not a production finding, and it must not be treated as one.
