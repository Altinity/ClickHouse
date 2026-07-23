---
description: 'Design for closing the CAS fetch-by-relink commit-before-release gap (codex-6, #42/#43) with publish-then-confirm: the receiver publishes its manifest + precommit as today, then verifies via a lane-linearized EXACT-token query that the sender''s committed ref still names the transferred manifest, and promotes only on a positive answer. EDGE-BEFORE-OBSERVE lifted to part level; zero GC/codec/sweep changes. Rev.3 after three adversarial review rounds: exact-token + lane-linearized confirm with an apply-pending poison rule, precommitAdd mint-tightening, a locked snapshot predicate, a relink recursion brake, honest abort-wedge semantics, and a typed exchange boundary. Supersedes the 2026-07-15 retention-pin design and the short-lived 2026-07-23 reserved-precommit design. Also covers B66b relink-into-detached and the RPL-5 REPLACE PARTITION queue-clone test slice.'
sidebar_label: 'CAS Fetch-Handoff Publish-Confirm'
sidebar_position: 20260724
slug: /superpowers/specs/cas-fetch-handoff-publish-confirm-design
title: 'CAS fetch-handoff — publish-then-confirm relink'
doc_type: 'reference'
---

# CAS fetch-handoff — publish-then-confirm relink {#cas-fetch-handoff-publish-confirm}

**Date:** 2026-07-23 (rev.3 — three same-day adversarial review rounds folded in)
**Branch:** `cas-gc-rebuild`
**Status:** design (user-approved brainstorm incl. the failure-matrix correction; rev.2 fixed the
confirm primitive after review round 2; rev.3 folds in round 3 — one interaction with a KNOWN
pre-existing ledger defect, plus implementation-contract precision; the approach itself has
survived all three rounds unchanged)
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
- **Rev.2** (same day): round 2 proved a *naive* confirm — a cached `resolveRef` by ref NAME —
  unsound on two axes: (1) the sender's cached committed view can lag its own durable removal
  (`putIfAbsentControlled` commits before `applyRefLogTxn` flips the cache; an `Unresolved`
  outcome wedges the lane and *deliberately* leaves the cache unchanged — a durable-but-invisible
  drop, demonstrated by `gtest_cas_ref_writer.cpp:778-782`); (2) a committed ref is legally
  repointable and droppable+recreatable — name-only confirmation is an ABA. Fixed by the
  exact-token, lane-linearized confirm primitive.
- **Rev.3** (same day): round 3 attacked the rev.2 primitive and found one genuine remaining
  window — a **post-durable-PUT apply exception** (the catch after `applyRefLogTxn` installs no
  wedge, so "durable, unapplied, unwedged, idle, warm" is reachable via an allocation failure;
  this is exactly the KNOWN pre-existing ledger defect already tracked as the BACKLOG
  "post-durable-PUT allocation window" consult item) — closed here by the apply-pending poison
  rule, which is also that backlog item's fail-closed half. Round 3 also: tightened
  `precommitAdd` to txn-minted ids (the state machine legally re-owns an exact old unowned
  `ManifestRef` — `gtest_cas_promote_republish.cpp:283` — even though no production path does);
  pinned the confirm's two-mutex snapshot protocol; added the relink recursion brake B66b would
  otherwise remove; replaced the false "retry self-heals an uncertain abort" claim with the real
  lane-wedge semantics; and pushed the typed result across the exchange boundary (the current
  boolean `adoptPartFromManifest` catches everything and returns false —
  `ContentAddressedMetadataStorage.cpp:2003` — which would silently convert `SourceProofFailed`
  into the forbidden same-sender byte retry).

Both abandoned designs tried to *carry trust across two single-writer domains*. This design stops
doing that: each party only ever writes its own domain, and the receiver *verifies* instead of
trusting.

## Core idea {#core-idea}

**Publish, then confirm, then promote** — EDGE-BEFORE-OBSERVE lifted to part level:

1. The receiver runs today's flow unchanged: stage its own manifest body (own write, own tree),
   `precommitAdd` — its `+1` edge is now durable in its own journal, with a present body (**T1**).
   (`precommitAdd` is durable on return: the append goes through the synchronous lane and the
   caller waits for the acked conditional PUT — append at `CasPartWriteTxn.cpp:926-970`,
   synchronous completion at `CasRefLedger.cpp:988-1030`; batching does not weaken this.)
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
committed lookup at `CasRefLedger.cpp:198-205` — and is provably stale in the windows below).

