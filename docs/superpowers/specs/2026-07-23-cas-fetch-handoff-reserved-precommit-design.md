---
description: 'Design for closing the CAS fetch-by-relink commit-before-release gap (codex-6, #42/#43) with a reserved precommit: the receiver reserves the manifest identity in its own journal before the fetch, the sender materializes the manifest body at the reserved id before releasing the part, and the GC fold treats the pending reservation as a normal (non-anomalous, non-suppressing) state. Supersedes the 2026-07-15 retention-pin design. Also covers B66b relink-into-detached and pulls in the RPL-5 REPLACE PARTITION queue-clone test slice.'
sidebar_label: 'CAS Fetch-Handoff Reserved Precommit'
sidebar_position: 20260723
slug: /superpowers/specs/cas-fetch-handoff-reserved-precommit-design
title: 'CAS fetch-handoff — reserved precommit (commit-before-release without pins)'
doc_type: 'reference'
---

# CAS fetch-handoff — reserved precommit {#cas-fetch-handoff-reserved-precommit}

**Date:** 2026-07-23
**Branch:** `cas-gc-rebuild`
**Status:** design (user-approved brainstorm 2026-07-23; supersedes
[`2026-07-15-cas-fetch-handoff-retention-pin-design.md`](2026-07-15-cas-fetch-handoff-retention-pin-design.md) —
banner added there)
**Covers:** codex-6 / #42/#43 (relink commit-before-release gap), B66b (relink-into-detached, RPL-4
perf cliff), RPL-5 test slice (`REPLACE PARTITION` queue-clone relink proof).
**Out of scope:** B66a (local-backend torn read — `BACKLOG.md` §14 {#local-backend}), bulk
write-replica warm-up (future extension), any change to the ordinary `Precommit` semantics.

## Problem {#problem}

Same-pool replication uses **fetch-by-relink**: the sender transfers only the part's manifest, the
receiver publishes its own ref over the already-shared blobs (zero byte cost). The correctness hole
(codex-6, self-documented at `ContentAddressedMetadataStorage.cpp:1990`):

- The relink **sender is fire-and-forget**: `Service::processQuery` streams the manifest bytes and
  releases its `DataPartPtr` when the response completes — **before** the receiver's publish.
- The receiver then runs a normal local build (`publishEntries`: `stageManifest` → `precommitAdd` →
  `promote`). If its edge-PUT stalls across ≥ 2 GC folds while the sender's now-`Outdated` part is
  concurrently collected and the blob has no other ref, the blob is condemned and deleted; the
  receiver later commits a manifest whose blobs are gone — a **dangling committed manifest**
  (fsck-detected, not silent). Only the same-token tail remains (`deleteExact` covers every
  token-CHANGE recovery), but it is real.

The 2026-07-15 answer was a GC **retention pin** (a pool-global overlay object). It is superseded
because it adds a second reachability source to GC (union overlay + removal-deferral — a genuine
fold-semantics extension), a multi-party object protocol, and owner-liveness machinery. The design
below achieves the same seal with **event ordering alone**, reusing the journal, the fold barrier,
the dead-build escape, and the abandoned-precommit cleanup that already exist.

## Design constraints (user-set) {#constraints}

1. **Minimal change to working code.** No new GC information source, no overlay, no pins, no janitor.
2. **GC must never pause for a healthy handoff.** The "reservation pending" state must be a **normal
   event** for the fold — not an anomaly, not a round-suppression trigger. (The existing barrier
   clamps + sets `suppress_destructive` for the whole round via `!report.anomalies.empty()`,
   `CasGc.cpp:1360` — routine relinks must never hit that hammer.)
3. The latent bug must be **gone**, not narrowed: no interleaving of receiver stalls/freezes with
   sender part collection may produce a dangling committed manifest.

## Core idea {#core-idea}

Two ordering changes, one new precommit form:

1. **The receiver reserves first.** Before contacting the sender it mints the manifest identity
   (manifests are identity-addressed — `ManifestId` = `{root_namespace, ManifestRef{writer_epoch,
   build_sequence, ordinal}}` — so the id and its object key are known before any bytes exist) and
   appends a **reserved precommit** (`RefOwnerKind::Reserved`) to its **own** ref journal. Single-writer
   discipline is untouched: nobody ever writes another server's journal.
2. **The sender materializes the body before releasing.** The fetch request carries the reserved
   `ManifestId`. The sender — still holding the `DataPartPtr` — re-encodes its manifest under the
   receiver's identity and writes it to the reserved key with a conditional PUT, then replies. The
   part is released only after the body is durable.

The protection chain has no gap, by arithmetic rather than by suppression:

```
[sender-ref +1 (long-standing)] → [body durable at reserved id] → [sender releases]
→ [sender part dropped → journal −1] → [any fold that sees the −1 can fold the reserved +1]
```

The sender's `−1` cannot exist before the body is durable, so no fold can ever apply the `−1`
without the receiver's `+1` being foldable. Blob in-degree never touches zero across the handoff.

## The `Reserved` precommit form {#reserved-form}

- New value in the existing `RefOwnerKind` enum (the field already rides every journal edge record;
  the fold already dispatches on it — `CasGc.cpp:1132`). Journal codec gains the value; pre-release,
  no compatibility scaffolding ([[feedback_ca_no_compat_scaffolding_predev]]).
- Semantics: *"this identity is reserved by my build; the body will be materialized by a third
  party."* Everything else is ordinary precommit: same unique-ref rules, same removal event on
  abort, same abandoned-precommit cleanup (mount-time sweep, destructor `abandon`), same
  `promote` owner-move as the terminal success (`promote` accepts a `Reserved` binding as the
  promotable prior state).
- Writer API: a txn-level `reserveManifest(ns, final_ref_name) → ManifestId` (mints the ordinal and
  appends the `Reserved` binding); `promote` unchanged in shape.

## Fold semantics: pending reservation is a normal event {#fold-semantics}

For a `+1` edge with `owner_kind == Reserved` whose body GET returns 404:

- **Quiet per-table hold**: the table's fold cursor does not advance past the event; the log is
  re-read next round (exactly the existing barrier mechanics) — **but no anomaly is recorded and no
  round-level `suppress_destructive` is triggered.** Pool-wide GC keeps condemning and deleting
  normally; only this table's fold waits.
- Observability: a dedicated ProfileEvent (e.g. `CasGcReservedPending`) and a gc-round-log counter
  (`reserved_pending`) — green telemetry, not red.
- Terminal outcomes (all existing machinery):
  - body appears → ordinary activation (`+1` edges fold);
  - removal appears (receiver abort) → "removed precommit that never activated": skip, no clamp
    (`CasGc.cpp:1132-1133`);
  - the receiver's build is provably dead (watermark floor / fenced epoch, `prefixEligible`) →
    dead-build skip (`CasGcDeadPrecommitSkipped` path); an already-materialized body becomes an
    orphan manifest and is reclaimed by the orphan sweep (its active set = committed +
    live-precommit view — a dead/removed reservation drops out).
- **Orphan-sweep requirement (implementation-must):** the orphan-manifest sweep's active set MUST
  include **live `Reserved` bindings** exactly like live precommits — otherwise the sweep would
  reclaim a just-materialized body in the healthy window between the sender's PUT and the
  receiver's promote. (Dead/removed reservations drop out as above — that is the intended
  reclamation path, gated on the same watermark facts the dead-build skip uses.)
- **Promote-peek rule (keeps the hammer for real anomalies):** if the visible log suffix already
  contains a later owner binding for the same manifest id (the receiver promoted), a 404 on the body
  is **provably a store lie** — `promote` required the body — and is treated exactly like today's
  committed-ref-missing-body case: anomaly + clamp + suppression. The quiet treatment applies only
  while the reservation is genuinely pending (the `Reserved` binding is the newest for that ref).
- The ordinary `Precommit` branch is untouched: absent-body there still means a store lie (writers
  stage the body before `precommitAdd`), still an anomaly. The separation is clean because the two
  states are different enum values, not a heuristic.

## Fetch protocol {#protocol}

Receiver (in `Fetcher`, before the HTTP request):

1. Compute the final ref name exactly as the byte path would — `<part_name>` for an active fetch,
   `detached/<final_name>` for `to_detached` (same collision handling as today).
2. Open the part-write txn, `reserveManifest(ns, ref_name)` → `ManifestId` (durable journal append).
3. Send the fetch request; it additionally carries the reserved `ManifestId` (namespace + ref triple
   — the sender cannot derive the receiver's namespace: table UUIDs differ between replicas outside
   `Replicated` databases).

Sender (`Service::processQuery`, inside the existing CA relink branch, still holding the part):

4. Decode its own manifest, substitute the receiver's `ref`/`root_namespace_id`, recompute
   `payload_digest`, encode (`refMatchesBody`/`manifestNamespaceMatches`/digest validation make a raw
   `CopyObject` impossible by design — the identity fields are digest-protected). Entries are copied
   verbatim.
5. Conditional PUT (`If-None-Match`) at the reserved key. On success reply: relink cookie + the
   manifest bytes (so the receiver needs no extra GET). Then return → part released.

Receiver (on response):

6. Decode the returned bytes, sanity-check the identity fields against the reservation, build the
   local part object over the entries (as today), `promote`.
7. Any failure at any step → abort the reservation (ordinary precommit-removal append) → fall back
   to byte fetch (existing path, unchanged semantics).

Retries mint a **fresh ordinal** (a new `ManifestId`): no key collisions, no idempotency puzzles;
an abandoned reservation's body (if the PUT landed) is orphan-swept after the removal/dead-build
resolution. Concurrent same-part fetches on one receiver are already serialized by MergeTree
(`currently_fetching_parts`).

Cost per relink is unchanged vs today: the manifest body PUT moves from the receiver to the sender
(one hop shorter for the bytes), one journal append is re-ordered, the response gains nothing heavy.
No pin PUT/DELETE, no extra GET.

## B66b: relink-into-detached {#b66b}

The sender is **target-name-agnostic** — the manifest body carries `ManifestRef` + namespace, never
the ref name; the name lives only in the receiver's journal binding. So detached support is a
receiver-side naming decision:

- Lift the `!to_detached` gate in the relink advertisement (`DataPartsExchange.cpp:540-545`).
- The receiver reserves `detached/<final_name>` (the `detached/` fold, B181, already routes and
  folds these refs; GC semantics identical — a detached ref is an ordinary committed ref).
- `relinkPartToDisk` learns the detached target: publish at the final detached name (no tmp-rename —
  no bytes move), return a part object rooted at `detached/<name>`, no active-set commit.
- Name collision (`detached/<name>` already committed) → `promote` hits the unique-ref invariant →
  abort the reservation → byte-fetch fallback, which fails with the same "already exists" the user
  sees today. Behavior is unchanged from the user's point of view.
- The same-pool advertisement check is untouched: cross-pool `FETCH PARTITION` keeps streaming bytes.

This closes the RPL-4 perf cliff (`FETCH PART/PARTITION` and any `to_detached` replication fetch
stream full bytes today even when every blob is already in the shared pool).

## Correctness argument {#correctness}

- **Zero-gap:** body durable strictly before sender release; sender's `−1` strictly after release;
  a fold snapshot that contains the `−1` therefore always finds the body readable → the reserved
  `+1` folds in the same round → in-degree ≥ 1 throughout. No reliance on `suppress_destructive`.
- **Receiver freeze/crash at any point:**
  - before the reservation → nothing happened;
  - after the reservation, before the request → body never written; dead-build skip ends the hold;
    nothing referenced, nothing lost;
  - after the request → body durable regardless of receiver state; blobs stay protected while the
    build is live; if the build dies (lease/epoch fenced) the reservation is skipped, the body is
    orphan-swept, and — crucially — the dead receiver **cannot promote** (fence-gated publish), so a
    dangling committed ref is unreachable by construction; the thawed receiver retries with a fresh
    build.
- **Sender crash mid-PUT:** the receiver times out, aborts the reservation, retries/byte-fetches.
  A conditional PUT is atomic: the key holds either nothing or the complete body.
- Bonus: today's narrow race "late `+1` folds into an already-`delete_pending` blob" (the
  `structurally impossible` guard, `CasGc.cpp:484-486`) becomes genuinely impossible for relink —
  the `+1` is always foldable before the last `−1`, so the blob never graduates while a live
  handoff references it.

