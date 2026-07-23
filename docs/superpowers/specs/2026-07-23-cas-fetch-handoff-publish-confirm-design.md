---
description: 'Design for closing the CAS fetch-by-relink commit-before-release gap (codex-6, #42/#43) with publish-then-confirm: the receiver publishes its manifest + precommit as today, then verifies via a lane-linearized EXACT-token query that the sender''s committed ref still names the transferred manifest, and promotes only on a positive answer. EDGE-BEFORE-OBSERVE lifted to part level; zero GC/codec/sweep changes. Rev.2 after a second adversarial review: the confirm primitive is exact-ManifestRef and append-lane-linearized (naive cached resolveRef is provably unsafe). Supersedes the 2026-07-15 retention-pin design and the short-lived 2026-07-23 reserved-precommit design. Also covers B66b relink-into-detached and the RPL-5 REPLACE PARTITION queue-clone test slice.'
sidebar_label: 'CAS Fetch-Handoff Publish-Confirm'
sidebar_position: 20260724
slug: /superpowers/specs/cas-fetch-handoff-publish-confirm-design
title: 'CAS fetch-handoff — publish-then-confirm relink'
doc_type: 'reference'
---

# CAS fetch-handoff — publish-then-confirm relink {#cas-fetch-handoff-publish-confirm}

**Date:** 2026-07-23 (rev.2 same day, after the second adversarial review)
**Branch:** `cas-gc-rebuild`
**Status:** design (user-approved brainstorm incl. the failure-matrix correction; rev.2 folds in the
codex `gpt-5.6-sol` xhigh round-2 findings — both Criticals resolved by redefining the confirm
primitive, no architecture change)
**Covers:** codex-6 / #42/#43 (relink commit-before-release gap), B66b (relink-into-detached, RPL-4
perf cliff), RPL-5 test slice (`REPLACE PARTITION` queue-clone relink proof).
**Out of scope:** B66a (local-backend torn read — `BACKLOG.md` §14), `[UNMATCHED-MINUS-ONE]`
(verified harmless, BACKLOG §3 — only a pinning test remains), bulk warm-up.

## Problem {#problem}

Same-pool replication uses fetch-by-relink: the sender transfers only the part's manifest; the
receiver publishes its own ref over the already-shared blobs (zero byte cost). The gap
(self-documented at `ContentAddressedMetadataStorage.cpp:1990`; note that comment's internal
citation `DataPartsExchange.cpp:256-259` is stale — the source part is released when
`processQuery` returns at `:276`): the sender is fire-and-forget — it releases the part when the
HTTP response completes, before the receiver's publish. If the receiver's `precommitAdd` edge-PUT
is not yet durable while the sender's now-`Outdated` part is collected (removal `-1` folded,
condemned, graduated, deleted — ≥ 3 GC rounds + `old_parts_lifetime`), the receiver later commits
a manifest whose blobs are gone — a dangling committed manifest (fsck-detected, not silent).

## Design history — why the previous two designs are gone {#history}

- **Retention pin** (`2026-07-15-cas-fetch-handoff-retention-pin-design.md`, superseded): a
  pool-global GC overlay — a second reachability source, union folding, removal-deferral,
  owner-liveness machinery. Rejected as structural growth in the most safety-critical component.
- **Reserved precommit** (2026-07-23, removed same day; git history is its archive): the receiver
  reserves the manifest identity before the fetch and the *sender* materializes the body at the
  reserved key. Abandoned after an adversarial review (codex `gpt-5.6-sol`, xhigh). The review's
  headline counterexample — a late foreign PUT after a dead-build skip yields an **unmatched
  `-1`** — is real up to its last step, but the deletion consequence was later refuted: in-degree
  is a source-edge SET applied last-wins per key (`CasBlobInDegree.h:139`, `.cpp:574`), so an
  unmatched remove is a per-key no-op and cannot strip other manifests' edges (see BACKLOG §3
  `[UNMATCHED-MINUS-ONE]`). The design was dropped for the review's OTHER confirmed findings,
  each alone disqualifying: the `Reserved` kind sprawls through the whole owner state model
  (kindless state rows, snapshot codec that hard-requires `Precommit`, sweep reconstruction,
  recovery, fsck); foreign materialization bypasses `stageManifest`'s caps/sealing/controlled-PUT
  and `promote`'s `adoptEvidence` discipline; and publishing at final names contradicts the
  existing `tmp-fetch_<part>` + `renameTempPartAndReplace` fetch contract. Root lesson stands: a
  *foreign writer* into another server's manifest domain drags the entire state model into scope.