**Input — the exact source token**, minted by the sender at offer time and carried alongside the
manifest bytes: `{pool_uuid, server_root_id, root_namespace, ref_name, ManifestRef}`. The
`server_root_id` is required for routing: a pool UUID is deliberately shared across server
roots/disks, so it alone cannot select the answering mount. The transferred manifest body already
embeds `ref` + `root_namespace_id` (`CasPartManifestFormat.h:76-78`) — the receiver stops
discarding them (today `adoptPartFromManifest` deliberately ignores sender identity,
`ContentAddressedMetadataStorage.cpp:1954`; for the confirm they become load-bearing).

**Answer rules — pessimistic by construction.** Return *yes* iff ALL hold:

1. the mount fence is live and this instance is the current single writer of the namespace
   (checked LAST, before releasing the locks below — race-free against a concurrent fence trip);
2. the table's runtime is warm (recovered, not `recovery_in_progress`, not
   `superseded_by_remount`, resident). A cold/evicted/recovering table returns *unknown* — the
   confirm path performs **zero object-store I/O** by contract (no recovery from storage);
3. the append lane for this table is **quiescent**: no `RefAppendWedge`, no pending queue items,
   no `leader_active` in-flight batch. Any of these returns *unknown* — closing both stale-cache
   windows of rev.2 (the durable-PUT→cache-flip gap and the arbitrarily long `Unresolved`-wedge
   gap);
4. the **apply-pending poison marker is clear**. NEW (rev.3): a non-allocating marker armed
   before the lane's durable PUT and cleared only after `applyRefLogTxn` succeeds. It makes the
   post-durable-PUT apply-exception state — durable, unapplied, unwedged, idle, warm; reachable
   today because the catch after the apply installs no wedge (`CasRefLedger.cpp:~1512`; even a
   removal allocates COW tombstones, so a recoverable allocation failure fits between the PUT and
   the install) — visible as *unknown*. This marker is also the fail-closed half of the KNOWN
   pre-existing "post-durable-PUT allocation window" ledger defect (BACKLOG
   §ref-ledger-consult-followups-2026-07-21); the full no-throw-install fix remains that item's
   scope;
5. the current committed row for `ref_name` exists and its `ManifestRef` **equals the token's**
   exactly. A missing row, a repointed row (CSN fill-in rewrites, removal marks), or a
   dropped-and-recreated ref all fail this equality — closing the ABA.

**Lock protocol (rev.3, review round 3):** the predicates span two mutexes today (`pending` /
`leader_active` under `ref_queue_mutex` — `CasRefLedger.h:267,344`, admission at `:978`; rows and
wedge under `state_mutex`), so the confirm is ONE internal snapshot operation: acquire
`ref_queue_mutex`, pin the resident runtime, acquire `state_mutex` without releasing the queue
lock, evaluate rules 2→5 in order, evaluate rule 1 (fence) last, then release. An append admitted
after release is ordered strictly after T2 — which is the safe direction.

**ABA hardening (rev.3):** exact-`ManifestRef` equality is sufficient only if an unowned old
`ManifestRef` can never be re-owned. The state machine currently permits it (`precommitAdd`
accepts any namespace-matching id, not only ids minted by this transaction —
`CasPartWriteTxn.cpp:906`; re-adding an exact old unowned pair is tested-legal,
`gtest_cas_promote_republish.cpp:283` — though no production path does it). This spec therefore
REQUIRES tightening `precommitAdd`: an unowned `ManifestId` may enter ownership only from the
transaction that freshly staged it (already-committed idempotence stays a read-only no-op). One
writer-side check; closes the latitude structurally.

*Yes* therefore proves: at T2, the exact transferred manifest was the committed binding, no
removal of it was durable, none was in flight past the admission point, no durable-but-unapplied
transaction existed, and the answerer was the live single writer. Any removal `-1` for it is
appended strictly after T2 > T1.

**Cost:** zero S3 requests; two mutexes + comparisons on the warm path. Pessimistic *unknown*
under write-load collisions just routes the receiver to the retry path; correctness never depends
on the optimistic answer.

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
- **`-1` durable before T2 — in any form**: applied (equality fails → *no*), wedge-hidden
  (*unknown*), in-flight (*unknown*), or durable-but-unapplied via the apply-exception window
  (poison marker → *unknown*). This is the codex-6 window plus every counterexample of review
  rounds 2 and 3.
