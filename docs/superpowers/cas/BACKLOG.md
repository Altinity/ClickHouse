---
description: 'Consolidated live backlog of all still-pending CAS MergeTree work items. Index of the topic files under BACKLOG/, plus the Inbox append target for quick adds. Issue IDs preserved (never renumbered).'
sidebar_label: 'CAS Backlog (live)'
sidebar_position: 9
slug: /superpowers/cas/backlog
title: 'CAS MergeTree — Live Backlog (pending issues)'
doc_type: 'guide'
---

# CAS MergeTree — Live Backlog {#cas-backlog}

Live backlog: only open work. History and removed entries live in git; verification record in
`consolidation-2026-08/`.

This file is the **index**. The backlog itself lives in per-topic files under
[`BACKLOG/`](BACKLOG/), split 2026-08-04 so a reader (or grep) can go straight to one subsystem
instead of scanning one flat multi-hundred-item file. Item format is uniform:
`- **[id] Title** — PRIORITY — 1-3 lines: current status / the open ask / evidence pointer` (a few
genuinely-live long designs keep structured detail below their header line instead of being
compressed). Issue IDs are never renumbered.

**Blob-publication baseline since 2026-08-23.** Every blob decision begins with `HEAD`; an absent or
`Condemned` body is published unconditionally, while metadata/control objects retain native
conditional operations for create-if-absent and conditional replacement. The [real-storage gate](/superpowers/cas/unconditional-blob-publication-live-results)
and [performance gate](/superpowers/cas/unconditional-blob-publication-performance) are both still
blocked for the external/evidence reasons recorded in those reports. Backlog text below marked
historical or closed must not be read as the current body-publication API.

## Topic files {#topics}

| File | Items | Covers |
|---|---|---|
| [`BACKLOG/ref-protocol.md`](BACKLOG/ref-protocol.md) | 11 | Rev.6 lease-boundary exclusivity, the ref-lane state machine, ref-ledger internals. Top items: `[Late Predecessor PUT]`, `[PART-WRITE-RELEASE-SEAM]`, `[MOUNT-CLAIM-EPOCH-REGRESSION]`. |
| [`BACKLOG/gc.md`](BACKLOG/gc.md) | 46 | GC scalability & byte cost, correctness/observability follow-ups, throughput-collapse and fsck-vs-GC RCAs. Top items: `[gc-frontier-one-list]`, `[GC-DEFER-DECISION-LIST-COST]`, `[gc-rebuild-lease-interlock]`. |
| [`BACKLOG/mounts-and-lifecycle.md`](BACKLOG/mounts-and-lifecycle.md) | 10 items + 2 prose sections | Mount-lease/fence recovery, CA disk lifecycle (rev.8 residuals), pool bootstrap, operator recovery. Top items: `[POOL-REFUSAL-NODE-FATAL]`, `[decommission-successor-mount-race]`, disk-lifecycle-leak (deferred, prose section). |
| [`BACKLOG/formats-and-storage.md`](BACKLOG/formats-and-storage.md) | 23 | Staging/adoption, real-store backends (S3/GCS/Azure), the local/emulated backend, codec/format items. Top items: `[GATE #1: Azure]`, `[B66a]` atomic ordinary emulated writes, `[sec4-decoder-size-bounds]`. |
| [`BACKLOG/replication.md`](BACKLOG/replication.md) | 7 | `MOVE PART`/`PARTITION` onto CA disks, merge/insert retry vs. the mount-lease fence, cross-replica relink. Top items: `[move-part-to-ca-architecturally-unimplemented]`, `[merge-progress-reset-mount-fence]`, `[RPL-5 slice]`. |
| [`BACKLOG/testing-and-ci.md`](BACKLOG/testing-and-ci.md) | 39 | Test coverage & harness, gate-filter/gate-suite gaps, soak/chaos hygiene, standing testing-methodology rules. Top items: `[unconditional-blob-publication-live-gate]`, `[gate-filter-gap-3-backend-contract]`, `[4h continuous chaos soak]`. |
| [`BACKLOG/operability-and-introspection.md`](BACKLOG/operability-and-introspection.md) | 24 | Operability & release gates, disk-error audit follow-ups, fsck/introspection surfaces, the `lazy_load_tables` decision. Top items: `[B197]` SYSTEM control surface, `lazy_load_tables` USER DECISION, `[fsck-partial-degrade-false-consistency]`. |
| [`BACKLOG/performance.md`](BACKLOG/performance.md) | 26 | Read/write path, write-path optimization candidates, stage 2 (postponed), scalability findings from the full-scale campaign. Top items: `[ckpt-read-policy]`, `[ref-catalog-write-hotspot]`, stage-2 concurrent commitPart (postponed). |
| [`BACKLOG/docs-and-cleanup.md`](BACKLOG/docs-and-cleanup.md) | 25 | Architecture/refactoring (no behavior change), minor/polish, source-layout residue, standing hygiene checklist items. Top items: `[refactor: CasGc split]`, `[Group G]` upstream carve-outs, `[phase4-blob-uploader-descoped]`. |

Priority legend: **GATE** = release gate; **HARD** = agreed-necessary, not yet done; **DESIRABLE** =
valuable, not committed; **DOC** = documentation debt; **TEST/INFRA** = validation/harness/CI;
**MINOR** = small concrete improvement; **VERIFY** = believed open, confirm before working. These
seven are the canonical set; individual items also carry more specific free-form qualifiers where an
item's author wanted to say something the seven don't capture (`QUESTION`, `DESIGN QUESTION`, `WATCH`,
`GAP`, `INFRA`, `LOW`/`LOW-PRI`, `PARTIAL`, `MEASURED`, `IN PROGRESS`, `TRACKED, by design`, and
similar) — read those as elaborating one of the seven, not as a competing taxonomy.

**2026-08-04 orphaned-open triage merge:** 367 open-verdict clusters from the docs-consolidation
corpus were 4-way classified; 54 effective new/still-open findings (57 minus 3 rechecked and closed
by design) were merged into the topic files above, each marked with a `## New findings from the
2026-08-04 orphaned-open triage` heading; 35 duplicates were folded into their existing matching item
as a confirmation note rather than inserted separately. Full triage record:
`.superpowers/sdd/2026-08-03-cas-docs-map-reduce-consolidation/orphan-triage-final.md`.

## Inbox {#inbox}

Append new items here — quick adds and concurrent-agent findings land in this section, unformatted
is fine. They get triaged into the topic files above during the next grooming pass. Do not delete
from here without triaging; do not hand-sort into a topic file without checking the item's anchor
isn't referenced elsewhere first.



The eleven items below are untouched since the 2026-08-04 consolidation-audit findings that produced
them; the nine below those are the "found during the 2026-08 documentation consolidation" batch from
the same day. Both sets are moved here verbatim as part of the 2026-08-04 folder restructure — content
unchanged, only relocated.

### From the 2026-08-04 destructive-baseline / soak audit {#inbox-audit-batch}

## `[s27-list-anomaly-aimed-at-a-retired-path]` S27 perturbs a prefix nothing lists any more {#s27-list-anomaly-aimed-at-a-retired-path}

**Found by the full scenario sweep (2026-08-31). S27 FAILED, and it failed for the right reason.**

The card injects LIST anomalies — pagination ambiguity, duplicate and missing pages — on
`cas/refs/` and checks that discovery survives them. Its verdict was:

> LIST anomalies were injected on cas/refs/ (test not vacuous): expected > 0 perturbed LISTs,
> observed 0 — proxy perturbed 0 LISTs — discovery may not have re-listed cas/refs/

Discovery does not list that prefix any more, and has not since discovery authority moved to the
pool-wide `cas/ref_catalog` object. The registry records the change in as many words: "Value 10 is
retired: discovery authority is the pool-wide `cas/ref_catalog` object rather than a roots registry
object or a physical stream listing." A grep of the CAS tree finds no listing of `cas/refs/` at all.

**The card did the right thing.** It carries a not-vacuous guard, and that guard is what fired: rather
than reporting a serene PASS for an injection that reached nothing, it failed and said the injection
reached nothing. A scenario without that guard would have been quietly reporting success against a
retired code path for as long as the path has been retired.

**The subject is not gone, only moved.** LIST is still load-bearing, for GC rather than discovery:
`CasNamespaceJanitor` pages `namespaceRootPrefix()`, `CasOrphanManifestSweep` pages
`casManifestsPrefix()`, and `CasGc` pages its own prefix. Pagination ambiguity on any of those is
exactly the hazard S27 was written for, and none of them is covered today.

**Fix direction:** re-aim the injection at the prefixes GC actually pages, and assert on GC's
outcome — no object deleted that a complete listing would have shown as reachable — rather than on
discovery's. Retiring the card instead would drop a real hazard class on the floor.

## `[ca-write-buffer-allocation-concentration]` Two write-buffer constructors account for essentially all CAS allocation activity {#ca-write-buffer-allocation-concentration}

**Found by profiling a one-hour chaos soak (2026-08-31) against `system.trace_log`.**

**Read the counts as a ratio, not as a census.** The same export that produced them was
CAS-filtered (`WHERE stack LIKE '%DB::Cas::%'`), so no non-CAS allocator appears here and this is
not a ranking of server-wide allocation; and it summed 38 snapshots of a cumulative table, so every
absolute number is inflated by roughly the number of snapshots a sample survived into. Neither
defect touches the finding, because both act as a near-uniform multiplier across the frames being
compared, and the finding is the **200-fold gap between second and third place** — a ratio that no
plausible per-frame variation in either defect can manufacture. The Memory profile also rests on
25k-154k samples rather than the CPU profile's few hundred. Do not quote the absolute sample counts
anywhere; quote the ratio.

Aggregating CAS frames by sample count, the Memory profile is not merely dominated by two frames —
everything else is invisible next to them:

| frame | Memory samples |
|---|---:|
| `CaContentWriteBuffer::CaContentWriteBuffer` | 85,991 |
| `CaInlineWriteBuffer::CaInlineWriteBuffer` | 35,600 |
| next entry (`PartWriteTxn::promote`) | 175 |

A factor of 200 between second and third place. Reading the constructor explains it: each content
write allocates its streaming buffer via `clampCasWriteBufferSize`, and then a **second per-stream
spill buffer**, and creates a temp directory and a random temp path. Every blob write pays this.

**What this does NOT establish.** These are allocation samples, not time. The Real profile's top is
the upload wait — `ensureBlobPresent` and `fanOutBlobUploads` at ~116,000 samples each against 3,750
CPU samples, a ratio near 31:1 — so the write path is overwhelmingly I/O-bound and buffer allocation
is not visibly on the critical path. A pooling change could remove a great deal of allocation churn
and move the wall clock by nothing at all.

That caution is not hypothetical: on this same campaign two changes that plainly removed waste
measured exactly zero, and one that plainly removed a copy measured a reproducible *regression*. The
one that paid was the one whose mechanism predicted which formats would improve.

**Measure before changing anything:** what fraction of a blob write's wall time is buffer
construction, from a scoped profile of the write path rather than from the sample counts above. If it
is under a percent, this entry should be closed as "concentrated but not costly" rather than acted
on.

**Retracted: there is no usable CPU profile from this soak, so nothing here ranks by CPU.**
An earlier revision of this entry ranked `ObjectStorageBackend::nativeHead` in "the CPU top" and
then explained the mechanism. Three defects, each fatal on its own, mean no CPU ranking from this
run may be cited:

1. **Filtered by construction.** The export ran `WHERE stack LIKE '%DB::Cas::%'`, so every frame not
   named `DB::Cas::*` — hashing, compression, serialization, the whole of the generic engine — was
   excluded before aggregation. Asking why checksums are absent from that file is asking why a file
   does not contain what it was built to exclude.
