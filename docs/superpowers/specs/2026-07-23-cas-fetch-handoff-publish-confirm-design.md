---
description: 'Unified design: closing the CAS fetch-by-relink commit-before-release gap (codex-6, #42/#43) with publish-then-confirm — a two-layer confirm (part-anchored liveness gate + exact-token lane-linearized ledger check) — plus the ref-lane hardening it depends on (apply-pending poison marker, precommitAdd mint-tightening, and the full post-durable-PUT no-throw-install fix), B66b relink-into-detached, the RPL-5 REPLACE PARTITION test slice, and the execution order. Rev.4 unifies everything into one spec (user direction) and adds the part-anchored gate 0 (user insight: a part still Active/Outdated proves no removal was ever initiated, cache-independent). Supersedes the 2026-07-15 retention-pin design and the short-lived 2026-07-23 reserved-precommit design.'
sidebar_label: 'CAS Fetch-Handoff Publish-Confirm'
sidebar_position: 20260724
slug: /superpowers/specs/cas-fetch-handoff-publish-confirm-design
title: 'CAS fetch-handoff — publish-then-confirm relink (+ ref-lane hardening)'
doc_type: 'reference'
---

# CAS fetch-handoff — publish-then-confirm relink {#cas-fetch-handoff-publish-confirm}

**Date:** 2026-07-23, rev.4 2026-07-24 (re-verified against HEAD after the stage1 ref-lane
landing, 2026-07-24)
**Branch:** `cas-gc-rebuild`
**Status:** design (user-approved brainstorm; three adversarial review rounds folded in; rev.4
unifies the confirm protocol with the ref-lane hardening into ONE spec per user direction and adds
the part-anchored confirm gate per user insight)

> **Re-verification note (2026-07-24).** The ref lane was rewritten under this spec by the parallel
> stage1 round (chunked flush `commitRefChunk`, counts-only admission caps, streaming recovery,
> `SetPublishedAt`). Every load-bearing claim was re-checked against HEAD; the design is unchanged,
> with three substantive updates folded in: (a) the post-durable-PUT window NARROWED — the overlay
> fold is now swallowed as an optimization, leaving only `applyRefLogTxn` itself, which shrinks
> phase 7's scope; (b) the poison marker must be **sticky** until a fresh recovery, since the cache
> stays divergent and later flushes would build on the stale base; (c) chunked flush adds a
> mid-tenure partially-durable state — already covered by the `leader_active` predicate, but it
> widens the *unknown* window under load. Citations throughout were re-anchored to HEAD.
**Covers:** codex-6 / #42/#43 (relink commit-before-release gap); B66b (relink-into-detached,
RPL-4 perf cliff); RPL-5 test slice; ref-lane hardening: apply-pending poison marker,
`precommitAdd` mint-tightening, and the full **post-durable-PUT no-throw-install** fix (absorbs
the BACKLOG "post-durable-PUT allocation window" consult item, gpt-5.6-sol F2).
**Out of scope:** B66a (local-backend torn read — `BACKLOG.md` §14), bulk warm-up.

## Problem {#problem}

Same-pool replication uses fetch-by-relink: the sender transfers only the part's manifest; the
receiver publishes its own ref over the already-shared blobs (zero byte cost). The gap
(self-documented at `ContentAddressedMetadataStorage.cpp:1990`; that comment's internal citation
`DataPartsExchange.cpp:256-259` is stale — the source part is released when `processQuery`
returns at `:276`): the sender is fire-and-forget — it releases the part when the HTTP response
completes, before the receiver's publish. If the receiver's `precommitAdd` edge-PUT is not yet
durable while the sender's now-`Outdated` part is collected (removal `-1` folded, condemned,
graduated, deleted — ≥ 3 GC rounds + `old_parts_lifetime`), the receiver later commits a manifest
whose blobs are gone — a dangling committed manifest (fsck-detected, not silent).

## Design history {#history}

- **Retention pin** (2026-07-15 spec, superseded): a pool-global GC overlay — a second
  reachability source, union folding, removal-deferral, owner-liveness machinery. Rejected as
  structural growth in the most safety-critical component.