### Accepted residual (documented, out of trust model) {#residual}

A **selectively persistent false-404** of the receiver's manifest body — durable object unreadable
at ≥ 3 consecutive round folds (condemn, graduation, delete) — combined with the sender's ref
disappearing inside that same window, while the same store correctly serves the sender's manifest
body for the `−1` fold, could delete blobs under a pending reservation without tripping any
anomaly. Real S3's read-after-write consistency excludes this; the rustfs false-404 class
(rustfs#3231) is transient-under-load and already a release-gate blocker (F2). The promote-peek rule
covers the post-promote window (there a 404 IS an anomaly). Recorded here as the accepted residual;
no code carries it silently — the TLA model (below) pins the assumption.

## Scope and non-goals {#scope}

- Ordinary `Precommit` semantics, the INSERT/merge write path, fold anomaly semantics for
  committed/promoted refs: **untouched**.
- No GC overlay, no pins, no new GC information source; the 2026-07-15 pin spec is superseded (its
  shared "retention-overlay primitive" remains referenced only by the read-replica design, which
  carries its own justification).
- B66a (local-backend torn read) — `BACKLOG.md` §14; orthogonal (local-backend atomicity, S3
  unaffected).
- Bulk write-replica warm-up — future extension: a warm-up batch is N reservations + one request
  asking the sender to materialize N bodies; nothing in v1 precludes it.
- `checkOpAdmitted`/read-only/fence gates apply to the reservation append and promote exactly as to
  today's precommit/promote — no new admission surface.

## Testing {#testing}

- **Fold gtests:** pending reservation → quiet hold, cursor held, `reserved_pending` counted, NO
  anomaly, destructive phases run for other tables; reservation + removal → skip; reservation +
  dead build → skip + body orphan-swept; reservation followed by promote with body 404 → anomaly
  (promote-peek rule); ordinary `Precommit` absent-body → anomaly (unchanged).
- **codex-6 regression:** failpoint/SIGSTOP the receiver between reservation and promote while the
  sender's part is merged away and GC runs to fixpoint → blobs survive, zero suppressed rounds,
  fsck clean; on resume the receiver either promotes (blobs intact) or cleanly retries.
- **Handoff integration** (extend `test_cas_replicated_relink`, 2-replica rustfs fixture):
  - plain INSERT/merge fetch relinks (existing) still green, now over the reserve protocol —
    blob-count proof (`CasBlobPut == 0` on the receiver);
  - **B66b:** `FETCH PART` into `detached/` on the same pool → relink proof + subsequent `ATTACH`
    serves correct data; name collision → clean byte-fetch fallback error; cross-pool fetch →
    byte path unchanged;
  - **RPL-5 slice (pulled in):** `REPLACE PARTITION`/`ATTACH PARTITION ... FROM` on a 2-replica CA
    table; assert the queue-cloned `REPLACE_RANGE` fetch on the second replica takes the relink
    branch (blob-count proof), not a silent byte re-fetch.
- **Chaos scenario card (s13-class):** kill/freeze the receiver mid-fetch under load → pool GC
  keeps reclaiming (assert nonzero deletes during the freeze window), no dangle after heal.
- **TLA extension** of the fold model: reserve / body-PUT / release / `−1` / fold interleavings;
  the dead-build skip; the promote-peek rule; the false-404 residual expressed as an explicit
  fairness assumption on body reads (so the accepted residual is a named model assumption, not an
  accident).

## Naming {#naming}

`Reserved` precommit — «бронь манифеста»: enum `RefOwnerKind::Reserved`, writer verb
`reserveManifest`, journal/event wording "reserved precommit awaiting body". Rejected names:
"dirty precommit" (pejorative, says nothing), `Expected` (passive), `Inbound` (ties the concept to
fetch; reservations generalize to warm-up/backup pulls).

## Relationship to superseded work {#supersedes}

- [`2026-07-15-cas-fetch-handoff-retention-pin-design.md`](2026-07-15-cas-fetch-handoff-retention-pin-design.md)
  — superseded by this spec (banner added). Its transport-impossibility analysis (half-duplex — no
  sender-side ACK of receiver commit) remains valid and is inherited: this design needs no reverse
  channel because the confirmation the receiver needs (body durable) rides the response of the
  request the receiver itself initiated.
- The read-replica snapshot-pin design keeps its own §5 overlay primitive discussion; nothing here
  depends on it.
