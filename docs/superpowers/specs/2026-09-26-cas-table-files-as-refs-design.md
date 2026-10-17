---
description: 'Design study, rev.2: record the top-level entries of a CAS table directory (table-level files and subdirectories) in the ref log, so every directory probe is answered from the resident ref table. File objects stay where they are and are still modified in place; only creation and removal of a top-level entry appends an op. Replaces rev.1 (file contents as refs, dedup claims), which review round 1 rejected. Alternative to the striped cache of spec 2026-09-26-cas-directory-probes-no-list-design.md §3.2.'
sidebar_label: 'CAS table directory entries in the ref log'
sidebar_position: 13
slug: /superpowers/specs/cas-table-files-as-refs-design
title: 'CAS table directory entries in the ref log'
doc_type: 'design'
---

# CAS table directory entries in the ref log — rev.2, closed (2026-09-27) {#cas-table-dir-entries}

Design study, no code. Rev.1 of this file proposed storing table-level file contents as refs and the
deduplication window as claims in the ref log; codex review round 1
(`docs/superpowers/reports/2026-09-26-cas-table-files-as-refs-codex-reviews/review_r1.md`) rejected
it: content migration is not idempotent with the existing primitives, the deduplication log on a CAS
disk creates a file per insert so claims or file refs would cost a durable write per insert, and the
`MergeTree` touch was larger than stated. Rev.2 keeps the idea that made rev.1 attractive, a
resident and durable answer for directory probes, and drops everything else: file objects stay under
`cas/ns/state/<life_id>/_files/`, modified in place as today; the ref log records only which
top-level entries the table directory has.

CAS-95.1 (`PartFile` shape for paths inside a part) is independent and needed either way. The
competing design is the striped cache of `2026-09-26-cas-directory-probes-no-list-design.md` §3.2.

Source paths are relative to `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` and
line numbers refer to `altinity/antalya-26.6` at `8d62c314ec1`. This is a format change (two ref-log
op kinds and one snapshot section), so it revisits decision 4: reader generation 2, no compatibility
read path, downgrade not supported once a namespace carries generation 2 ops.

## 1. What the table directory contains today {#today}

Under a table directory on a CAS disk, parts are refs (ref table: snapshot plus log under `cas/ns/`,
resident as `RefTableRuntime`), `detached/` and `moving/` are ref prefixes, and everything else is a
plain object under the life's `_files/` prefix (`Formats/CasLayout.h:254`), written with
HEAD-before-PUT by `CasPlainObjects` and enumerated only by a LIST of that prefix.

Top-level entries `MergeTree` creates: `format_version.txt` (`MergeTreeData.cpp:508-559`),
`tmp_mutation_<n>.txt` moved to `mutation_<n>.txt` (`MergeTreeMutationEntry.cpp:51-95`, CSN appended
at `:111-116`), the directories `detached` (`MergeTreeData.cpp:518-519`), `deduplication_logs`
(`MergeTreeDeduplicationLog.cpp:97-98`) and `partition_exports`
(`MergeTreePartitionExportScheduler.cpp:55-63`). Inside `deduplication_logs/` a new segment is
created on every insert, because the CA disk does not support append and the log rotates per record
(`MergeTreeDeduplicationLog.cpp:240`), and outdated segments are removed (`:227`). Non-MergeTree
engines put their data files at top level: `Log` writes `<column>.bin`, `__marks.mrk`, `sizes.json`,
`StripeLog` writes `data.bin`, `index.mrk`, `sizes.json`, all rewritten in place on every insert
(probe `docs/superpowers/reports/2026-09-27-cas-non-mergetree-engines-probe.md`; persistent `Join`
and `Set` are broken by a parser rule, CAS-318, independent of this design).

