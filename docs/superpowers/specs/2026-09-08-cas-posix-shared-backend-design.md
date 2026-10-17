---
description: 'Design for a coordinator-free CAS backend over local and shared POSIX filesystems (local disks, NFSv3/v4, CephFS, Lustre, GPFS): the full Cas::Backend contract from link, mkdir, rename, unlink and rmdir, where every conditional operation is a server-decided create-if-absent of a deterministic successor name; names inside a chain are never freed and chains are freed only as a whole into unique names; chain creation is sourced from a uniquely named intent declared before the predecessor is observed, and a chain is freed only when no intent on it exists — so a delayed or stalled RENAME is refused at the server by a missing source, as If-Match is refused by S3; durable through fsync; no in-process state, no clock, no bound on pauses; replaces EmulatedSingleProcess.'
sidebar_label: 'CAS POSIX shared backend'
sidebar_position: 50
slug: /superpowers/specs/cas-posix-shared-backend-design
title: 'CAS backend over local and shared POSIX filesystems'
doc_type: 'design'
---

# CAS backend over local and shared POSIX filesystems {#cas-posix-shared-backend}

Revision 10, 2026-09-10. Nine review rounds (`tmp/cas-posix-spec-review-astra-r{1..9}.md`)
established: on POSIX an occupied name is the only fence; any name freed while a stalled writer still
aims at it is a false-success target; the stall may be inside the syscall; and a source revoked by
`rename` does not stop an NFS `LINK` that already resolved its file handle, while it does stop a
`RENAME`, which names its source. Revision 10 uses exactly two kinds of names:

- **names inside a chain** (`<g>-<n>`), which are never freed while the chain exists — a delayed
  `link` at such a name meets `EEXIST`, or lands in a chain that was moved as a whole into a
  unique `.gone-*` directory nobody reads;
- **chain names** (`c-<x>-<m>`), unique for all time — `x` is a random token drawn once per life of
  the object directory and `m` is monotone within that life — created only by
  `rename` of a uniquely named **intent** declared before the predecessor was observed, and freed
  only when no intent on them exists — a delayed `RENAME` from a killed intent is refused at the
  server by its missing source name, whenever it executes.

No clock, no bound on pauses, no handle-identity assumption. Revision 10 adds the **life token**
(`§3.4`): because every chain name carries the token of the `D(k)` life it belongs to, a chain name
never recurs even after `D(k)` is removed and the key reborn, so `D(k)` is removable on every plane
and a deleted key leaves nothing behind. Revision 9's attempt to reach the same end with several
coexisting live chains per blob was refuted (`tmp/cas-posix-spec-review-astra-r9.md`: a condemned
incarnation that physically survives a republication can become adoptable again while its delete
is still in flight) and is withdrawn: one observable incarnation per key, as on S3. Decisions taken
with the user: coordinator-free (no Keeper); one mode for local and shared filesystems; SMB out of
v1 (`§2`); no per-deleted-key residue.

## 1. Problem {#problem}

`object_storage_type = local` routes a CAS disk to `ObjectStorageBackend::Mode::EmulatedSingleProcess`
(`ContentAddressedMetadataStorage.cpp`, `openPoolView`): the conditional contract of `Cas::Backend`
emulated with one process-wide `std::mutex` and an in-memory `mtime`-derived token map. Correct for
one process, silently wrong for two, and not crash-durable even for one (no `fsync` anywhere; mutable
control objects rewritten in place with `O_TRUNC`).

The goal is one backend mode, `Mode::Posix`, that satisfies the whole `Cas::Backend` contract
(`Backend/CasBackend.h`) on every filesystem in `§2`, with no in-process state and no assumption
about how long a process or a syscall may stall. The request engine, GC decisions, mount lease, ref
lanes and `_ckpt` are unchanged, and the only persisted-format change is one additive field
(`§9`, decommission); what the mode needs outside the backend is in `§9`.

## 2. Supported filesystems and the facts relied on {#filesystems}

| Filesystem | Status | Mandatory mount options (`§6`) |
|---|---|---|
| Local ext4 / xfs / btrfs / zfs | supported; one process; replaces `EmulatedSingleProcess` | none |
| NFSv4, 4.1, 4.2 | supported | effective `hard`, `lookupcache=none`, and `softreval` **absent** |
| NFSv3 | supported | effective `hard`, `lookupcache=none`, and `softreval` **absent** |
| CephFS, Lustre | supported (coherent caches; `RENAME` names its source on both) | none |
| GPFS | supported on the same facts only after the two-client suite of `§11` has been run on it; there is no CI recipe for it, so it is not certified in v1 | none |
| SMB / CIFS | not in v1: the Linux client keeps a negative dentry for about one second regardless of options (`fs/smb/client/dir.c`) | — |

`lookupcache=none`, not `positive`: a positively cached dentry lets a client keep resolving a
directory by a name it no longer has after another client renamed it, so both the post-check of
`§3.3` and a reader's chain selection could bind to a moved chain (`fs/nfs/dir.c`,
`nfs_do_lookup_revalidate`). With `lookupcache=none` every path component is looked up at the
server; the cost is one `LOOKUP` per component of the paths the backend resolves (all short). The
soft-revalidation bit must be clear: `softreval` (combinable with `hard`, and also set by `softerr`;
option order matters, `hard` clears it) makes `nfs_lookup_revalidate_done` accept a cached positive
binding when the revalidating `LOOKUP` times out, which would let the post-check and a reader bind
to a moved chain exactly as a cached dentry would. The gate checks the **kernel-reported** options
(`/proc/self/mountinfo` renders `softreval` when the bit is set and nothing when it is clear;
Linux v6.12 does not even parse a `nosoftreval` token), so the requirement is "`softreval` absent",
never a literal negative token. Directory delegations are
defined by NFSv4.1 (`GET_DIR_DELEGATION`) but not implemented by the Linux client; an open dirfd
keeps naming its directory after a rename, which is why hints from it are hints only.

Facts relied on, each decided at the server and verified by the probe (`§8`):