2. **No samples to speak of.** Over the hour the `CPU` trace type accrued **369 samples**, against
   963,049 for `Real`. The CPU profiler fires on thread CPU time, and these threads spent almost
   none: the workload is S3-bound. A few hundred samples cannot support a ranking.
3. **Multiply counted.** The dump queried the cumulative `system.trace_log` every ten minutes and
   the frame aggregate summed 38 such snapshots, so the same sample is counted once per snapshot it
   survived into. The arithmetic shows it plainly: the retained stacks hold 1,460 samples while the
   top frame claims 4,713.

The `Real` profile does not share defects 2 and 3's severity — 963k samples — but is subject to
defect 1, and remains the basis for the only claim worth keeping from this run: the write path waits
far more than it computes.

**What a real answer needs:** a CPU-bound workload (bulk insert of large parts, where content
hashing actually has bytes to chew) profiled with an unfiltered query and a single end-of-run
snapshot. Until that exists, this entry asserts nothing about where CAS spends CPU.

## `[ref-catalog-write-hotspot]` The pool-wide ref catalog is the only object that timed out under a full-suite workload {#ref-catalog-write-hotspot}

**Found by the first full local run of the stateless suite on CAS storage (2026-08-30), 11,137 tests.**

One test failed with `Code: 499 ... Timeout ... key cas_s3/cas/ref_catalog, object size 41454`. The
S3 client retried twice more and both retries timed out at the same size, so this is one logical
write, not three failures.

What makes it worth recording is the negative half: across 11,137 tests, **`ref_catalog` was the
only object class whose write ever timed out.** No blob, no manifest, no ref log. The catalog is
pool-wide, mutable, and rewritten whenever a namespace is created or dropped — and a full stateless
suite creates and drops tables continuously, so the catalog is both the hottest write in the pool and
the one that grows with the number of namespaces that have ever existed in it.

This is **not** established as a defect. The run had a load average above 20 with a saturated
single-node object store, and a 41 KB write timing out under that is plausible on its own. What is
established is where the pressure lands.

**Worth measuring before deciding anything:** how catalog size and rewrite frequency scale with
namespace churn, and whether the write is proportional to the whole catalog or to the change. If it
is the whole catalog on every change, the cost is quadratic in namespace count over a workload's
lifetime, and a busy pool reaches the timeout on merit rather than by luck.

This finding is the argument for the full lane existing at all: the 41-test CAS selector that stood
in for it could never have produced this, because it never creates enough namespaces to grow the
catalog.

## `[cas-decode-register-pressure]` WITHDRAWN — the finding was a build-flag artifact {#cas-decode-register-pressure}

**Raised 2026-08-30 on an assembly review, withdrawn the same day.**

The item claimed that the wire-key cut raised register pressure in `SourceEdgeRunReader::next`, on
the evidence that spill density rose from 22.1% to 25.8% with new spills inside the hot loop. That
comparison was taken across two binaries built with different frame-pointer settings: the after side
reserved `rbp`, the before side did not. On correctly matched binaries spill density **falls** in
every decode symbol inspected and does not move in the encode symbol. There is nothing here to fix.

Kept as a withdrawn entry rather than deleted, because the reasoning that produced it was published
and someone may come looking for it.

## `[cas-decode-per-row-scratch]` The `cas_run` reader rebuilds its scratch every row; the writer does not {#cas-decode-per-row-scratch}

**Found while reading the decode path during the wire-keys phase-3 review (2026-08-30).**

Every decoded `cas_run` row allocates a `String` for the line through `readLine`, constructs a
`JsonObjectReader` — which owns a `std::vector<String> seen_keys` that grows as keys are read — and
destroys both. The write side already solved this: `SourceEdgeRunWriter` holds a reused
`CasJsonWriter scratch`, documented as keeping memory bounded by the largest line ever assembled
rather than by record count. The read side never received the same treatment.

Two related pieces of waste sit in the same place. `JsonObjectReader::nextKey` rejects duplicate keys
with `std::find` over that vector, comparing whole strings, which is quadratic in the keys on a row;
since every format's key set is fixed and already enumerated by the shared collectors, the check
could be a bit per known key, with no allocation and no string comparison. And `readString` returns
by value, so each wide field is a fresh allocation.

**This is not a regression from the wire-key cut** — it costs the same on both sides and appears in
no delta. It was in fact the leading pre-measurement hypothesis for where the cut's cost would land,
and the measurement refuted it: the hypothesis predicts that the format with the most keys per row
suffers most, and that format (`cas_fold_seal`) decodes 7 points cheaper than its byte growth, the
best of the five. Worth doing as a straightforward win, not as a fix for anything the cut caused.

## `[s45-drop-member-sweep-untested]` GC wins S45's race, so `cas-drop-member`'s own sweep path is never exercised {#s45-drop-member-sweep-untested}

**Found while triaging an S45 soak failure during the wire-keys proof phase (2026-08-30).**

S45 exists to prove that decommissioning a member does not leave its `Removing` catalog rows behind
as permanent debris, and it asserted that `cas-drop-member` reported `namespaces_removed >= 3`. It
reported zero, deterministically, including on the seed that passed on 2026-08-03.

The card cannot win the race it depends on. `cas-drop-member` refuses to run while the victim's mount
lease is alive, so the card must wait for the lease to lapse — and the SURVIVOR is the pool's GC
leader, which retires `Removing` namespaces pool-wide during exactly that wait. Instrumenting the
catalog on both sides of the wait showed it plainly: three victim and three survivor `removing` rows
right after the kill, and none of the six by the time the tool returned. The survivor's own rows went
too, and the tool never touches those, so GC — not the tool — did the sweeping. The tool then
correctly reported nothing to remove, with `slot_removed=true`.

The verdict has been rewritten to assert the invariant the scenario actually protects (no victim rows
survive the decommission) plus a precondition check that the hidden rows existed at the kill. **What
that loses is the only coverage of the tool's own sweep path.** When GC wins, that path never runs, so
a regression in `cas-drop-member`'s namespace sweeping would not be caught by any scenario.

**Fix direction:** give S45 a compose variant with `cas_gc_enabled` off on the survivor, so the
`Removing` rows persist through the lease-lapse wait and the tool is the only thing that can sweep
them. That makes the premise holdable by construction instead of by luck, and restores the assertion
that `namespaces_removed` matches the table count.

## `[blob-reuse-resurrect-no-emitter]` `BlobReuseResurrect` has no emitter, and the condemned-token re-upload has no positive test {#blob-reuse-resurrect-no-emitter}

**Found while triaging an S16 soak failure during the wire-keys proof phase (2026-08-30).**

`CasEventType::BlobReuseResurrect` is still declared in `Primitives/CasEvent.h` and still maps to the
string `blob_reuse_resurrect` in `CasEvent.cpp`, but nothing in `src/` raises it — only
`BlobReuseAdopt` is emitted, from two sites in `Pool/CasPartWriteTxn.cpp`. `git log -S` puts the
removal at `907c3b5ce7d` ("Publish CAS blobs after mandatory `HEAD`"): once publication became
unconditional after a mandatory `HEAD`, a writer no longer splits reuse into adopt versus resurrect,
because it always re-uploads from source.

Two separate things are left over.

**A dead enum member.** It costs nothing at runtime, but it makes the event vocabulary lie: a reader of
`system.cas_log`'s event set will look for a value that can never appear, which is exactly the trap
S16 fell into — its verdict required the event and so could not pass at any scale.

**A real coverage gap, which is the part that matters.** S16 was the only positive check that a
CONDEMNED token specifically forces a re-upload rather than a revival. Its assertion has been replaced
with one that requires reuse to happen at all, so the resurrect invariant is now guarded only by S16's
proxy — correct data on every cycle plus no bad CA events. That proxy is genuine but negative: it
would catch a revival that corrupted data or raised a bad event, and would miss one that happened to
return the right bytes. Restoring a direct check means finding an observable that distinguishes
"re-uploaded from writer-owned source" from "revived from the condemned object" under the current
architecture — a counter, an event, or a fault-injected condemned object that must not be readable.

## `[gc-mf-cleanup-durable-retry]` Manifest-cleanup GC phase needs durable retry, not a cap {#gc-mf-cleanup-durable-retry}

**Found by a 24h soak (`soak-t6b-report.md`) after `gc_round_manifest_cleanup_budget` landed as one of
T6b's per-round work-envelope caps; the setting was removed entirely rather than tuned.**

The post-CAS `manifest_deletes` phase (`Gc::runRegularRound`, `Gc/CasGc.cpp`) is a **one-shot pipeline**:
the ref-log intake cursor that discovers each owner-removed manifest's `-1` edge commits in the SAME
round's CAS that produces the `mf_cleanup` set, before the deletes run. A cap on this phase does not defer
the excess to a later round of the same pipeline — a cap-declined entry is never re-derived, because the
cursor that would re-derive it has already moved past the log that produced it. The only remaining
reclaimer is the (much slower) orphan-manifest sweep backstop, which drains roughly 100 objects per round
and cannot keep pace with a real burst.

Soak evidence: run-1 (cap=5000) left 112,518 entries skipped, of which 110,218 were still unreachable at
checkpoint time (checkpoint FAIL). Run-2 (cap disabled) fully drained all 223,714 entries in-round with
zero left unreachable (PASS). The user decision was that the knob must not exist at all — a cap here
converts a bounded burst into a permanent leak, which is worse than no cap.

**Fix direction, when someone takes it:** real bounding needs the edge-consumption point moved to AFTER
the delete succeeds (durable retry), not before it, so a cap-declined entry stays discoverable by the next
round's intake instead of being silently dropped. This is a natural fit for a future
`gc-frontier-one-list` focused session (post-Stage-B), since it touches the same intake/cursor machinery.

## `[soak-predown-textlog-scope]` `predown_dump.sh` only captures error-shaped `text_log` rows {#soak-predown-textlog-scope}

**Found by the T8 criterion-4 anomaly-arm injection** (Stage-B soak, `2026-08-03-stage-b-RESULTS.md`
`{#criterion-4-evidence}`): the GC round's own `INFORMATION`-level narration line — the exact text
explaining why destructive work was suppressed for that round, plus phase narration and hold-cause
detail generally — is not captured anywhere `predown_dump.sh` writes, because its `text_log` extract
(`text_log_error_shapes.tsv`) is scoped to error-shaped rows only. Once the cluster is torn down (or, as
here, simply reset for the next run), that narration is gone for good; the round's own structured
`system.content_addressed_garbage_collection_log` phase rows survived and carried the criterion, but the
human-readable confirmation did not.

**Fix direction:** `predown_dump.sh` should also capture `system.text_log` rows from the CAS loggers at
`Information` level, bounded by a time window and/or row cap (an unbounded dump risks turning the predown
step itself into the next `cas_log.tsv`-sized artifact). Not attempted here — recorded as a tooling gap
so the next investigation that needs this evidence doesn't rediscover the gap the hard way.

## `[damaged-object-diagnose-and-repair]` fsck must diagnose AND repair a damaged rebuildable object; the runbook must say how {#damaged-object-repair}

**Found by the T8 criterion-4 injection** (Stage-B soak; evidence pack
`.superpowers/sdd/2026-08-02-cas-stage-b-remaining/crit4-injection-evidence/`): a single namespace
checkpoint (`cas/ns/state/<life>/_ckpt`) was overwritten with garbage under a live writer. The GC fold
behaved exactly as designed — it detected the damage, classified the namespace as an anomaly/hold and
suppressed every irreversible family, round after round — but nothing in the product ever repaired the
object, and the live ref lane went to `CASRefNeedsRecovery` and stayed there for the remaining ~20
minutes of the run, including after the exact original bytes were restored. Byte-level damage to a
durable object is outside the trusted-store fault model this design assumes, so this is not a
correctness defect; it is an OPERABILITY hole: the system fails closed forever and hands the operator
no lever.