- **Reserved precommit** (2026-07-23, removed same day; git history is its archive): the receiver
  reserves the manifest identity, the *sender* materializes the body at the reserved key.
  Abandoned after review round 1 for state-model sprawl (kindless owner rows, snapshot codec,
  sweep reconstruction), foreign-writer bypass of `stageManifest`/`adoptEvidence` discipline, and
  the `tmp-fetch_<part>` contract mismatch. (The round's headline unmatched-`-1` data-loss claim
  was later refuted — in-degree is a source-edge SET applied last-wins per key,
  `CasBlobInDegree.h:139`/`.cpp:574`; see BACKLOG §3 `[UNMATCHED-MINUS-ONE]`, now a pin-the-property
  test item.)
- **Rev.2** (round 2): a naive confirm — cached `resolveRef` by ref NAME — proven unsound: the
  cached committed view can lag a durable removal (`putIfAbsentControlled` commits before
  `applyRefLogTxn`; an `Unresolved` wedge deliberately leaves the cache unchanged —
  `gtest_cas_ref_writer.cpp:778-782`), and name-only confirmation is an ABA against
  repoint/recreate. Fixed by the exact-token, lane-linearized primitive.
- **Rev.3** (round 3): the remaining reachable stale state — a **post-durable-PUT apply
  exception** ("durable, unapplied, unwedged, idle, warm"; the catch installs no wedge; even a
  removal allocates COW tombstones) — recognized as the KNOWN pre-existing ledger defect
  (BACKLOG consult item F2). Closed by the poison rule; plus `precommitAdd` mint-tightening
  (the state machine legally re-owns an exact old unowned `ManifestRef` —
  `gtest_cas_promote_republish.cpp:283` — though no production path does), the two-mutex snapshot
  protocol, the relink recursion brake, honest abort-wedge semantics, and the typed exchange
  boundary.
- **Rev.4** (user review): ONE unified spec — the confirm protocol, the ledger hardening it leans
  on (poison as the fail-closed half, the full no-throw-install as the real fix), and the
  execution order. New **part-anchored gate 0** (user insight): a part still `Active`/`Outdated`
  in the sender's in-memory parts set proves no removal was ever initiated — the `Deleting`
  transition (`MergeTreeData::grabOldParts`, `MergeTreeData.cpp:3538`, under the parts lock)
  happens-before the first CAS removal op, and after a restart the parts set is rebuilt from the
  journal-derived truth. This closes the ENTIRE whole-part-removal branch without touching the
  ledger cache; the exact-token ledger check remains only for the repoint/recreate branch.

Common thread: never carry trust across two single-writer domains — each party writes only its
own domain, and the receiver *verifies* instead of trusting.

## Core idea {#core-idea}

**Publish, then confirm, then promote** — EDGE-BEFORE-OBSERVE lifted to part level:

1. The receiver runs today's flow unchanged: stage its own manifest body, `precommitAdd` — its
   `+1` edge is durable in its own journal with a present body (**T1**). (`precommitAdd` is
   durable on return: `CasPartWriteTxn.cpp:920` appends via `store->appendRefOps` at `:940`, and
   the caller blocks on `while (!item->done)` — `CasRefLedger.cpp:1008` — which the lane sets only
   after the acked conditional PUT. Re-verified against the stage1 chunked lane: chunk boundaries
   fall on ITEM boundaries only — a chunk commits when admitting the next item's ops would exceed
   `ref_txn_max_ops` (`CasRefLedger.cpp:1448-1454`), and a single item exceeding the cap is
   rejected outright (`:1426`) — so one item's ops are never split across durable transactions and
   T1 stays atomic.)
2. **Confirm** (`confirmExactRef`, one read-only interserver query): "is the transferred manifest
   still the live binding of `<part>` on your side?" — answered under the two-layer rules below
   (**T2**).
