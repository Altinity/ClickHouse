---
description: 'Design for the first sub-project of the CA soak-test effort: a read-only open mode for the content-addressed disk (also a write-one-read-many feature) and a clickhouse-disks fsck command that consumes it to independently verify pool reachability (INV-NO-LOSS), plus a read-only GC delete-preview that cross-checks the GC against that independent reachability. Also hardens clickhouse-disks list/read on CA disks.'
sidebar_label: 'CA fsck + read-only disk mode'
sidebar_position: 5
slug: /superpowers/specs/ca-fsck-readonly-design
title: 'CA Introspection — Read-Only Disk Mode + clickhouse-disks fsck'
doc_type: 'guide'
---

# CA Introspection — Read-Only Disk Mode + `clickhouse-disks fsck` {#ca-fsck-readonly}

**Status:** approved design (brainstormed 2026-06-13).

**Goal:** Give the content-addressed (CA) storage a trustworthy, *independent* introspection surface so we
can prove on-disk correctness — that every object reachable from a live ref exists (INV-NO-LOSS), that GC
deletes only genuinely-unreachable objects, and that orphans converge to zero. The surface is a
`clickhouse-disks fsck` command (plus a `ca-gc-dryrun` preview) built on a new **read-only open mode** for
the CA disk. The read-only mode is independently useful as a **write-one-read-many (WORM)** deployment.

**Why now:** the CA feature works, but trust must be *built*. The planned 24-hour deterministic soak test
(workload + seeded chaos + crash injection over two replicas sharing one CA pool) is blind without an
independent way to read the pool's true reachability state. This document is **sub-project A** of that
effort — the foundation everything else asserts against. Sub-projects B (deterministic workload + oracle),
C (fault injector + orchestration), and D (assertion/metrics loop + the 24h schedule) are **out of scope
here** and get their own specs.

**Source of truth for protocol behavior:** `docs/superpowers/specs/2026-06-10-ca-incarnation-store-design.md`
(reachability graph, GC fold/retire/fence/recheck/deleteExact, the §9 invariants). This document never
restates protocol rules; it adds an observation surface over them.

## 1. Background: the model fsck verifies {#background}

The CA pool is **content-addressed with incarnation tokens**, not reference-counted. Reachability is a
graph the fsck recomputes from the authoritative roots:

```
roots/_registry  ──>  per-namespace shard manifests  ──>  refs[ref_name] = {tree_id, mutable_files, ...}
                                                              │
                                                              ▼
                                                           trees  ──>  TreeEntry[] (Blob | Inline | subtree)
                                                              │
                                                              ▼
                                                           blobs  (content-keyed objects)
```

A part is one `ref`. `dropRef` unlinks a ref; the now-unreachable trees/blobs are reclaimed *later* by
`Cas::Gc::runRegularRound` (fold the journal deltas into an in-degree snapshot `gc/snap`, retire
zero-in-degree nodes, fence, recheck, then `deleteExact` — the **only** reachability delete in the core,
guarded by the INV-NO-LOSS barriers). There is **no `refcount(hash)`**; "is this object still needed" is
answered by graph reachability, not a counter.

The single most valuable continuous assertion is therefore **INV-NO-LOSS**: every blob/tree reachable from
a live ref must *exist*. A reachable-but-missing object is data loss. fsck computes this directly and
needs no GC state to do it.

## 2. Architecture {#architecture}

```
clickhouse-disks fsck --disk ca_s3
        │  (opens the CA disk in READ-ONLY mode)
        ▼
ContentAddressedMetadataStorage [read_only]  ──reuses──>  Cas::Store read API
        │                                                  listNamespaces / listRefs /
        │                                                  resolveRef / readTree / locate
        ├── walk live refs ──────────────────────►  reachable_keys  (blobs + trees)
        ├── backend.list(pool_prefix) ───────────►  all_keys        (raw enumeration)
        ├── read gc/snap + gc/state (read-only) ──►  Gc::previewDeletes -> preview_set
        ▼
   CLASSIFY each key:  reachable | dangling(reachable&missing) | unreachable(present&unreferenced)
   CROSS-CHECK:        preview_set ⊆ unreachable        (GC never plans to delete a reachable object)
   REPORT:             counts, bytes, dedup ratio; --detail per-object; exit≠0 iff dangling>0
```

The design principle is **one code path opened in different modes**, not a parallel fsck-only
reimplementation. fsck reuses the *production* read API (the same calls real `SELECT`s read through), so a
walk bug is also a read bug caught elsewhere. The only fsck-independence that matters is the *derivation*:
fsck computes reachability from the authoritative refs and **never** consults `gc/snap` for reachability,
while the GC preview is derived from `gc/snap`+`gc/state` — two independent derivations whose agreement is
the trust signal.

## 3. Component A1 — read-only open mode {#read-only-mode}

`ContentAddressedMetadataStorage` (and the `Cas::Store`/backend it opens) gain a **`read_only`** mode,
threaded from the disk layer. When set:

1. **No mutating capability probe.** Today `startup()` runs `CasProbe`, which *writes and deletes* a probe
   object to confirm the backend enforces conditional ops. In read-only mode this is replaced by a
   **check-only** probe (read pool meta; assert the conditional-op capability from recorded pool metadata
   if available) or skipped — fsck only reads, so it must never mutate a live pool it is inspecting.
2. **No background GC scheduler** (`gc_scheduler` is not created/started).
3. **No background heartbeat / writer registration** (`pool_config.background_heartbeats=false`; the Store
   opens without minting a writer identity).
4. **Every mutating entry point fails closed** with a clear exception (`READONLY` /
   "content-addressed disk is opened read-only"): `writeFile`, `commit`/`tryCommit`, `createHardLink`,
   `moveFile`/`moveDirectory`, `removeRecursive`/`removeDirectory`/`unlinkFile`, namespace-file writes, and
   any GC entry point.

**Two triggers:**
- the existing disk `readonly` config attribute (→ the **WORM** deployment: one writer server, N read-only
  mounters sharing the pool, none perturbing it);
- an internal override `clickhouse-disks fsck` applies so it works on any CA disk without reconfiguration.

The read API (`existsFile`/`listDirectory`/`readFile`/`resolveRef`/`readTree`/`locate`/`listNamespaces`/
`listRefs`) is fully available in read-only mode.

## 4. Component A2 — `fsck` reachability + classification {#fsck}

Algorithm (all reads; no writes):

1. **Reachable set.** `listNamespaces()` → for each namespace `listRefs()` → for each ref `resolveRef()`
   → `readTree(tree_id)` and walk `TreeEntry[]` (recursing into subtree placements), collecting the
   physical object key of every `Blob` entry via `locate()` and every tree id. Record, per reachable key,
   *which ref(s)* reference it (for the `--detail` / dangling diagnostics). Inline entries carry no object.
2. **All keys.** Enumerate every object under the pool prefix with `backend.list(prefix, cursor, limit)`
   paginated to exhaustion, restricted to the content planes (blobs + trees); the `roots/`, `gc/`, and
   `_files/` planes are pool *metadata*, not reachability-graph content, and are excluded from the
   reachable-vs-object diff (they are listed separately for the report's byte accounting only).
3. **Classify** each content key:
   - **`reachable`** — in `reachable_keys` and present in `all_keys`.
   - **`dangling`** — in `reachable_keys` but **absent** from `all_keys`. **INV-NO-LOSS violation.**
   - **`unreachable`** — present in `all_keys` but not in `reachable_keys` (in-grace debris *or* a leak;
     fsck does not distinguish — see §6).
4. **Dedup stats:** `physical_bytes` = Σ distinct blob object sizes (the header-stripped logical size is
   reported alongside the on-disk size); `referenced_bytes` = Σ `file_size` over all reachable tree
   entries (the logical bytes a non-deduped store would hold); `dedup_ratio = referenced_bytes /
   physical_bytes`.

**Output & exit code.** Default: a human summary. `--format json|tsv` for machine consumption (the soak
test parses this); `--detail` adds a per-object row (`key, kind, class, size, reachable_from[]`). **Exit
code is nonzero iff `dangling > 0`** so a test can gate purely on exit status. `unreachable > 0` is
*reported, never a failure on its own* — the caller interprets it against convergence (§6).

## 5. Component A3 — `ca-gc-dryrun` (read-only GC delete preview) {#gc-dryrun}

A new **additive, write-free** method on `Cas::Gc` (working name `previewDeletes`) that derives the set of
objects the next regular round *would* delete, from the **durable** `gc/snap` (in-degree graph) +
`gc/state` (round / fence / retire epochs) + persisted retire-sets — reusing the existing fold/candidate
logic (`GcSnap::zeroInDegreeKnown`, the retire/fence predicates) but performing **zero** CAS writes and
**zero** deletes. It emits `{key, reason, retire_epoch, fence_round}` where `reason ∈ {unreachable,
in_grace, fenced_pending}`.

Exposed as `clickhouse-disks ca-gc-dryrun --disk ca_s3` (read-only open). The command, and the soak test,
assert the **safety direction**:

> **`{preview deletes} ⊆ {fsck unreachable}`** — GC must never plan to delete an object fsck can still
> reach. This is the strongest single trust signal: two independent derivations (live-ref walk vs durable
> in-degree snapshot) agreeing on what is safe to delete.

The *completeness* direction (GC eventually deletes every past-grace orphan) is **liveness**, validated by
the soak test over time (run fsck before/after real GC rounds: every deleted object must have been
`unreachable`; `unreachable` must trend to zero at convergence), not by a single dry-run.

## 6. In-grace vs leaked orphans — why fsck stays simple {#grace}

fsck deliberately does **not** try to perfectly distinguish in-grace debris from a leaked orphan — that
distinction is GC-internal (retire epoch / fence / grace barrier) and coupling fsck to it would defeat the
independence. Instead:

- fsck reports a single `unreachable` count.
- the **caller** establishes the threshold from convergence: at a fully quiescent checkpoint (writers
  paused, GC run to fixpoint, retire-grace elapsed) `unreachable` must drop to ~0. The soak test (D) owns
  this orchestration.
- the `ca-gc-dryrun` `reason` field *does* expose the in-grace/leaked split (from GC state) for debugging a
  discrepancy — but it is an explanation aid, not part of the core INV-NO-LOSS assertion.

## 7. Component A4 — `clickhouse-disks` integration + `ls`/`cat` hardening {#cli}

- New commands under `programs/disks/`: `CommandFsck` (`command_name = "fsck"`) and `CommandCaGcDryRun`
  (`command_name = "ca-gc-dryrun"`), registered like the existing commands (`makeCommand…` + `DisksApp`
  registry). Both open the target disk **read-only** and reject non-CA disks with a clear message.
- **Harden `list` and `read` on CA disks.** Verify `clickhouse-disks list` (including recursive listing of
  table/part/projection dirs — the virtual-directory semantics) and `read` (blob-backed files via the
  FileView read path, mutable per-part files, verbatim namespace files) work against a populated CA disk;
  fix any path-resolution / listing / read bugs surfaced. Mutating commands (`write`/`remove`/`move`/…) are
  out of scope for this sub-project and are rejected by read-only mode when the disk is opened that way.

## 8. Error handling / fail-closed {#errors}

- Read-only mode rejects **all** mutation fail-closed (never silently no-ops a write).
- fsck treats a **decode failure** while walking a reachable ref/tree (corrupt manifest/tree/envelope) as a
  hard error (typed `CORRUPTED_DATA`), not a skip — a corrupt reachable object is itself a finding.
- `dangling > 0` is surfaced (nonzero exit + detail), never masked.
- `ca-gc-dryrun` performs no writes even on internal error; a failure to read `gc/snap`/`gc/state`
  propagates rather than emitting an empty (falsely-clean) preview.
- The fsck walk and the raw listing are both **snapshots at slightly different instants**; because fsck
  runs at quiescence (no concurrent publish/GC), the only legitimate skew is none. If invoked against a
  live, non-quiescent pool, `dangling` remains sound (a reachable ref's object must exist regardless of
  timing) but `unreachable` may transiently include just-published-not-yet-listed objects — documented as a
  caller responsibility (run at quiescence for the strict assertion).

## 9. Testing {#testing}

**gtest** (`src/Disks/tests/`, the existing auto-globbed CA battery):
- read-only mode: every mutating entry point throws `READONLY`; no GC scheduler / heartbeat is started;
  the read API still works.
- fsck classification on a constructed pool: publish several parts (shared blobs across parts to exercise
  dedup), drop some refs, then (a) delete one *reachable* blob out-of-band → assert it appears as
  `dangling` and fsck exits nonzero; (b) leave a dropped part's blobs unreclaimed → assert `unreachable`;
  (c) assert dedup_ratio matches the hand-computed value.
- `previewDeletes`: on the same pool, assert `{preview} ⊆ {unreachable}`; after a real `runRegularRound`,
  assert every deleted key was previously in `{preview}` and in `{unreachable}`.

**CLI smoke:** `clickhouse-disks list`/`read`/`fsck`/`ca-gc-dryrun` against a small CA disk — a minimal
stateless `.sh` test, or deferred into sub-project D's harness (decided at planning time; the gtest tier is
the authoritative correctness gate).