- **Rev.2 of this design** (same day): the round-2 review proved a *naive* confirm — a cached
  `resolveRef` by ref NAME — unsound on two axes: (1) the sender's cached committed view can lag
  its own durable removal (`putIfAbsentControlled` commits at `CasRefLedger.cpp:~1458` before
  `applyRefLogTxn` flips the cache; worse, an `Unresolved` outcome wedges the lane and
  *deliberately* leaves the cache unchanged until a later resolution — a durable-but-invisible
  drop, demonstrated by `gtest_cas_ref_writer.cpp:778-782`); (2) a committed ref is legally
  repointable (`repointRef` with `allow_repoint`, standalone committed-part rewrites/removal
  marks) and droppable+recreatable — name-only confirmation is an ABA. Both closed below by the
  exact-token, lane-linearized confirm primitive; the approach itself is unchanged.

Both abandoned designs tried to *carry trust across two single-writer domains*. This design stops
doing that: each party only ever writes its own domain, and the receiver *verifies* instead of
trusting.

## Core idea {#core-idea}

**Publish, then confirm, then promote** — EDGE-BEFORE-OBSERVE lifted to part level:

1. The receiver runs today's flow unchanged: stage its own manifest body (own write, own tree),
   `precommitAdd` — its `+1` edge is now durable in its own journal, with a present body (**T1**).
   (`precommitAdd` is durable on return: it goes through the synchronous append lane and the
   caller waits for the acked conditional PUT — verified `CasPartWriteTxn.cpp:865`,
   `CasRefLedger.cpp:977`; batching does not weaken this.)
