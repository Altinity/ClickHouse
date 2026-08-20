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

## Topic files {#topics}

| File | Items | Covers |
|---|---|---|
| [`BACKLOG/ref-protocol.md`](BACKLOG/ref-protocol.md) | 11 | Rev.6 lease-boundary exclusivity, the ref-lane state machine, ref-ledger internals. Top items: `[Late Predecessor PUT]`, `[PART-WRITE-RELEASE-SEAM]`, `[MOUNT-CLAIM-EPOCH-REGRESSION]`. |
| [`BACKLOG/gc.md`](BACKLOG/gc.md) | 46 | GC scalability & byte cost, correctness/observability follow-ups, throughput-collapse and fsck-vs-GC RCAs. Top items: `[gc-frontier-one-list]`, `[GC-DEFER-DECISION-LIST-COST]`, `[gc-rebuild-lease-interlock]`. |
| [`BACKLOG/mounts-and-lifecycle.md`](BACKLOG/mounts-and-lifecycle.md) | 10 items + 2 prose sections | Mount-lease/fence recovery, CA disk lifecycle (rev.8 residuals), pool bootstrap, operator recovery. Top items: `[POOL-REFUSAL-NODE-FATAL]`, `[decommission-successor-mount-race]`, disk-lifecycle-leak (deferred, prose section). |
| [`BACKLOG/formats-and-storage.md`](BACKLOG/formats-and-storage.md) | 23 | Staging/adoption, real-store backends (S3/GCS/Azure), the local/emulated backend, codec/format items. Top items: `[GATE #1: Azure]`, `[disk-error-audit]` temp-file+rename, `[sec4-decoder-size-bounds]`. |
| [`BACKLOG/replication.md`](BACKLOG/replication.md) | 7 | `MOVE PART`/`PARTITION` onto CA disks, merge/insert retry vs. the mount-lease fence, cross-replica relink. Top items: `[move-part-to-ca-architecturally-unimplemented]`, `[merge-progress-reset-mount-fence]`, `[RPL-5 slice]`. |
| [`BACKLOG/testing-and-ci.md`](BACKLOG/testing-and-ci.md) | 38 | Test coverage & harness, gate-filter/gate-suite gaps, soak/chaos hygiene, standing testing-methodology rules. Top items: `[gate-filter-gap-3-backend-contract]`, `[rule-no-chassert-over-handled-branch]`, `[4h continuous chaos soak]`. |
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

## `[gc-round-budgets-are-not-backpressure]` Round budgets throttle the consumer while the producer is unaware — four defaults changed, the real fix is a time deadline {#gc-round-budgets-not-backpressure}

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

