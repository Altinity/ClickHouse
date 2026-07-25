---
description: 'Unified design: closing the CAS fetch-by-relink commit-before-release gap (codex-6, #42/#43) with publish-then-confirm — an exact-token, lane-linearized confirm (with a part-anchored fast filter) — plus the ref-lane exception-safety fix it depends on (no-throw install at all three post-durable sites, DENY_ALLOCATIONS_IN_SCOPE enforcement, an apply-pending poison state machine, precommitAdd mint-tightening), B66b relink-into-detached, the RPL-5 test slice, the S42 allocation-fault soak, and the execution order. Rev.5 (review round 4): the pre-existing post-durable-PUT window is UPGRADED to a data-loss class (a poisoned cache can publish a snapshot that permanently hides a durable transaction, diverging the writer view from GC), so the ledger fix moves FIRST; gate 0 is demoted to an availability filter with gate 1 authoritative for every yes. Supersedes the 2026-07-15 retention-pin design and the short-lived 2026-07-23 reserved-precommit design.'
sidebar_label: 'CAS Fetch-Handoff Publish-Confirm'
sidebar_position: 20260724
slug: /superpowers/specs/cas-fetch-handoff-publish-confirm-design
title: 'CAS fetch-handoff — publish-then-confirm relink (+ ref-lane exception safety)'
doc_type: 'reference'
---

# CAS fetch-handoff — publish-then-confirm relink {#cas-fetch-handoff-publish-confirm}

**Date:** 2026-07-23; rev.5 2026-07-24
**Branch:** `cas-gc-rebuild`
**Status:** design (user-approved brainstorm; FOUR adversarial review rounds folded in — the
approach survived all four; rev.5 reorders the work and demotes gate 0 after round 4)
**Covers:** codex-6 / #42/#43 (relink commit-before-release gap); ref-lane exception safety
(absorbs the BACKLOG "post-durable-PUT allocation window" consult item, F2 — severity upgraded
below); B66b (relink-into-detached, RPL-4 perf cliff); RPL-5 test slice; ca-soak scenario S42.
**Out of scope:** B66a (local-backend torn read — `BACKLOG.md` §14), bulk warm-up.

## Problem 1 — the relink handoff gap (codex-6) {#problem-relink}

Same-pool replication uses fetch-by-relink: the sender transfers only the part's manifest; the
receiver publishes its own ref over the already-shared blobs (zero byte cost). The gap
(self-documented at `ContentAddressedMetadataStorage.cpp:1990`; that comment's own citation
`DataPartsExchange.cpp:256-259` is stale — the source part is released when `processQuery` returns
at `:276`): the sender is fire-and-forget. If the receiver's `precommitAdd` edge-PUT is not yet
durable while the sender's now-`Outdated` part is collected (removal `-1` folded, condemned,
graduated, deleted — ≥ 3 GC rounds + `old_parts_lifetime`), the receiver later commits a manifest
whose blobs are gone: a dangling committed manifest (fsck-detected, not silent).

## Problem 2 — the post-durable-PUT window is a DATA-LOSS class (severity upgraded, rev.5) {#problem-ledger}

Round 4 established that the pre-existing ledger defect is materially worse than "a stale cache
until remount". Chain, each link verified at HEAD:

1. A ref-log transaction `X` becomes durable (`putIfAbsentControlled`, `CasRefLedger.cpp:1676`),
   then its in-memory apply throws — reachable via allocation failure, since the COW containers
   allocate on apply and the tracked allocator can throw `MEMORY_LIMIT_EXCEEDED`. (The sibling
   overlay-fold catch, `:1718-1728`, says so explicitly for its own step.)
2. `applyRefLogTxn` has the STRONG exception guarantee — on throw the live state is byte-for-byte
   unchanged (`CasRefProtocol.cpp:429`), so `greatest_applied` stays below `X`.
3. A later transaction `Y` only needs `greatest_applied < Y.txn_id` — **contiguity is not checked**
   (`CasRefProtocol.cpp:402-407`) — so `Y` applies cleanly onto a base that never saw `X`.
4. Snapshot publication copies that live state and labels it with the current `greatest_applied`,
   i.e. `Y` (`CasRefLedger.cpp:2029-2036`).
5. Recovery skips every log id at or below the selected snapshot — including `X`
   (`CasRefLedger.cpp:426-427`, "at or below the selected snapshot: already covered").

So `X` is **permanently erased from the recovered writer view**, while GC folds the ref LOGS
directly and therefore still sees `X`. The two consumers diverge:

- **`X` was a removal (`-1`)** → GC applies it, condemns and deletes the blobs; the writer's
  recovered view still holds the ref → **dangling committed manifest = data loss class**
  (INV-NO-DANGLE violated, fsck-detectable).