2. **Confirm** (one read-only interserver query, `confirmExactRef`): "does your committed ref for
   `<part>` still name **exactly this** `ManifestRef`?" — answered by the sender under the
   linearization rules of §[The confirm primitive](#confirm-primitive) (**T2**).
3. Answer *yes* → `promote`. Anything else → abort per §[Failure taxonomy](#failure-taxonomy).

Under `INV-NO-DANGLE`, every blob of the part stays alive exactly as long as the sender's
committed binding of *that manifest* lives, and the blobs lose that protection only through the
sender's removal `-1`. So one part-level check at the right moment replaces any per-blob probing —
and the "right moment" is *after* the receiver's own evidence is durable, which is what kills the
TOCTOU.

## The confirm primitive — exact-token, lane-linearized {#confirm-primitive}

The confirm is a dedicated read primitive on the sender's `CasRefLedger`, NOT a call to the
ordinary `resolveRef` (which reads the cached `RefTableState` with no lane synchronization —
`CasRefLedger.cpp:170-199` — and is provably stale in both windows below).

**Input — the exact source token**, minted by the sender at offer time and carried alongside the
manifest bytes: `{pool identity, root_namespace, ref_name, ManifestRef}`. The transferred manifest
body already embeds `ref` + `root_namespace_id` (`CasPartManifestFormat.h:67`) — the receiver
stops discarding them (today `adoptPartFromManifest` deliberately ignores sender identity,
`ContentAddressedMetadataStorage.cpp:1954`; for the confirm they become load-bearing).

**Answer rules — pessimistic by construction.** Return *yes* iff ALL hold, evaluated under
`state_mutex` after a lane-linearization step:

- the mount fence is live and this instance is the current single writer of the namespace;
- the table's runtime is warm (recovered and resident). A cold/evicted table returns *unknown* —
  the confirm path performs **zero object-store I/O** by contract (no `ensureRefTableRecovered`
  recovery from storage);
- the append lane for this table is **quiescent for the queried ref**: no wedge
  (`RefAppendWedge`), and no pending/in-flight append batch. A wedge or pending batch returns
  *unknown* — this closes both stale-cache windows: the microsecond durable-PUT→cache-flip gap
  and the arbitrarily long `Unresolved`-wedge gap (durable drop invisible to the cache until a
  later mutation resolves the wedge);
- the current committed row for `ref_name` exists and its `ManifestRef` **equals the token's**
  `ManifestRef` exactly. A missing row, a repointed row (CSN fill-in rewrites, removal marks), or
  a dropped-and-recreated ref all fail this equality — closing the ABA.

*Yes* therefore proves: at T2, the exact transferred manifest was the committed binding, no
removal of it was durable, none was in flight past the admission point, and the answerer was the
live single writer. Any removal `-1` for it is appended strictly after T2 > T1.

**Cost:** zero S3 requests; a mutex + comparisons on the warm path. Pessimistic *unknown* under
write-load collisions (in-flight batch on the same table) just routes the receiver to the retry
path; correctness never depends on the optimistic answer.

## Why there is no TOCTOU {#correctness}

Let T1 = receiver's `+1` durable (body present), T2 = a *yes* from the confirm primitive, T3 =
sender's removal `-1` of the confirmed manifest appended. Physical deletion of a blob requires
three GC phases: condemn (round N) → `delete_pending` (N+1) → pre-CAS delete (N+2), each derived
from folded journals (fold precedes the round's deletes — verified `CasGc.cpp:392,410`).

- **`-1` appended after T2** (the "sender deletes immediately after answering" case): any fold
  that reads the `-1` runs after T3 > T2 > T1, when the receiver's `+1` is durable and foldable.
  The round's NET in-degree includes both → never zero → condemn never happens.
- **A round straddles T1** (reads the receiver's journal before T1, the sender's after T3): that
  round can condemn. But the next round's fold *cannot miss* a durable `+1` — the cursor never
  advanced past it (`CasGc.cpp:1115,1197`) — so the blob's in-degree recovers and the entry is
  **spared** during graduation: settlement sends any `indeg > 0` to `spared`
  (`CasBlobInDegree.cpp:405`), including `delete_pending` rows (`CasGc.cpp:482` logs and does not
  delete). Deletion needs the `+1` invisible for three consecutive folds — impossible for a
  durable journal entry.
- **False-404 of the receiver's staged body** at a later fold: live precommit + absent body =
  today's fold barrier — anomaly + clamp + `suppress_destructive` (`CasGc.cpp:1354,1360`). This
  design changes **nothing** in the fold, so the existing hammer keeps covering the store-lie
  case.
- **`-1` durable (possibly wedge-hidden) or manifest repointed/recreated before T2**: the confirm
  primitive answers *no*/*unknown* (lane quiescence + exact-`ManifestRef` equality), promote never
  runs, nothing dangles — this is the codex-6 window and both round-2 counterexamples.
- **Receiver frozen between confirm-yes and promote** (arbitrarily long): its durable precommit's
  folded `+1` protects the blobs while the build lives; if the build dies, the old epoch is fenced
  and `promote` fails before committing (`CasPartWriteTxn.cpp:125,1016`) — the thawed receiver
  retries with a fresh build. No dangling outcome exists.
- **Sender's `delete_tmp_*` rename flow**: rename publishes the destination before dropping the
  source (`PartFolderAccess.cpp:374`) and repoints the SAME manifest — during the overlap the
  exact-token confirm may legitimately still answer *yes* (the manifest stays committed-bound);
  after the drop it answers *no*. No unprotected drain interval (verified in review round 2).

## The prepared-relink handle (receiver-side lifetime) {#relink-handle}

Splitting today's atomic `publishEntries` (stage+precommit+promote in one call,
`PartFolderAccess.cpp:338`) across an HTTP round-trip requires explicit ownership — a leaked
same-epoch precommit is NOT reclaimed by the stale sweep (it removes only older-epoch bindings,
`CasRefLedger.cpp:1956`), and `PartWriteTxn` destruction retires only the build sequence without
abandoning the precommit (`CasPartWriteTxn.cpp:118`; today `publishEntries` supplies the
catch-and-`abandon` discipline, `PartFolderAccess.cpp:355`).

**`PreparedRelink` handle**: owns the `PartWriteTxn`, the receiver `ManifestId`, the target ref
name, the decoded entries, and the exact source token. Operations: `promote` and durable `abort`
(append the precommit removal). A scope guard invokes `abort` on every non-promote exit: confirm
*no*/*unknown*, transport failure, local part-construction failure, cancellation, exception. If
`abort` itself ends UNCERTAIN (backend outage), the handle fails the fetch retryably and leaves
the precommit in place — protection errs long, never short; the binding is then cleaned by the
next explicit abort/retry or, after a remount, by the stale sweep (the destructor-abandon debris
class is already tracked in BACKLOG §disk-error-audit).

Holding the txn across the confirm RTT is mechanically fine: `temporary_directory_lock` stays
owned by `fetchSelectedPart` (`DataPartsExchange.cpp:504`) and relink opens no disk transaction.

## Failure taxonomy {#failure-taxonomy}

Today relink returns a boolean and **every** `false` triggers the same-sender byte re-request
(`DataPartsExchange.cpp:764`). That is wrong for confirm failures (the sender provably lacks the
part), so the receiver-side result becomes typed:

| Result | When | Action |
|---|---|---|
| `Success` | confirm *yes* + promote committed | return the relinked part; the normal `tmp-fetch_<part>` → final re-key follows |
| `SourceProofFailed` | confirm *no* / *unknown* / confirm transport failure | `abort` the handle, then **throw a locally-generated retry-later error** (e.g. `NETWORK_ERROR`) — never return `false`. The replication queue stores the exception and its `num_tries`/`last_exception_time_ms` backoff postpones the entry (`ReplicatedMergeTreeQueue.cpp:1614,2152`); on re-execution source and covering-part discovery are recomputed (`StorageReplicatedMergeTree.cpp:2636,5200`). NOTE: re-selection does not guarantee a *different* replica (shuffled scan may repick the sender) — a repeat confirm then fails fast again until the entry converges on another source or a covering part. |
| `MechanismFallbackAllowed` | relink cannot work here but the sender still has the part: protocol-version/cookie mismatch, manifest decode failure, receiver-side publish/`promote` ref conflict | today's same-sender byte re-request (`DataPartsExchange.cpp:731-738`; unknown-cookie fallback logs at `:741-747`, returns at `:748`) |

Notes: (a) there is **no** `ABORTED` backoff exemption in the GET_PART queue path — any stored
exception engages the postpone logic; `ABORTED` is only logged specially for fetches
(`StorageReplicatedMergeTree.cpp:4222`). The earlier draft's "#37 exemption" concern applies to
merge tasks, not fetches. (b) "promote aborted on a condemned blob" is NOT a real
mechanism-failure class — promotion deliberately does not probe tokenless adopted leaves
(`CasPartWriteTxn.cpp:1031-1035`). (c) Manual `SYSTEM FETCH PART` / `FETCH PARTITION` callers
swallow several remote transport codes (`StorageReplicatedMergeTree.cpp:8128,8283`) — the
`SourceProofFailed` error is locally generated precisely so it surfaces instead of being
swallowed.

## Confirm wire protocol {#wire-protocol}

- A new action on the existing interserver endpoint (today it has one part-stream operation,
  protocol version negotiated at `DataPartsExchange.cpp:91,170`): `confirm_ca_ref`, carrying the
  opaque exact source token. Issued as a second HTTP request after the relink response is fully
  consumed (`assertEOF` at `:760`).
- Routing: by pool/disk identity from the token to the exact CA exchange instance that made the
  offer — NOT via the `DataPart` (which may already be gone).
- Response: authenticated `yes` / `no` / `unknown`; transport errors and timeouts map to
  `SourceProofFailed` on the receiver.
- Version negotiation: relink advertisement gains a confirm-capable protocol version. A new
  sender relinks only for confirm-capable receivers — otherwise it streams bytes. A new receiver
  seeing an old/unknown relink cookie falls back exactly as today. Mixed-version windows degrade
  to bytes, never to the unconfirmed relink with the known gap.

## B66b: relink-into-detached {#b66b}

Corrected scope after round-2 review — lifting the `!to_detached` advertise gate alone is
**ineffective**, because both manual detached callers pass `try_fetch_shared=false`, which already
makes `try_zero_copy` false (`StorageReplicatedMergeTree.cpp:8125,8273`):

- **Decouple** the CA-relink advertisement from the legacy zero-copy switch: relink capability
  becomes its own condition (CA target disk + same-pool + confirm-capable version), no longer
  gated on `try_zero_copy`/`try_fetch_shared` (`DataPartsExchange.cpp:534-545`).
- `relinkPartToDisk` gains a `to_detached` flag: today it hardcodes the active table parent
  (`DataPartsExchange.cpp:1107-1128`); for detached it constructs the temporary storage under the
  detached parent. CA routing already folds any `detached/<name>` ref
  (`ContentAddressedMetadataStorage.cpp:1241`).
- Detached finalization keeps its existing contract — `renameTo(detached/<part>, true)` at
  `StorageReplicatedMergeTree.cpp:5715`, NOT `renameTempPartAndReplace` — including its existing
  collision behavior (racy pre-check + replace, `:8115`, `DataPartStorageOnDiskBase.cpp:795`).
  The spec makes no collision-semantics change on either path.
- The confirm step is identical. Cross-pool `FETCH PARTITION` keeps streaming bytes.

This closes the RPL-4 perf cliff (same-pool `FETCH PART/PARTITION` and `to_detached` replication
fetches stream full bytes today).

## Scope and non-goals {#scope}

- **Zero changes** to the GC fold, journal codec, snapshot format, sweeps, orphan protection, or
  the anomaly/suppression semantics. No new journal states. No foreign writers. Every write stays
  in its owner's domain. The only new sender-side surface is the read-only confirm primitive.
- The ordinary write path (INSERT/merge) is untouched.
- B66a — `BACKLOG.md` §14 (local-backend atomicity, orthogonal).
- The ordinary-`Precommit` unmatched-`-1` interleaving (false-404 at activation-fold × dead-build
  skip × body readable at removal-fold), surfaced while reviewing the abandoned reserved design,
  was **verified harmless**: in-degree is a source-edge set, an unmatched remove is a per-key
  no-op (BACKLOG §3 `[UNMATCHED-MINUS-ONE]`). The backlog item now only pins the load-bearing
  set-membership property with a regression test.
- Bulk write-replica warm-up (future): confirm generalizes trivially — one batched exact-token
  confirm for N parts after N publishes; nothing here precludes it.

## Testing {#testing}

- **Confirm-primitive determinism** (sender-side gtests):
  (a) durable-PUT→cache-flip pause window (failpoint between the controlled PUT and
  `applyRefLogTxn`) → confirm answers *unknown*, never *yes*;
  (b) durable-but-wedged drop (the `gtest_cas_ref_writer.cpp:778` shape) → *unknown*;
  (c) repointed ref (same name, different `ManifestRef`) and dropped+recreated ref → *no*
  (exact-token mismatch — the ABA regression);
  (d) cold/evicted table → *unknown* with zero backend requests;
  (e) live committed exact match, quiescent lane → *yes*.
- **Race integration tests** (failpoint between `precommitAdd` and confirm): sender drops the part
  in the window → confirm *no* → abort → retryable failure → queue re-selects (assert the
  covering-part / other-replica path fires and NO byte re-request goes to the original sender);
  sender alive → *yes* → promote → relink proof (`CasBlobPut == 0` on the receiver).
- **codex-6 regression:** stall the receiver's publish ≥ 3 GC rounds under an aggressive config
  (`old_parts_lifetime` small) while the sender's part is merged away and GC runs to fixpoint →
  the stalled attempt must NOT produce a committed ref (confirm rejects on resume); fsck clean,
  dangling=0.
- **Straddle-spare regression:** force the condemn-then-spare interleaving (receiver's `+1` folds
  one round after the sender's `-1`) and assert the blob is spared and never deleted.
- **Abort-discipline test:** kill/except every non-promote exit of the `PreparedRelink` handle and
  assert the precommit removal is appended (no same-epoch leak); force the abort itself UNCERTAIN
  and assert the fetch fails retryably with the binding retained.
- **B66b:** same-pool `FETCH PART` into `detached/` (both manual callers) → relink proof +
  subsequent `ATTACH` reads correctly; collision behavior byte-identical to the byte path;
  cross-pool → bytes; capability decoupled from `try_fetch_shared=false`.
- **RPL-5 slice (pulled in):** `REPLACE PARTITION`/`ATTACH PARTITION ... FROM` on a 2-replica CA
  table (extend `test_cas_replicated_relink`): assert the queue-cloned `REPLACE_RANGE` fetch
  relinks (blob-count proof), not a silent byte re-fetch.
- **Version-mix test:** confirm-capable receiver × legacy sender cookie → clean byte fallback.
- **TLA mini-model:** two journals + round fold with cursors + three-phase graduation + spare;
  property: *if the exact-token confirm observed the committed binding at T2 under a quiescent
  lane and the receiver's activation precedes T2, no blob of the receiver's manifest is deleted
  before the receiver's own removal event*. Model the straddle interleaving, the wedge-hidden
  durable drop (must yield unknown), the ABA repoint, and the false-404 branch (existing clamp).

## Naming {#naming}

"Publish-then-confirm relink". The confirm primitive: `confirmExactRef` (read-only, exact-token,
lane-linearized). No new journal vocabulary — deliberately.