- **[gate-filter-countingbackendshape-escape] gtest suite `CountingBackendShape` escapes the `CAS*` filter** — TEST/INFRA — Found during Task 14 (AGENTS.md) review: this suite does not carry a `Cas`/`CAS` prefix, so it is invisible to the `CAS*` gtest gate filter documented as the covering mechanism. Same class as the three prior gate-filter-gap findings in this file (testing-and-ci.md, `{#gate-filter-gap-3-backend-contract}`) — rename the suite or extend the filter.
- **[gc-anomaly-never-emitted] `CasEventType::GcAnomaly` is defined but never emitted** — MINOR — Found during the deep-verification batch (batch-006): the event type exists in the enum but no call site constructs one, so any doc or dashboard describing GC-anomaly events as observable is currently wrong. Either wire an emit site or remove the dead enum value. (An orphaned 2026-08-04-triage finding on catalog/fold-seal capacity-reservation correctness is adjacent to this GC-observability gap — folded in as a related note, not a separate item.)
- **[reftxnid-wraparound-guard-missing] `nextRefTxnId` lacks a `UINT64_MAX` wraparound guard** — MINOR — Found during the deep-verification batch (batch-021, cluster C-0514): the sibling counter at `CasRefProtocol.cpp:941` has an explicit wraparound guard; `nextRefTxnId` does not. Add the matching guard or document why it is provably unreachable. (Confirmed 2026-08-04: this is the same finding as orphaned-open cluster C-0514 from the docs-consolidation triage — already tracked, not a separate item.)
- **[system-md-missing-cas-verbs] `SYSTEM CAS` verbs missing from `docs/en/sql-reference/statements/system.md`** — DOC — Found during Task 12 (operations runbooks): `SYSTEM CAS FSCK`/`FORGET`/`GC STOP`/`GC START` (and siblings) are documented in the CAS-specific pages but absent from the generic `SYSTEM` statement reference, where a user would naturally look first.
- **[casrequestcontrol-comment-settings-stale] `CasRequestControl` header comments cite settings that do not exist** — DOC — Found during the Task 12 fix round: the header comments name `cas_s3_retry_initial_backoff_ms`/`cas_s3_retry_max_backoff_ms` as if they were configurable settings; they exist only in the comment text — the real budget is hardcoded in `CasRequestBudget`. Either implement the settings or fix the comments to stop implying a configuration surface that isn't there.
- **[s3cache-config-comment-stale] stale comment in `utils/ca-soak/configs/storage_conf_s3cache_ch1.xml`** — MINOR — The comment claims cache-over-CA fails with `NOT_IMPLEMENTED`; this was fixed by `3ed0e5f5030` (2026-07-08) and the cache-over-CA path is now live-validated (see the quick-start cache example, `380688e8a66`). Remove the stale comment.
- **[part-folder-validate-never-gating] `part_folder_validate=never` needs a gate, not a silent accept** — HARD (user settings-policy direction) — `PartFolderAccess.h:135-138` accepts `never` (skip the `ForceFresh` body re-proof entirely) with no acknowledgment of the risk. Either remove the `never` value or require an explicit risk-acknowledgment setting alongside it. Docs already carry a strong warning on this value (`configuration.md`); the code should not make it this easy to select silently.
- **[gc-enabled-false-silent] `gc_enabled=false` accumulates garbage silently** — HARD (user settings-policy direction) — Disabling the background GC scheduler produces no ongoing signal that reclamation has stopped. Add a periodic warning log line plus a metric while `gc_enabled=false` and the pool has reclaimable debris, so an operator who disabled GC for a legitimate reason (or by mistake) finds out before the pool grows unbounded.
- **[dedup-presence-only-window-recheck] re-verify the deduplication presence-only-admit corruption window** — VERIFY (user-flagged from memory, re-derive from the disk-error audit) — The 2026-07-21 disk-error audit (operability-and-introspection.md, disk-error-audit-followups) identified a presence-only dedup admit as a corruption-window class; re-verify against HEAD whether this window is still open post the format/staging changes since that audit, and either close it out or fold it back into an active item with current evidence.

## `[gcs-conditional-overwrite-rethink]` GCS conditional overwrite needs re-thinking from the premise, not a bigger cap {#gcs-conditional-overwrite-rethink}

**SCOPE NARROWED (2026-08-04): the resurrect path no longer has this problem.** The condemned-blob
resurrect is now an UNCONDITIONAL write (`Backend::resurrect`): on remote object storage it streams
and takes multipart -- size-unlimited on GCS too; the local emulated mode materializes one body at a
time (see the spill-to-disk debt below). What remains capped is the CONDITIONAL write-once
CREATE (`If-None-Match`), still forced single-part under `gcs_max_conditional_put_bytes` -- so this
item is now only about creating a blob larger than the cap on GCS, and everything below about the
overwrite/resurrect shape is historical context.

GCS honours no preconditions on multipart completion — Google's own XML API documentation says
"Preconditions are not supported in the requests", and it was measured independently on 2026-07-03
(`0a3bc2f1fc6`). A lost precondition there does not fail the write, it **silently overwrites**, which
is the one outcome CAS's whole token protocol exists to prevent. Hence `s3_force_single_part_upload`
for generation-token stores, and `gcs_max_conditional_put_bytes` (1 GiB) as its forced companion: with
multipart off, the body must go in ONE part, buffered whole in RAM.

The consequences are larger than a settings row suggests, and they are worth re-deriving rather than
patching:

- **A conditional overwrite of a body above the cap is impossible on GCS**, not slow. Blob bodies have
  no size cap, so a large-enough blob simply cannot be resurrected from a condemned incarnation there.
- **Raising the cap does not help**, it just moves the memory ceiling: one part means one RAM buffer.
- The streaming design (`docs/superpowers/specs/2026-08-04-cas-streaming-conditional-overwrite-design.md`)
  fixes S3 and deliberately does NOT fix this — it only makes the limit fail early and legibly.