- **Manifest identity re-bound after deletion (ABA)**: impossible once `precommitAdd` is
  mint-tightened (§[confirm primitive](#confirm-primitive)); repoints and recreations mint fresh
  `ManifestRef`s and fail the exact-token equality.
- **Receiver frozen between confirm-yes and promote** (arbitrarily long): its durable precommit's
  folded `+1` protects the blobs while the build lives; if the build dies, the old epoch is fenced
  and `promote` fails before committing (`requireAlive` — fence-bearing check at
  `CasPartWriteTxn.cpp:125`, invoked by `promote` at `:991`) — the thawed receiver retries with a
  fresh build. No dangling outcome exists.
- **Sender's `delete_tmp_*` rename flow**: rename publishes the destination before dropping the
  source (`PartFolderAccess.cpp:399-400`) and repoints the SAME manifest — during the overlap the
  exact-token confirm may legitimately still answer *yes* (the manifest stays committed-bound);
  after the drop it answers *no*. No unprotected drain interval (verified in review round 2).

## The prepared-relink handle (receiver-side lifetime) {#relink-handle}

Splitting today's atomic `publishEntries` (stage+precommit+promote in one call,
`PartFolderAccess.cpp:338`) across an HTTP round-trip requires explicit ownership — a leaked
same-epoch precommit is NOT reclaimed by the stale sweep (older-epoch predicate at
`CasRefLedger.cpp:1983-1985`), and `PartWriteTxn` destruction retires only the build sequence
without abandoning the precommit (`CasPartWriteTxn.cpp:118`; today `publishEntries` supplies the
catch-and-`abandon` discipline, `PartFolderAccess.cpp:355`).

**`PreparedRelink` handle**: move-only, owns the `PartWriteTxn`, the receiver `ManifestId`, the
target ref name, the decoded entries, and the exact source token; explicit terminal states.
Operations: `promote` and durable `abort` (append the precommit removal). A scope guard invokes
`abort` on every non-promote exit: confirm *no*/*unknown*, transport failure, local
part-construction failure, cancellation, exception.

**Abort-uncertain semantics (rev.3 — honest wording).** If the abort's lane append ends
`Unresolved`, the table's append lane wedges — the PRE-EXISTING lane discipline for any uncertain
append, not a new mechanism: the wedge retains `{id, key, bytes}` and every later append into the
namespace first re-runs `resolveByExactGet` on it (`CasRefLedger.cpp:~1150-1235`). If the in-doubt
PUT eventually landed, it is applied-before-unwedging and the abort completes; if it never lands,
the resolution stays `Unresolved` and the lane remains wedged **until remount** (GET-based
resolution never re-issues the PUT). Consequences stated plainly: (a) the leaked precommit keeps
the blobs over-protected — fail-long, never fail-short; (b) the wedge blocks ALL ref appends for
that table on this replica (inserts included) until resolution or remount — again the existing
uncertain-append blast radius, to which this feature adds one more producer; (c) a normal fetch
retry does NOT self-heal the wedge — it fails retryably like every other append. The handle
therefore never destructs-and-forgets: its terminal state records the uncertain abort, and the
wedge itself (holding the bytes) is the persistent cleanup owner.

Holding the txn across the confirm RTT is mechanically fine: the temporary-directory lock stays
owned by `fetchSelectedPart` (acquired at `DataPartsExchange.cpp:509`) and relink opens no disk
transaction.

## Failure taxonomy {#failure-taxonomy}

Today relink returns a boolean and **every** `false` triggers the same-sender byte re-request
(decision at `DataPartsExchange.cpp:768-770`). Worse, `adoptPartFromManifest` catches every
`Exception` — including `NETWORK_ERROR` — and returns `false`
(`ContentAddressedMetadataStorage.cpp:2003`), which would silently convert a confirm failure into
the forbidden same-sender byte retry. So (rev.3) the **exchange boundary becomes typed**: the
metadata storage exposes a *prepare* operation returning either a `PreparedRelink` handle or
`MechanismFallbackAllowed`; `Fetcher` owns the handle and performs confirm, abort, and the throw
OUTSIDE any metadata-storage catch.

| Result | When | Action |
|---|---|---|
| `Success` | confirm *yes* + promote committed | return the relinked part; the normal `tmp-fetch_<part>` → final re-key follows |
| `SourceProofFailed` | confirm *no* / *unknown* / confirm transport failure | `abort` the handle, then **throw a locally-generated retry-later error** (e.g. `NETWORK_ERROR`) from `Fetcher` — never return `false`. The replication queue stores the exception and its `num_tries`/`last_exception_time_ms` backoff postpones the entry (`ReplicatedMergeTreeQueue.cpp:1614,2152`); on re-execution source and covering-part discovery are recomputed (`StorageReplicatedMergeTree.cpp:2636,5200`). NOTE: re-selection does not guarantee a *different* replica (shuffled scan may repick the sender) — a repeat confirm then fails fast again until the entry converges on another source or a covering part. |
| `MechanismFallbackAllowed` | relink cannot work here but the sender still has the part: protocol-version/cookie mismatch, manifest decode failure, receiver-side publish/`promote` ref conflict | today's same-sender byte re-request (`DataPartsExchange.cpp:731-738`; unknown-cookie fallback logs at `:741-747`, returns at `:748`) |

Notes: (a) there is **no** `ABORTED` backoff exemption in the GET_PART queue path — any stored
exception engages the postpone logic; `ABORTED` is only logged specially for fetches (generic
catch at `StorageReplicatedMergeTree.cpp:4222`, the special logging at `:4229`). (b) "promote
aborted on a condemned blob" is NOT a real mechanism-failure class — promotion deliberately does
not probe tokenless adopted leaves (`CasPartWriteTxn.cpp:1072-1106`). (c) Manual
`SYSTEM FETCH PART` / `FETCH PARTITION` callers swallow several REMOTE transport codes
(`StorageReplicatedMergeTree.cpp:8128`) — the `SourceProofFailed` error is locally generated
precisely so it surfaces instead of being swallowed.

## Confirm wire protocol {#wire-protocol}

- A new action on the existing interserver endpoint (today it has one part-stream operation,
  protocol version negotiated at `DataPartsExchange.cpp:91,170`): `confirm_ca_ref`, carrying the
  opaque exact source token. Issued as a second HTTP request after the relink response is fully
  consumed (`assertEOF` at `:762`). Dispatch happens BEFORE the handler resets the body or
  requires/parses the `part` parameter (`:170`) — the confirm has no part argument.
- Routing: by `{pool_uuid, server_root_id}` from the token to the exact CA disk/exchange instance
  that made the offer — NOT via the `DataPart` (which may already be gone). The narrow
  `IContentAddressedExchange` interface (`ContentAddressedExchange.h:35`) gains `ownsNamespace`
  and `confirmExactRef`; ambiguous matches and namespaces outside the identified root are
  rejected (answer *unknown*).
- Response: authenticated `yes` / `no` / `unknown` (interserver authentication happens before
  endpoint dispatch — `InterserverIOHTTPHandler.cpp:29`); transport errors and timeouts map to
  `SourceProofFailed` on the receiver.
- Version negotiation: relink advertisement gains a confirm-capable protocol version. A new
  sender relinks only for confirm-capable receivers — otherwise it streams bytes. A new receiver
  seeing an old/unknown relink cookie falls back exactly as today. Mixed-version windows degrade
  to bytes, never to the unconfirmed relink with the known gap.

## B66b: relink-into-detached {#b66b}

Corrected scope after review rounds 2–3:

- **Dedicated `allow_ca_relink` mode, with the recursion brake.** Lifting `!to_detached` alone is
  ineffective — both manual detached callers pass `try_fetch_shared=false`
  (`StorageReplicatedMergeTree.cpp:8125,8281`), which already makes `try_zero_copy` false. But
  simply decoupling relink from `try_zero_copy` would REMOVE the existing recursion brake: today
  the same-sender byte fallback recursively calls `fetchSelectedPart` with `try_zero_copy=false`
  (`DataPartsExchange.cpp:731`), which is precisely what stops the fallback request from
  re-advertising relink (comment at `:534-545`) — without a brake, a persistent mechanism failure
  loops forever. So: relink capability becomes its own `allow_ca_relink` flag — manual detached
  fetches set it true independently of `try_fetch_shared`; EVERY same-sender byte fallback sets
  it false; legacy `try_zero_copy` stays untouched for real zero-copy (separate capability path,
  `DataPartsExchange.cpp:566`; CA disks correctly report zero-copy unsupported).
- `relinkPartToDisk` gains a `to_detached` flag: today it hardcodes the active table parent
  (`DataPartsExchange.cpp:1107-1128`); for detached it constructs the temporary storage under the
  detached parent. CA routing already folds any `detached/<name>` ref
  (`ContentAddressedMetadataStorage.cpp:1241`).
- Detached finalization keeps its existing contract — `renameTo(detached/<part>, true)`
  (branch at `StorageReplicatedMergeTree.cpp:5715`, the rename itself at `:5719`), NOT
  `renameTempPartAndReplace` — including its existing collision behavior (racy pre-check +
  replace, `:8115`, `DataPartStorageOnDiskBase.cpp:795`). No collision-semantics change on either
  path.
- The confirm step is identical. Cross-pool `FETCH PARTITION` keeps streaming bytes.

This closes the RPL-4 perf cliff (same-pool `FETCH PART/PARTITION` and `to_detached` replication
fetches stream full bytes today).

## Scope and non-goals {#scope}

- **Zero changes** to the GC fold, journal codec, snapshot format, sweeps, orphan protection, or
  the anomaly/suppression semantics. No new journal states. No foreign writers. Every write stays
  in its owner's domain. New sender-side surface: the read-only confirm primitive, the
  apply-pending poison marker (also the fail-closed half of the pre-existing
  "post-durable-PUT allocation window" BACKLOG item), and the `precommitAdd` mint-tightening.
- The ordinary write path (INSERT/merge) is untouched (the mint-tightening only rejects a
  transition no production path performs).
- B66a — `BACKLOG.md` §14 (local-backend atomicity, orthogonal).
- The ordinary-`Precommit` unmatched-`-1` interleaving was **verified harmless** (source-edge set;
  BACKLOG §3 `[UNMATCHED-MINUS-ONE]` — only the pinning test remains).
- Bulk write-replica warm-up (future): confirm generalizes trivially — one batched exact-token
  confirm for N parts after N publishes; nothing here precludes it.

## Testing {#testing}

- **Confirm-primitive determinism** (sender-side gtests):
  (a) failpoint between the lane's committed PUT and `applyRefLogTxn` success (the
  apply-exception window) → poison armed → confirm answers *unknown*, never *yes*;
  (b) durable-but-wedged drop (the `gtest_cas_ref_writer.cpp:778` shape) → *unknown*;
  (c) repointed ref (same name, different `ManifestRef`) and dropped+recreated ref → *no*;
  (d) mint-tightening regression: a transaction attempting `precommitAdd` of an exact old unowned
  `ManifestId` it did not stage → rejected (replaces the legality pinned at
  `gtest_cas_promote_republish.cpp:283`);
  (e) cold/evicted/recovering table → *unknown* with zero backend requests;
  (f) pending/in-flight append or wedge → *unknown*; live committed exact match on a quiescent
  lane → *yes*;
  (g) lock-protocol race test: an append admitted concurrently with the confirm snapshot is
  ordered strictly after it (no torn read across `ref_queue_mutex`/`state_mutex`).
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
- **Abort-discipline tests:** kill/except every non-promote exit of the `PreparedRelink` handle
  and assert the precommit removal is appended (no same-epoch leak). Force the abort UNCERTAIN →
  assert the lane wedges, a later append resolves the landed PUT (applied-before-unwedge, binding
  removed), and the never-landing variant keeps the lane wedged with retry-later errors until
  remount cleans up — matching the documented pre-existing semantics, no silent handle
  destruction.
- **Recursion-brake test:** persistent mechanism failure (e.g. forced decode failure) → exactly
  one relink attempt, then bytes; the fallback request must NOT re-advertise relink
  (`allow_ca_relink=false`), no loop.
- **B66b:** same-pool `FETCH PART` into `detached/` (both manual callers) → relink proof +
  subsequent `ATTACH` reads correctly; collision behavior byte-identical to the byte path;
  cross-pool → bytes; capability independent of `try_fetch_shared=false`.
- **RPL-5 slice (pulled in):** `REPLACE PARTITION`/`ATTACH PARTITION ... FROM` on a 2-replica CA
  table (extend `test_cas_replicated_relink`): assert the queue-cloned `REPLACE_RANGE` fetch
  relinks (blob-count proof), not a silent byte re-fetch.
- **Version-mix test:** confirm-capable receiver × legacy sender cookie → clean byte fallback.
- **TLA mini-model:** two journals + round fold with cursors + three-phase graduation + spare;
  property: *if the exact-token confirm observed the committed binding at T2 under a quiescent,
  poison-clear lane and the receiver's activation precedes T2, no blob of the receiver's manifest
  is deleted before the receiver's own removal event*. Model the straddle interleaving, the
  wedge-hidden durable drop (unknown), the apply-exception window (unknown via poison), the ABA
  attempt (rejected by mint-tightening), and the false-404 branch (existing clamp).

## Naming {#naming}

"Publish-then-confirm relink". The confirm primitive: `confirmExactRef` (read-only, exact-token,
lane-linearized, poison-aware). No new journal vocabulary — deliberately.
