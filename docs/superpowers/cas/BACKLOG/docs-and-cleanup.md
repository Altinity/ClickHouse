---
description: 'Live backlog — architecture/refactoring (no behavior change), documentation debt, and minor/polish items.'
sidebar_label: 'Docs & cleanup'
sidebar_position: 9
slug: /superpowers/cas/backlog/docs-and-cleanup
title: 'CAS Backlog — Docs and cleanup'
doc_type: 'guide'
---

# CAS Backlog — Docs and cleanup {#docs-and-cleanup}

Part of the [CAS live backlog](/superpowers/cas/backlog). Topic file for deferred architecture/
refactoring work (no behavior change), documentation debt, and minor/polish items.

## Architecture / refactoring (deferred, no behavior change) {#refactoring}

- **[refactor: CasGc split] break `CasGc.cpp` into workflow units** — DESIRABLE — Split scan / reachability / deletion / cursor / budget out of the 2.3k-line file; keep `Gc` as orchestration (pure extraction). Author's second-highest-value refactor. (An orphaned 2026-08-04-triage finding covers the same split, plus a separate "centralize backend token policy" half that already landed as C1/C2 above — folded in as confirmation.)
- **[refactor: Store de-god-classing] extract remount-thread / caches / ref-append-lane out of `Cas::Store`** — DESIRABLE — 8-responsibility god class; friend-triangle with `Build`/`Gc`.
- **[refactor: Store::open modes] split into create / open-rw / open-ro** — MINOR (real bug behind it) — Read-only `Store::open` can still write `_pool_meta` on an empty pool (`PoolMeta::createOrValidate`); make read-only semantics visible (`createOrLoad` vs `loadExisting`) or pass `create_if_missing=false` when `read_only`.
- **[DiskSelector per-disk isolation]** — HARD / upstream — `DiskSelector::initialize()` has no per-disk try/catch; one unreachable disk aborts disk-selector init server-wide. Pre-existing upstream gap; carve to an upstream PR (Group G).
- **[Group G] carve generic Ring-2 fixes into separate upstream PRs** — {#refactor-group-g} — MINOR (fork hygiene) — Shrinks the fork's long-term conflict surface: `ThreadStatus parent_thread_group` (B90), `ReadBufferFromFileView` (B115), `ReadBufferFromS3` cancel-stop (B117), `LocalObjectStorage` TOCTOU (B38), `MergeTreeDeduplicationLog` null-writer (B37), `copyS3File message_format_string`, `Expect:100-continue` opt-in, `S3Exception::isPreconditionFailed`, GCS conditional dialect + GOOG4 signer, generic conditional-S3-write plumbing. Non-blocking.

## Refactoring candidates, derived from what actually broke {#refactor-candidates-from-defects}

Ranked by value-per-risk, each backed by real defects it would have prevented.

1. **DONE, differently than proposed.** "Make catalog-life absence expressible in the type
   (`resolveLifeOrSentinel` → `std::optional`)" was the top item here; the current API already does
   this under a different name — `CasRefCatalog::lifeIfCataloged` returns
   `std::optional<NamespaceLifeId>`.
2. **One life resolution per round, threaded — not re-derived.** Five mechanisms still answer the
   same question across ~80 call sites (`resolveNamespaceLife`, `discoverUniverse`,
   `stageATransition`, `fromCatalogEntry`, plus the now-optional lookup). The fold has
   `FoldResult::live_incarnation` to consult instead of re-resolving; fsck and decommission still
   re-resolve per call.
3. **The destructive gate collapses per-namespace facts into a pool-wide boolean.**
   `suppress_destructive` is a single scalar OR over every namespace's anomalies/holds/frontier
   state, so one un-cataloged namespace stalls reclamation for the whole pool. Wants to be
   per-namespace.
4. **`Gc/CasGc.cpp` and `Pool/CasRefLedger.cpp` are ~18% of the subsystem by line count** — not an
   aesthetic complaint, real defects have hidden in both files' size. Extraction needs equivalence
   fences written BEFORE the move, not after; not during open Criticals.
5. **The fixture/production divergence should be one named seam, not a habit.** Raw test helpers
   write at the sentinel and bypass birth in ways production code never does; one documented helper
   instead of ad hoc divergence.
6. **Keep converting prose rules into executing checks.** Two conversions already held immediately
   (`FsckReport::clean` from a `static_assert`-guarded list; the gtest suite-list generator failing
   loud on any unclaimed suite) — every remaining "whenever X, also do Y" code comment is a candidate.

## Minor / polish {#minor}

- **[RUSTFS-ERROR-XML] SDK cannot parse RustFS error-body exception name** — MINOR — every RustFS `PreconditionFailed` logs `Unable to parse ExceptionName: PreconditionFailed Message: …` (18.5K in one soak window, `system.blob_storage_log`): the error XML is not AWS-shaped, so the AWS SDK fails to extract the name and CAS recognizes the condition from the message TEXT — works, but brittle against wording changes; add a shape-tolerant parse (or a startup capability note) before relying on it wider.
- **[Issue-6] B3 GC-health columns denormalized onto non-local mount rows** — MINOR — The four `GcHealth` columns are stamped identically on other servers' mount rows (`StorageSystemContentAddressedMounts.cpp:159-162`); NULL them on non-local rows.
- **[F4] CA `MOVE PARTITION` publishes ref CAS before validating the target disk is in the storage policy** — MINOR — S19: 2 `CasRootCas` ops during a correctly-rejected `UNKNOWN_DISK` move (safe, dangling=0); validate the destination disk in-policy before any ref CAS.
- **[snappatch-minor] `CasStore.cpp:2007` replay throw escapes `trySnapshotPublishOnce` without arming publish backoff** — MINOR (defensive) — Dead today by the `min(tail)>newest_snapshot_id` invariant; defensive-pass candidate (likely removed anyway by rev.6).
- **[Build::promote owner-liveness guard is race-only]** — MINOR — Fires only in the narrow promote-vs-dropNamespace window; make the race deterministically testable or remove the guard with a TLA argument.
- **[Ring-2 comment/convention nits]** — MINOR — `S3Common.h` comment overclaims for the RetryStrategy site; `static_assert(DEFAULT_EXPECT_CONTINUE_MIN_BYTES==0)`; `MergeTask::projection_uses_parent_transaction` could be a local; `ProfileEvents.cpp` changelog fragment in a description; `_ms` suffix on a `DateTime64(3)` column; the new `GC REBUILD` right abbreviates "GC" vs the sibling spelled-out "GARBAGE COLLECTION"; internal `cas_part_folder_cache_*` names outlived the key rename; `05011` `no-parallel` tag droppable; empty untracked `poc/` dir husk.
- **[C2-followups] more pagination loops for `forEachListedKey`** — MINOR — Three more identical loops in `CasRefIntake.cpp`/`CasServerRoot.cpp`; `forEachListedKey` also lacks a stop-on-true/page-boundary hook to let `deletePrefixWholesale` + ns-cleanup migrate (interface addition, design first).

- **[stale-recover-ref-table-comments] 3 comments still name the dead `recoverRefTableDetailed`/`recoverRefTable` functions** — MINOR — `Gc/CasOrphanManifestSweep.cpp:32`, `:167`, `Gc/CasGc.cpp:80`; the functions were replaced by `recoverRefTableDetailedFromAuthority`; comment-only fix.
- **[fsck-short-keys-spell-out] short keys `ns`/`me`/`p`/`ha` in `CasEvent::detail` / fsck-report maps: spell out or keep** — MINOR, USER DECISION — user-facing abbreviations in `CasFsck`/`CasGc` detail maps were deliberately left outside the obscure-names rename; decide spell-out vs keep, then a small follow-up rename if spelling out.
- **[prev-indeg-rename-never-happened] correction of record: commit `60691b11e7f`'s message claims a `prev_indeg` → `prev_indegree` rename that did not happen** — MINOR — the key had no emitter (it existed only in two description strings, which were replaced with really-emitted keys); commit messages are immutable, this entry is the durable pointer; no action.

## Source-layout refactoring residue (2026-07-16) {#source-layout-residue}

- **[source-layout-bisect-hazard] source-layout intermediate commits `592b9b8..9d714dd8` are not clean-buildable — a bisect hazard** — MINOR — A Phase-2 include sweep stranded 3 external-consumer include fixes outside the sweep commit's pathspec, so those intermediate commits reference moved CA headers at dead paths (per-step gtest only looked green because incremental builds saw uncommitted working-tree fixes). Accepted as-is (dev branch, not upstream, no-amend/no-rebase rule) — a bisect landing in that range fails to build the external consumers; document and route around it. Lesson for future reorg sweeps: pathspecs must include every sweep-touched file including external consumers, and verify the COMMITTED state builds, never trust incremental-build green for a move/sweep.
- **[source-layout-casstore-followups] source-layout post-decomposition `CasStore` follow-up candidates** — Task 3.6 composition-root checkpoint on `Pool/CasStore.{h,cpp}`: `CasStore.cpp` = 1184 lines, `CasStore.h` = 771. The spec's "~400" target was a stale pre-3.5 estimate that did not budget the mount claim/recovery orchestration 3.5 correctly kept inline. All planned components are extracted; honest post-decomposition size ≈1180 is accepted (mostly irreducible mount protocol). Two stray inline blocks flagged as OPTIONAL future component candidates (not extracted now): LIST-discovery (`listNamespaces`+`listMirroredChildren`, ~112 lines, a clean stateless-service extraction candidate) and anomaly-policy (`reportImpossibleInterference`+`peekForeignRefLogHeader`, ~113 lines, deliberately kept on Store by the 3.5 mount plan). Extracting both would reach ≈960 — still far from 400 because the mount protocol dominates. Low priority; the composition root is sound as-is.
- **[phase4-blob-uploader-descoped] Phase 4 (`CasBlobUploader`) DESCOPED — writer-protocol blob lane, not a separable byte engine** — Step-4 source-layout Phase 4 ("extract `Pool/CasBlobUploader` from `CasBuild`") is DESCOPED by decision (user-confirmed), backed by two independent read-only reviews that agree. NOT done in any form. Finding: the spec's clean "byte-delivery engine vs transactional decision" split is not realizable — `putBlob`→`observeAndAdmit`→(throws ABORTED)→`uploadFromSource`→`observeAndAdmit` is one mutually-recursive machine where the adopt/resurrect decision materializes mid-delivery because write-once conditional PUTs use the 412 as the discovery mechanism; a "pure I/O engine returning an outcome struct" would require a control-flow rewrite (driver-loop + coroutine), the opposite of a trustworthy relocation. These methods ARE the blob lane of the writer protocol, not a byte engine that happens to live in `Build`; splitting one invariant's text across two files joined by `std::function` callbacks makes the audit harder, not easier. Optional future work (not scheduled): promote the genuinely state-free lambdas to free functions verbatim (cosmetic, shortens `uploadFromSource` ~300→~200, moves zero decisions); a real separable design would need extracting the adopt/resurrect DECISION as a pure "plan" first — a genuine redesign, not a mechanical move. Neither is worth it now; `CasBuild` stays whole.
- **[source-layout-build-naming] Source-layout Phase-5.2: `Build` helper-method naming consistency** — LOW-PRI — After the `Build`→`PartWriteTxn` class rename, a handful of helper/method names still contain "Build" as an English/protocol word and were deliberately not renamed (`promoteBuild`, `registerInflightBuild`, `cancelInflightBuildsForNamespace`, `startBuildFor`, `precommittedBuildFor`, `startStagingBuild`) — several are arguably correct as-is since they operate on the protocol `inflight_builds` registry concept, which the spec deliberately spares. Optional follow-up: decide per-method whether "Build" means the (renamed) class or the (spared) protocol concept, and rename only the former. Not worth expanding the rename diff now; no correctness impact.
- **[build-dir-rust-localize-drift] local build-dir config drift: rust `localize_rust_c_*` rules lost their reference-library args** — MINOR (GREEN-DEBT) — A past cmake reconfigure produced a `build.ninja` where every rust-contrib localize rule (chdig, polyglot, wasmtime, delta_kernel_ffi) carries zero reference-library args, so any future `ninja` touching a rust contrib fails in this build dir. Fix: full cmake re-configure of `build/`, and identify which configure produced the argless state to guard against recurrence. The nightly image build was unaffected (built from a binary that predates this).

## Standing hygiene / prose-review rules {#hygiene-rules}

- **[CLEANUP-dead-prerev6-keys] delete dead pre-rev.6 config keys** — MINOR — From F4a review 2026-07-21: delete dead pre-rev.6 config keys `content_addressed_allow_shared_pool` and `content_addressed_gc_grace_sec` from the ~7 integration-test XMLs that still set them, then drop both from `ContentAddressedSettings`' `non_cas_keys` skip-set so typo detection covers that namespace again. They are read nowhere in the current factory.
- **[CLEANUP-srid-naming-unify] unify `srid`/`server_root_id` naming** — MINOR — From final-review polish 2026-07-21: unify `content_addressed_garbage_collection_log`'s own `srid` column (and the `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER` input-arg shorthand docs) with the spelled-out `server_root_id` naming F3 landed for `system.content_addressed_mounts`.
- **[CHANGELOG-unknown-config-key-rejection] changelog line owed for unknown-CAS-config-key rejection** — MINOR — From final-review polish 2026-07-21: write the release-note/changelog line for the now-live unknown-CAS-config-key rejection (fails disk startup on a typo'd key; was previously a silent no-op) once the feature ships.

## New findings from the 2026-08-04 orphaned-open triage {#orphan-triage-2026-08-04}

- **[partpathparser-duplicated-path-constants] `PartPathParser` duplicating canonical ClickHouse path constants instead of deriving them** — MINOR — Concrete maintainability/consistency risk: a drift between the duplicated constants and the canonical ones would silently misparse part paths.
- **[behavior-preserving-refactor-sequence] behavior-preserving refactor sequence (remove `CasDbg*` instrumentation, centralize event emission/cursor keys, introduce `RefId`/`ObjectId`)** — DESIRABLE — A genuine but broad refactor-candidate list; low urgency, real value.

## Bucket requirements: lifecycle / Object Lock / storage-class transitions undocumented; Glacier read unclassified (2031-triage CAS-012) {#bucket-requirements-lifecycle-worm-glacier}

The settled position (a CAS pool requires a plain bucket: no lifecycle expiration, no versioning, no
Object Lock/WORM, no storage-class transitions; CAS cannot detect any of them without admin access)
is only half-delivered in the user docs. `docs/en/antalya/cas/.../bucket-requirements.md:26,29-31`
documents versioning only; a grep across all of `docs/en/antalya/cas/` finds no requirement text for
`lifecycle`, `Object Lock`, `WORM`, `storage class`, or `Glacier`. Add them there — the operator
cannot infer a requirement that is nowhere written.

Second half: a blob transitioned to Glacier surfaces as a raw `S3Exception` — no `InvalidObjectState`
handling exists anywhere in CAS or `src/IO/S3/`. It fails closed (`isObjectNotFound`,
`CasObjectStorageBackend.cpp:323-343`, does not swallow it), so this is diagnosability, not
correctness: classify that status into a message naming the storage-class requirement instead of a
bare S3 error. No restore-and-retry path is wanted (a CAS pool must not live on a restore-latency
class).

## The pool trust boundary is nowhere stated for operators (2031-triage CAS-027) {#pool-trust-boundary-undocumented}

The settled position — the bucket credential IS the whole trust boundary, and every party holding it
is trusted exactly as much as every other pool member — matches the code (nothing in the pool
protocol authenticates the writer of a control object: `CasServerRoot.cpp`'s owner/mount writes are
guarded by a conditional token, never by an identity), but it is stated nowhere an operator will
read it. A grep across all of `docs/en/antalya/cas/` finds no security or trust-boundary text: no
statement that a party with pool write credentials can retire a member (`owner` tombstone), fence
its writes (`gc_fenced`), or claim its mount slot; no guidance that the pool prefix must not be
shared with a role or tenant that is not trusted with every member's availability; and no note that
backup/log-shipping/analytics roles pointed at the pool should be read-only. Add a short
trust-boundary section (index or bucket-requirements) saying exactly that. Consequence of the gap is
operational, not a code defect: an operator can hand out pool credentials believing them to be
narrower than they are.

## Bucket requirements never state "one pool = one bucket+prefix, no replication over it" (2031-triage CAS-032) {#pool-exclusive-prefix-undocumented}

Pool identity is deliberately not tied to an endpoint or bucket (`ContentAddressedExchange.h:156-158`
rejects endpoint-based identity; `PoolMeta` carries only pool_id / blob_header_len / gc_shards /
min_reader_generation / algos_used, and `CasPool.cpp:124-128` only catches a FOREIGN pool_id — a
cross-region-replicated copy shares it, so it looks like the same pool). Read-only mounts of such a
copy fail loud or read stale rather than corrupt; the corrupting case is a WRITABLE mount of a
replication destination, or bidirectional replication over the prefix, which violates the
CAS-exclusive-prefix premise the design already assumes.

That premise is nowhere in the user docs: `docs/en/antalya/cas/.../bucket-requirements.md` never says
"one pool lives in exactly one bucket+prefix, nothing else writes there, and no bucket replication
may target it". Add it (same pass as {#bucket-requirements-lifecycle-worm-glacier}). Optional
belt-and-braces: record the endpoint advisorily in the mount lease so a mismatch can be reported —
advisory only, identity stays pool_id-based.

## Condemned-displacement comments name a `putOverwrite` branch that no longer exists, and cite a pruned BACKLOG anchor (2031-triage CAS-088) {#c2-displacement-comment-stale}

`PartWriteTxn::uploadFromSource`'s displacement block says "the two displacement calls below
(`resurrect` / `putOverwrite`)" (`Pool/CasPartWriteTxn.cpp:691-693`), but both arms now call
`Backend::resurrect` (`Pool/CasPartWriteTxn.cpp:732` staged, `:759` local) — `putOverwrite` survives
only as a controller helper (`Backend/CasRequestControl.cpp:504-521`) that this path never reaches.
The pinning tests carry the same stale vocabulary (`gtest_cas_fence_generation.cpp:376,394,417`
speak of a "`putOverwrite` displacement branch").

Same block cites the BACKLOG anchor `{#c2-resurrect-putoverwrite-fence-check}`
(`Pool/CasPartWriteTxn.cpp:691`, `:719`, and `gtest_cas_fence_generation.cpp:376`), which no longer
exists — it was pruned by `f95458a1b79`. Per the comment policy (no internal refs), the fix is to
keep the REASON (a raw backend write with no controller/fence coupling, hence the explicit
capture-at-decision + check-before-write) and drop the anchor and the `rev.7 [C2]` provenance.

Cosmetic only: the code and the fence checks themselves are correct.

## `resolveRef`'s `allow_stale` is an inert parameter with one stale doc comment left behind it (2031-triage CAS-110) {#resolve-ref-allow-stale-inert-parameter}

`CasRefLedger::resolveRef` names the parameter only in a comment
(`Pool/CasRefLedger.cpp:275`, `bool /*allow_stale*/`) and the body never branches on it; both
declarations keep it for source compatibility (`Pool/CasRefLedger.h:125`, `Pool/CasPool.h:509`) and
`Pool::resolveRef` forwards it verbatim (`Pool/CasPool.cpp:1633-1635`). This is intended: with the
snapshot+log protocol there is one authoritative cached `RefTableState` per mounted writer and no
second (per-shard decode) cache to be stale against, so the knob has nothing to select — stated at
the definition (`Pool/CasRefLedger.cpp:277-282`) and at the ledger declaration
(`Pool/CasRefLedger.h:120-122`). No behavioural residue: every caller takes the identical path.

What is owed is cleanup only: drop the parameter from both declarations and the forward and from the
two remaining `/*allow_stale=*/` call sites (`Parts/PartFolderAccess.cpp:318`, `:607`), and fix the
one comment that still describes a semantics that no longer exists —
`Parts/PartFolderAccess.h:62` (`CachedForLoad`, "stale-tolerant resolve (allow_stale=true)").
`Freshness` itself stays load-bearing: `ForceFresh`/`StrictValidate` still change `getView`'s
manifest-body proof and single-flight participation (`Parts/PartFolderAccess.cpp:266-270`), only the
resolve half of the distinction is gone. P3.