All build/test conventions per `CLAUDE.md` (build logs to file + subagent summaries; Allman braces; no
`no-*` test tags unless required).

## 10. Out of scope / deferred {#out-of-scope}

- **Sub-projects B/C/D** of the soak test (deterministic workload + oracle; fault injector + container
  orchestration; assertion/metrics loop + the 24h schedule) — separate specs.
- **Mutating `clickhouse-disks` commands** on CA disks beyond rejection in read-only mode.
- **Full-walk GC / debris reclaim / packs** (milestone M-F) — unaffected.
- The PoC GC code (deleted at M-W) — untouched.
- B130 (`dynamic_cast`→virtual dispatch) is *mitigated* here for the fsck path (read-only mode is the seam,
  no concrete downcast into live storage), but the broader refactor remains its own item.

## 11. Risks & open questions {#risks}

- **`previewDeletes` factoring.** The candidate-derivation logic is currently private inside
  `runRegularRound`. Extracting a write-free derivation must not duplicate the retire/fence predicates
  (single source of truth) — the planning step should refactor the shared predicate out, with the real
  round and the preview both calling it. Adversarial review applies (this reads the real delete path's
  inputs).
- **Read-only probe.** Confirm the backend exposes enough recorded pool metadata to *assert* conditional-op
  capability without a write; if not, the read-only path simply skips the probe (fsck does not rely on
  conditional ops — it only reads).
- **Pool-prefix listing cost.** A full `backend.list` over a large pool (millions of objects in a long
  soak) is O(objects); acceptable at 30–60-minute quiescent checkpoints, not for high-frequency polling.
  Documented; the soak test (D) calls fsck only at checkpoints.
- **WORM completeness.** This sub-project delivers the *read-only* half of WORM (safe shared read mounters).
  A full WORM product story (config surface, docs, multi-mounter cache coherence) is broader and is noted,
  not delivered here.