**What is missing, in priority order.**

1. **`ca-fsck` should diagnose the class precisely.** Today a damaged object surfaces as a suppressed
   GC round plus a counter; fsck's report has no row that says "namespace N's checkpoint is present but
   undecodable" (as distinct from absent, which is a legal cold-recovery state). Add the distinction:
   *present-and-undecodable* vs *absent* vs *decodable-but-inconsistent*, per affected object kind
   (`_ckpt`, fold seal, `gc/state`, catalog), naming the exact key.
2. **`ca-fsck --repair` (or an explicit sibling verb) should REBUILD what is rebuildable.** The
   checkpoint is a derived accelerator over the durable ref-log, so a damaged one is reconstructible by
   the same recovery walk the writer already implements (`recoverRefTableDetailed` / the recovery-epoch
   seal). The repair verb should: re-derive the object from its authoritative source, publish it by the
   ordinary CAS write path (no new object kinds, no protocol change), and refuse — loudly — for any
   object whose content is NOT derivable (a blob body, a committed ref-log record: those are the real
   data, and their loss is a restore-from-backup situation, not a repair).
3. **The lane must be able to leave `NeedsRecovery` once the source is sound again.** Our single
   observation says it did not, even after byte-identical restore. Whether that is a wedge, a
   remount-only exit, or an artifact of the injected shape is UNVERIFIED — determine it, and if the only
   exit is a remount, say so in the runbook and consider making recovery retry on its own.
4. **Runbook section: "a CAS object is damaged".** Operator-facing, in the numbered doc set, covering:
   how the condition ANNOUNCES itself (suppressed rounds naming the namespace, the fsck row from item 1,
   the `CASRefNeedsRecovery` counter); why there is no urgency (GC has already frozen everything
   irreversible — the pool is safe, it is just not reclaiming); the asymmetry an operator must know
   (an ABSENT checkpoint is a legal state that triggers cold recovery, a CORRUPT one is not — so the
   fallback of last resort is to DELETE the damaged derived object, never to hand-edit it); the repair
   sequence once item 2 exists; what NOT to do (`DROP POOL MEMBER` is for dead members, not damaged
   data; never hand-delete blob bodies or ref-log records; never "restore" bytes from an unofficial
   copy); and when the answer really is backup/restore because the damaged object is authoritative.

**Note on scope.** Items 1, 2 and 4 are operability work and need no protocol change. Item 3 may reveal
a real recovery-path defect; treat its outcome as its own item if so.

## [cas-join-set-truncate] `StorageJoin`/`StorageSet::truncate` throw retry-later, self-healing, on a CAS disk {#cas-join-set-truncate}

`StorageJoin::truncate` and `StorageSet::truncate` call `disk->removeRecursive(path)` then immediately
`disk->createDirectories(path)`. On a content-addressed disk `createDirectories` is a pure admission
no-op (`ContentAddressedTransaction::createDirectory` never touches the catalog), so the real re-mint
happens lazily on the first write after `TRUNCATE` returns — that write resolves the namespace through
`CasRefLedger::namespaceLife`.