3. *Yes* → `promote`. Anything else → abort per §[Failure taxonomy](#failure-taxonomy).

Under `INV-NO-DANGLE`, the part's blobs stay alive exactly as long as the sender's committed
binding of *that manifest* lives; protection is lost only through the sender's removal/repoint
`-1`. One part-level check after the receiver's own evidence is durable replaces any per-blob
probing — and its placement (after T1) is what kills the TOCTOU.

## The confirm primitive — two layers {#confirm-primitive}

**Input — the exact source token**, minted by the sender at offer time:
`{pool_uuid, server_root_id, root_namespace, ref_name, part_name, ManifestRef}`. `server_root_id`
is required for routing (a pool UUID is shared across server roots). The manifest body already
embeds `ref` + `root_namespace_id` (`CasPartManifestFormat.h:76-78`) — today's adoption discards
them (`ContentAddressedMetadataStorage.cpp:1954`); for the confirm they become load-bearing.

### Gate 0 — part-anchored liveness (closes the whole-part-removal branch) {#gate-0}

Look up `part_name` in the sender table's in-memory parts set: it must exist in state
`Active` or `Outdated` on the CA disk identified by the token. Anything else — absent,
`PreActive`, `Deleting`, `DeleteOnDestroy`, different disk — answers *no*.

Why this is cache-independent and sufficient for removals: the removal flow transitions the part
to `Deleting` under the parts lock (`grabOldParts`, `MergeTreeData.cpp:3538`) **before** the same
thread drives any CAS removal op, so `Active/Outdated` at T2 ⟹ no removal `-1` exists in ANY
stage — not durable, not wedged, not in-flight, regardless of ledger-cache state. Race direction
is safe: a concurrent grab serializes on the parts lock — if the confirm read saw
`Active/Outdated`, the grab (and every removal op after it) happens after T2 > T1. After a
crash-restart the parts set is rebuilt from the CA refs (journal truth): a part whose removal was
durably initiated does not reload as `Active/Outdated`. `DETACH` moves the part out of the set →
*no* (pessimistic-correct: the detached rename is net-zero for blobs, but *no* just routes the
receiver to retry/bytes).

### Gate 1 — exact-token identity under the lane snapshot (closes the repoint/recreate branch) {#gate-1}

Gate 0 cannot see a **repoint on a live part**: a committed ref is legally rebound without
deleting the part — CSN fill-in rewrites (inline-only content, blob-set preserving — harmless)
and **removal-mark unlinks** (Task 8; content-REMOVING — the dangerous kind). The offered
`ManifestRef` must therefore still be the committed binding, and that read must not trust a
possibly-lagging cache. Return *yes* iff ALL hold, evaluated as ONE snapshot:

1. lock `ref_queue_mutex`, pin the resident runtime, lock `state_mutex` (the predicates span both
   mutexes — `pending`/`leader_active` are guarded by the queue mutex, `CasRefLedger.h:414-415`,
   admission `pending.push_back` at `CasRefLedger.cpp:1006`; rows and wedge under the state
   mutex);
2. table warm: `recovered` and not `recovery_in_progress` (`CasRefLedger.h:338,343`), not
   `superseded_by_remount` (`:425`), resident — else *unknown* (the confirm performs **zero
   object-store I/O**; no recovery from storage). Streaming recovery (stage1 T13) publishes into
   the runtime in one atomic step (`CasRefLedger.h:492`), so there is no half-recovered view to
   read;
3. lane quiescent: no `RefAppendWedge`, no pending items, no `leader_active` in-flight batch —
   else *unknown* (closes the durable-PUT→cache-flip and wedge windows). **Chunked-lane note
   (stage1):** one leader tenure now commits MULTIPLE durable transactions
   (`commitRefChunk`, `CasRefLedger.cpp:1592`), so mid-tenure a table can be *partially* durable —
   chunk 1's removal applied while chunks 2..N are unwritten. `leader_active` stays set for the
   whole tenure, so this state is already *unknown*; the practical effect is a wider unknown
   window under write load, not a hole (correctness never rests on the optimistic answer);
4. **apply-pending poison marker clear** — else *unknown* (closes the post-durable-PUT
   apply-exception window; §[Ledger hardening](#ledger-hardening));
5. the committed row for `ref_name` exists and equals the token's `ManifestRef` exactly — else
   *no* (repoints and recreations mint fresh `ManifestRef`s once `precommitAdd` is
   mint-tightened, §[Ledger hardening](#ledger-hardening));
6. the mount fence is live (checked LAST, before releasing the locks) — else *unknown*.

An append admitted after the snapshot releases is ordered strictly after T2 — the safe direction.

*Yes* therefore proves: at T2 the part was live (no removal initiated) AND the exact transferred
manifest was its committed binding with no rebinding in flight or hidden. Any `-1` touching it is
appended strictly after T2 > T1. **Cost:** zero S3 requests; a parts-set lookup + two mutexes.

## Why there is no TOCTOU {#correctness}

T1 = receiver's `+1` durable (body present); T2 = confirm *yes*; T3 = any removal/repoint `-1` of
the confirmed manifest appended. Deletion of a blob requires condemn (round N) → `delete_pending`
(N+1) → pre-CAS delete (N+2); the fold precedes each round's deletes (`CasGc.cpp:392,410`).

- **`-1` after T2** (sender drops the part a microsecond after answering): every fold that reads
  the `-1` also finds the receiver's durable `+1` → net in-degree ≥ 1 → never condemned.
- **Round straddles T1**: can condemn, but the next fold cannot miss a durable `+1` (cursor held —
  `CasGc.cpp:1115,1197`); settlement spares any `indeg > 0`, including `delete_pending` rows
  (`CasBlobInDegree.cpp:405`, `CasGc.cpp:482`). Deletion needs the `+1` invisible for three
  consecutive folds — impossible for a durable journal entry.
- **`-1` at any stage before T2**: whole-part removal ⇒ gate 0 *no* (part not `Active/Outdated`);
  repoint ⇒ gate 1 — applied (*no* by equality), in-flight/pending (*unknown*), wedge-hidden
  (*unknown*), apply-exception (*unknown* via poison).
- **ABA (old identity re-owned)**: excluded by mint-tightening; repoints/recreations mint fresh
  `ManifestRef`s and fail equality.
- **False-404 of the receiver's staged body** at a later fold: today's barrier — anomaly + clamp
  + `suppress_destructive` (`CasGc.cpp:1354,1360`) — untouched by this design.
- **Receiver frozen between confirm and promote** (arbitrarily long): its folded live precommit
  protects the blobs; a dead build is fenced and `promote` fails before committing
  (`requireAlive` at `CasPartWriteTxn.cpp:125`, invoked by `promote` at `:991`); the thawed
  receiver retries fresh.
- **`delete_tmp_*` rename**: publishes the destination before dropping the source
  (`PartFolderAccess.cpp:399-400`), same manifest — no unprotected drain interval; gate 0 answers
  *no* anyway once the part is grabbed.

## Ledger hardening (in-scope, absorbed from the BACKLOG consult item) {#ledger-hardening}

Three sender-side changes, ordered from interim to full:

1. **Apply-pending poison marker** (the fail-closed half; confirm gate 1 rule 4). A
   non-allocating marker armed before the lane's durable PUT, cleared only after `applyRefLogTxn`
   succeeds.
   **Re-verified against the stage1 lane (2026-07-24) — the window NARROWED but did not close.**
   In `commitRefChunk` the durable PUT is `CasRefLedger.cpp:1676`, the apply `:1694`. The
   post-apply overlay fold now has its OWN inner catch that SWALLOWS a failure as an
   optimization-only step (`:1718-1728`: "the commit succeeded... nothing is bricked", each
   container coherent-on-throw), so `materializeCommitted` is no longer part of the window. What
   remains is `applyRefLogTxn` **itself** throwing: shape/logic failures are argued unreachable
   (whole-item validation precedes object creation), but an ALLOCATION failure is not — the COW
   containers allocate on apply (`RefCowMap::erase` tombstones a base-only row,
   `CasRefCowMap.cpp:162`), and the tracked allocator can throw `MEMORY_LIMIT_EXCEEDED` (the
   sibling fold catch says so explicitly). The outer catch (`:1730-1755`) fails the survivors and
   RETHROWS `LOGICAL_ERROR`; `completeOwnedItemsAndReleaseLeadership` (`:1100-1126`) then clears
   `leader_active`. No wedge, no fence, no remount — the exact "durable, unapplied, unwedged,
   idle, warm" state, now with a loud error to the WRITER but nothing observable to a READER.
   (Build asymmetry: `LOGICAL_ERROR` aborts under `DEBUG_OR_SANITIZER_BUILD`, so this state is
   release-only — which is precisely where it must not be silent.)
   **The marker is STICKY**: the cache stays divergent from durable truth until the table is
   re-recovered, and later flushes would apply on top of that stale base — so the marker is
   cleared ONLY by a fresh recovery (remount / evict-and-reload), never by the next successful
   flush. Effect on this feature: relink from that sender for that table degrades to bytes until
   recovery — fail-closed and self-limiting. After item 3 lands the marker remains as an
   assert-layer (it should never trip).
2. **`precommitAdd` mint-tightening** (confirm gate 1 rule 5's soundness). An unowned
   `ManifestId` may enter ownership only from the transaction that freshly staged it.
   Re-verified 2026-07-24: `precommitAdd` (`CasPartWriteTxn.cpp:920`) still validates ONLY
   `id.root_namespace == target_ns` (`:926-929`) — any namespace-matching id is accepted — so the
   ABA latitude is unchanged and the tightening is still required. Re-owning an exact old unowned
   pair is tested-legal (`gtest_cas_promote_republish.cpp:283` — that pin is replaced).
   Already-committed idempotence stays a read-only no-op (the closure's existing
   already-committed-to-this-exact-`manifest_ref` short-circuit, `:952-954`). No production path
   performs the rejected transition.
3. **No-throw-install** (the full fix for the post-durable-PUT allocation window; was BACKLOG
   §ref-ledger-consult-followups-2026-07-21, F2 — folded in here). Scope is now NARROWER than the
   consult framing, because stage1 already solved the fold half (item 1): only the **apply** step
   must become allocation-free after durability. Restructure `commitRefChunk`'s commit arm:
   construct AND fully materialize the exact candidate `RefTableState` BEFORE the PUT (`:1676`) —
   the per-chunk trial apply's result is kept instead of discarded; after durability, install via
   a verified no-throw move under `state_mutex` (`static_assert` on noexcept) in place of the
   throwing `applyRefLogTxn` at `:1694`; tail counters (plain atomics) after. Per-chunk, so the
   candidate is bounded by one chunk's ops (`ref_txn_max_ops`), not a whole tenure.
   Apply the same pattern to the SECOND call site — wedge resolution (`CasRefLedger.cpp:1205+`
   applies post-durable too; the wedge can retain the candidate alongside `{id, key, bytes}`).
   **New finding (2026-07-24), fold into this item:** the two sites are ASYMMETRIC — the
   wedge-resolution arm calls `applyRefLogTxn` → `materializeCommitted` → `wedge.reset()` with NO
   inner swallow (unlike `commitRefChunk`'s `:1718-1728`). A `materializeCommitted` throw there
   leaves the transaction applied but the wedge NOT cleared, so the next resolution re-applies the
   same txn (the `txn_id`-equality guard does not help — it only prevents applying a DIFFERENT
   wedge) and double-bumps the tail counters. Harmless-ish today (erase/insert are idempotent;
   counters only drive the snapshot threshold), but it is the same class and should be fixed with
   the same restructure — or, minimally, by giving the wedge arm `commitRefChunk`'s swallow +
   reset ordering.
   - **Measurement gate FIRST** (consult amendment b): benchmark wedge-resolution-followed-by-
     flush and re-run `BM_FlushInstall` — the current COW shape was justified by numbers; the
     naive pre-PUT materialization must not regress them.
   - **Memory note:** holding the materialized candidate extends the transient 2×-base window
     from "briefly post-apply" (today's `materializeCommitted`) to the PUT duration (up to the
     lane budget, ~90s under retry) per in-flight flush per table — bounded by the admission byte
     budget; acceptable, to be confirmed by the benchmark gate.
   - **Catch reframing** (consult amendment a): the "permanently unreplayable history" wording
     over-claims — after restructuring, narrow the remaining catch to the install step, where it
     becomes provably unreachable.

## The prepared-relink handle (receiver-side lifetime) {#relink-handle}

Splitting today's atomic `publishEntries` (stage+precommit+promote, `PartFolderAccess.cpp:338`)
across an HTTP round-trip requires explicit ownership — a leaked same-epoch precommit is NOT
reclaimed by the stale sweep (older-epoch predicate, `CasRefLedger.cpp:1983-1985`), and
`PartWriteTxn` destruction retires only the build sequence without abandoning
(`CasPartWriteTxn.cpp:118`; today `publishEntries` supplies catch-and-`abandon`,
`PartFolderAccess.cpp:355`).

**`PreparedRelink`**: move-only; owns the `PartWriteTxn`, receiver `ManifestId`, target ref name,
decoded entries, and the exact source token; explicit terminal states; operations `promote` and
durable `abort` (precommit-removal append); a scope guard aborts on every non-promote exit.
(`~PartWriteTxn` — `CasPartWriteTxn.cpp:119-124` — only retires the build sequence; it does NOT
abandon, so destruction alone is never cleanup.)

**Abort-uncertain semantics (honest).** An `Unresolved` abort append wedges the table's lane
(`CasRefLedger.cpp:1784-1796`) — the PRE-EXISTING discipline for any uncertain append: the wedge
retains `{id, key, bytes}`; every later append first re-runs `resolveByExactGet` (`:1205`); a
landed PUT is
applied-before-unwedging (abort completes); a never-landing PUT keeps the lane wedged **until
remount** (GET-based resolution never re-issues). Consequences: fail-long over-protection of the
blobs; the wedge blocks all ref appends for that table on this replica until resolution/remount
(existing blast radius; this feature adds one more producer); a fetch retry does NOT self-heal
the wedge. The handle never destructs-and-forgets — its terminal state records the uncertain
abort; the wedge itself is the persistent cleanup owner.

The txn safely spans the confirm RTT: the temporary-directory lock stays owned by
`fetchSelectedPart` (`DataPartsExchange.cpp:509`); relink opens no disk transaction.

## Failure taxonomy {#failure-taxonomy}

Today relink returns a boolean; **every** `false` triggers the same-sender byte re-request
(decision at `DataPartsExchange.cpp:768-770`), and `adoptPartFromManifest` catches every
`Exception` — including `NETWORK_ERROR` — returning `false`
(`ContentAddressedMetadataStorage.cpp:2003`). So the **exchange boundary becomes typed**: a
*prepare* operation returns `PreparedRelink` or `MechanismFallbackAllowed`; `Fetcher` owns
confirm/abort/throw OUTSIDE the metadata-storage catch.

| Result | When | Action |
|---|---|---|
| `Success` | confirm *yes* + promote committed | return the relinked part; normal `tmp-fetch_<part>` → final re-key follows |
| `SourceProofFailed` | confirm *no* / *unknown* / confirm transport failure | `abort`, then **throw a locally-generated retry-later error** (e.g. `NETWORK_ERROR`) from `Fetcher` — never return `false`. The queue stores the exception; `num_tries`/`last_exception_time_ms` backoff postpones (`ReplicatedMergeTreeQueue.cpp:1614,2152`); re-execution recomputes source + covering-part discovery (`StorageReplicatedMergeTree.cpp:2636,5200`). Re-selection does not guarantee a different replica — a repeat confirm fails fast until the entry converges elsewhere. |
| `MechanismFallbackAllowed` | relink cannot work here but the sender has the part: version/cookie mismatch, manifest decode failure, receiver-side publish/`promote` ref conflict | today's same-sender byte re-request (`DataPartsExchange.cpp:731-738`; unknown-cookie fallback logs `:741-747`, returns `:748`) |

Notes: (a) no `ABORTED` backoff exemption exists in the GET_PART queue path — any stored
exception engages postpone; `ABORTED` is only logged specially (`StorageReplicatedMergeTree.cpp:4222`,
`:4229`). (b) "promote aborted on a condemned blob" is not a real class — promotion does not
probe tokenless adopted leaves (`CasPartWriteTxn.cpp:1072-1106`). (c) manual `SYSTEM FETCH PART`
/ `FETCH PARTITION` swallow several REMOTE codes (`StorageReplicatedMergeTree.cpp:8128`) — the
locally-generated error surfaces instead.

## Confirm wire protocol {#wire-protocol}

- New action `confirm_ca_ref` on the existing interserver endpoint (one part-stream operation
  today; version negotiation at `DataPartsExchange.cpp:91,170`), carrying the token; issued as a
  second HTTP request after the relink response is consumed (`assertEOF` at `:762`); dispatched
  BEFORE the handler resets the body or parses `part` (`:170`).
- Routing by `{pool_uuid, server_root_id}` to the exact CA disk/exchange instance — NOT via the
  `DataPart`. `IContentAddressedExchange` (`ContentAddressedExchange.h:35`) gains `ownsNamespace`
  and `confirmExactRef`; gate 0 additionally needs the part-state lookup, which lives at the
  `Service` level (it holds the table — `DataPartsExchange.h:53`). Ambiguous matches → *unknown*.
- Response: authenticated `yes`/`no`/`unknown` (interserver auth precedes dispatch —
  `InterserverIOHTTPHandler.cpp:29`); transport errors/timeouts → `SourceProofFailed`.
- Versioning: relink advertisement gains a confirm-capable version; a new sender relinks only for
  confirm-capable receivers, else bytes; a new receiver on an old cookie falls back as today.
  Mixed versions degrade to bytes, never to unconfirmed relink.

## B66b: relink-into-detached {#b66b}

- **Dedicated `allow_ca_relink` mode with the recursion brake.** Lifting `!to_detached` alone is
  ineffective (manual detached callers pass `try_fetch_shared=false` —
  `StorageReplicatedMergeTree.cpp:8125,8281` — so `try_zero_copy` is already false), and naive
  decoupling would REMOVE the existing brake: the byte fallback recursively re-requests with
  `try_zero_copy=false` precisely so relink is not re-advertised (`DataPartsExchange.cpp:534-545`,
  recursion at `:731`). So: relink capability = own `allow_ca_relink` flag; manual detached
  fetches set it true independently of `try_fetch_shared`; EVERY same-sender byte fallback clears
  it; legacy `try_zero_copy` untouched for real zero-copy (`:566`).
- `relinkPartToDisk` gains `to_detached` (today hardcodes the active parent,
  `DataPartsExchange.cpp:1107-1128`); detached target = temporary storage under the detached
  parent; CA routing already folds `detached/<name>` refs (`ContentAddressedMetadataStorage.cpp:1241`).
- Detached finalization keeps `renameTo(detached/<part>, true)` (branch
  `StorageReplicatedMergeTree.cpp:5715`, rename `:5719`) with its existing collision behavior
  (`:8115`, `DataPartStorageOnDiskBase.cpp:795`). No collision-semantics change.
- Confirm step identical; cross-pool `FETCH PARTITION` keeps streaming bytes.

## Execution order {#execution-order}

One spec, one plan, phases in this order (writing-plans maps them to tasks):

1. **`[UNMATCHED-MINUS-ONE]` pinning gtest** (BACKLOG §3): fold an unmatched `-1`, assert sibling
   edges survive — pins the set-membership property this spec's history leans on. Independent,
   zero risk.
2. **TLA mini-model** (before code, per project discipline): two journals + round fold + cursors +
   three-phase graduation + spare; property: *exact-token confirm yes at T2 under gate 0+1 with
   receiver activation < T2 ⟹ no referenced blob deleted before the receiver's own removal*.
   Interleavings: straddle, wedge-hidden drop (unknown), apply-exception (unknown via poison),
   ABA attempt (rejected), repoint-on-live-part (gate 1), false-404 (existing clamp).
3. **Poison marker + mint-tightening** (ledger hardening 1–2): small, independently landable
   prerequisites; each with its gtest; the poison failpoint test doubles as phase 7's red test.
4. **Publish-confirm core**: `confirmExactRef` (gate 0 + gate 1 snapshot) → typed exchange
   boundary + `PreparedRelink` → wire/versioning → receiver flow split.
5. **B66b + recursion brake** (rides phase 4 plumbing).
6. **Test battery**: race/failpoint integration, codex-6 regression, straddle-spare regression,
   abort-discipline (incl. wedged-abort semantics), recursion-brake, B66b (both manual callers),
   RPL-5 slice, version-mix.
7. **No-throw-install** (ledger hardening 3): measurement gate first (wedge→flush bench +
   `BM_FlushInstall`), then the restructure, failpoint battery, soak. Deliberately LAST: phases
   3–6 are correct without it (poison makes the window visible), the release cares about the
   codex-6 blocker first, and landing it later never invalidates the confirm (gate 1 rule 4
   simply stops firing). Alternative (rejected): before phase 4 to touch the flush code once —
   not worth delaying the blocker for measurement+soak gates.

## Testing {#testing}

- **Gate 0** (sender gtests/integration): part in `Deleting` → *no*; part absent / other disk →
  *no*; `Active`/`Outdated` live → gate 1 proceeds; concurrent grab vs confirm → safe direction
  (grab after snapshot ⇒ `-1` after T2); restart with durably-initiated removal → part not
  reloaded → *no*.
- **Gate 1 determinism**: failpoint injecting an allocation failure inside `applyRefLogTxn` after
  the chunk's committed PUT → poison armed → *unknown*, and the marker STAYS armed across a later
  successful flush (sticky), clearing only after a fresh recovery; mid-tenure partially-durable
  chunked flush (`CarvePhaseForTest::ChunkReseed`) → *unknown* while `leader_active`;
  durable-but-wedged drop (`gtest_cas_ref_writer.cpp:778` shape) → *unknown*;
  repointed live part (removal-mark unlink) → *no* by equality — the gate-0-bypass regression;
  dropped+recreated → *no*; mint-tightening: `precommitAdd` of an unstaged old id → rejected
  (replaces `gtest_cas_promote_republish.cpp:283`); cold/evicted/recovering → *unknown*, zero
  backend requests; pending/in-flight/wedge → *unknown*; quiescent exact match → *yes*;
  two-mutex snapshot race test.
- **Race integration**: failpoint between `precommitAdd` and confirm; sender drops the part →
  *no* → abort → retryable fail → queue re-selects (covering-part / other replica; NO byte
  re-request to the original sender); sender alive → *yes* → promote → relink proof
  (`CasBlobPut == 0`).
- **codex-6 regression**: receiver publish stalled ≥ 3 GC rounds (aggressive
  `old_parts_lifetime`), sender part merged away, GC to fixpoint → no committed ref from the
  stalled attempt; fsck clean, dangling=0.
- **Straddle-spare regression**: receiver `+1` folds one round after sender `-1` → spared, never
  deleted.
- **Abort discipline**: every non-promote exit appends the removal (no same-epoch leak); forced
  UNCERTAIN abort → lane wedges → landed-PUT variant resolves+applies on the next append;
  never-landing variant keeps retry-later until remount — matching documented semantics.
- **Recursion brake**: persistent mechanism failure → exactly one relink attempt, then bytes; the
  fallback request does not re-advertise relink.
- **B66b**: both manual detached callers relink same-pool (`CasBlobPut == 0`) + `ATTACH` reads;
  collision behavior byte-identical to the byte path; cross-pool → bytes; capability independent
  of `try_fetch_shared`.
- **RPL-5 slice**: `REPLACE PARTITION`/`ATTACH PARTITION ... FROM` on 2-replica CA (extend
  `test_cas_replicated_relink`): queue-cloned `REPLACE_RANGE` fetch relinks (blob-count proof).
- **Version-mix**: confirm-capable receiver × legacy sender cookie → clean byte fallback.
- **No-throw-install (phase 7)**: bench gates (wedge→flush, `BM_FlushInstall`, plus a chunked
  multi-transaction tenure — the candidate is now built per chunk) before/after; failpoint battery
  incl. allocation-failure injection between PUT and install (post-fix: install provably cannot
  throw — the poison assert-layer never trips); wedge-arm symmetry test (a `materializeCommitted`
  throw during wedge resolution must not leave an applied-but-unreset wedge → no double-apply, no
  double-counted tail); soak.

## Naming {#naming}

"Publish-then-confirm relink". Confirm primitive: `confirmExactRef` (read-only; gate 0
part-anchored, gate 1 exact-token lane-linearized, poison-aware). No new journal vocabulary.