1. `link(src, dst)` fails `EEXIST` when `dst` exists (`EEXIST` also when `dst` is a directory); on
   success `dst` and `src` are one inode.
2. `mkdir(p)` fails `EEXIST` when `p` exists.
3. `rename(src, dst)` **names its source**: it fails `ENOENT` when the source name no longer exists,
   whatever handles anyone holds; for directories it fails (`ENOTEMPTY`/`EEXIST`) when `dst` is a
   non-empty directory and `ENOTDIR` when `dst` is a file; onto an absent name it succeeds atomically.
4. `rmdir(p)` fails `ENOTEMPTY` when `p` has any entry.
5. A `LOOKUP` of an absent name is answered by the server, not by a client cache (`§6`).
6. A client's own create/unlink/rename in a directory invalidates that client's cached listing of it
   before a *new* directory stream is read (Linux NFS: `nfs_post_op_update_inode` on the parent sets
   `NFS_INO_INVALID_DATA` for v3 creates in `nfs3proc.c`, `nfs4_update_changeattr_locked` for v4 in
   `nfs4proc.c`; `nfs_readdir` revalidates the mapping before serving cached pages).

Not relied on: `O_EXCL`, `flock`/`fcntl`, extended attributes, `renameat2`, timestamps, handle
identity across time (a stale handle's `ESTALE` is an ambiguity, `§5`), `unlink` as a revocation of
an in-flight `LINK` (Linux NFS silly-renames a referenced file; a local `vfs_link` proceeds on a
resolved inode), listing order or completeness, any clock, any bound on the duration of a pause or
of a syscall.

## 3. Layout {#layout}

```
P(k)/D(k)/                            the object directory; one LIFE at a time; removed when it holds no chain (§4.4)
P(k)/D(k)/c-<x>-<m>/                  chain m of life x (x = 16 hex random per life of D(k); m = 16 hex, monotone within the life)
P(k)/D(k)/c-<x>-<m>/<g>-<n>           incarnation n; g = 16 hex random per chain, n dense from 1
P(k)/D(k)/c-<x>-<m>/<g>-<n>/          a MARKER (directory): the tombstone that ends a chain
P(k)/D(k)/c-<x>-<m>/.tmp-<u>          scratch for a data incarnation (not an intent)
P(k)/D(k)/c-<x>-<m>/.tomb-<u>/        scratch for a tombstone inside a chain (not an intent)
P(k)/D(k)/.new-<r>-<e>-<u>/           an INTENT to create the next chain of the current life: root r, epoch e (0 at bootstrap), uuid u
P(k)/.new-<r>-<e>-<u>/                a private directory that becomes D(k) itself (a new life), already holding c-<x>-1
P(k)/D(k)/.gone-<u>/                  a chain, or a killed intent, on its way to deletion
<dir>/.d-<u>                          a listing-freshness dotfile (§4.6), created and removed at once
```

### 3.1 Values and the current incarnation {#values}

The contract's **value** is `c-<m>/<g>-<n>`, minted by the writer before the write. An incarnation
file is written into scratch, linked to its final name, and never modified: `VALUE ⟹ CONTENT` by
construction. The **current incarnation** is the top of the greatest chain, both found by
**lookup, never by listing**: `readdir` gives a hint (which also yields the life token `x`, shared
by every chain in `D(k)`), then `fstatat` probes `c-<x>-<m+1>`, …, then `<g>-<n+1>`, … until the
server answers `ENOENT` (fact 5). Two invariants make the probe sound:
names inside a chain are never freed (`§3.3`), and chains are reclaimed **in order, lowest first**
(`§4.4`), so "`c-<x>-<m>` exists and `c-<x>-<m+1>` does not" means `c-<x>-<m>` is the greatest — a
gap cannot exist below a live chain. A hinted chain that is gone (`ENOENT` on it), or an empty
hint, restarts from a fresh listing (`§4.6`): a `D(k)` that holds no chain reads as absent. **Binding validation:** after selecting the greatest chain by probing, the reader
re-looks-up `c-<x>-<m>` by path (authoritative under `§6`) and restarts if it is gone — so a reader never
serves a chain it reached through a stale binding — and an empty listing hint is refreshed
(`§4.6`) before absence is declared.

A top that is a **marker** is a tombstone: the object is absent. A `D(k)` with no chain, an absent
`D(k)`, and a `D(k)` whose greatest chain is tombstoned, all read as **absent** for every reader
and probe. A chain
directory that exists but is empty cannot occur (`§4.2`) and is reported as corruption, never as
absence.

### 3.2 Successor names are deterministic and exclusive {#successors}

For a current incarnation `c-<x>-<m>/<g>-<n>` the successor has exactly one name; for an absent
object the "successor" is the object's first chain:

| Predecessor | Successor name | Created by |
|---|---|---|
| `n < K(k)` | `c-<x>-<m>/<g>-<n+1>` | `link` from `.tmp-*` (data) or `rename` from `.tomb-*/` (tombstone) |
| `n ≥ K(k)`, or a tombstoned top | `c-<x>-<m+1>` | `rename` of a `.new-*` intent in `D(k)` holding `<g'>-1` (data) or a marker `<g'>-1/` (tombstone) |
| absent `D(k)` | `D(k)` itself, holding `c-<x'>-1` with a fresh life `x'` | `rename` of a private `P(k)/.new-*` directory onto `D(k)` (`§3.4`) |

`K(k)` is a **protocol constant** by plane: `1` for blob bodies, `64` elsewhere (including `.meta`).
The successor name
is a pure function of the predecessor, and facts 1–3 make its creation exclusive — including a data
file against a marker directory at the same name (`link` onto a directory: `EEXIST`; directory
`rename` onto a file: `ENOTDIR`) — so at most one writer succeeds a given predecessor, and a
writer's successor and a remover's tombstone are decided against each other by the server.

### 3.3 The two name classes {#name-classes}

**Inside a chain nothing is ever freed.** `<g>-<n>` names are never unlinked while `c-<x>-<m>`
exists. Space is reclaimed only by moving a whole chain: `rename(c-<x>-<m>, .gone-<u>)`. A delayed `link` or
`rename` aimed at a name inside a moved chain either fails (`ENOENT` by path) or lands inside the
unique `.gone-<u>` directory that nothing reads and that the reclaimer deletes (a late arrival makes
its `rmdir` fail `ENOTEMPTY`; the janitor retries). A delayed operation after the chain is deleted
meets `ENOENT`/`ESTALE`. Nothing inside a chain therefore needs an intent — but a mutation that
landed inside `.gone-*` through a directory handle resolved before the move must not be reported
as success. **Post-check:** after every successful in-chain `link` or `rename`, the writer does
`fstatat(D(k), c-<x>-<m>)` **by path, authoritative** (`§2`: `lookupcache=none` on NFS; a cached
positive dentry would defeat this check); `ENOENT` ⇒ ambiguity (`§5`). This is sound because
`c-<x>-<m>` is never created twice (`§3.4`): if it is present after the mutation it was present throughout, so
the mutation landed in the live chain; if it is gone the mutation may have landed in `.gone-*`, and
the engine settles by an exact read, which returns the new chain's value. The same check applies to
every reconstructed-success arm of `§5`.

**A chain name is created only from an intent, and freed only when no intent on it exists.**

1. **Declare before observing.** A writer that may create a chain (`§3.2`, `n ≥ K`; the absent
   path; a rebirth) creates `.new-<r>-<e>-<u>/` in `D(k)` **before** it looks up the current
   chain. The intent holds the complete future chain content (`<g'>-1` or a marker).
2. **Create by `rename` from the intent.** `rename(.new-*, c-<x>-<m₀+1>)`, where `m₀` is the
   greatest chain of life `x` the writer observed *after* declaring. `RENAME` names its source, so a killed intent is
   refused at the server whenever the request executes (fact 3).
3. **Free a chain only where no intent exists.** A chain may be renamed away only after a **fresh**
   listing of `D(k)` (fact 6: create and unlink `.d-<u>`, then open a new directory stream) shows no
   `.new-*`. An intent of a live epoch defers the reclaim. An intent of an epoch that is dead by the
   mount-lease certificate (`§4.4`) is first **killed** by `rename(.new-*, .gone-<u>)`, then the
   reclaim proceeds.

Why this suffices. A creator aims only at `c-<x>-<m₀+1>` for an `m₀` it observed after its intent
existed. A reclaimer frees only superseded chains (`c-<x>-<m>` with `c-<x>-<m+1>` present) or
tombstoned ones, in order, after a fresh listing. Either the creator's intent existed at that listing — then
the reclaim was deferred, or the intent was killed and the creator's `rename` fails `ENOENT` — or the
intent was declared after the listing, in which case the creator observed a state in which the
freed chain's successor already existed and it aims higher. The reclaimer's own delayed `rename` is
harmless because its source `c-<x>-<m>` is never created again (`§3.4`), not even after `D(k)` is
removed and the key reborn.

### 3.4 The life token, and why `D(k)` can be removed {#life-token}

Revisions 3–8 kept `D(k)` and a floor chain forever because a chain counter that restarts after
`rmdir D(k)` would let a reclaimer's delayed `rename(D(k)/c-1, .gone-*)` detach a reborn key's live
chain. The life token removes the restart instead of the removal:

- **A life of `D(k)` begins with an exclusive `rename`.** The creator of an absent object builds a
  private directory `P(k)/.new-<r>-<e>-<u>/` that already contains the first chain `c-<x>-1/<g>-1`
  with a fresh random 64-bit `x`, `fsync`s it, and `rename`s it onto `D(k)`. Fact 3 makes exactly one
  creator win (the loser's target is non-empty); the winner's `x` is the life token, and every later
  chain of that life is `c-<x>-<m>` for the monotone `m` of `§3.2`. `x` is discovered by every later
  writer from the existing chain names (`§3.1`), never from a separate file, so there is no fixed-name
  object to unlink and no replay hazard.
- **A chain name never recurs.** Within a life, `m` is monotone; across lives, `x` differs (a
  repeat needs a 64-bit collision between two lives of one key *and* a mutation from the earlier
  life still in flight — the probability is 2⁻⁶⁴ per rebirth, the same class of assumption the
  per-chain `g` already makes). Every argument that rested on "`c-<m>` is never created again"
  (the post-check of `§3.3`, the reclaimer's own delayed `rename`) holds unchanged with `c-<x>-<m>`,
  and now also across `rmdir D(k)`.
- **`D(k)` is removed when it holds no chain** (`§4.4` step 5). A delayed `rmdir` after a rebirth
  meets `ENOTEMPTY`. A delayed reclaim `rename` of a chain from the previous life meets `ENOENT`.
- **A delayed first-life `rename` is S3-equivalent.** A creator that prepared `P(k)/.new-*` after
  observing the key absent and stalled may land its `rename` after another life was created and
  removed; it then creates a fresh life with its own content and reports success. That is exactly
  what a delayed `PUT If-None-Match: *` does on S3 when it arrives after a create-and-delete: the
  key was absent at execution time, so the create succeeds. No intent rule is needed for the first
  life; the intent rule of `§3.3` governs chain creation *inside* a life, where the target name is
  derived from observed history.
- **Rebirth of a present but tombstoned key** stays inside the current life: an intent in `D(k)`
  and `c-<x>-<m₀+1>`, as before. Only a `D(k)` that holds no chain at all is reborn by a new life.
- **A `D(k)` that holds no chain but is not empty** (a `.gone-*` or a killed intent awaiting the
  sweep) is absent to readers and cannot be reborn until the sweep empties and removes it: the
  creator's `rename` fails `ENOTEMPTY`, and the creator reports a transport ambiguity for the engine
  to retry later (delay-class; the sweep visits every prefix page in turn). A `D(k)` that is
  genuinely empty is replaced by the creator's `rename` where the filesystem allows a directory to
  replace an empty directory (POSIX), and otherwise `rmdir`ed and retried.

**Write-once keys** need no separate rule: after `removeManyWriteOnce` tombstones and reclaims them,
`rmdir D(k)` follows like everywhere. The engine may *read* a late duplicate that recreates such a
key (the orphan sweep and fsck list and decode what they find; recovery consults logs through a
sampled checkpoint) but never *depends* on it: what recovery and fold coverage need is protected
from pruning, and a disappearance under an advanced checkpoint restarts recovery
(`Pool/CasRefProtocol.cpp`, `Pool/CasRefLedger.cpp`, `Gc/CasOrphanManifestSweep.cpp`). A reclaimer
from the previous life detaching such a duplicate therefore destroys something the engine treats as
debris in any case.

**Blob bodies and markers** use the same single-chain mechanism as every other key: one observable
incarnation per key, `K = 1` chains for bodies, `K = 64` for `.meta`, intents for chain creation,
tombstone compaction, lowest-first reclaim. The blob protocol's invariant — a republication replaces
the observable incarnation, so an exact delete issued against the old one can no longer succeed —
holds because the old chain is superseded or tombstoned before the new one is observable, exactly
as in revision 8.

## 4. Operations {#operations}

`scratch(chain, bytes)`: create `.tmp-<u>` in the chain, write, `fsync`, close. `intent(bytes | marker)`:
`mkdir .new-<r>-<e>-<u>` in `D(k)`, populate (`<g'>-1` via a scratch file linked inside it, or a
marker directory), `fsync`. `settle(dir)`: `fsync` of a directory. `chain(k)`: `§3.1`. `fresh(dir)`:
create and unlink `.d-<u>` in `dir`, open a new stream, `readdir`. Errors not listed: `§5`.

### 4.1 Contract methods {#contract-methods}

| Method | Implementation |
|---|---|
| `write(k, bytes, expected = absent)` | `chain(k)`. **No `D(k)`, or an empty one:** build `P(k)/.new-<r>-<e>-<u>/c-<x'>-1/<g>-1` with fresh `x'`, `fsync`, `rename(P(k)/.new-*, D(k))`: success ⇒ `settle(P(k))`, return `c-<x'>-1/<g>-1`; `ENOTEMPTY`/`EEXIST` ⇒ re-`chain(k)` and fall through to the cases below (or, for an empty `D(k)` on a filesystem that refuses to replace it, `rmdir D(k)` and retry once); `ENOENT` on the source ⇒ `§5`. **`D(k)` with a live top:** `RawConflict`. **`D(k)` whose greatest chain is tombstoned:** `intent(bytes)` in `D(k)`; re-`chain(k)` (still tombstoned, life `x`, greatest `m₀`) else remove intent, `RawConflict`; `rename(.new-*, c-<x>-<m₀+1>)`: success ⇒ `settle(D(k))`, return `c-<x>-<m₀+1>/<g'>-1`; `ENOTEMPTY`/`EEXIST` ⇒ remove own intent, `RawConflict`; `ENOENT` on the source ⇒ `§5`. **`D(k)` with no chain but not empty** (debris awaiting the sweep): transport ambiguity (`§3.4`) |
| `write(k, bytes, expected = c-<x>-<m>/<g>-<n>)`, `n < K` | `fstatat(c-<x>-<m>, <g>-<n>)` succeeds else `RawConflict`; `scratch`; `link(.tmp-*, c-<x>-<m>/<g>-<n+1>)`: success ⇒ `unlink(.tmp-*)`, `settle`, post-check (`§3.3`), return `c-<x>-<m>/<g>-<n+1>`; `EEXIST` ⇒ `§5` ownership check, else `unlink(.tmp-*)`, `RawConflict`; `ENOENT` on the chain ⇒ `RawConflict` |
| `write(k, bytes, expected = c-<x>-<m>/<g>-<n>)`, `n ≥ K` | `intent(bytes)`; `chain(k)` must equal the expected value else remove intent, `RawConflict`; `rename(.new-*, c-<x>-<m+1>)` as above; success ⇒ return `c-<x>-<m+1>/<g'>-1`, then opportunistic `§4.4` |
| `remove(k, c-<x>-<m>/<g>-<n>)` | **declare first**: `n < K` ⇒ `mkdir c-<x>-<m>/.tomb-<u>`; `n ≥ K` ⇒ `intent(marker)`. Then `chain(k)`: not that value ⇒ remove own scratch/intent, `Mismatch` / `Gone`. Then `rename(.tomb-*, c-<x>-<m>/<g>-<n+1>)` or `rename(.new-*, c-<x>-<m+1>)`: `EEXIST`/`ENOTEMPTY`/`ENOTDIR` ⇒ `Mismatch`; success (post-checked, `§3.3`) ⇒ `Removed`. Then **compaction**, best-effort here and completed by the sweep: a tombstoned chain that still holds data files is compacted by creating the successor of its marker top — `intent(marker)` **first**, then a fresh `chain(k)` confirming the tombstoned data-bearing chain is still current (never the removal's earlier observation), then `rename(.new-*, c-<x>-<m+1>)` — after which `c-<x>-<m>` is superseded and reclaimable in order (`§4.4`); a rebirth competes for the same name and either outcome is fine (a rebirth that loses retries at `c-<x>-<m+2>`). A marker-only chain is then the only chain, is reclaimable (`§4.4` step 3), and once it is gone `D(k)` is removed (step 5); compaction never applies to a marker-only chain (its stop condition). `DeleteMarker` is never returned |
| `publish(request)` | unconditional by protocol: loop { `chain(k)`; absent ⇒ the absent-`write` path with the body; present ⇒ the conditional path against the current value } until success or `posix_publish_max_attempts`, then a transport failure. Envelope + bounded payload copy + exact size check as `emuPublishBlobAtomically` today |
| `read(k)` / `head(k)` / `stream(k)` | `chain(k)`; absent ⇒ nullopt / null; open the incarnation; value = the name opened. `stream` returns a `ReadBufferFromFileDescriptor` owning the descriptor; a remote unlink surfaces as the read error it is |
| `list(prefix, cursor, limit)` | `§4.6` |
| `removeManyWriteOnce(keys)` | for each key: **tombstone first** — loop `chain(k)` and tombstone its current top exactly as `remove` would (any expected value; an already-tombstoned or absent key is success) until a marker is on top; the contract's "absent is success" is thereby true when the call returns (`read`/`head`/`list` report absent). Physical reclaim follows `§4.4`, ending with `rmdir D(k)` like every other plane |
| `probeSentinelRaw(k)` | `chain(k)` with error classification: pool root absent ⇒ `ContainerAbsent`; `EACCES`/`EPERM` ⇒ `AccessDenied`; other errors ⇒ `Indeterminate`; absent ⇒ `KeyAbsent`; present ⇒ `Present` with the body. Ordinary `read`/`head` classify a missing pool root the same way and throw |

### 4.2 Why a chain has one `g` and is never empty {#one-gen}

A chain becomes visible only by one `rename` of an intent that already holds `<g'>-1` or a marker
(fact 3); every later incarnation is `<g>-<n+1>` linked against an existing `<g>-<n>`. Nothing
creates a second `g` in a chain, and nothing empties a live chain.

### 4.3 Identity of an intent {#intent-identity}

`.new-<r>-<e>-<u>`: `r` is the server root, `e` an epoch, `u` a fresh uuid. The identity is fixed
when the operation is admitted and frozen through every retry of that operation; it never silently
becomes a successor epoch. Sources, one per request plane:

| Operation class | `r` | `e` | Certificate that kills its intents |
|---|---|---|---|
| Pool bootstrap, owner claim, epoch allocation | the configured root | `0` | none — persistent residue (`§4.5`) until an operator drains it offline |
| Mount claim | the configured root | the epoch just allocated | the slot shows a greater epoch, or this epoch fenced/farewelled |
| Ordinary and farewell planes | the mount's root | the mount's epoch | same |
| GC leader | the leader's root | the leader's mount epoch | same |
| `cas-drop-member` | the victim root | the administrative claim's epoch | same, on the victim slot |
| `cas-gc-rebuild` | the pool's root as configured | **must run under an administrative mount claim** (a change to the tool: today it writes without claiming a mount) so its intents carry a certifiable epoch | same |
| `cas-fsck` | read-only; declares no intent; lists without `fresh` and reports that its listing is a hint | — | — |

### 4.4 Reclaim {#reclaim}

Reclaim is the only way a chain name is ever freed, and it obeys `§3.3` rule 3 and the order rule:

1. `fresh(D(k))`. If any `.new-*` belongs to a **live** epoch, stop: deferred. Liveness is the
   mount-lease certificate: an intent `(r, e)` is dead iff
   `e ≠ 0 ∧ (slot(r).epoch > e ∨ (slot(r).epoch = e ∧ (fenced ∨ farewelled)))`. This is **not**
   `isCreatorFenceTerminal` as it stands (`Pool/CasServerRoot.cpp`: its last arm treats *any*
   different epoch as terminal, which would kill a newer epoch's mount-claim intent while an older
   fenced slot is still present); the predicate above is derived from the same decoded slot fields.
   A missing, older-than-`e`, undecodable or unreadable slot, and any `e = 0` intent, is treated as
   live; a read error never authorizes a kill.
2. Kill every dead intent: `rename(.new-*, D(k)/.gone-<u>)`.
3. Free the lowest reclaimable chain: `c-<x>-<m>` is reclaimable iff it is superseded
   (`c-<x>-<m+1>` exists), or it is tombstoned (marker on top — a marker-only chain, or a
   data-bearing tombstoned chain that compaction has not yet replaced, in which case compaction
   runs first, `§4.1`); and **no lower chain exists**. Rename it to `.gone-<u>`; `settle(D(k))`.
4. Delete `.gone-*` contents and `rmdir` them.
5. If `D(k)` now holds no chain and no intent: `rmdir D(k)` (`ENOTEMPTY` ⇒ a creator or debris
   arrived; leave it), `settle(P(k))`.

No floor is kept: the life token (`§3.4`) is what keeps chain names from recurring. Who reclaims: the writer that created a chain (for the chain it superseded), and
the physical sweep (`§9`). Reclaim never blocks a writer; a deferred reclaim is retried on the next
visit. Intents with `e = 0` are never killed online; they are persistent residue (`§4.5`).

**Certificate lifetime.** A certificate compares epochs, so the epoch counter of a root must never
reset while any physical name under that root can still be aimed at. Today decommission deletes the
epoch object and a same-owner successor may re-allocate epoch 1 (`Tools/CasDecommission.cpp`,
`Pool/CasServerRoot.cpp`), which would make a retained `(20, farewelled)` observation kill the
successor's live epoch-1 intents. `§9` therefore changes decommission to retire the epoch object in
place (a `retired` marker carrying the counter) so the allocator continues from the retired value,
and the sweep treats a root whose epoch object is absent as **live** (defer).

### 4.5 Residue {#residue}

**Nothing permanent, on any plane.** A deleted key is reclaimed down to and including `D(k)`
(`§4.4` step 5), whether it is a blob body, a marker, a manifest, a ref log, a snapshot, a GC
artifact or a control object.

Transient residue: intents of live epochs, superseded or tombstoned chains awaiting reclaim,
`.gone-*`, a `D(k)` holding only debris until the sweep visits it, and intents whose epoch never
gets a certificate (`e = 0`, or a root retired before its intents were drained), which defer
reclaim of the chains they name — and of their `D(k)` — until an operator drains them offline.

### 4.6 Listing {#listing}

`list` performs one complete recursive enumeration under `prefix`, sorted, spooled to a scratch file
under `cas_scratch_path` and paged by `<spool id>:<offset>` (kept until exhausted or
`posix_list_spool_ttl_sec`); an object is a `D(k)` whose `chain` is live data; `.new-*`, `.gone-*`,
`.tmp-*`, `.tomb-*`, `.d-*`, `.nfs*`, markers and absent objects are skipped; values absent
(`supportsListTokens = false`). On NFS **every directory the enumeration descends is read after
`fresh`** with a new directory stream (fact 6), so the listing is complete as of the enumeration at
every level; the engine's emptiness proofs consume `keys`, never raw entries. Cost: two extra RPCs
per directory per enumeration.

## 5. Ambiguity and lost replies {#ambiguity}

| Primitive | After a possibly-lost reply |
|---|---|
| `link(.tmp-*, name)` → `EEXIST` | `stat(name)` vs `stat(.tmp-*)`: same `st_dev`/`st_ino` ⇒ ours, success. Different ⇒ real conflict |
| `rename(.new-*, c-<m>)` → `ENOENT` (source) | `stat(c-<m>/<g'>-1)` with our `g'` ⇒ ours. Otherwise **ambiguity** (our intent was killed, or our create landed and was superseded); never `RawConflict` |
| `rename(.tomb-*, name)` → `EEXIST`/`ENOTDIR` | `name` is a directory ⇒ the tombstone exists (ours or a concurrent remover's of the same incarnation: `Removed` either way); a file ⇒ `Mismatch` |
| `rename(.tomb-*, name)` → `ENOENT` (source) | our earlier attempt consumed it ⇒ `stat(name)` is a directory ⇒ `Removed`; else ambiguity |
| `rename(c-<m>, .gone-*)` → `ENOENT` | done by someone; own `.gone-<u>` is ours to delete |
| `unlink`/`rmdir` → `ENOENT` | success |
| any `ESTALE` | ambiguity |

Everything else after a request may have been sent — timeouts, `EIO`, a failed `fsync`/`close` — is
a **transport ambiguity**: the backend throws a `Poco::Exception`-derived exception, the class the
request engine settles by an exact read (`Backend/CasRequests.cpp`; a non-`Poco::Exception` is
re-thrown as a local fault). No `std::system_error` escapes; a `DB::Exception` only for definite
refusals. `CAS_WRITE_UNATTRIBUTED` cannot arise.

## 6. Absence and caches {#absence}

The authorization-class consumers of absence in the engine (blob `.meta` on a dedup hit, GC's
`HEAD` before marker cleanup, the owner/epoch emptiness proofs, `IdentityLost`, the empty-table
probe, the settlement read) require server-decided absence. The backend guarantees it by
construction: existence is a `LOOKUP` of a specific name (`§3.1`), listings are fresh at every level
(`§4.6`), on NFS `lookupcache=none` sends every lookup, negative and positive, to the server (`fs/nfs/dir.c`),
and cached positive *attributes* of an incarnation file are harmless because its content never
changes (the binding checks of `§3.1` and `§3.3` are lookups, never attribute reads). The
mount-option gate is mandatory (`§8`): for `nfs`/`nfs4` a writable mount is refused unless the
kernel-reported options contain `hard` and `lookupcache=none` and do not contain `softreval`. The mount is identified by the pool root's open descriptor: `mnt_id` from
`/proc/self/fdinfo/<fd>` matched against `/proc/self/mountinfo` (no precedent in the codebase;
missing or unreadable procfs fails closed).

## 7. Durability {#durability}

Scratch and intent content is `fsync`ed before it is linked or renamed; after a name changes,
`fsync` the directory; after a directory is created or renamed, `fsync` its parent; new ancestors in
order. On Linux NFS a directory `fsync` is a no-op (namespace operations are synchronous at the
server). A failed `fsync`/`close` after a mutation is an ambiguity. An `async` export or storage that
acknowledges before persisting is an operator precondition, documented next to the S3 ones.

## 8. Capability probe {#probe}

`checkPoolPreconditions` for `Mode::Posix` runs before the generic battery under a per-mount random
prefix and fails closed with `NOT_IMPLEMENTED` naming the failed fact: (1) mount-option gate; (2)
fact 1 including `EEXIST` onto a directory; (3) fact 2; (4) fact 3 including `ENOENT` on a missing
source, refusal onto a non-empty directory, `ENOTDIR` onto a file; (5) fact 4; (6) fact 6: create a
file, list with a new stream, unlink, list again with a new stream — the name must be absent; (7)
`fsync` of a file and of a directory; (8) layout check: a regular file where `D(k)` is expected is a
retired-emulated-layout pool ⇒ refuse, naming migration.

`checkSkipAccessCheckSupport` refuses `skip_access_check = true` for a writable `Mode::Posix` mount
(`Pool::open` skips `checkPoolPreconditions` on that path, `Pool/CasPool.cpp`).
`checkConditionalWriteSingleAttemptSupport` stays a no-op. A single-client probe cannot verify
cross-client atomicity of facts 1–5 or export durability; those are operator preconditions and the
subject of `§11`'s two-client test.

## 9. What this needs outside the backend {#outside-backend}

- **Intent identity.** `TransportAccess` (`Backend/CasTransportAccess.h`) carries only an attempt
  number; it gains the caller's server root and writer epoch (0 before one is held), set by the
  engine where it admits an operation. GC's operations carry the leader's identity.
- **Blob payload reads.** `ContentAddressedMetadataStorage` hands the logical blob key to the
  object storage for ranged reads (`ContentAddressedMetadataStorage.cpp`); in `Mode::Posix` the read
  path obtains a **backend-owned ranged reader** (`Backend::openPayloadRange`, new) that resolves the
  key to the current incarnation and, on `ENOENT`/`ESTALE` mid-read, re-resolves and reopens at the
  same payload offset — correct because every incarnation of a blob key carries the same payload
  after the pool-constant envelope (`blob_header_len` from `_pool_meta`; the envelope's
  `incarnation_tag` differs, the payload does not). Cache identity is the incarnation path.
- **Physical sweep.** The namespace janitor walks only `cas/ns/` and has no epoch-death evidence
  (`Gc/CasNamespaceJanitor.cpp`). A backend-driven physical sweep is added to the GC leader's bounded
  janitor page: one prefix page per round across all planes, calling `§4.4`. **Certificate view:**
  phase 3 decodes every slot's epoch, `gc_fenced` and farewell but keeps only counts and
  token/time observations (`HeartbeatFloor`, `mount_obs`, `Pool/CasServerRoot.h`), and the local
  floor object dies before the janitor page runs (`Gc/CasGc.cpp`). The leader retains a per-root
  `(epoch, fenced, farewelled)` view from phase 3 — counting a fence only once its write is
  confirmed — and hands it to the page; a root absent from the view defers. Data plumbing only; no
  GC decision changes.
- **Decommission.** `Cas::decommissionPoolMember` (`Tools/CasDecommission.cpp`: drains manifests,
  staging and mountpoints under its claim, writes farewell, then deletes mount, epoch and retires
  the owner) changes in two ways. **It no longer deletes the epoch object**: it retires it in place
  with a marker that keeps the counter, so a same-owner successor continues from the retired epoch
  and retained certificates stay ordered (`§4.4`, certificate lifetime). Concretely: the epoch
  object is conditionally replaced (a successor of its captured incarnation) by a body with the
  same `next_writer_epoch` plus a `retired` field — one additive, tolerant-decoded field on
  `ServerEpoch` (`Formats/CasServerRootFormats.h`), the single format change this design admits —
  and the committed value is kept. The successor-presence recheck (today `current_mount ||
  current_epoch`, `Tools/CasDecommission.cpp`) changes to: a present mount, or an epoch object whose
  value differs from the retired one just committed, means a successor; the exact retired object
  means none, and owner retirement proceeds. Ignoring epoch presence outright would miss a
  successor that allocated but has not yet published its mount. Epoch exhaustion (`uint64_t`
  wrap) fails closed. And it gains a physical-intent drain **between
  farewell and mount deletion**: it kills only
  intents of the victim root with `e ≤` the retired epoch (never `e = 0`, never a newer epoch — a
  successor may already be claiming), and it does so while the slot still exists, since the slot is
  the certificate. Intents with `e = 0` and intents declared after the drain by a bootstrap that was
  paused before declaring are **persistent residue** until an operator drains them offline with
  every client of that root stopped; `SYSTEM CAS FORGET` does not erase the slot and is not a
  remedy. This is stated, not promised away.
- **Tools.** `cas-gc-rebuild` today writes without claiming a mount (`programs/disks/CommandCaGcRebuild.cpp`);
  under this mode it must run under an administrative mount claim like `cas-drop-member` so its
  intents carry a certifiable epoch (`§4.3`).
- **Dialect.** `Dialect::Emulated`, grammar "non-empty" (`Backend/CasEtag.cpp`); values persist as
  strings compared within one key's context (`Formats/CasRecordStreamFormat.cpp`).
- **Settings** through `ContentAddressedSettings`/`openPoolView`: `posix_list_memory_budget_bytes`
  (256 MiB), `posix_list_spool_ttl_sec` (600), `posix_publish_max_attempts` (16). `K` is not a
  setting.
- **Mode selection.** `object_storage_type = local` selects `Mode::Posix`; `EmulatedSingleProcess`
  and its `emu_*` state are deleted.
- **Nothing in GC's decisions, the ref lanes, the mount lease or the request engine's
  retry/settlement logic changes; the only format change is the additive `retired` field on
  `ServerEpoch` (decommission item above).**

## 10. Cost {#cost}

| Item | Cost |
|---|---|
| Object at rest | `D(k)` + one chain + one file: 3 inodes |
| Conditional write inside a chain | scratch create + write + `fsync` + `link` + `unlink` + directory `fsync`: ~6 RPCs on NFS |
| Chain creation (rotation, rebirth, every blob publish since `K = 1`) | intent `mkdir` + scratch + `link` + `fsync` + `rename` + `fsync(D(k))`: ~10 RPCs, plus a reclaim visit (`fresh` 2 RPCs + renames) when a chain was superseded |
| First life of a key | private directory in `P(k)` + chain + file + `fsync` + `rename` + `fsync(P(k))`: ~10 RPCs |
| `head`/`read` | `readdir` hint + one `LOOKUP` per level + `open` + `read`: ~5 RPCs on NFS |
| Hot mutable key | ≤ `K` incarnations per chain; the superseded chain is reclaimed by its rotator unless an intent is present |
| Blob republish | a new chain per publication (intent + `rename`); the superseded chain is reclaimable at once unless an intent of a live epoch is present in `D(k)` |
| Deleted key never reborn | nothing (`§4.5`) |
| `list` | one enumeration per walk, spooled; on NFS two extra RPCs per directory |

## 11. Tests {#tests}

Unit (`CAS*` suites, gtest, standard gate filter):

- `CASPosixBackend`: the full contract on a temporary directory; then two independent backend
  instances on one directory: create-vs-create, overwrite-vs-overwrite, remove-vs-overwrite from one
  predecessor (marker vs file at one name, both orders; never both `Removed` and success),
  publish-vs-remove with rebirth, rotation with two writers, stale token after rebirth.
- `CASPosixIntent` (the theorem's schedules, with the stall placed both in user space and inside the
  syscall via a shim that delays the RPC): a creator declares intent, observes `m₀`, stalls; another
  creator installs `c-<m₀+1>`, the pool rotates twice; reclaim of `c-<m₀+1>` is **deferred** while the
  intent's epoch is live; the epoch is certified dead, the intent killed, the chain reclaimed, the
  stalled `rename` executes: `ENOENT`, reported as ambiguity. Same for a remover at `n ≥ K`. A delayed
  `link` inside a chain that was moved: lands in `.gone-*` or `ENOENT`, never visible. A reclaimer
  stalled before its `rename` after another reclaimer freed the same chain and a rebirth happened:
  `ENOENT`.
- `CASPosixRemoveOrder`: `remove` paused between declaring and `chain` across full deletion and
  rebirth, below and at `K`: `Mismatch`, no second `g` in any chain.
- `CASPosixLostReply`: dropped replies for `link`, `mkdir`, `rename`, `unlink`, `rmdir`; ownership by
  inode or `g'`; landed-then-superseded creation ⇒ ambiguity; no `std::system_error` escapes.
- `CASPosixChain`: stale `readdir` hints with fresh lookups, including a retained low chain with
  later chains reclaimed in order: `chain` returns the true current; never an old value.
- `CASPosixList`: hash-order `readdir`, page limit 1, exactly-once in order; spool reuse; per-level
  freshness with a new stream.
- `CASPosixLife`: a key is created, deleted, reclaimed to `rmdir D(k)`, and reborn with a new life
  token; a reclaimer's `rename` of a previous-life chain delayed until after the rebirth finds
  `ENOENT`; a delayed first-life `rename` landing after a create-and-delete creates a fresh life and
  reports success (the engine's settlement read then sees the new content, as it would on S3); a
  delayed `rmdir D(k)` after a rebirth meets `ENOTEMPTY`; a `D(k)` holding only `.gone-*` debris
  reads absent and blocks creation until the sweep removes it; two first-life creators race, one
  wins by `rename`, the loser reports `RawConflict` and removes its private directory; a write-once
  key recreated by a late duplicate after bulk deletion is detached by a previous-life reclaimer
  without affecting recovery or the orphan sweep.
- `CASPosixReclaim`: order rule (a lower chain blocks reclaim above it); `rmdir D(k)` after the last
  chain; write-once
  keys tombstone before return and read/list absent immediately; a live epoch's intent defers;
  `e = 0` intents untouched; an absent epoch object defers; a retained `(20, farewelled)`
  observation never kills a successor's intent because the counter continues from 20.
- `CASPosixDurability`: crash/power-loss injection at each step of create, overwrite, remove,
  rotate, publish, reclaim.
- `CASPosixReadPath`: table reads (full, ranged, cached) after publish, republish, rebirth, and
  with the incarnation reclaimed mid-read on a second instance (reopen at offset).
- `CASPosixBinding`: a shim serving a cached positive binding of a moved chain: the post-check and
  the reader's binding validation reject it (ambiguity / restart); every reconstructed-success arm
  of `§5` under the same shim; detach between validation and probe.
- `CASPosixCompaction`: a tombstoned chain with data is compacted to a marker-only chain and the
  old chain reclaimed in order; compaction racing a rebirth for `c-<m+1>` in both orders; N
  rebirths leave nothing once reclaimed; after compaction and reclaim `D(k)` is gone; compaction of
  a marker-only chain is a no-op; compaction re-observes after declaring.
- `CASPosixIdentity`: every request plane's intent carries the frozen `(r, e)` of admission
  across retries; bootstrap intents are `e = 0` and never killed by the sweep; two `e = 0`
  bootstraps of one root: one installs `c-1`, the other meets a non-empty target; an older
  fenced/farewelled slot never kills a newer epoch's intent; `cas-gc-rebuild` refuses to run
  without an administrative claim.
- `CASPosixProbe`: each fact fails closed on a violating shim; missing mount option
  (`lookupcache` other than `none`); emulated layout; `skip_access_check` refused; procfs
  unreadable.
- `CASPosixBackendDeathTest`: every `LOGICAL_ERROR` site as `EXPECT_DEATH` with `std::_Exit`.

Integration: `test_cas_posix_shared` — an NFS server container, two `clickhouse-server` containers as
separate NFS clients with the mandatory options, a `ReplicatedMergeTree` on both, inserts on both,
`SYSTEM CAS GC RUN`, `ca-fsck` `dangling=0`; a `SIGSTOP`ped server resumed after the other fenced it
and rotated its keys (every write of the resumed server fails as ambiguity, never succeeds); mount
without `lookupcache=none`, with `softreval`, or with `softerr` after `hard` refused, judged on
kernel-reported options; a timeout-driven `softreval` fallback shim
(v3 and v4) never yields success on the post-check or a reader; residue counted after the run. Soak:
`utils/ca-soak/docker-compose-nfs.yml` with the existing scenario suite. The stateless `cas storage`
lane becomes the single-process durability lane.

## 12. Implementation checklist {#implementation-checklist}

Carried from the closing review (`tmp/cas-posix-spec-review-astra-r8.md`, no remaining CRITICAL or
MAJOR) into the implementation plan:

- [ ] `retired` on `ServerEpoch`: encoding, default when absent, cleared on the next allocation;
  preserve the **next** counter exactly; the decommission recheck compares the committed
  incarnation value; old/new codec round trips (`Formats/CasServerRootFormats.cpp` already skips
  unknown fields).
- [ ] Decommission schedules: uncontended completion; successor allocation before retirement;
  allocation after retirement but before mount publication or the recheck; lost retirement reply;
  interruption and retry. Bound the intent drain by the captured farewell's **writer epoch**, not
  `next_writer_epoch`.
- [ ] Exhaustion checks before every epoch increment (including recovery from a surviving mount)
  and before advancing the chain counter; boundary tests without wrap or name reuse.
- [ ] Reclaim ordering: fix the candidate chain before the fresh intent enumeration; never reuse an
  earlier no-intent result for a newly discovered candidate; lowest-first, `rmdir D(k)` after the last chain,
  declare-before-observe compaction, marker-only no-op.
- [ ] Mount gate on real mount information: safe omission, `softreval`, `softerr`, both option
  orders (`hard` clears all three soft flags in the v6.12 parser), bind and container mounts,
  unreadable procfs; tokens parsed from the descriptor-selected mount only.
- [ ] Deterministic stalls inside syscalls, independent NFS clients and caches, every
  reconstructed-success post-check; definite conflict versus transport ambiguity decided from the
  operation's history (the engine treats a returned value as committed and recognises only
  `Poco::Exception` in its transport catch).
- [ ] Ranged reopen across different envelope tags and mid-buffer offsets; terminate on
  authoritative absence or unrecoverable errors; cursor expiry, scratch cleanup, bounded-memory
  sort; read-only diagnostic enumeration.
- [ ] Measure directory blocks as well as inodes; the three-directory figure is the fully reclaimed
  case, not a bound during growth or while intents pin predecessors; carry per-filesystem
  durability and delayed-operation qualification; GPFS stays uncertified in v1.

## 13. Migration and out of scope {#out-of-scope}

Pools written by `EmulatedSingleProcess` are refused by probe gate 8; no converter (no production
data). Out of scope: Keeper coordination; SMB; the single-write blob optimization (conflicts with
`BlobSource::open`); trustworthy `list` values; any change to
persisted formats beyond the `ServerEpoch` `retired` field, or to the request engine's retry and
settlement logic.