**Verdict: TRANSIENT, not permanent.** A unit-level test
(`CASRefWriterNamespaceRemoval.FilesOnlyNamespaceTruncateThrowsRetryLaterUntilGcReclaimsThenRebirths` in
`src/Disks/tests/gtest_cas_ref_writer.cpp`) reproduces the exact sequence — birth a files-only
namespace life (the shape `StorageJoin`/`StorageSet` tables use, no MergeTree part ever published),
`dropNamespace` it (the `removeRecursive`-shaped call), then immediately call `namespaceLife` again on
the same name. It throws a typed `NETWORK_ERROR` ("CAS namespace … is Removing: creation waits for its
terminal fold and catalog removal to complete; retry later"), because the catalog row is still
`Removing` until a GC round actually deletes it. After draining GC (two rounds, same shape used
throughout this test file), the identical call mints a fresh incarnation and writes succeed normally —
self-healing, no operator action required.

Practically: `TRUNCATE` on a `StorageJoin`/`StorageSet` table backed by a CAS disk completes without
error (`removeRecursive`/`createDirectories` do not themselves touch `namespaceLife`), but the very next
write to that table (the next `INSERT`, or backup rewrite) throws a retry-later error until the
background GC round reclaims the just-removed row — a window bounded by GC round latency, not by
anything the client controls. A client without retry-on-`NETWORK_ERROR` will see the write it issues
right after `TRUNCATE` fail; retrying it (or simply waiting for the next GC round) succeeds.

**Before the `existsDirectory` fix** (the `DirShape::TableDir` cleanup-completeness probe), the same
`TRUNCATE` was silently a no-op on these engines: `existsDirectory` never reported the directory present
in the first place (it only answered "has at least one committed part", and these engines never publish
one), so `removeRecursive` was skipped entirely and the table kept its old contents. This is a change of
which wrong thing happens on `TRUNCATE`, not a newly introduced break: the old behavior silently ignored
the user's `TRUNCATE`; the new one executes it and imposes a bounded retry-later window on the following
write.

**Direction, not a fix here.** A real fix belongs in the CAS layer's rebirth semantics — either give
`namespaceLife` a fast, non-error path for "predecessor is provably terminal, just needs its row
folded" instead of forcing every caller through the GC-latency retry-later window, or have
`StorageJoin`/`StorageSet::truncate` itself wait for the removal to fully settle before returning
(mirroring `DROP TABLE ... SYNC`'s own synchronous-completion contract) rather than leaving the very next
write to discover the window. Out of scope for the fix-verify pass that found this; tracked here as a
usability rough edge, not a correctness defect.

## [disks-exit-code-upstream] `clickhouse-disks --query` non-interactive exit code — carve-out obligation {#disks-exit-code-upstream}

`DisksApp::main` now returns a failing command's error code as the process exit code for
non-interactive `--query` runs, so CI and cron can gate on `clickhouse-disks` at all. It **rides in the
CAS pull request for now** — pre-release, and the gating it enables is needed there — but it is a
behavior change to a shared tool for every user of it, so it must later be carved out into its own
upstream PR together with the integration-test fix it forces.

The record lives with the carve inventory, not here: `docs/superpowers/cas/upstream.md`, §G list plus
the G-item section below it (site, rationale, the two reviewer-facing details, the latent
`test_replicated_table_structure_alter` defect it exposed with its mechanism, and the blast-radius
conclusion).

## `[cas-tests-unchecked-optional-deref]` A test that dereferences a disengaged optional takes every later test in the binary with it {#cas-tests-unchecked-optional-deref}

A gtest that dereferences a disengaged `std::optional` does not fail — it aborts the process, and
every test scheduled after it in the same binary never runs. The gate then reports a smaller total
that still reads as green, so the regression that emptied the optional is invisible twice over: once
as its own missing failure, once as the suites it silently deleted from the run. This bit three times
in one night, each time presenting as "a suite disappeared" rather than as a failure.

The shape to write instead depends on the enclosing function's return type, and this is the part that
makes a blind `EXPECT_TRUE` → `ASSERT_TRUE` sweep wrong:

- **`void` test body** — `ASSERT_TRUE(x.has_value())` is correct and sufficient; `ASSERT_*` returns.
- **non-`void` helper** — `ASSERT_*` does not compile there (it expands to a bare `return;`). The
  helper must expect and then bail on its own: `EXPECT_TRUE(x.has_value()); if (!x) return {};`, or
  fold the guard into the value expression, `return x ? x->field : Field{};`. Both shapes already
  exist in the suite — `sealedCursorOf` and `holdOf` in `gtest_cas_gc_hold_grammar.cpp`, and
  `relinkTokenOf` in `gtest_cas_confirm_exact_ref.cpp` — and their comments state the reason.

**Measured on the branch at the time of writing**, not recalled: a scan for `const auto x = …`
followed within four lines by `x->` or `x.value()` with no intervening guard reports **13 candidate
sites** across 9 files, the largest groups being `gtest_ca_wiring.cpp`,
`gtest_cas_gc_frontier_gate.cpp`, `gtest_cas_orphan_nomination.cpp` and `gtest_cas_ref_writer.cpp`
(2 each). A first, naive version of the same scan reported 52 — the difference is entirely false
positives from shapes that ARE guarded: `if (const auto got = backend.get(…))`, and
`pending = e && e->delete_pending`. Any sweep must therefore be eyeballed per site, and the 13 are
candidates rather than confirmed defects; three were confirmed by reading
(`gtest_cas_lifecycle_condition.cpp:40`, `gtest_cas_orphan_nomination.cpp:180` and `:184`, each an
`EXPECT_TRUE` immediately followed by an unguarded `->`).

Separately, `EXPECT_TRUE(x.has_value())` appears 9 times against 401 `ASSERT_TRUE(x.has_value())`.
The `EXPECT` form is not wrong by itself — in a non-`void` helper it is the only option — but it is
the marker worth grepping for, because it is exactly where the author needed a guard and may have
stopped at the expectation.

The durable fix is not a one-off sweep: a sweep fixes today's sites and the next test written
reintroduces the class. What would actually close it is making the deref fail loudly at the point of
use — a checked accessor the CA test helpers use in place of `->` — so the shape is unavailable
rather than merely discouraged.

## `[gc-multidelete-conditional-gap]` batch `DeleteObjects` cannot replace GC's exact-token deletes as-is {#gc-multidelete-conditional-gap}

T9's destructive-baseline soak measured **944,155** individual `DiskS3DeleteObjects` calls across a
single 90-minute specimen's four destructive families (`pending_deletes`, `manifest_deletes`,
`ref_object_cleanup`, generation pruning inside `round_commit`) — every one a single-key
`removeObjectIfTokenMatches` call (`Backend::deleteExact`, `Backend/CasObjectStorageBackend.cpp:955`)
carrying an `If-Match` ETag precondition, the exact-token-match safety property that stops GC from
deleting a body a writer has already displaced (the CAS resurrection-safety invariant). ClickHouse
already has a working batch-delete path — `deleteFilesFromS3` (`IO/S3/deleteFileFromS3.cpp:80`,
default batch 1000, `IO/S3Defines.h:48`), reachable via `S3ObjectStorage::removeObjectsImpl` — but
no CAS delete-family call site uses it, including `deletePrefixWholesale`, which already LISTs a
whole prefix in pages and still deletes each listed key one at a time
(`Gc/CasGc.cpp:3563-3570`). The reason is not an oversight: the batch `DeleteObjects` request only
sets `Key` per `Aws::S3::Model::ObjectIdentifier` (`deleteFileFromS3.cpp:118-122`) — AWS's batch API
has no per-key conditional precondition, so wiring GC's existing calls to it as-is means dropping
the exact-token check, which is a correctness regression, not an optimization.

**Ceiling, if the conditional gap is ever closed** (e.g. a design that proves a delete cohort
collision-free at round-commit time without a per-key check): `944,155 → ⌈944,155/1000⌉ = 945`
batch requests, a >99.9% cut in delete request count. This is a REQUEST-COUNT ceiling, not a
wall-time prediction — the soak's backend (RustFS) measures ~650–700µs mean per-delete latency
(`DiskS3WriteMicroseconds`/`DiskS3DeleteObjects` ≈ 645µs for `pending_deletes` alone), far below
real S3 RTT, so the wall-time win against AWS S3 is unmeasured by this specimen and likely larger
than what RustFS would show.

**Falsification:** if no design can prove a cohort of exact-token deletes collision-free without a
per-key conditional (i.e. the safety property is fundamentally incompatible with a keys-only batch
API), this item stays permanently blocked and the correct scope is delete-side concurrency
(`[gc-delete-concurrency-serial]`) instead. Full measurement:
`docs/superpowers/reports/2026-08-04-gc-destructive-baseline-perf.md#opp-multidelete`.

## `[gc-delete-concurrency-serial]` GC's destructive deletes run with almost no overlap {#gc-delete-concurrency-serial}

The same T9 baseline measured `pending_deletes` and `manifest_deletes` running near-serially
despite already dispatching through a thread pool: `pending_deletes` wall (208.77s, ch1) is 87% of
the SUM of its individual requests' `DiskS3WriteMicroseconds` (181.3s) — the requests overlap very
little. `manifest_deletes` shows the same shape (409.52s wall vs. 368.56s summed, 90%). Together
these two phases are 618.29s of ch1's 4352.1s total phase wall (14.2%) in this specimen. A bounded
worker pool issuing K concurrent conditional deletes (same shape as the existing `meta_pool`) could
plausibly cut this toward `wall/K`, independent of `[gc-multidelete-conditional-gap]` — the two
levers compose (concurrent batch calls) rather than compete, once/if the conditional gap closes.

**Falsification:** if concurrent deletes against the same backend/prefix trigger throttling
(RustFS or S3 `SlowDown`/503) at a K nobody has tried yet, the real win is smaller than linear —
this baseline never issued concurrent deletes and cannot rule that out. Full measurement:
`docs/superpowers/reports/2026-08-04-gc-destructive-baseline-perf.md#opp-delete-concurrency`.

## `[gc-fold-intake-readbuffer-head]` `fold_ref_intake`'s HEAD/GET pairing is the generic read-buffer size probe, not the HEAD Task 15 already removed {#gc-fold-intake-readbuffer-head}

T9's baseline found `fold_ref_intake` — the single largest wall-time phase in a destructive round
(2303.0s of ch1's 4352.1s phase wall, 52.9%) — issuing `DiskS3GetObject` and `DiskS3HeadObject` in
an exact 1:1 pairing (1,183,381 each). This is NOT a regression of the predecessor's
`{#opp-fold-head}` (drop the HEAD in `foldManifestEdges`), which is confirmed delivered — the
source comment at `Gc/CasGc.cpp:1301-1312` states the HEAD was removed because the following GET
already carries the absence signal. The HEAD still visible here is a different, generic one:
`ReadBufferFromS3::getObjectSizeFromS3` (`IO/ReadBufferFromS3.cpp:463-469`) issues a `HeadObject`
to learn `Content-Length` before every ranged `GetObject`, for every S3 disk read in ClickHouse —
not CAS-specific.

**Not yet sized.** This entry only establishes that the pairing exists and where it comes from;
whether an existing known-size read-buffer constructor already avoids it on some call paths, and
what the real win would be, is unmeasured. **Falsification:** if the size-probe HEAD is required
for correctness on every generic S3 disk consumer (e.g. detecting a truncated/resized object
mid-read), this is a ClickHouse-wide question and does not belong on this CAS backlog at all. Full
measurement: `docs/superpowers/reports/2026-08-04-gc-destructive-baseline-perf.md#opp-fold-head-successor`.

## `[decommission-waits-on-the-wrong-predicate]` `cas_mounts` liveness and `NoWait` decommission disagree about what "dead" means {#decommission-wrong-predicate}

`SYSTEM CAS DROP POOL MEMBER` under the `NoWait` policy refused a genuinely dead node in CI
(`test_cas_drop_pool_member::test_drop_dead_pool_member_heals_the_pool`, PR 2073, integration
amd_tsan 4/6), 15.5 seconds AFTER the target's lease wall-clock expiry:

```
CAS decommission 'node2': pool member is alive or contended -- mount lease held by
uuid=... epoch=1 pid=10 hostname=node2 (expires_at_ms=1785811895007). Refusing ...
```

This is not a stuck lease. The two sides use different definitions of dead, and each is right on its
own terms:

- **The observable one** is wall-clock: `CasServerRoot.cpp:236` computes `live = !gc_fenced &&
  expires_at_ms > now_ms`, and that is what a `cas_mounts` reader sees. The same file's own operator
  text carries a `CLOCK SKEW CAVEAT` about precisely this comparison.
- **The one reclaim requires** refuses that comparison outright. `claimMount`'s comment
  (`CasServerRoot.cpp:410-424`) says a same-uuid/different-epoch lease is reclaimed "ONLY on a
  certificate of death that needs no fresh wall-clock trust -- never by comparing `expires_at_ms`
  against `now_ms`": `gc_fenced`, the clean marker, or a `proven_dead_token`. A `kill=True` stop
  leaves none of the three, and `NoWait` passes an empty `proven_dead_token` (`CasPool.cpp:668`),
  skipping the observation wait that would mint one.

So the only route to `NoWait` success for a hard-killed node is a GC round fencing the dead mount
first. In the failing run GC rounds were executing on their ~1s cadence but reporting
`deferred`/`candidates=0` — the fence had not happened yet. The test's precondition polls
`cas_mounts.state != 'live'` for up to 90s, which the wall-clock definition satisfies on its own, so
passing that gate does not establish what the call it guards actually needs.

**Not yet decided, and the decision is the work here:** whether this is a test that waits on the wrong
predicate (fix: wait for the fence, or use the waiting policy), or a product gap (fix: `NoWait`
decommission should accept a hard-killed member without requiring GC to get there first, or say in
its refusal what the operator must wait for). Do not "fix" it by weakening the certificate-of-death
rule — that rule is what keeps a live twin from being decommissioned across two clocks.

Falsification: if a rerun passes on unchanged code, it is a cadence race rather than a deterministic
gap, which changes the fix but not the mismatch.

## `[gc-round-budgets-are-not-backpressure]` Round budgets throttle the consumer while the producer is unaware — the real fix is a time deadline {#gc-round-budgets-not-backpressure}

> Correction (2031-triage CAS-034, 2026-08-21): the title used to claim "four defaults changed" — those
> four budgets are still 5000 at HEAD, so the claim was stale and is removed rather than restated.

A per-round count cap is not backpressure. It bounds what GC does in one round while inserts and
merges — the producers of the work — know nothing about it. If arrival exceeds `budget × rounds/sec`,
the deficit is not smoothed, it accumulates. Whether that is harmless, degrading, or a leak depends
entirely on **what happens to the excess**, which turns out to differ per budget. Classified against
the code, not the names:

**A. Feedback loop (was capped, now unbounded).** `gc_round_graduation_budget`,
`gc_round_redelete_budget`. Excess is pushed back into `still_retired` "carry UNCHANGED"
(`CasBlobInDegree.cpp:472`), and the next round reads that list in full — `CasGc.h` marks the cost
`O(retired)`. So the round's cost grows with the debt while its useful work stays capped: rounds
lengthen, their rate drops, throughput drops, the debt grows faster. Worse than linear lag.

**B. Genuinely cursor-paced (unchanged, these caps are correct).** `manifest_sweep_list_budget_keys`,
`manifest_sweep_delete_budget_keys`, `gc_round_sweep_namespace_budget`,
`gc_round_sweep_recovery_op_budget`, `gc_round_prefix_wholesale_budget`. A cursor advances and never
regresses; a partially drained page or generation is simply finished next round. Nothing is
re-read, nothing accumulates. `gc_round_ref_cleanup_budget` is adjacent: it keeps no cursor but
`planRefCleanup` recomputes the same remaining candidates from durable state, so work is deferred,
not lost.

**C. A cap on one-shot work, i.e. a leak (was capped, now unbounded).**
`gc_round_handoff_prefix_wholesale_budget`. The struct's own comment says the hand-off "is a ONE-SHOT
event with no reclaimer behind it besides `fsck`: a generation it cannot fully reclaim this round is
never revisited (the parent-seal difference that triggers it does not recur)". This is the same shape
as the manifest-cleanup cap that was removed outright after a soak proved it leaked permanently.

**D. Audit loss (was capped, now unbounded).** `gc_round_outcome_entry_budget`. Nothing is retried on
exhaustion because the decision already happened; the only casualty is the audit row explaining it —
and it is dropped precisely on the busiest rounds, the ones an investigation would need.

**E. Not a throttle at all — an off switch (raised to effectively unbounded).**
`gc_frontier_probe_budget`. Exhaustion does not defer work: unprobed namespaces are simply unproven,
and one unproven namespace suppresses ALL destruction for the round (`CasGc.cpp:2047-2048`). It scales
with namespace count, i.e. with table count, so a value that is ample for ten namespaces becomes a
permanent GC stop for a large enough pool. **Its `0` cannot be redefined as "unbounded"**: unlike
every other budget here, `0` means "probe nothing", and the tests drive that exhaustion path
deliberately — so the default is spelled as a maximum instead. That inconsistency is itself an
operator trap and wants a proper sentinel.

**F. Memory bound, must stay capped.** `rebuild_edge_budget` — its comment is explicit that memory is
`O(budget)`, never `O(edges)`.

### What is still missing, and it is the real fix {#gc-budgets-need-a-deadline}

**A GC round has no time deadline anywhere in the code.** The count budgets have been serving as a
surrogate for one. That is why removing them is not free: a round holds the GC lease, and a round
that outruns the lease TTL gets fenced — the wedge class already fixed once in P3.1. The correct shape
is a per-round WALL-CLOCK deadline plus a cursor everywhere class A currently carries a list: the
round then does as much as it can inside its lease, stops cleanly, and resumes where it stopped
without re-reading the debt. Until that exists, the unbounded defaults above trade a silent
accumulation risk for a round-length risk, deliberately and with the user's decision.

Falsification for class A: with the caps off, a sustained-load soak should show round wall time
tracking arrival rate rather than climbing while `pending_condemned` climbs.

### Found during the 2026-08 documentation consolidation {#consolidation-2026-08-findings}

New items surfaced while writing/verifying the docs-consolidation pages (2026-08-01 to 2026-08-04);
continuing the ID series, not renumbering anything above.

- **[gate-filter-countingbackendshape-escape] ✅ CLOSED by renaming the suite to `CASCountingBackendShape` (`683b68606e2`, 2026-08-24)** — TEST/INFRA — The strict `CAS*` gate then passed `2141/2141` Debug tests from `298` suites and `2140/2140` ASan tests from `297` suites with zero sanitizer, leak, logical, fatal, or signal markers. The generator now emits the same `298`-suite strict set with the three remaining generic-infrastructure exclusions.
- **[gc-anomaly-never-emitted] `CasEventType::GcAnomaly` is defined but never emitted** — MINOR — Found during the deep-verification batch (batch-006): the event type exists in the enum but no call site constructs one, so any doc or dashboard describing GC-anomaly events as observable is currently wrong. Either wire an emit site or remove the dead enum value. (An orphaned 2026-08-04-triage finding on catalog/fold-seal capacity-reservation correctness is adjacent to this GC-observability gap — folded in as a related note, not a separate item.)
- **[reftxnid-wraparound-guard-missing] `nextRefTxnId` lacks a `UINT64_MAX` wraparound guard** — MINOR — Found during the deep-verification batch (batch-021, cluster C-0514): the sibling counter at `CasRefProtocol.cpp:941` has an explicit wraparound guard; `nextRefTxnId` does not. Add the matching guard or document why it is provably unreachable. (Confirmed 2026-08-04: this is the same finding as orphaned-open cluster C-0514 from the docs-consolidation triage — already tracked, not a separate item.)
- **[system-md-missing-cas-verbs] `SYSTEM CAS` verbs missing from `docs/en/sql-reference/statements/system.md`** — DOC — Found during Task 12 (operations runbooks): `SYSTEM CAS FSCK`/`FORGET`/`GC STOP`/`GC START` (and siblings) are documented in the CAS-specific pages but absent from the generic `SYSTEM` statement reference, where a user would naturally look first.
- **[casrequestcontrol-comment-settings-stale] `CasRequestControl` header comments cite settings that do not exist** — DOC — Found during the Task 12 fix round: the header comments name `cas_s3_retry_initial_backoff_ms`/`cas_s3_retry_max_backoff_ms` as if they were configurable settings; they exist only in the comment text — the real budget is hardcoded in `CasRequestBudget`. Either implement the settings or fix the comments to stop implying a configuration surface that isn't there.
- **[s3cache-config-comment-stale] stale comment in `utils/ca-soak/configs/storage_conf_s3cache_ch1.xml`** — MINOR — The comment claims cache-over-CA fails with `NOT_IMPLEMENTED`; this was fixed by `3ed0e5f5030` (2026-07-08) and the cache-over-CA path is now live-validated (see the quick-start cache example, `380688e8a66`). Remove the stale comment.
- **[part-folder-validate-never-gating] `part_folder_validate=never` needs a gate, not a silent accept** — HARD (user settings-policy direction) — `PartFolderAccess.h:135-138` accepts `never` (skip the `ForceFresh` body re-proof entirely) with no acknowledgment of the risk. Either remove the `never` value or require an explicit risk-acknowledgment setting alongside it. Docs already carry a strong warning on this value (`configuration.md`); the code should not make it this easy to select silently.
- **[gc-enabled-false-silent] `gc_enabled=false` accumulates garbage silently** — HARD (user settings-policy direction) — Disabling the background GC scheduler produces no ongoing signal that reclamation has stopped. Add a periodic warning log line plus a metric while `gc_enabled=false` and the pool has reclaimable debris, so an operator who disabled GC for a legitimate reason (or by mistake) finds out before the pool grows unbounded.
- **[dedup-presence-only-window-recheck] ✅ CLOSED by the unconditional blob-publication rewrite (2026-08-23); kept for provenance** — The presence cache was deleted. Every blob decision now begins with an exact blob `HEAD`, validates the observed envelope-adjusted size, and only then adopts or publishes. The surviving local-store atomic-install debt is tracked in `BACKLOG/formats-and-storage.md`; it can fail publication or leave a bad object that the size check refuses, but cannot silently admit a truncated body.

## `[gcs-conditional-overwrite-rethink]` ✅ CLOSED: blob publication is unconditional and no longer GCS-capped {#gcs-conditional-overwrite-rethink}

Closed by the [unconditional blob-publication design](/superpowers/specs/cas-unconditional-blob-publication-design)
and its implementation on 2026-08-23. The historical problem was real: GCS does not enforce the
required destination precondition at multipart completion, so a conditional blob-body design either
needed a one-part ceiling or a more elaborate compose protocol. The implemented answer removes the
premise instead. Every blob decision starts with `HEAD`; an absent or `Condemned` body is then
published unconditionally. Fresh streaming uses ordinary multipart, and the first absent staged
publication may use native same-store copy. A `Condemned` or subsequent staged attempt retags and
streams so an already-queued exact delete cannot remove the new incarnation.

Consequently, blob bodies above the former ceiling are supported and do not use
`gcs_max_conditional_put_bytes`. That setting now applies to every conditional non-blob `PUT`,
including create-if-absent metadata/control artifacts and conditional replacements.
Native-conditional plumbing remains for those writes, native-token `HEAD`, and exact deletion; it
is not a blob-body transport.

Evidence is intentionally not overstated. The [real-storage results](/superpowers/cas/unconditional-blob-publication-live-results)
record complete test scenarios, but all 25 credentialed GCS cases skipped because credentials and the
TLS fault driver were unavailable; release readiness is blocked. `test_storage_s3` is also blocked by
the unavailable `clickhouse/clickhouse-server:23.3.19.33.altinitystable` image. The
[performance report](/superpowers/cas/unconditional-blob-publication-performance) records passing
target-only runs, but no matched same-environment pre-change binary; its control-adjusted sequence
ratios are not a code-version delta, so performance acceptance is also blocked pending a matched pair
and explicit human acceptance. The earlier cap/compose analysis remains in git history as the decision
record that led to this simpler protocol.


## `[gc-deferred-round-pays-full-list]` A Deferred GC round still pays the full ref-prefix listing — measured at 23% of server CPU under the parallel stateless lane {#gc-deferred-round-pays-full-list}

Measured live (2026-08-04, `system.trace_log` type=CPU, 10-minute window, evidence in the run's
`build/cpu_trace_diagnosis.md`): `CasGcScheduler::loop` appeared in 479/2097 (22.8%) of all sampled
CPU stacks and 70% of background-thread CPU. The single chain
`runRegularRound → enumerateRefPrefix → Backend::list → LocalObjectStorage::listObjects`
(`readdir`/`lstat`) was 11.7% — larger than any individual test query. Four test disks each ran a GC
round at ~1 Hz, and ~89% of those rounds finished `Deferred`: the full directory walk was paid every
second with no payoff.

Two independent contributors, each with its own fix:

1. **The listing is eager even when the round will defer.** The defer decision (fold threshold /
   nothing changed) is made AFTER enumerating. A cheap staleness probe before the walk — or feeding
   the defer decision from the previous round's cursor instead of a fresh enumeration — would make a
   quiet pool cost near nothing per round. This is the durable fix and applies to production pools,
   not just tests.
2. **The disks belonged to finished tests.** This is the known disk-lifecycle leak (custom disks are
   never torn down on `DROP TABLE`), here given a price for the first time: leaked 1 Hz schedulers
   from completed tests kept scanning for the rest of the run. The lifecycle redesign
   (`UNMOUNT` stops background work and ejects the disk) subsumes this half.

## `[emulated-resurrect-should-spill-to-disk]` Emulated `publishBlob` should spill before atomic install {#emulated-resurrect-spill-to-disk}

**REFRAMED 2026-08-23; identifier and history preserved.** The separate resurrection API was deleted.
The remaining debt is `ObjectStorageBackend::publishBlob` in `EmulatedSingleProcess` mode: it
materializes the complete `[header][payload]` body before installing it. This retains complete-object
visibility and bounds concurrency, but peak memory is still the largest single blob even though the
backing store is a local disk.

Atomic install itself is already implemented by `emuPublishBlobAtomically`: the fully materialized
body is written to a sibling temporary object and renamed into place under `emu_mutex`. The separate
remaining follow-up is to stream directly to that temporary file (or the pool scratch path), validate
the declared size, and retain airtight cleanup on success, size mismatch, and mid-stream exception.
This is emulated-mode peak-memory work only; native S3/GCS `publishBlob` already streams or uses
native copy and is not affected.

## CAS-021 (issue #2207) adjudication follow-ups: controller-outcome honesty + condemn-memo staleness (2026-08-20) {#cas-021-followups}

Adjudication of https://github.com/Altinity/ClickHouse/issues/2207 (two read-only code sweeps against
HEAD `684161dcc03`; updated for the 2026-08-23 rewrite): the controller-outcome observation remains
relevant to mutable blob metadata, but the old conditional body-displacement branch was deleted. The
claimed integrity consequence remains neutralized — the delete path is guarded by the normative
delete-site in-degree re-read, exact-token deletion cannot remove a fresh retagged publication, and
the writer checks its admitted fence generation before publication;
the equality-resolved meta etag is consumed by NOBODY (`writeCondemnedMeta` reads only `.outcome`);
the ref-log lane adjudicates authorship by byte equality over a payload that carries txn identity
(`classifyRefLogOccupant`, `CasRefLedger.cpp:2240`); a false-`Occupied` → mount-fault path does not
exist. GC's gate predicate is "durable Condemned evidence exists" — which the equality-resolve GET
literally proves — same as the already-Condemned arm at `CasGc.cpp:137`.

Follow-ups, in recommended packaging:

- (1) **Honesty patch over the request controller** (one coherent change, NO durable-op/wire/behavior
  change): split the equality-resolved outcome out of `Committed` (e.g. `IntendedStateDurable`), stop
  returning the observed occupant token on that arm (it claims authorship no caller has; today unused
  — make that structural); rename `slotOccupy`'s misleading `NotUnresolved` label; add the
  "trust model" doc-block at the resolution ladder (what equality-resolve proves / does not prove,
  pointers to the three system invariants that make it safe) and the ownership-decidability table by
  key class (immutable content-addressed / mutable identity-in-payload / mutable identity-free /
  owner-anchor `claimOwnerOrThrow`); cross-reference sentences at `writeCondemnedMeta` ("a foreign
  `Condemned` marker satisfies the predicate by design") and `reconcileMetaClean` ("an
  equality-resolved desired `Clean` record is already durable");
  rename the pin tests to read as spec. ~150-250 line diff + test renames; controller = adversarial
  review mandatory. This addresses the CORE of CAS-021 at the type level: the external auditor's
  reading becomes impossible to write.
- (2) **Stale condemn-marker memoization — ACCEPTED RESIDUAL, do NOT fix with re-reads** (user
  decision 2026-08-20): the in-process `condemn_markers_confirmed` note survives a legitimate
  `Condemned -> Clean` transition (no `forgetCondemnMarker` on writer replacement without an intervening fold),
  so `confirm_condemned_marker` (`CasGc.cpp:1885`) can graduate an entry whose durable meta says
  Clean. Consequence when it fires (ultra-rare race): ONE spurious `deleteExact` — an S3 DELETE,
  which is FREE — self-healing at `CasGc.cpp:862-870` (TokenMismatch drops the confirmation, meta
  untouched). The re-read fix would cost +1 BILLABLE GET per graduating condemned entry on the
  COMMON path (P9 GET-budget class) to save free DELETEs in a rare race — worse than the disease.
  No zero-cost invalidation exists either (the window is by definition "nothing observed the fresh
  replacement"). Only sanctioned improvement: the observability LABEL at the self-heal site (counter/
  log as "spared by token rotation", not an anomaly) — zero extra requests; fold into (1) if done.
- (3) One trust-model paragraph for conditional writes in the numbered doc set
  (`03-writer-protocol.md`) — documentation only.

Issue response drafted (2026-08-20); post/adaptation is the user's call.

## Issue #2233 adjudication residue: soak-harness observability + Poco shared-pool risk (2026-08-20) {#issue-2233-followups}

Adjudication of https://github.com/Altinity/ClickHouse/issues/2233 ("replica HTTP dies on green-path soak
after relink NETWORK_ERROR storm"): the refusal storm is the known, designed
`{#relink-confirm-busy-lane}` behavior (all four remediations there still open — the per-ref rule-3
refinement is the availability fix); the claimed causality "storm -> HTTP death" is contradicted by our
own artifacts (a 90-minute phase-3 soak absorbed 112,598 refusals — peak 9,219/min — and ended
`PHASE3 OK` with both replicas alive; the reporter saw ~278 total), and no fd/socket/thread leak exists
on the abandoned-relink path (drain-then-throw + `SCOPE_EXIT` verified). Prime suspects for the
reporter's observation: VM-level OOM (28g `mem_limit` x2 on a 16 GiB Docker Desktop VM; their upstream-
compose symptom was "Connection refused after the peer exits") and the Poco shared-`server_pool`
silent-refusal upstream bug (below). Items:

- (1) **ca-soak compose: `ch2` has NO healthcheck** (`docker-compose.yml` — only `ch1` has the HTTP
  `/ping` probe, added for capability-probe serialization). "Container healthy while HTTP dead" on ch2
  is therefore vacuous. Add the same healthcheck to ch2. Trivial.
- (2) **soak driver: `TRANSPORT FAILURE` is an `else`-branch catch-all** (`soak/run.py:2020-2026`) that
  names a subsystem it never diagnosed — the same triage-misdirection failure mode #2219 complains
  about, one layer up. Phase 1 additionally does exactly one attempt (`transport_resilient=False`) and
  checkpoints do not gate on HTTP health (phase-2-only wait), so any transient `OSError` becomes the
  issue's exact headline. Split the label (name the errno/op) and consider a phase-1 HTTP-health gate
  at checkpoints. Small.
- (3) **Poco shared-`server_pool` silent connection refusal — assess exposure** (upstream bug, comment
  in `base/poco/Net/src/TCPServerDispatcher.cpp:154-180`): one `Poco::ThreadPool` capped at
  `max_connections` is shared by 8123/HTTPS/native/9009; `_currentThreads` is per-dispatcher, so
  saturation by long-lived interserver byte fetches can make the 8123 dispatcher drop accepted sockets
  with NO ClickHouse-level error (client sees RST; at most a Poco `Warning`). Relink-storm second-order
  effect: refusals suppress zero-byte relinks and FORCE long byte fetches, i.e. the storm converts
  cheap transfers into thread-holding ones. Candidate observability first: expose refused-connection
  counts / alarm on pool saturation before considering upstream surgery (upstream file = consult-first).
- (4) **Confirm-path observability gaps** (feeds `{#relink-confirm-busy-lane}` items (b)/(c)): no
  ProfileEvents pair for proven/refused confirms; refusal reason (which rule) logged only at Debug on
  both sides; the receiver collapses refusal vs transport failure vs timeout into one message — the
  reporter's logs could not distinguish them even in principle. This adjudication would have taken
  minutes with (b)+(c) implemented.

## Issue #2243 CONFIRMED: local port exhaustion fences out the mount lease (2026-08-20) {#issue-2243-port-exhaustion-lease}

https://github.com/Altinity/ClickHouse/issues/2243 — read-heavy concurrent `SELECT FINAL` on a CAS
default-policy disk exhausts the container's ephemeral ports (errno 99 `EADDRNOTAVAIL`), which kills
the mount-lease renewal → fence → `TransientNotLive` → ~42 s full-disk refusal → self-remount discards
in-flight `PartWriteTxn`s. CONFIRMED REAL (adjudicated from code; mechanism matches our own S07 finding
from 2026-07-06). The rev.8 fail-close chain itself worked exactly as designed; the defects are in the
trigger and its classification.

Mechanism (verified at file:line): the S3 client keep-alive default is 5 s (`S3Defines.h:12`); pooled
DISK connections expire at 0.8×5 s idle (0.1×5 s once the group crosses `disk_connections_soft_limit`),
`mustReconnect` resets any connection whose request STARTED > 4.5 s ago, and `http_keep_alive_max_requests`
rotates every 100 requests. With P pooled connections and R req/s, reuse survives only while P/R < 4 s —
concurrent FINAL prefetch puts P in the dead band, so nearly every request connects fresh and the CLIENT
closes on return → TIME_WAIT at request rate (reporter: ~430 GET/s × 60 s ≈ 26k of 28,232 ports). The
store honoring keep-alive is irrelevant. Retry amplification: `EADDRNOTAVAIL` is classified as remote
transient at EVERY layer (Poco → `CoreErrors::NETWORK_CONNECTION` → retryable; CAS controller →
`Unresolved`); reads ride `s3_retry_attempts=500`. Lease renewal = ONE bare single-attempt PUT per 10 s
through the same DISK pool (no reserve, no controller budget — WEAKER than any data write); outage length
is constructive: `mountObservationThresholdMs = ttl + 5% + cadence = 36.5 s` (field answer to
{#fence-window blast radius}'s "measure where remount time goes").

Fix directions, in value order:
- (1) **Classify local socket-resource errors as local** (`EADDRNOTAVAIL`, `EMFILE`, `ENFILE`): hard
  backoff instead of retry-at-rate. Template = the existing DNS sub-classification branch
  (`PocoHTTPClient.cpp:840`); CAS side maps it out of `Unresolved`'s full budget. Retry loops are the
  positive-feedback term — this cuts the amplifier.
- (2) **Mount-lease resilience**: in-period fast retry (today a failed renewal waits the FULL next
  period — `CasServerRoot.cpp:1437` `continue` re-enters `wait_for(period)`), and/or reserved
  connection / dedicated small budget for the renewal PUT. Note the margin arithmetic: ttl=30s /
  period=10s / margin=2s allows at most TWO ride-out warnings per lease generation, not three.
- (3) **Keep-alive defaults for CAS-over-S3 profiles**: 5 s TTL + 100-request rotation is the churn
  engine under sustained read concurrency; evaluate raising `http_keep_alive_timeout` (60 s) and
  `http_keep_alive_max_requests` in the CAS disk profile / docs. Caveat: the server-advertised
  `Keep-Alive: timeout=N` header mins the client value (`HTTPClientSession.cpp:377`) — capture what
  RustFS advertises first. Diagnostic BEFORE any change: `DiskConnectionsCreated/Reused/Expired/Reset/
  Preserved` + `DiskConnectionsTotal/Stored` (prediction: Created≈request rate, Expired>>Reset, Stored≈0;
  if Reset>>Expired the cause is the non-drained-body branch and the fix belongs in the read path).
- (4) NOT a fix: capping pool limits below the ephemeral range (reporter's #1) — limits gate KEEPING,
  not CREATING; TIME_WAIT is invisible to them (hence errno 99, never `HTTP_CONNECTION_LIMIT_REACHED`);
  lower caps flip the pool into the 0.5 s TTL regime sooner and worsen churn.

Housekeeping folded in:
- **S07 wide-part port-exhaustion finding re-rated**: was closed 2026-07-06 as "cost/latency only, not a
  data bug" (`performance.md:131`, DESIRABLE) — #2243 refutes that scope: the same condition takes the
  LEASE down and discards in-flight write txns. Availability class, not just cost.
- **[B196] is stale as written**: `s3_max_connections` is dead code for the disk path (an AWS-SDK
  `ClientConfiguration` field; `PocoHTTPClient` never reads `maxConnections`) — the disk path is governed
  by `disk_connections_*` + keep-alive, and NONE of those caps concurrent socket creation either.
- **Soak-harness blind spot**: `soak/cluster.py` classifies "mount lease not held" as retryable
  `NETWORK_ERROR` — our own soaks would ride through a #2243 event and score it recoverable; add a
  lease-loss detector (count `TransientNotLive` windows / `CasMountLeaseKeeper` errors) to checkpoints.

## Issue #2244 (filed by us): lease/remount retry asymmetry — CI RCA of job 96307284077 (2026-08-20) {#issue-2244-lease-retry-asymmetry}

https://github.com/Altinity/ClickHouse/issues/2244 — full RCA in the issue. Before the minimum fix, the
two operations keeping a CAS mount alive were the only S3 operations without retries: renewal sent
one 5-second-timeout `PUT` per 10-second period, while remount claim used `SingleAttempt` inside a
roughly 15-operation sequential chain with a 36.5-second observation window. An intermittent-timeout
episode that the data plane rode out on `Attempt 2/501 succeeded` therefore tripped the fence and cost
roughly 15 minutes of full-disk write refusal. This was field evidence #2 for
{#fence-window blast radius} and {#fence-window observability}.

Fix directions (tracked in the issue, value order): (1) in-period renewal retries while
`now + margin < confirmed_deadline` — would have prevented this trip outright; (2) per-step retries in
the remount chain + investigate the own-ambiguous-claim observation-window reset (STID-3982 family);
(3) observability: log the trip reason + every remount attempt step at default level, ProfileEvents
for renewals/remounts; (4) rate-limit the snapshot-publication refusal loop (125,952 warnings/16 min
on one table — NEW defect, no backoff on that loop). CI-env extra: RustFS logs at ERROR-only — raise
its log level in the CA lanes (re-opens the [CAS CI observability gaps] rustfs item at the level
dimension).

The minimum pre-release fix is specified in
`docs/superpowers/specs/2026-08-23-cas-mount-renewal-retry-design.md`: ambiguity-aware in-period
renewal retries, renewal/remount observability, and the snapshot-refusal backoff hole. It deliberately
does not change the remount protocol. **Implemented 2026-08-24:** the minimum cut and its focused/full
TLA+, Release/Debug, proxy-integration, and 15-minute S39 gates are complete. The separately anchored
per-step remount follow-up below remains open.

### Issue #2244 follow-up: per-step remount recovery {#issue-2244-remount-retry-follow-up}

After the minimum renewal fix, replace whole-chain remount retry with an explicitly modeled state
machine. Required design questions: per-step retry classification; which owner/catalog/epoch results
may be preserved; whether an own ambiguous claim preserves or resets token-stability observation;
prevention of repeated epoch burning; cancellation at every teardown boundary; and deterministic
fault injection before/after every state-changing step. This is safety-critical single-writer work and
requires its own focused TLA+ gate plus a refinement audit against `CaCasMountCore`; do not fold it into
the minimum renewal implementation without that design review.

Triage bonus recorded: the RustFS "Erasure decode failed ... downstream_closed" error class is a red
herring = ClickHouse's silent-by-design mid-body aborts (cancellations, LIMIT, abandoned prefetch);
do not chase it in future triage.

## CAS disk settings whitelist rejects valid S3 keys (found by #2243 mitigation attempt, 2026-08-20) {#cas-disk-s3-key-whitelist-gap}

Field report (Carlos, clickhouse-regression run 32408309167/job 96552561919): adding
`<http_keep_alive_timeout>60</http_keep_alive_timeout>` to a CAS disk block — the mitigation
suggested in #2243 — kills the server at startup with `Unknown setting 'http_keep_alive_timeout'
(UNKNOWN_SETTING)` during metadata loading.

Mechanism: on a CAS disk the S3 keys are direct children of the same block as the CAS settings, and
`ContentAddressedSettings.cpp` fail-closes on any key that is neither a CAS setting nor in the
`non_cas_keys` whitelist. The whitelist was built by enumerating in-repo configs, so it contains only
the S3 keys those configs happened to use — `http_keep_alive_timeout` and
`http_keep_alive_max_requests` (both genuinely consumed on the disk path,
`ObjectStorages/S3/diskSettings.cpp:169-170`) are missing, as is every other unused-in-repo
`S3AuthSettings` key (`connect_timeout_ms`, `max_connections`, `session_token`, ...). No workaround
exists: the disk path reads S3 auth settings only from the disk block
(`S3Settings::loadFromConfigForObjectStorage`), never from the global `<s3>` per-endpoint section.

Fix: instead of growing the enumerated whitelist, skip every key whose name is a builtin
`S3AuthSettings` or `S3RequestSettings` name (BaseSettings exposes the builtin-name enumeration),
keeping `non_cas_keys` only for the genuinely ad-hoc generic-disk-layer keys (`type`, `name`,
`use_fake_transaction`, ...). Update the four-way-scan comment accordingly. Release-relevant: this
blocks the #2243 CI mitigation on CAS disks.

## Issue #2211: `SYSTEM CAS GC RUN` on a follower silently does nothing (adjudicated 2026-08-21) {#issue-2211-gc-run-follower-noop}

https://github.com/Altinity/ClickHouse/issues/2211 — CONFIRMED as described; the report's code anchors
all check out on HEAD. History splits it in two:

1. **No-steal on manual runs is DELIBERATE** — commit `74d67b85021` (2026-07-13, "manual GC rounds
   never steal a lease"): the observation-window steal protocol's safety argument needs the two
   "incumbent frozen" observations spaced by real wall time (the loop's paced ticks); two manual
   calls can land microseconds apart and fake a frozen incumbent → two concurrent destructive GC
   actors. The pre-fix manual path also never heartbeat-protected an acquired lease. Keep as is;
   the issue itself agrees steal is the wrong fix.
2. **The silent success row was NOT a chosen contract** — commit `cb111510c1a` (2026-07-20) merely
   surfaced the already-computed `RoundReport` as a result set ("mirroring the DROP POOL MEMBER
   precedent", deferred-register item 11). No record anywhere (commits, specs, backlogs) weighs
   throw-vs-row for the follower case; the docs (`operations/debugging.md` `{#sql-gc-run}`) don't
   mention the follower no-op either. Genuine operator-contract gap.

Also confirmed: `GcLease` is `{owner: UInt128 random gc_id, seq}` — no host identity
(`CasGcStateFormat.h:17`), and `system.cas_mounts.is_leader` is populated only for the local mount,
so a follower cannot name the leader today.

Fix shape (DECIDED 2026-08-21, user call): keep the quiet idempotent OK — no exception. Rationale:
with default `distributed_ddl_output_mode=throw`, a throwing follower inverts the bug for
`ON CLUSTER` (leader ran the round, N-1 followers threw, the statement reports failure), and a node
inside the DDL fan-out cannot tell it is part of `ON CLUSTER`, so selective throwing is impossible.
A follower's "not my lease" is a valid outcome of "run a round here if this node may", and quiet OK
keeps scripts/harnesses that poke `RUN` on every node working. Instead, make the outcome
first-class and visible:
- add a `finish` column (`Success`/`NotALeader`/`Deferred` — already exists in `cas_gc_log`, the
  interpreter row just doesn't emit it) to the `RUN` result set, so the operator reads a word, not
  infers from `acquired_lease=0` + zeros;
- add advisory identity to `GcLease`, mirroring the existing `MountLease` precedent
  (`CasServerRootFormats.h` carries `hostname`/`pid`/`server_uuid` next to its protocol fields for
  exactly this purpose): `hostname` (+ `server_uuid`/`pid` for symmetry), written at acquire/steal.
  The protocol part stays untouched — `owner` MUST remain a random per-process-instance UInt128
  (a restarted server is a NEW GC actor and must not resume the old lease; hostname is neither
  unique nor per-instance), which is WHY host identity was never the owner: the advisory field was
  simply never needed until #2211 (YAGNI, not a considered rejection — no record deciding against
  it). Durable-format change, pre-release so no compat scaffolding
  ([[feedback_ca_no_compat_scaffolding_predev]]);
- follower `RUN` row then carries `leader_host` — `NotALeader, leader_host='replica-2'` in one read,
  no operator discovery query. Rejected as contract (user call): documenting a
  `clusterAllReplicas(system.cas_mounts) WHERE is_leader=1` discovery recipe as the way to find the
  leader — too strange a requirement once the row can name the holder;
- bonus: `system.cas_mounts.is_leader` can be populated for ALL rows (match `gc/state`
  hostname/server_uuid against mount slots), not local-only;
- docs `{#sql-gc-run}`: state the leadership model in one sentence.
No-steal on manual `RUN` stays untouched.

## Reviewer ask: full-word wire keys in all CAS persisted formats (2026-08-21, RESOLVED 2026-08-30) {#wire-keys-full-words}

Reviewer feedback: after the move to JSON text formats, the 2-5-char field keys (`su`, `eat`, `fen`,
`nwe`, ...) are illegible and force manual remapping when reading raw objects. Wanted: human-readable
identifiers everywhere, matching the struct/enum names, no mapping table needed.

Resolved by `docs/superpowers/specs/2026-08-28-cas-semantic-wire-keys-design.md`, shipped as the
semantic-wire-keys generation reset: every persisted key became a "sufficiently full" semantic word
(metadata written once per object gets a descriptive name; fields repeated once per record get a
short semantic word; enum values render as full words). Exact full C++ member names everywhere —
the reviewer's literal ask — were deliberately REJECTED for repeated records and for the fixed
`cas_blob` descriptor: the design's "Rejected alternatives" section records why, and points at the
Altinity PR #2288 implementation of the rejected alternative as a worked illustration of the cost
(the descriptor no longer fits the default payload offset, and the raw uncompressed formats grow
materially). `Formats/README.md` carries the naming rule the reviewer originally quoted.

## Issue #2219: relink refusals logged at Error with stack trace (adjudicated 2026-08-21, fix queued) {#issue-2219-relink-refusal-log-level}

https://github.com/Altinity/ClickHouse/issues/2219 — CONFIRMED, cosmetic but worth fixing (the issue
documents a multi-hour false triage chasing a network fault that wasn't there; up to 53% of relink
proofs refuse under small-part load, each printing `Error` + stack trace + `NETWORK_ERROR`).

Mechanism: the throw site is fine (`DataPartsExchange.cpp` taxonomy row 3, ~:1550 — deliberately
thrown so the byte-fallback does NOT run); the noise comes from the generic queue handler
`StorageReplicatedMergeTree::processQueueEntry` (:4224-4239), whose demotion list
(`NO_REPLICA_HAS_PART`/`ABORTED`/`PART_IS_TEMPORARILY_LOCKED` → `LOG_INFO`, no stack trace) does not
know this code. `NETWORK_ERROR` is NOT load-bearing on this path — no `e.code()` branch in the queue
or fetch path tests it; "retry-later" is the default for any stored exception.

FIXED 2026-08-26 (`081c473904e`; branch `cas-relink-refusal-classification` for the antalya-26.6
PR). The code chosen is `NO_REPLICA_HAS_PART`, not the `ABORTED` this record originally planned: both
are in the demotion lists of BOTH queue executors (`processQueueEntry` AND
`ReplicatedMergeMutateTaskBase::executeStep` — the second one matters for merge-entries executed as
fetch), but `ABORTED` also sets `need_to_save_exception = false`, the exact
"treated as not an error, no backoff engages" pathology `[merge-progress-reset-mount-fence]`
documents; `NO_REPLICA_HAS_PART` keeps the exception on the queue entry. It is also the one
fetch-transient code stateless `part_log` hygiene checks whitelist (`02265_column_ttl` broke in the
CAS lanes on exactly this: PR #2159 CI, 13/14 reruns under `prefer_fetch_merged_part_size_threshold=1`).
Verified: the only two `e.code() == NO_REPLICA_HAS_PART` branches in the tree are those demotions;
`enqueuePartForCheck` keys on `replica.empty()`, not the code. Original plan text follows for
provenance: reuse `ABORTED`, which is already in the `processQueueEntry` demotion list with the
comment "Interrupted merge or downloading a part is not an error" → `LOG_INFO`, no stack trace.
Change only the exception code at the two CA retry-later throw sites in `DataPartsExchange.cpp`
(taxonomy row 3 and the `CaRelinkPromote::Unresolved` promote); the message text stays
self-describing. Verified: nothing on the fetch/queue path branches on `ABORTED` except the
demotion itself (shutdown-`ABORTED` overload is theoretical there). Also removes the misdirecting
`(NETWORK_ERROR)` label the issue complains about. Rejected: a dedicated new error code + a fourth
demotion branch (touches upstream, vetoed); `PART_IS_TEMPORARILY_LOCKED` (foreign semantics + a
`cleanup_thread.wakeup()` side effect); demoting all `NETWORK_ERROR` (hides real network faults).
Sender side already logs at `Debug` — receiver only.

## Nested `server_root_id` + prefix-based decommission victim selection (2031-triage CAS-007) {#nested-srid-decommission}

`validateServerRootId` accepts slashes (`Pool/CasServerRoot.h:199-229`; `gtest_cas_mount.cpp:97` asserts
`shard-01/replica-a` valid, and multi-segment srid support was deliberately fixed in `b97847d32f9`),
while `CasDecommission` picks victims by path prefix (`Tools/CasDecommission.cpp:146,150` +
prefix-LIST drains of `cas/manifests/<srid>/`, `staging/`, `roots/`). So
`SYSTEM CAS DROP POOL MEMBER 'a'` destroys the namespaces and control objects of a LIVE member
`a/b`. Existing test covers only the sibling case (`gtest_cas_decommission.cpp:699-716`). Partial
mitigation exists in one direction only: mounting `a` when `a/b` already exists fails closed
(`CasServerRoot.cpp:145-149`); the reverse order is unguarded. Note the asymmetry — relink routing
compares srid for EXACT equality (`ContentAddressedMetadataStorage.cpp:2021`), i.e. the prefix rule
is decommission-local.

Fix options (decide at fix time): (1) make decommission victim selection exact-srid + an explicit
refusal when any other member's srid is prefixed by the victim's, or (2) forbid nesting at validation
(reject a srid that is a prefix of, or prefixed by, an existing member's) — cheaper but removes the
multi-segment layouts that were deliberately enabled. Either way add the nesting case to
`gtest_cas_decommission.cpp`. P2: destructive but operator-initiated and requires a nested-srid
layout.

## Empty conditional token passes as an unconditional write on the Native backend (2031-triage CAS-010) {#empty-token-unconditional-write-guard}

`CasObjectStorageBackend`'s conditional-write path validates only the token TYPE
(`mintingTypeMatches`, `CasObjectStorageBackend.cpp:918-930`), and `Token::type` defaults to `ETag`
(`Primitives/CasTypes.h:259`), so a default-constructed `Token{}` passes every check; the S3 writer
then omits `If-Match` for an empty string (`WriteBufferFromS3.cpp:656-657,746-747`) and the "fenced"
write becomes an unconditional clobber. Emulated/InMemory backends fail closed, so the gap is
Native-only and invisible to the emu doubles.

No call site intentionally passes an empty expected token: callers either guard `head.exists` or use
`optional<Token>`. Generation-token paths additionally validate the numeric generation value, but an
empty ETag can still pass the type-only guard in `putOverwrite`, `casPut`, or `deleteExact`. The one
path that can legitimately produce an empty token on a `Done` write is `tokenFromWriteResult`'s
`HEAD` fallback, whose value flows into
`MountLeaseKeeper::last_token` (`CasServerRoot.cpp:1551,1688,1747,1828`); reaching it needs two
simultaneous store anomalies. Fix: reject an empty ETag token at the conditional-write entry
(fail-closed `LOGICAL_ERROR`-class throw) so no future call site can turn a fence into a clobber,
plus a Native-backend unit test. P2 — missing guard, not a demonstrated data-loss path.

## `[s06-column-subset-verdict-measures-an-untouched-counter]` S06's column-subset check counts `CASBlobGet`, which the data read path never increments {#s06-column-subset-verdict-inert}

**Found by rerunning S06 at `--scale full` (2026-08-31) after `dev` and `ci` both left it
`INCONCLUSIVE`.** Scale is not the blocker, and no scale setting can be.

The verdict asks whether a few-column `SELECT` fetches far fewer blobs than an all-column scan, and
measures it as a `CASBlobGet` delta around each scan. At `full` — 10,000 columns, 2,000-row blocks —
both windows measured **zero**, so the card took its `all_gets == 0` branch and declared itself
inconclusive. It did the honest thing; the premise it needs was never established.

Zero is not explained by the reads being cheap or cached, because the run demonstrably moved data:
`CASBlobPut` 61,650, 60,078 objects and 300 MB in the pool. The discriminator is which code path
increments the counter, and the data read path is worth spelling out because it looks at first like
it could not possibly avoid the backend — a blob carries a fixed-length envelope, so its payload
cannot be read as if it started at byte zero.

It does not. `CasManifestReader::locate` returns a `BlobLocation` whose `offset` is the pool's
**fixed** `blob_header_len`, so no per-object header read is needed. `readBlobPayload` then calls
`object_storage->readObject` — the generic `IObjectStorage`, not `Cas::Backend` — and wraps the
result in a `ReadBufferFromFileView` starting at that offset. So the envelope IS skipped, by the
view's start offset rather than by a backend call, and `CasInstrumentedBackend` is nowhere on the
path. `checkOpAdmitted(CasOpClass::ContentRead)` is the only CAS-side gate on it, and it counts
nothing.

`CASBlobGet` is raised only in `CasInstrumentedBackend::get`, on the metadata/object path GC and the
ref machinery use; the sibling `CASBlobGetStream` is the backend's streaming get and is equally off
this path, which is why it too stayed at zero. The cumulative counters show the split directly:
`S3GetObject` 50,146 against `CASBlobGet` 14,509.

So the check is inert by construction, at `dev`, `ci` and `full` alike, and the two prior
`INCONCLUSIVE` verdicts carried no information about the property under test.

**Fix:** measure the delta the read path actually moves — `DiskS3GetObject` (or `S3GetObject`) —
and keep `CASBlobGet` only where a CAS backend get is genuinely expected. **Confirming experiment
before changing the verdict:** run the two scans unchanged and record both counters; the fix is
warranted only if `DiskS3GetObject` moves while `CASBlobGet` stays flat. If neither moves, the reads
are being served locally and the card needs a cache-drop, which is a different defect.

**The sweep is done, and it found one more.** Four cards touch `CASBlobGet`:

- **S06** (`s06_s08_manifest_parts.py`) — the verdict above.
- **S21** (`s19_s22_clone_fetch.py`) — `column-subset fetches only required blobs`, gated on
  `1-column CASBlobGet << all-column CASBlobGet`. **Same defect, same inert branch**, and it too has
  been `INCONCLUSIVE` at every scale it has been run at. Fix both together.
- **S12/S14** (`s12_s14_faults.py`) — reads the counter into observations, but its verdict is about
  `CASRootList`/`CASRootGet` at startup, which genuinely do traverse the CAS backend. Correct as is.
- **S41** (`s41_wide_insert_baseline.py`) — observations only, and its own docstring states that
  "`CASBlobGet` is a metadata GET here". That is independent corroboration of the discriminator:
  the counter's metadata scope was already understood and written down in this tree, which is what
  makes S06's and S21's use of it to measure column-data reads wrong rather than merely unlucky.

## `[s23-idle-rss-growth-500mb]` 500 MB of RSS growth on an empty, idle pool over 15 minutes {#s23-idle-rss-growth}

**Found by rerunning S23 at `--scale full` (2026-08-31), which FAILED where `dev` and `ci` were
inconclusive.** The `dev` window is 4 minutes at 5 s per minute; `full` is 15 real minutes, and that
is what made the verdict discriminate.

`memory flat over idle window` failed with `idle_rss_growth` = **500,146,176 bytes**. Per node,
resident went 677 MB to 1,178 MB on `ch1` and 641 MB to 1,087 MB on `ch2`. It is not only allocator
noise: ClickHouse's own `mem_tracking` roughly doubled too, 337 MB to 666 MB.

What makes it worth a look is what the pool held while this happened: **nothing**. `pool_shape` is
zero across every class — no blobs, no manifests, no refs, no roots, no files — `leftover_ca_tables`
is empty, `max_s3_ops_per_round` is 0, and GC deleted nothing across 11 successful rounds.

**Not yet established as a leak, and the reason is the window.** Fifteen minutes cannot separate a
server settling into its steady state — thread pools, caches and arenas materializing after boot —
from growth that does not stop. Peak resident (1,158 MB) sits close to final (1,181 MB), which is
weak evidence for a plateau.

**There is no `trace_log` evidence for this, and three separate things must change before a rerun
can produce any.** Asked whether the trace told us where the 500 MB went, the answer is no:

1. **The artifact dump does not collect it.** `predown_dump.sh` loops `for tt in CPU Real` — the
   `Memory` trace type is never queried, so no allocation stacks reach the run directory.
2. **There would be nothing to collect.** `total_memory_profiler_step` — the server-level knob that
   samples background and idle allocations — defaults to **0**, meaning off, and `ca-soak`'s
   `profiling.xml` sets only the two query profilers. The per-query `memory_profiler_step` (4 MiB)
   cannot fire during an idle window because no query is running. This is why the earlier one-hour
   chaos soak did have ~25k `Memory` samples while an idle run would have none: those came from
   queries.
3. **What was collected is not time-scoped — but the capability exists and is simply unused.**
   `predown_dump.sh` already accepts `FROM_TS`/`TO_TS` and folds them into a `${WINDOW}` clause on
   every trace query; the scenario runner just never passes them. So S23's `Real` aggregate spans the
   whole server lifetime, and although it is full of CAS write frames — `commit` 380 samples,
   `publishStaging` 378, `publishBlob` 365, `fanOutBlobUploads` 368, and
   `CityHash128BlobHashingWriteBuffer::nextImpl` 172 — those most plausibly belong to the setup phase
   and cannot be attributed to the idle minutes either way. The pool was empty by the end. Fixing
   this is a matter of the card passing the window it already knows, not of adding a feature.

**Items 1 and 2 are now fixed and verified (2026-08-31).** `predown_dump.sh` dumps `Memory` alongside
`CPU` and `Real`, and `configs/memory.xml` sets `total_memory_profiler_step` to 4 MiB. The setting
had to go in `memory.xml` rather than `profiling.xml`, because the latter is mounted into `users.d`
where a server setting is ignored. Verified on a live cluster: the setting reports `changed = 1`, a
write workload produced **1,064 background** `Memory` samples where the count would previously have
been zero, and the dump wrote 1.27 MB of `Memory` stacks with no error output.

**Experiment that decides it,** with those three fixed first: set `total_memory_profiler_step` to a
few MiB on the server, have the dump query the `Memory` trace type, and scope the aggregate to the
idle window by `event_time`. Then run one idle scenario with a 60-plus-minute window, sampling RSS
and `mem_tracking` each minute, and read the shape of the curve rather than its endpoints. A plateau
means the verdict's threshold is wrong for a freshly booted server; continued linear growth on an
empty pool means a leak, and the `Memory` stacks will say whose. Do not file a leak against the
product on the 15-minute number alone.

## `[s10-patch-premise-blocked-by-a-hardcoded-workaround]` S10 forces the non-patch delete path, so its patch-part premise cannot hold {#s10-patch-premise-blocked}

**Supersedes an earlier entry of mine that named the wrong cause.** I first attributed
`max_patch_parts_observed = 0` to `apply_patches_on_merge` being set at session level. That setting
defect is real, but it is not why patch parts are absent.

The actual cause is one line: `lw_supported = False` is **hardcoded** in S10, so the `if
lw_supported:` branch never fires and the lightweight `DELETE FROM` beneath it is dead code. The card
falls through to `ALTER TABLE ... DELETE`, which produces mutations, not patch parts. No setting and
no scale can produce a patch part while that line stands.

The comment above it states the reason: lightweight `DELETE` is "unreliable on this build (CA storage
path diverges)". **That claim is the thing to test first**, because the two outcomes lead opposite
ways. If it is stale, the workaround is the only blocker and removing it restores the premise. If it
is current, then lightweight `DELETE` diverging on the CA path is a product defect that deserves its
own entry and its own test, rather than living as a silent `False` inside one scenario — a card that
works around a product defect hides it.

**Secondary, and worth fixing regardless:** `_probe_patch_parts` issues `SET apply_patches_on_merge
= 1`, which throws `Code: 115` because the name is a **MergeTree table setting**
(`MergeTreeSettings.cpp:861`), not a session one. The probe then returns `True` anyway, because
`allow_experimental_lightweight_update` was accepted and any single acceptance satisfies it. So the
probe reports patch support on the strength of a setting that does not by itself produce patch parts,
and swallows the one genuine rejection into an observation. Apply it with `ALTER TABLE ... MODIFY
SETTING` or at `CREATE`, and make the probe require the setting that actually matters rather than any
of them.

## `[s10-one-unreachable-manifest-survives-gc-fixpoint]` A single unreachable manifest survives GC to fixpoint, three rounds running {#s10-manifest-residual}

**Found at `--scale full` (2026-08-31); the same run as the entry above, so read that first — the
patch-part premise did not hold, and this residual arises from the lightweight-delete workload
rather than from patch content.**

`no unbounded leftovers` and `obsolete patch content reclaimed` both failed on one object:
`fsck_final` reports `unreachable: 1, dangling: 0` against `reachable: 163`, and
`residual_classification` puts it in `leak` as `unreachable:_manifests: 1` — not `pipeline`, not
`bookkeeping`. `reclaimable_drain_check` agrees it is reclaimable. `gc_fixpoint_history` is
`[1, 1, 1]`: GC was driven to fixpoint three times and the residual did not move.

One object is small but the shape is not benign — a reclaimable, unreachable manifest that survives
repeated fixpoints is by definition not being reclaimed. Also worth a glance in the same run:
`graduation_drain_history` is a run of ones with a single **128** in it.

**Before calling it a product defect:** identify the object and what still points at it, and
re-derive whether the workload can legitimately leave it (a manifest published after the fold's
coverage seal is bookkeeping, not a leak). The classification is the card's, and the card's premise
was broken in the same run.

## `[ca-event-log-loses-gc-manifest-deletes]` GC deleted 517 manifests and the CA event log recorded one {#ca-event-log-loses-manifest-deletes}

**Found while auditing S10's leftover-manifest finding in `cas_log` (2026-08-31).** This is why that
audit could not be done.

In the full-scale S10 run, `ch1`'s `gc_log` sums to exactly **517** `manifests_deleted` across its
rounds — round 6 alone deletes 133 — while `ch1`'s `cas_log` holds exactly **one**
`manifest_delete` event. `ch2` deleted none and logged none, so the whole discrepancy sits on one
node.

It is not a semantics mismatch between the two numbers. `CasGc.cpp` PHASE 15/18 emits
`CasEventType::ManifestDelete` **unconditionally, once per attempt**, inside the `mf_cleanup_now`
loop, and increments `report.manifests_deleted` only when the outcome classifies as `Deleted`. So
attempts are greater than or equal to deletions, and the event count must be **at least** 517.
It is 1.

One event did land — round 1, for a namespace owned by the other server root — so the emitter is
wired and reachable. That argues for events being dropped or lost rather than never produced.
Candidates not yet distinguished: a bounded queue in the event sink discarding under burst (round 6's
133 deletions in one phase is exactly a burst), a per-call `EventEmitter{*store}` binding to a sink
that is not the disk's configured log on most calls, or rows buffered in `ca_event_log` and lost when
the scenario tears the cluster down.

**Consequence for anything that reads `cas_log`:** manifest reclaim is effectively invisible there,
so `cas_log` cannot support any claim about whether a manifest was deleted, retried or skipped. The
S10 residual finding must be re-derived from `gc_log` and `fsck` until this is fixed.

**First step:** count events against `attempted` rather than against `manifests_deleted` — the phase
already records `attempted` as a metric — and check whether `system.ca_event_log` shows drops of its
own before looking for a bug in the emitter.