The mechanism named when the limit was introduced is: unconditional multipart to a TEMPORARY key, then
a CONDITIONAL `Compose` onto the target. ClickHouse already carries `S3::ComposeObjectRequest` and
`Client::ComposeObject` (added for GCS copy), but the request exposes no precondition — it would need
`x-goog-if-generation-match` through `GetRequestSpecificHeaders`, i.e. a change to SHARED UPSTREAM code
in `src/IO/S3/`. It also creates a debris class that does not exist today: temporary keys orphaned by a
crash between the upload and the compose, which GC or fsck must then own.

**What to re-think, before designing that.** Whether `Compose` is even the right primitive; whether the
temp-key debris is acceptable given that every other CAS object is either write-once or exactly-token
deleted; whether GCS should instead be documented as supporting CAS only below a stated blob size;
and whether the upstream change is worth it for one backend. The answer may legitimately be "cap it,
document it, and refuse bigger blobs" — that is a design decision, and it has not been made.


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

## `[emulated-resurrect-should-spill-to-disk]` The emulated resurrect sits ON a local disk and still builds the body in RAM {#emulated-resurrect-spill-to-disk}

The emulated (local object storage) resurrect materializes the whole `[header][payload]` in a
`String` before installing it. Today this is bounded by serializing resurrections process-wide — one
body at a time, so the peak is the largest single body — which restored the guarantee the deleted
byte-weighted admission used to give. But the bound is the wrong SHAPE for this backend: the whole
point of the emulated mode is that the storage IS a local disk, so the natural staging area for a
body of any size is a scratch file next to the destination, not gigabytes of RAM.

The debt: stream the reader into a temp file under the emulated root (or the pool scratch path),
then install it under `emu_mutex` — rename where the object layout allows it, read-back+`emuWrite`
where it does not. That removes both the materialization AND the serialization (no reason to run
resurrections one at a time once they stop competing for RAM), and the size guard moves to the spill
loop, still refusing before anything becomes current. Temp-file lifecycle must be airtight on every
exit path: success, size mismatch, exception mid-drain — no residue in any outcome.

Not urgent: the emulated mode serves CI lanes, local development, and tests — no production
deployment runs CAS over local paths, and a single materialized body was this path's behaviour for
its whole life. It is recorded because the current shape is a contradiction (a disk-backed backend
buffering in memory), not because anything is on fire.

## CAS-021 (issue #2207) adjudication follow-ups: controller-outcome honesty + condemn-memo staleness (2026-08-20) {#cas-021-followups}

Adjudication of https://github.com/Altinity/ClickHouse/issues/2207 (two read-only code sweeps against
HEAD `684161dcc03`): all six quoted controller behaviors are real, but every claimed integrity
consequence is neutralized on the current tree — the delete path is guarded by the normative
delete-site in-degree re-read (`CasBlobInDegree.cpp:423`, spec §5 arm 3), the exact-token delete
against a resurrect-rotated `incarnation_tag` (`CasPartWriteTxn.cpp:741`), and the [C2] fence checks;
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
  Condemned marker satisfies the predicate by design, same as the `:137` arm") and
  `writeResurrectMetaClean` ("false Committed = desired Clean record already durable — benign");
  rename the pin tests to read as spec. ~150-250 line diff + test renames; controller = adversarial
  review mandatory. This addresses the CORE of CAS-021 at the type level: the external auditor's
  reading becomes impossible to write.
- (2) **Stale condemn-marker memoization — ACCEPTED RESIDUAL, do NOT fix with re-reads** (user
  decision 2026-08-20): the in-process `condemn_markers_confirmed` note survives a legitimate
  `Condemned -> Clean` transition (no `forgetCondemnMarker` on resurrect-without-intervening-fold),
  so `confirm_condemned_marker` (`CasGc.cpp:1885`) can graduate an entry whose durable meta says
  Clean. Consequence when it fires (ultra-rare race): ONE spurious `deleteExact` — an S3 DELETE,
  which is FREE — self-healing at `CasGc.cpp:862-870` (TokenMismatch drops the confirmation, meta
  untouched). The re-read fix would cost +1 BILLABLE GET per graduating condemned entry on the
  COMMON path (P9 GET-budget class) to save free DELETEs in a rare race — worse than the disease.
  No zero-cost invalidation exists either (the window is by definition "nothing observed the
  resurrect"). Only sanctioned improvement: the observability LABEL at the self-heal site (counter/
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