- **`X` was an addition** → the writer loses a ref GC still counts → over-protection leak plus an
  fsck anomaly.
- **No covering snapshot in between** → recovery replays `X` then `Y`, and `Y` may be invalid
  against `X` (e.g. conflicting owner) → `CORRUPTED_DATA` on EVERY recovery → the table is
  **bricked** (exactly what the outer catch's own comment predicts, `:1730-1755`).

This is why rev.5 moves the ledger fix to the FRONT of the plan: it is no longer a dependency of
the confirm, it is an independent correctness fix whose absence can lose data.

## Design history {#history}

- **Retention pin** (2026-07-15, superseded): a pool-global GC overlay — second reachability
  source, union folding, removal-deferral, owner-liveness machinery. Rejected as structural growth
  in the most safety-critical component.
- **Reserved precommit** (2026-07-23, removed same day; git history is its archive): the receiver
  reserves the manifest identity, the *sender* materializes the body. Abandoned after round 1 for
  state-model sprawl, foreign-writer bypass of `stageManifest`/`adoptEvidence` discipline, and the
  `tmp-fetch_<part>` contract mismatch. (Its headline unmatched-`-1` data-loss claim was later
  refuted: in-degree is a source-edge SET, last-wins per key — `CasBlobInDegree.h:139`, `.cpp:574`;
  BACKLOG §3 `[UNMATCHED-MINUS-ONE]` is now just a pin-the-property test.)
- **Rev.2** (round 2): a naive confirm via cached `resolveRef` by ref NAME is unsound — the cache
  can lag a durable removal, and name-only equality is an ABA. Fixed by the exact-token,
  lane-linearized primitive.
- **Rev.3** (round 3): the post-durable apply-exception state identified as the remaining stale
  window; poison rule, mint-tightening, two-mutex snapshot protocol, recursion brake, honest
  abort-wedge semantics, typed exchange boundary.
- **Rev.4**: one unified spec; part-anchored gate 0; re-verified against the stage1 ref-lane
  rewrite (chunked flush, admission caps, streaming recovery).
- **Rev.5** (round 4): (a) the window is a data-loss class (§[Problem 2](#problem-ledger)) ⇒ the
  ledger fix goes first and the poison marker stays a thin assert-layer instead of growing into an
  operational fence; (b) **gate 0 is demoted** — a failed filesystem removal rolls a part back
  `Deleting → Outdated` (`MergeTreeData::rollbackDeletingParts`, `MergeTreeData.cpp:3550`, called
  from `clearPartsFromFilesystemAndRollbackIfError`, `:3911`), so `Active/Outdated` does NOT prove
  "no removal was ever initiated"; (c) a THIRD post-durable install site exists (wedge
  construction); (d) the stage1 trial state cannot be reused as the fix's candidate.

Common thread across all four rounds: never carry trust across two single-writer domains — each
party writes only its own domain, and the receiver *verifies* instead of trusting.

## Part A — ref-lane exception safety (lands FIRST) {#ledger-hardening}

### A1. No-throw install at all THREE post-durable sites {#no-throw-install}

Make the region between "the object is durable" and "the runtime records it" allocation-free, so
it cannot throw. Sites:

1. **Chunk commit apply** — `commitRefChunk`: PUT at `CasRefLedger.cpp:1676`, throwing apply at
   `:1694`.
2. **Wedge-resolution apply** — `CasRefLedger.cpp:1205+`: applies post-durable too. Note the
   existing asymmetry (rev.4 finding): this arm has no inner swallow around
   `materializeCommitted`, so a throw there leaves the txn applied but the wedge NOT reset → the
   next resolution re-applies it and double-bumps the tail counters. Fix with the same restructure.
3. **Wedge construction** (NEW, round 4) — on `Unresolved`, `rt->wedge = RefAppendWedge{id, key,
   bytes}` (`CasRefLedger.cpp:1788`) copies two `String`s (`CasRefLedger.h:308-310`) and can
   allocate. If that allocation fails, the object may be durable while the runtime records
   **neither** the transaction nor the wedge — strictly worse than a wedge, because the next append
   then allocates a fresh id and proceeds against a state missing a possibly-landed txn.
   Preconstruct the complete wedge (with its candidate) BEFORE the PUT, use its key/body for the
   request, and install it by verified `noexcept` move on `Unresolved`.

**Candidate construction (corrected, round 4).** The stage1 validation loop's trial state is NOT
reusable: trial ids advance locally per op (`CasRefLedger.cpp:1532`, `trial_id.ref_sequence += 1`)
while the durable transaction gets one independently allocated pool-wide id — reusing the trial
state would install the wrong `greatest_applied` (and, for namespace removal, the wrong removal
id). So, per chunk: allocate the real txn id → **COW-copy** the live state → apply the complete
chunk once with the REAL id → PUT → install by no-throw move → fold.

**Refinement (2026-07-24, derived while planning — do NOT materialize the candidate before the
PUT).** The consult's "fully materialize before the PUT" would force an **O(n) base rebuild per
chunk**: a candidate that shares its COW base with the live state cannot fold in place, so
materializing it copies the whole base — whereas today's post-apply fold is O(overlay) precisely
because the live state uniquely owns its base (`CasRefLedger.cpp:1702-1717`). Materialization is
NOT required for a no-throw install: moving an unmaterialized candidate (base pointer + overlay)
is equally noexcept. Correct order, cost-neutral vs today:
1. pre-PUT: `candidate = rt->state` (COW copy, cheap — base shared) and
   `applyRefLogTxn(candidate, chunk_txn)` with the REAL id (allocates the overlay; a throw here is
   now a clean PRE-durability failure — the same class as today's validation rejects);
2. PUT;
3. on `Committed`, under `state_mutex` + `DENY_ALLOCATIONS_IN_SCOPE`: `rt->state =
   std::move(candidate)` + the atomic tail bumps. Destroying the old state restores unique base
   ownership;
4. still under `state_mutex` but OUTSIDE the deny region: today's `materializeCommitted()` inside
   its existing swallow — in-place O(overlay), exactly as now.
Net change vs today: one extra cheap COW copy, and the apply MOVES from after the PUT to before it.
The benchmark gate (§[Order](#execution-order) step 2) measures this rather than assuming it.
Safety note: between the copy and the install only this leader mutates `rt->state` (single leader
per table; the wedge-resolution apply runs earlier in the same flush; publishers only read), so add
a debug assert that `greatest_applied` is unchanged at install time — a non-allocating comparison,
safe inside the deny region.

**Enforcement — `DENY_ALLOCATIONS_IN_SCOPE`** (`Common/MemoryTracker.h:35`; thread-local flag makes
`MemoryTracker` throw `LOGICAL_ERROR` on any allocation in scope; no-op in Release). Upstream
precedent with identical intent: `AsyncLoader.cpp:363-364` ("We do not want any exception to be
thrown after this point, because the following code is not exception-safe"), also
`ThreadPool.cpp:795`. Contract (round-4 tightening):

- take `state_mutex` BEFORE entering the deny region;
- inside: only the move of the pre-existing candidate and the atomic tail-counter bumps;
- outside: candidate construction (default-constructing the COW members allocates), logging,
  events/`ProfileEvents`, survivor completion, exception creation, cv notify, and
  `materializeCommitted`;
- keep the `static_assert` on noexcept move — the macro proves the code path, the assert proves the
  type contract;
- an allocation inside a `noexcept` install terminates rather than reporting nicely; under this
  project's abort-on-logical-error convention that is an acceptable CI signal, and the negative
  control is written as a **death test**;
- Release safety rests on the restructure, not the macro (debug-only).

Why the macro matters here: the defect class is invisible in ordinary testing because allocations
normally *succeed*. The deny region makes the **presence** of an allocation the failure signal, so
every debug/ASan/TSan run that commits a ref op enforces the invariant.

### A2. Apply-pending poison — explicit state machine {#poison}

Even with A1, keep a cheap defense-in-depth marker (it is also the confirm's rule 4). Round 4
showed the rev.4 wording was self-contradictory ("cleared after apply succeeds" vs "cleared only by
fresh recovery"), so the lifecycle is now explicit, per table:

- `Clean → ApplyPending` — armed (allocation-free, atomic) before the durable PUT;
- `ApplyPending → Clean` — after a successful apply; **also** on proven non-durability, where no
  apply is owed: a conclusive `putIfAbsentControlled` throw (`CasRefLedger.cpp:1678-1686`),
  `DefiniteFailure` (`:1775-1783`), and wedge resolution proving the key absent;
- `ApplyPending → Poisoned` — the apply threw although the object is durable;
- `Poisoned` is **terminal for the runtime**: never cleared by a later flush, only by a fresh
  recovery (a replaced runtime). It is exported as a metric.

**Ordering caveat (important).** A `Poisoned` marker alone is NOT a safety net: with A1 unlanded, a
poisoned cache can still accept later transactions and publish a snapshot that permanently hides
the missing one (§[Problem 2](#problem-ledger)). So either A1 lands first (this spec's order), or
the marker must be upgraded to a hard per-namespace fence — reject subsequent `appendRefOps`,
stale-precommit sweeps and snapshot admissions, expose reads/confirm as unknown, and force
runtime replacement. **Do not ship the marker as the only protection.** Note also that "fresh
recovery" is not spontaneous: eviction runs only above the cache budget and only when another
table is recovered (`CasRefLedger.cpp:621,652`), and `quiesceRefTablesForRemount` (`:781`) is not
triggered by poison — so if the fence variant is ever taken, it must actively drive bounded
per-table recovery rather than wait.

### A3. `precommitAdd` mint-tightening {#mint-tightening}

An unowned `ManifestId` may enter ownership only from the transaction that freshly staged it.
Re-verified 2026-07-24: `precommitAdd` (`CasPartWriteTxn.cpp:920`) still validates ONLY
`id.root_namespace == target_ns` (`:926-929`), and re-owning an exact old unowned pair is
tested-legal (`gtest_cas_promote_republish.cpp:283` — that pin is replaced). Already-committed
idempotence stays a read-only no-op (`:952-954`). This is what makes exact-`ManifestRef` equality
(gate 1 rule 5) a sound ABA barrier.

## Part B — publish-then-confirm relink {#core-idea}

**Publish, then confirm, then promote** — EDGE-BEFORE-OBSERVE lifted to part level:

1. The receiver runs today's flow unchanged: stage its manifest body, `precommitAdd` — its `+1` is
   durable in its own journal with a present body (**T1**). Durability on return re-verified:
   `CasPartWriteTxn.cpp:920` appends via `store->appendRefOps` (`:940`); the caller blocks on
   `while (!item->done)` (`CasRefLedger.cpp:1008`), set only after the acked conditional PUT.
   Chunk boundaries fall on ITEM boundaries only (`:1448-1454`; an oversized single item is
   rejected at `:1426`), so one item is never split across durable transactions.
2. **Confirm** (`confirmExactRef`, one read-only interserver query) — §[below](#confirm-primitive)
   (**T2**).
3. *Yes* → `promote`. Anything else → §[Failure taxonomy](#failure-taxonomy).

Under `INV-NO-DANGLE` the part's blobs live exactly as long as the sender's committed binding of
*that manifest*, and protection is lost only through a removal/repoint `-1`. One part-level check,
placed AFTER the receiver's evidence is durable, replaces per-blob probing — the placement is what
kills the TOCTOU.

### The confirm primitive {#confirm-primitive}

**Token** (minted by the sender at offer time):
`{pool_uuid, server_root_id, root_namespace, ref_name, part_name, ManifestRef}`. `server_root_id`
is required because a pool UUID is shared across server roots. The manifest body already embeds
`ref` + `root_namespace_id` (`CasPartManifestFormat.h:76-78`); today's adoption discards sender
identity (`ContentAddressedMetadataStorage.cpp:1954`) — for the confirm it becomes load-bearing.

**Gate 0 — part-anchored fast filter (DEMOTED in rev.5; availability only, never a proof).**
Look up `part_name` in the sender table's parts set (unique by parsed `MergeTreePartInfo`,
`MergeTreeData.h:1562`; the valid-state filter excludes `Deleting`) and answer *no* unless it is
`Active`/`Outdated` on the CA disk identified by the token. Rev.4 claimed this PROVES no removal
was initiated; round 4 refuted that: `rollbackDeletingParts` restores `Deleting → Outdated` after a
failed filesystem removal (`MergeTreeData.cpp:3550`, via `:3911`), and
`clearOldTemporaryDirectories` can finish a partial `delete_tmp_*` cleanup while the part object
still reads `Outdated`. So gate 0 is a cheap conservative filter (fast *no*, no ledger work) —
**every *yes* is authorized by gate 1 alone**, for whole-part removal exactly as for repoints.
Additional contract: `MOVE ... TO DISK` leaves a same-name `Active` part on ANOTHER disk
(`MergeTreeData.cpp:6355`), so the matched exchange instance must be compared with the part's
current disk; the parts-set read happens under its own lock, released BEFORE any ledger lock;
projection names are not looked up independently (projections are nested manifest entries, so a
projection change repoints the parent ref and is caught by gate 1).

**Gate 1 — exact-token identity under a lane snapshot (authoritative).** Return *yes* iff ALL hold,
as ONE snapshot:

1. take `ref_queue_mutex`, pin the resident runtime, then take `state_mutex` without releasing it
   (predicates span both: `pending`/`leader_active` under the queue mutex,
   `CasRefLedger.h:414-415`, admission `pending.push_back` at `CasRefLedger.cpp:1006`; rows and
   wedge under the state mutex);
2. table warm: `recovered`, not `recovery_in_progress` (`CasRefLedger.h:338,343`), not
   `superseded_by_remount` (`:425`), resident — else *unknown*. Zero object-store I/O by contract;
   streaming recovery publishes atomically (`CasRefLedger.h:492`), so no half-recovered view;
3. lane quiescent: no `RefAppendWedge`, no pending items, no `leader_active` tenure — else
   *unknown*. **Chunked-lane note:** one tenure commits MULTIPLE durable transactions
   (`commitRefChunk`, `:1592`), so mid-tenure a table is partially durable; `leader_active` covers
   the whole tenure, so this is already *unknown* — a wider unknown window under load, not a hole;
4. poison state is `Clean` — else *unknown*;
5. the committed row for `ref_name` exists and its `ManifestRef` equals the token's exactly — else
   *no*;
6. the mount fence is live and this instance is the namespace's current single writer — checked
   LAST, before releasing the locks.

*Yes* proves: at T2 the exact transferred manifest was the committed binding, no removal of it was
durable, none was in flight past admission, no durable-but-unapplied transaction existed, and the
answerer was the live writer. Any `-1` touching it is appended strictly after T2 > T1. Cost: zero
S3 requests; a parts-set lookup plus two mutexes.

### Why there is no TOCTOU {#correctness}

T1 = receiver `+1` durable; T2 = confirm *yes*; T3 = a removal/repoint `-1` appended. Deletion
needs condemn (round N) → `delete_pending` (N+1) → pre-CAS delete (N+2), each from folded journals
(fold precedes the round's deletes, `CasGc.cpp:392,410`).

- **`-1` after T2:** every fold reading it also sees the durable `+1` → net in-degree ≥ 1 → no
  condemn.
- **Round straddles T1:** may condemn, but the next fold cannot miss a durable `+1` (the cursor
  never advanced past it — close-out at `CasGc.cpp:1187-1205`); settlement sends any `indeg > 0` to
  `spared` (`CasBlobInDegree.cpp:412-426`), including `delete_pending` rows (`CasGc.cpp:482`).
  Deletion needs the `+1` invisible for three consecutive folds — impossible for a durable entry.
- **`-1` durable before T2, in any form:** applied (*no* by equality), in-flight/pending
  (*unknown*), wedge-hidden (*unknown*), durable-but-unapplied (*unknown* via poison; and after
  Part A this state cannot arise).
- **ABA:** excluded by mint-tightening (A3); repoints and recreations mint fresh `ManifestRef`s.
- **False-404 of the receiver's staged body** at a later fold: today's barrier — anomaly + clamp +
  `suppress_destructive` (`CasGc.cpp:1357-1364`); the fold is untouched by this design.
- **Receiver frozen between confirm and promote:** its folded live precommit protects the blobs; a
  dead build is fenced and `promote` fails before committing (`requireAlive`,
  `CasPartWriteTxn.cpp:126`, invoked by `promote` at `:1005`); the thawed receiver retries fresh.
- **`delete_tmp_*` rename:** destination published before the source drop
  (`PartFolderAccess.cpp:374`, ordering at `:399-400`), same manifest — no unprotected drain
  interval; and the in-memory part path is deliberately not updated
  (`DataPartStorageOnDiskBase.cpp:937`), which is precisely why gate 0 cannot be trusted alone.

### The prepared-relink handle {#relink-handle}

Splitting today's atomic `publishEntries` (stage+precommit+promote, `PartFolderAccess.cpp:338`)
across an HTTP round-trip needs explicit ownership: a leaked same-epoch precommit is NOT reclaimed
by the stale sweep (older-epoch predicate, `CasRefLedger.cpp:2225-2250`), and `~PartWriteTxn`
(`CasPartWriteTxn.cpp:119-124`) only retires the build sequence — today `publishEntries` supplies
the catch-and-`abandon` discipline (`PartFolderAccess.cpp:355`).

**`PreparedRelink`**: move-only; owns the `PartWriteTxn`, receiver `ManifestId`, target ref name,
decoded entries, exact source token; explicit terminal states; `promote` and durable `abort`; a
scope guard aborts on every non-promote exit.

**Abort-uncertain semantics (honest).** An `Unresolved` abort append wedges the table's lane
(`CasRefLedger.cpp:1784-1796`) — the pre-existing discipline: the wedge retains `{id, key, bytes}`
and every later append first re-runs `resolveByExactGet` (`:1205`); a landed PUT is
applied-before-unwedging and the abort completes; a never-landing PUT keeps the lane wedged **until
remount** (GET-based resolution never re-issues). Consequences stated plainly: fail-long
over-protection of the blobs; the wedge blocks all ref appends for that table on this replica until
resolution/remount (existing blast radius, one more producer); a fetch retry does NOT self-heal it.
The handle never destructs-and-forgets — its terminal state records the uncertain abort; the wedge
itself is the persistent cleanup owner.

The txn safely spans the confirm RTT: the temporary-directory lock stays owned by
`fetchSelectedPart` (`DataPartsExchange.cpp:509`); relink opens no disk transaction.

### Failure taxonomy {#failure-taxonomy}

Today relink returns a boolean and every `false` triggers the same-sender byte re-request
(`DataPartsExchange.cpp:768-770`), while `adoptPartFromManifest` catches every `Exception` —
including `NETWORK_ERROR` — and returns `false` (`ContentAddressedMetadataStorage.cpp:2002`). So
the exchange boundary becomes **typed**: a *prepare* operation returns `PreparedRelink` or
`MechanismFallbackAllowed`; `Fetcher` owns confirm, abort and the throw, OUTSIDE that catch.

| Result | When | Action |
|---|---|---|
| `Success` | confirm *yes* + promote committed | return the relinked part; the normal `tmp-fetch_<part>` → final re-key follows |
| `SourceProofFailed` | confirm *no* / *unknown* / confirm transport failure | `abort`, then **throw a locally-generated retry-later error** from `Fetcher` — never return `false`. The queue stores the exception; `num_tries`/`last_exception_time_ms` backoff postpones (`ReplicatedMergeTreeQueue.cpp:1614,2152`); re-execution recomputes source and covering-part discovery (`StorageReplicatedMergeTree.cpp:2636,5200`) — but does NOT guarantee a different replica, so a repeat confirm may fail fast again until the entry converges elsewhere |
| `MechanismFallbackAllowed` | relink cannot work here but the sender has the part: version/cookie mismatch, manifest decode failure, receiver-side publish/`promote` ref conflict | today's same-sender byte re-request (`DataPartsExchange.cpp:731-738`; unknown-cookie fallback logs `:741-747`, returns `:748`) |

Notes: (a) there is NO `ABORTED` backoff exemption in the GET_PART queue path — any stored
exception engages postpone; `ABORTED` is only logged specially (`StorageReplicatedMergeTree.cpp`
generic catch `:4222`, special logging `:4229`). (b) "promote aborted on a condemned blob" is not a
real class — promotion does not probe tokenless adopted leaves (`CasPartWriteTxn.cpp:1072-1106`).
(c) manual `SYSTEM FETCH PART`/`FETCH PARTITION` swallow several REMOTE codes
(`StorageReplicatedMergeTree.cpp:8128`) — the locally-generated error surfaces instead.

### Confirm wire protocol {#wire-protocol}

- New action `confirm_ca_ref` on the existing interserver endpoint (one part-stream operation
  today; version negotiation at `DataPartsExchange.cpp:91,170`), carrying the token; issued as a
  second HTTP request after the relink response is consumed (`assertEOF` at `:762`); dispatched
  BEFORE the handler resets the body or requires/parses `part` (`:170`). Authentication parity is
  inherent — the shared handler authenticates before endpoint dispatch
  (`InterserverIOHTTPHandler.cpp:29`).
- **Routing contract (round-4 requirement).** `Service` holds the table (`DataPartsExchange.h:55`)
  and can enumerate its disks; `IContentAddressedExchange` (`ContentAddressedExchange.h:36`)
  currently exposes only pool UUID and manifest operations, so it gains `ownsNamespace` and
  `confirmExactRef`. Resolution rule: match `{pool_uuid, server_root_id, root_namespace}` to
  exactly one CA disk/exchange instance; **multiple or zero matches → *unknown***; then compare the
  matched instance against the part's current disk (the `MOVE ... TO DISK` same-name case).
- Response: authenticated `yes`/`no`/`unknown`; transport errors and timeouts → `SourceProofFailed`.
- Versioning: the relink advertisement gains a confirm-capable version; a new sender relinks only
  for confirm-capable receivers, else bytes; a new receiver on an old/unknown cookie falls back as
  today. Mixed versions degrade to bytes, never to unconfirmed relink.

### B66b: relink-into-detached {#b66b}

- **Dedicated `allow_ca_relink` mode with the recursion brake.** Lifting `!to_detached` alone is
  ineffective (manual detached callers pass `try_fetch_shared=false`,
  `StorageReplicatedMergeTree.cpp:8125,8281`, so `try_zero_copy` is already false), and naive
  decoupling would REMOVE the existing brake: the byte fallback recursively re-requests with
  `try_zero_copy=false` precisely so relink is not re-advertised (`DataPartsExchange.cpp:534-545`,
  recursion at `:731`) — without a brake a persistent mechanism failure loops forever. So relink
  capability becomes its own `allow_ca_relink` flag; manual detached fetches set it true
  independently of `try_fetch_shared`; EVERY same-sender byte fallback clears it; legacy
  `try_zero_copy` stays untouched for real zero-copy (`:566`; CA disks report zero-copy
  unsupported).
- `relinkPartToDisk` gains `to_detached` (today it hardcodes the active parent, `:1107-1128`);
  detached target = temporary storage under the detached parent. CA routing already folds any
  `detached/<name>` ref (`ContentAddressedMetadataStorage.cpp:1241`).
- Detached finalization keeps `renameTo(detached/<part>, true)` (branch
  `StorageReplicatedMergeTree.cpp:5715`, rename `:5719`) with its existing collision behavior
  (`:8115`, `DataPartStorageOnDiskBase.cpp:795`). No collision-semantics change on either path.
- Confirm step identical; cross-pool `FETCH PARTITION` keeps streaming bytes.

## Execution order {#execution-order}

Reordered in rev.5: the ledger fix is a data-loss fix, so it leads; the confirm work then builds on
a lane whose post-durable window is closed by construction.

1. **`[UNMATCHED-MINUS-ONE]` pinning gtest** (BACKLOG §3) — independent, zero risk.
2. **A1 no-throw install** — measurement gate FIRST (bench the extra pre-PUT apply+copy per chunk,
   `BM_FlushInstall`, wedge-resolution→flush, and a chunked multi-transaction tenure), then the
   restructure at all three sites + `DENY_ALLOCATIONS_IN_SCOPE` + failpoint battery + soak.
   Implement from the landed stage1 head — do NOT develop competing edits to `commitRefChunk`.
3. **A2 poison state machine + A3 mint-tightening** — small, each with its gtest; poison is now a
   thin assert-layer because step 2 closed the window.
4. **TLA mini-model** (before the confirm code, per project discipline): two journals + fold with
   cursors + three-phase graduation + spare; property: *exact-token confirm yes at T2 under gates
   0+1 with receiver activation < T2 ⇒ no referenced blob deleted before the receiver's own removal
   event*. Interleavings: straddle; wedge-hidden drop; the (now-closed) apply-exception path; ABA
   attempt; repoint-on-live-part; the `Deleting → Outdated` rollback; false-404 clamp.
5. **Confirm core** — `confirmExactRef` (gate 0 filter + authoritative gate 1) → typed exchange
   boundary + `PreparedRelink` → routing/versioning → receiver flow split.
6. **B66b + recursion brake.**
7. **Test battery** — §[Testing](#testing), including S42.

If step 2 is ever deferred behind the confirm work, step 3's poison MUST become the hard
per-namespace fence of §[A2](#poison) — the marker alone is not a safety net.

## Testing {#testing}

- **A1 (no-throw install):** failpoint injecting an allocation failure between the durable PUT and
  the install at EACH of the three sites → post-fix the install cannot throw; the poison assert
  never trips. Negative control as a **death test** (a deliberate allocation inside the deny region
  must fail loudly — proves the region is entered and armed). Wedge-arm symmetry: a
  `materializeCommitted` throw during wedge resolution must not leave an applied-but-unreset wedge
  (no double-apply, no double-counted tail). No dedicated "does it allocate?" test is needed — the
  whole CAS gtest battery and CA stateless lane become the enforcement surface under debug.
- **A2/A3:** poison transitions `Clean → ApplyPending → Clean` (success; conclusive throw;
  `DefiniteFailure`; wedge-resolves-absent) and `→ Poisoned` (durable + apply threw), with
  `Poisoned` never cleared by a later flush; **snapshot-fence assertion**: with poison set, no
  snapshot may advance past the missing transaction (the §[Problem 2](#problem-ledger) chain must
  be untestable-by-construction after step 2, and explicitly blocked if the fence variant is
  taken). Mint-tightening: `precommitAdd` of an unstaged old id → rejected (replaces
  `gtest_cas_promote_republish.cpp:283`).
- **Gate 0 (filter semantics, not proof):** `Deleting` → *no*; absent/other-disk → *no*; the
  `Deleting → Outdated` rollback state → gate 1 must still reject if the ref is gone (the
  regression that demoted gate 0); `MOVE ... TO DISK` same-name-other-disk → *no* unless the
  matched instance is the part's current disk.
- **Gate 1 determinism:** repointed live part (removal-mark unlink) → *no* by equality;
  dropped+recreated → *no*; cold/evicted/recovering → *unknown* with zero backend requests;
  pending/in-flight/wedge/mid-tenure chunked flush (`CarvePhaseForTest::ChunkReseed`) → *unknown*;
  poison set → *unknown*; quiescent exact match → *yes*; two-mutex snapshot race test (an append
  admitted concurrently is ordered strictly after the snapshot).
- **Race integration:** failpoint between `precommitAdd` and confirm; sender drops the part → *no*
  → abort → retryable failure → queue re-selects (covering-part / other replica; NO byte
  re-request to the original sender); sender alive → *yes* → promote → relink proof
  (`CasBlobPut == 0`).
- **codex-6 regression:** receiver publish stalled ≥ 3 GC rounds (aggressive `old_parts_lifetime`),
  sender part merged away, GC to fixpoint → no committed ref from the stalled attempt; fsck clean,
  dangling=0.
- **Straddle-spare regression:** receiver `+1` folds one round after the sender's `-1` → spared.
- **Abort discipline:** every non-promote exit appends the removal (no same-epoch leak); forced
  UNCERTAIN abort → lane wedges → landed-PUT variant resolves+applies on the next append;
  never-landing variant stays retry-later until remount.
- **Recursion brake:** persistent mechanism failure → exactly one relink attempt, then bytes; the
  fallback request must not re-advertise relink.
- **B66b:** both manual detached callers relink same-pool (`CasBlobPut == 0`) + `ATTACH` reads
  correctly; collision behavior byte-identical to the byte path; cross-pool → bytes; capability
  independent of `try_fetch_shared`.
- **RPL-5 slice:** `REPLACE PARTITION`/`ATTACH PARTITION ... FROM` on 2-replica CA (extend
  `test_cas_replicated_relink`): the queue-cloned `REPLACE_RANGE` fetch relinks (blob-count proof).
- **Version-mix:** confirm-capable receiver × legacy sender cookie → clean byte fallback.
- **Allocation-fault soak — ca-soak scenario S42** (registered in
  `utils/ca-soak/scenarios/BACKLOG.md` §s42-allocation-fault-soak; next free id, `s41_…` is taken):
  - Leg A — `memory_tracker_fault_probability` (per-query `Float` Setting,
    `Core/Settings.cpp:2312`; NOT debug-gated, `MemoryTracker.cpp:340-342`; reachable through the
    driver's URL parameters, `utils/ca-soak/soak/cluster.py:247`, and applied to the query's
    tracker, `ThreadStatusExt.cpp:319`) armed over a soak-shaped workload. Note the ref append lane
    runs on the CALLER thread (`CasRefLedger.cpp:980`), so this reaches the CAS commit path.
  - Leg B — `cannot_allocate_thread_fault_injection_probability` (`ServerSetting`,
    `ServerSettings.cpp:233`, applied on config reload in `Server.cpp:2763-2764` — NOT
    `InterpreterSystemQuery.cpp:1158`, which is `SYSTEM START THREAD FUZZER`). It reaches
    thread-creating paths such as the background snapshot dispatcher
    (`CasRefLedger.cpp:1839`), NOT the ref lane. Arm/disarm via a reversible generated config
    overlay + reload, verified from live settings.
  - Leg C — disarm, quiesce, GC to fixpoint, fsck, then RESTART and compare. **Strengthened
    (round 4):** a wrong snapshot makes pre- and post-restart views identically wrong, so the
    oracle also replays from the last PRE-FAULT snapshot plus the raw tail logs and compares
    against the live cache; and asserts no snapshot advanced across a poisoned transaction.
  - **Soundness guard:** a nonzero `MEMORY_LIMIT_EXCEEDED` count only proves *some* allocation
    failed. Require a targeted post-PUT apply failpoint or a poison-transition counter to prove the
    target window was exercised — otherwise `inconclusive`, never a vacuous pass.
  - **Oracle:** queries may fail; invariants may not — zero `LOGICAL_ERROR`/abort in `err.log`
    (only expected injected query/thread errors during the armed window); every ACKED insert's rows
    present; replicas agree; fsck `dangling=0`/`unaccounted=0`, unreachable settles; GC recovers
    after disarm; no permanently wedged lane; no query hung past a bound.
  - Optional stronger mode: the same card on a DEBUG image additionally enforces every
    `DENY_ALLOCATIONS_IN_SCOPE` region under real allocation pressure.

## Scope and non-goals {#scope}

- **Zero changes** to the GC fold, journal codec, snapshot format, sweeps, orphan protection, or
  anomaly/suppression semantics. No new journal states, no foreign writers — every write stays in
  its owner's domain. New sender-side surface: the read-only confirm primitive plus the Part A
  ledger changes.
- The ordinary write path (INSERT/merge) is untouched behaviorally; A1 restructures HOW the ref
  lane installs a committed transaction, not WHAT it commits, and A3 only rejects a transition no
  production path performs.
- B66a — `BACKLOG.md` §14 (local-backend atomicity, orthogonal).
- `[UNMATCHED-MINUS-ONE]` — verified harmless (source-edge set); only the pinning test remains.
- Bulk write-replica warm-up (future): confirm generalizes — one batched exact-token confirm for N
  parts after N publishes.

## Naming {#naming}

"Publish-then-confirm relink". Confirm primitive: `confirmExactRef` (read-only; gate 0 = part
filter, gate 1 = authoritative exact-token lane-linearized check, poison-aware). No new journal
vocabulary.