The probes that cost a LIST (issue #2439 steady state, audit F16): `listDirectory` of the table
directory (`ContentAddressedMetadataStorage.cpp:1842-1861`, called by
`clearOldTemporaryDirectories` once a minute per table), `existsDirectory` and `listDirectory` of a
subdirectory (`:1702-1712`, `:1895-1904`), and the same enumeration in `removeRecursive`
(`ContentAddressedTransaction.cpp:1164`) and table rename (`:1295`).

## 2. Goals and non-goals {#goals}

Goals:

- Every probe of the table directory itself (`listDirectory`, `existsFile`, `existsDirectory` of a
  top-level name) is answered from the resident ref table. No LIST after mount.
- A restart costs one LIST per live table, never per part or per file.
- File contents keep today's write path: in-place HEAD-before-PUT, no manifest, no garbage, no
  extra request per insert for the deduplication log or the `Log` family.
- No change to upstream `MergeTree` code.
- Migration is non-destructive and needs no repair: objects are not moved.

Non-goals:

- Enumerating the contents of a subdirectory without a LIST (`listDirectory` of
  `deduplication_logs`, once at table load; `removeRecursive` of a subdirectory; the file copy loop
  of table rename). Those are rare and already bounded by one LIST per call.
- Sizes or modification times in the ref table: `getFileSize` and reads go to the object as today.
- The catalog GETs of the `detached` probe (CAS-95.3) and the parser rule of CAS-318.

## 3. Design {#design}

### 3.1 The directory table {#directory-table}

Each namespace's ref table gains a **directory table**: a set of top-level entries, each
`(name, kind)` with `kind ∈ {File, Directory}`. It records exactly the names that live under the
table directory and are neither part refs nor the reserved containers `detached` and `moving`
(those stay answered from refs, `:1712-1720`, and shadow namespaces are untouched).

Two ref-log op kinds (`Formats/CasRefLogFormat.h:35`): `DirEntryAdded = 6` with `ref_name` (the
entry name) and `entry_kind`, and `DirEntryRemoved = 7` with `ref_name`. The snapshot
(`Formats/CasRefSnapshotFormat.h:53`) gains a `dir_entries` section sorted by name.
`RefTableState::applyOp` (`Pool/CasRefProtocol.h:274`) applies both: add of a present name with the
same kind is a no-op, add with a different kind replaces, remove of an absent name is a no-op; the
snapshot byte accounting counts the name. Ops are commutative for different names, so concurrent
writers of one table need no coordination beyond the ref lane's own serialization of appends. A name
is at most a path component, far below `ref_op_max_bytes` (`Formats/CasRefLogFormat.h:112`).

### 3.2 Operation mapping {#mapping}

| operation | today | after |
|---|---|---|
| `listDirectory(<table>)` (`:1842-1861`) | refs plus LIST | refs plus directory table |
| `existsFile(<table>/<name>)` (`:1500-1505`) | catalog life, GET | directory table: `File` |
| `existsDirectory(<table>/<name>)` (`TableSubdir`, `:1702-1712`) | LIST | directory table: `Directory` |
| `listDirectory(<table>/<dir>)`, `existsDirectory(<table>/<dir>/<sub>)` | LIST | LIST of `_files/<dir>/` (unchanged, rare) |
| `existsFile(<table>/<dir>/<name>)`, `readFile`, `getFileSize`, all reads | GET | GET (unchanged) |
| `writeFile(<table>/<name>)` create (`ContentAddressedTransaction.cpp:825-851`) | HEAD, PUT | HEAD, PUT, then `DirEntryAdded{File}` |
| `writeFile` of an existing top-level file, rewrite or append | HEAD, PUT | HEAD, PUT (no op: the entry exists) |
| `writeFile(<table>/<dir>/<name>)` | HEAD, PUT | HEAD, PUT (no op: entries are top-level only) |
| `createDirectory(<table>/<dir>)` (`:1007`, no-op today) | nothing | `DirEntryAdded{Directory}` |
| `removeDirectory(<table>/<dir>)` (`:1020`) | today's object-side behaviour | same, then `DirEntryRemoved` |
| `unlinkFile(<table>/<name>)` (`:1655-1667`) | HEAD, DELETE | `DirEntryRemoved`, then HEAD, DELETE |
| `unlinkFile(<table>/<dir>/<name>)` | HEAD, DELETE | HEAD, DELETE (unchanged) |
| `moveFile`, `replaceFile` at top level (`:1470-1490`) | GET, PUT, DELETE | same, plus `DirEntryAdded` for the destination and `DirEntryRemoved` for the source |
| `removeRecursive(<table>/<dir>)` (`:1155-1170`) | LIST, DELETE per file | same, then `DirEntryRemoved` |
| table rename (`:1285-1305`) | `republishRef` loop, GET and PUT per file | same, plus `DirEntryAdded` per entry in the destination namespace |
| drop table | `dropNamespace`, janitor of `_files/` | unchanged (the directory table dies with the namespace) |

Ordering and crash windows. A file entry is added after its object is durable and removed before
its object is deleted, so an entry never names a missing object; a crash between the two steps leaves
an object without an entry, which is invisible, reported by fsck as an orphan, and reclaimed with the
life by the janitor. A directory entry may legitimately name an empty directory, as on a local disk
between `mkdir` and the first file. Whether the write of the object or the append of the op fails,
the exception propagates; nothing is guessed.

`existsFile` of a top-level name no longer touches the object. The only way for the table to say
`File` while the object is absent is a deletion that bypassed the transaction (an operator or a
generation 1 tool), which fsck reports.

### 3.3 Mount-time resync instead of a migration marker {#resync}

At every writable mount of a generation 2 build, for each cataloged `Live` life of this server root,
before the disk serves any request: one LIST of the life's `_files/` prefix, from which the top-level
names are derived (a name without `/` is a `File`, the first component of a nested name is a
`Directory`), and a single ref transaction appends `DirEntryAdded` for every entry the resident table
lacks and `DirEntryRemoved` for every entry the LIST does not support. The transaction is empty on a
clean mount and costs nothing; on the first mount after the upgrade it is the migration; on a mount
after a generation 1 build wrote to this root (a downgrade and re-upgrade during the rollout) it is the
repair. Objects are never moved or deleted by this step, so it needs no idempotence argument: it is a
reconciliation that can be repeated at will.

Cost: one LIST per live table per mount. That is the bound issue #2439 asks for.

Trust: the LIST at mount is a recovery-time cold LIST, trusted exactly as the ref-table recovery
trusts its own LIST of the log prefix (decision 2, the settled LIST-trust verdict); the hot LISTs it
replaces were the untrusted ones.

### 3.4 Generations, other builds, the floor {#generations}

`G_BUILD` (`Formats/CasFormat.h:21`) becomes 2 and every object a generation 2 build writes is
stamped with it (`:61-67`). A generation 1 build that decodes a ref transaction with an unknown op
kind throws `CORRUPTED_DATA` (`refOpKindFromWireWord`), which is used by the ledger, the GC fold
(`Gc/CasGc.cpp`, `Gc/CasOrphanManifestSweep.cpp`), fsck and inspect. So:

- A generation 1 node on its own unmigrated root works unchanged; nothing of it is touched.
- A generation 1 node reading a migrated namespace of another root (decommission takeover, the
  pool-wide GC fold, fsck, inspect) fails closed at decode. During a rolling upgrade, GC rounds run by
  generation 1 nodes abort on the first migrated namespace they fold; reclamation resumes when a
  generation 2 node runs the round. This is the degraded window and it is bounded by the rollout.
- A generation 1 build mounting a root after a generation 2 mount of that root fails closed at
  ref-table recovery of the first migrated namespace: downgrade is not supported, as accepted.

The pool floor (`min_reader_generation`, `Formats/CasPoolMetaFormat.cpp:155-158`) exists to turn the
recovery-time failure above into a mount-time error with a message. A generation 2 node raises it to 2
by CAS on the pool meta when every server root under `gc/server-roots/` (`Formats/CasLayout.h:417`,
the prefix the GC heartbeat already lists) has a mount object stamped with generation 2, that is,
when every root has been mounted by an upgraded build at least once. It is never raised by the first
node, and never while an unstamped root exists; an operator can also raise it through the fsck tool.
Read-only mounts do not resync and do not consult the directory table; the tools that use them read
objects, not directories.

### 3.5 What is removed and what stays {#removed}

Removed: the LIST in `listDirectory(TableDir)` and in the two `TableSubdir` branches for top-level
names; the GET in `existsFile` of a top-level name; the whole striped cache of the competing spec
(§3.2 there) and backlog task CAS-95.2. Stays: `_files/` objects and `CasPlainObjects`; the LIST for
the contents of a subdirectory; the file copy loop of rename; the janitor and fsck classification of
`_files/` keys; `PartFile` (CAS-95.1).

## 4. Risks, ranked {#risks}

1. **Rolling upgrade window.** Generation 1 GC rounds abort on migrated namespaces (§3.4). To be
   verified in the plan: that the fold aborts the round and never misfolds (the decoder throws before
   any state is applied). Mitigation: upgrade a pool's nodes in one window; the floor raise is
   automatic only when all roots are upgraded.
2. **No downgrade after the first generation 2 mount of a root.** Accepted; the floor gives the
   clear error once all roots are upgraded, the decoder gives `CORRUPTED_DATA` before that.
3. **Entry/object drift by tools.** A deletion or creation of a `_files/` object outside the
   transaction (generation 1 tools during the rollout, an operator) is repaired at the next mount
   resync and reported by fsck between mounts. Between a drift and the resync, `existsFile` can
   answer `File` for a missing object; the read then fails with the object's own error, which is what
   a local disk does when a file is removed under a running server.
4. **Format surface.** Two op kinds, one snapshot section, `applyOp`, scope and admission validation
   in `Pool/CasRefProtocol.cpp`, codecs, fsck and inspect printing, the TLA+ model's op set. Each is a
   mechanical extension of a place that already switches on the op kind.
5. **Directory semantics change slightly.** Today a subdirectory exists iff it has a file; after, it
   exists from `createDirectory` until `removeDirectory` or `removeRecursive`, like a local disk. The
   `MergeTree` callers create their directories explicitly (§1) and remove them on drop, so no caller
   observes the difference except an empty `deduplication_logs` after a window change, which is the
   local-disk answer too.
6. **CAS-318 is not fixed here.** Persistent `Join` and `Set` still fail on `tmp/<n>.bin`; the
   directory table would record `tmp` as a `Directory` correctly once the parser stops claiming it as
   a part.

## 5. Tests, failing-first {#tests}

Format (existing codec suites):

1. `DirEntryAdded`/`DirEntryRemoved` round-trip; the generation 1 decoder rejects them with
   `CORRUPTED_DATA`; the snapshot `dir_entries` section round-trips sorted.
2. `applyOp` semantics: add present same kind is a no-op, add with a different kind replaces, remove
   absent is a no-op; snapshot bytes count the names.

Directory table (`gtest_ca_wiring`, new `gtest_cas_dir_entries` over `CountingBackend`):

3. Create `format_version.txt`, `mutation_1.txt`, directory `deduplication_logs` with two segments,
   then `listDirectory(<table>)`, `existsFile`, `existsDirectory` of each top-level name: correct
   answers, `listTotal() == 0` and no GET for the top-level probes.
4. `listDirectory(<table>/deduplication_logs)` lists the segments with exactly one LIST; creating a
   third segment costs HEAD and PUT and no ref-log append.
5. `unlinkFile` of a top-level file: the entry disappears before the object; a fault injected between
   the op and the DELETE leaves an orphan the next resync ignores and fsck reports.
6. `moveFile` `tmp_mutation_1.txt` to `mutation_1.txt`: entries follow; `removeRecursive` of
   `deduplication_logs` removes the entry; `removeDirectory` of an empty directory removes it;
   table rename carries the entries; drop ends them.
7. Restart: after a snapshot and after log-only, the directory table is recovered and the first
   `listDirectory` issues no LIST beyond the mount resync's one per table.
8. Non-MergeTree: `Log` and `StripeLog` on the CAS disk insert, restart, append, truncate and drop
   with the same request profile as the probe report, plus zero LIST after mount.

Resync and generations (`gtest_cas_mount_resync`, integration `test_cas_upgrade_g1_to_g2` on a
26.6.4 `cas-nightly` data set):

9. Mount over a generation 1 pool: one LIST per live table, entries derived correctly for files,
   nested names and empty tables; a second mount appends an empty transaction.
10. Drift repair: delete a `_files/` object behind the server's back, remount: the entry is gone;
    create one, remount: the entry is present.
11. Generation 1 build on a migrated root fails closed at recovery with `CORRUPTED_DATA`; on its own
    unmigrated root it works; a generation 1 GC round over a pool with one migrated namespace aborts
    the round and applies nothing.
12. Floor: with two roots, the floor stays 1 after the first upgrade and becomes 2 after the second
    root's generation 2 mount; a generation 1 build then fails at mount with `UNKNOWN_FORMAT_VERSION`.
13. Stateless on a CAS disk: `clearOldTemporaryDirectories` over two minutes issues zero
    `CASRootList` (the acceptance of CAS-95 #2); restart with 2 tables × 20 and 2 tables × 200
    parts issues the same `CASRootList` count (acceptance #1, together with CAS-95.1).

## 6. Size and comparison {#size}

Estimated: codecs, `applyOp` and validation 300-400 lines; metadata storage and transaction changes
about 200 lines, net negative in the probe branches; mount resync about 100 lines; fsck, inspect and
TLA+ updates; tests the largest part. One and a half to two weeks with review, against about one week
for the striped cache and two to three weeks for rev.1.

Against the striped cache (`2026-09-26-cas-directory-probes-no-list-design.md` §3.2): the cache
derives names from a hot LIST and protects them with a mutex held across I/O, needs budget
accounting, and re-lists after every runtime eviction or remount; here the names are written by the
writer into the log it already appends to, recovered with the rest of the ref table, and never listed
after mount. The price is a format generation and the rolling-upgrade window of §3.4.

Against rev.1: no content moves, no per-insert cost, no `MergeTree` change, no claims.

## 7. Questions for review {#questions}

1. Is there a smaller or more elegant way to a resident, durable directory table than two op kinds
   and a snapshot section? One considered: a reserved ref `_files` whose manifest lists the entries
   as empty inline files, with no codec change; rejected here because every entry change would
   repoint a manifest (a PUT plus a conflict retry between concurrent writers) and generation 1 builds
   would still misread the reserved name as a part-shaped entry.
2. Is the mount-time resync (§3.3) sound as the only migration and repair mechanism, and is one LIST
   per live table per mount acceptable on the largest known pools?
3. Is the floor rule (§3.4, raised when every root's mount object is generation 2) safe for
   multi-node pools with dead or decommissioned roots?
4. Does the generation 1 GC fold fail closed on an unknown op kind before applying anything, and is
   the degraded window acceptable?
5. Is recording directories explicitly (§3.2, `createDirectory` and `removeDirectory` become ops)
   the right semantics, or should a directory entry be implied by its first nested object?

## 8. Conclusion after two review rounds {#conclusion}

Round 2 (`docs/superpowers/reports/2026-09-26-cas-table-files-as-refs-codex-reviews/review_r2.md`)
found the rev.2 mechanism materially smaller than rev.1 but still larger than the striped cache it
competes with, and not converging in one revision. The findings that decide it:

- Durability does not remove the same-name serialization the cache needs: the object PUT and
  DELETE happen outside the ref lane, so a rewrite racing an unlink of the same name, or the mount
  LIST racing a writer, still needs a stripe held across object I/O and table mutation.
- Two authorities contradict each other: `DirEntryRemoved` before DELETE plus a LIST-driven resync
  resurrects a file whose DELETE failed; an explicit empty directory is removed by the next resync
  because no object supports it. Fixing either needs tombstones or marker objects, that is, more
  machinery.
- The format surface is a full generation: ref-log codec, snapshot row grammar (a generation 2
  build must still read generation 1 snapshots), `applyOp`, scope validation, admission, fsck,
  inspect, two TLA+ models, `CasFormat.cpp` breaking-change registration, and the floor rule with
  every state a mount slot can be in.
- `createDirectories` reaches `createDirectoryRecursive` (`ContentAddressedTransaction.cpp:1015`),
  not `createDirectory`; `partition_exports` is parsed as a part today (CAS-318 class), so routing
  changes are required before any directory table can answer for it.

What survives from this study is round 2's own alternative, which is the cache with lazy population
replaced by population at mount: one LIST per live table before the disk serves any request, the
names pinned for the mount's lifetime (dropped with the mount runtime on fence loss, rebuilt at
remount), write-through under a plain mutex with no I/O under it. It keeps the request bound of §2,
needs no format change, no floor, no resync authority argument and no striped mutex, because no LIST
ever overlaps a writer. That is the next revision of the competing spec, not of this one.
