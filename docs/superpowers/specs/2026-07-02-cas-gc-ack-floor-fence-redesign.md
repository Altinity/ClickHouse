# CAS GC ack-floor fence redesign — design

**Status:** DESIGN (2026-07-02). **Branch:** `cas-gc-ack-floor-fence`.
**Replaces:** the per-round all-shard fence (R3) and the fold-through-fence recheck (R4) of the GC
round protocol, and the separate per-server watermark object.
**Goal:** a GC round costs **O(delta) + O(servers)** S3 requests. No O(universe) GET/PUT phases.
(The one remaining universe-proportional operation is the LIST sweep — O(universe/1000) LIST
requests — which discovery already performs today and which this design reuses as the round's
single sweep.)

## Problem

Every regular GC round today runs two O(universe) phases over the root-shard universe
(`cas/refs/<ns>/<shard>`, one object per (namespace × `root_shards`); namespaces are per-table):

1. **Fence (R3):** `mutateShard` on EVERY present root shard — bump `fence_round`, record
   `fence_version` — one GET + one CAS-PUT per shard, unconditionally, even for shards nothing
   touched.
2. **Recheck (R4):** `readShard` on EVERY fenced shard to fold the `(folded_cursor, fence_version]`
   window, plus a per-candidate `inDegreeInGeneration` that re-reads and re-materializes the target
   shard's run for each retired entry.

At 100 000 tables × 8 root shards this is ~800 000 CAS-PUTs and ~1 600 000 GETs per round
(≈ $4.6 and tens of minutes of wall time per round, repeated every round forever). The fence is
all-shard *by design* in the current protocol: the per-shard `fence_round` bump is the only thing
that forces a writer with a stale retire-view to refresh before publishing, and TLA+
(`CaGcRootLocalPartManifestCore`, `SabotageLazyFenceUnsafe`, `SabotageNoFence`) proves that fencing
only changed shards or reusing a stale fence position is unsound *within that mechanism*. So the
mechanism itself is replaced.

## Design summary

Replace the per-shard write fence with a **causal acknowledgement floor**:

- Each server maintains **one merged heartbeat object** (mount lease ∪ build watermark) carrying
  `observed_gc_round` — the newest GC round whose retired list the server has fully loaded.
- The **retired list** becomes a single current sorted artifact (per gc-shard runs, keyed by
  `blob_hash`), not per-round sets. An entry is `(blob_hash, token, condemn_round)`.
- A GC round is **one pass**: a three-cursor merge over (prior snapshot run, sorted incoming edge
  deltas, prior retired run) that simultaneously *verifies* old candidates, *graduates* the safe
  ones to deletion, and *condemns* new ones.
- An entry graduates (is actually deleted) only when `condemn_round < min_ack`, where
  `min_ack = min(observed_gc_round)` over all **live** heartbeats. Rounds never block on
  acknowledgements — entries simply wait in the retired list until the floor passes them.
- Writers, at commit, check every blob leaf against their in-memory copy of the retired list:
  a listed `(hash, token)` is **never referenced** — the writer *recreates* the blob from its own
  build source via a token-conditional overwrite (fresh incarnation, fresh token).
- Discovery stays LIST + ETag-diff (as the fold does today). One LIST sweep per round.

Safety needs no clocks on the happy path: deletion is gated purely by causality (every live writer
has demonstrably loaded the entry; every excluded writer demonstrably cannot write). Wall-clock
enters only where it already does today — declaring a crashed server's lease expired — and that
contract is inherited from the mount machinery unchanged, hardened by a fence-out write.

## Object model

### Merged server heartbeat (replaces mount object + watermark object)

Key: `gc/server-roots/<srid>/mount` (unchanged — S13 claim/recovery protocol addresses it).
The separate `roots/<server-hex>/_watermark` object and `WatermarkKeeper` are **removed**; their
fields move here. One `SingleWriterSlot` subclass, one background thread, one PUT per beat.

Body (existing mount fields plus the watermark's plus the ack):

```
server_uuid, writer_epoch, hostname, pid, started_at_ms, seq, expires_at_ms   -- as today (S13)
min_active_build_seq   -- watermark's orphan-sweep floor ("retired" sentinel supported)
observed_gc_round      -- NEW: the ack; monotone, never decreases
gc_fenced              -- NEW: set only by GC fence-out (see below); terminal for this incarnation
```

(Amendment 2026-07-02, Task 6: the separate `process_epoch` field is dropped — the writable path already
sets `process_epoch = writer_epoch` ("THE BRIDGE" in `CasStore.cpp`), so the merged body's `writer_epoch`
serves both the mount and the orphan-sweep epoch; no distinct field is needed.)

Beat cadence: one period for everything (proposal: 3–5 s — satisfies the watermark's ~2 s-class
freshness need, the mount TTL/3 rule for `mount_lease_ttl_ms` = 30 s, and bounds ack latency).
Net writer S3 delta per beat: **−1 PUT** (two heartbeats become one), **+1 GET** (`gc/state`).

### Retired list

Per gc-shard objects encoded with the existing strict `cas_retired_set` codec (proto `RetiredSetProto`,
magic CART), extended with a per-entry `condemn_round`; entries are sorted by `(kind, hash)` so the
bytes are byte-deterministic. Written by the GC pass as write-once artifacts under attempt-scoped keys
(same regime as snapshot runs), referenced from `gc/state.retired_refs` (gc-shard → object key). What
matters is the determinism and the publish-order property (the refs and the round land in the same
`gc/state` CAS); a dedicated run format (`RunFile` / `RunKind::Retired`) is unnecessary at
candidate-set sizes. This is the **current** outstanding-candidate set — small (candidates only), cheap
for writers to GET whole.

(Amendment 2026-07-02, Task 6: recorded during implementation. The codec is protobuf, not JSON as an
older header comment implied, and `retired_refs`/`condemn_round` are ADDITIVE proto fields — same regime
as `snap_attempt` — with no per-object version integer to bump; the only functional version guard is the
`compatibility_version` fail-closed-on-future check, and mixed-version pools are unsupported pre-release.)

Replaces: per-round retired sets (`retiredKey(...)`) and the multi-round `RetireView` fetch
protocol. `RetireView` now loads the single current list; `isCondemnedToken(hash, token)` becomes a
lookup in it.

### gc/state

- **Removed:** `fence_version` (the whole per-round per-shard map).
- **Added:** current retired-list run refs (per gc-shard key + checksum).
- One CAS per pass (round advance + generation adoption + retired refs + coverage), instead of
  today's ~4 (fold-adopt, retire, fence, seal).

**Publish order invariant:** within a pass, the retired-list runs (and snapshot runs) are durable
**before** the `gc/state` CAS that publishes `round := K`. A reader that observes round K can
always load retired list version K. (This subsumes the old retire-visibility barrier /
`ViewableRound`.)

### Root shards

`fence_round` is no longer bumped per round. It keeps exactly one role: the **birth floor**
stamped at shard creation (THM-NO-RETURN newborn ordering) — kept as defense in depth, zero
recurring cost. The promote-path trigger `retireView().round() < root.fence_round → refresh()`
stays and now fires only on newborn shards.

## Writer protocol

State per mounted pool: in-memory retire view (`view_round`, retired-list map), the local write
fence (`mayMutate`, existing), a view rwlock for the drain.

**Beat** (unified keeper, every `T_renew`):

1. `GET gc/state`. On failure: skip steps 2–3 (ack does not advance), still attempt step 4
   (lease renewal is availability-critical; a stale ack only stalls deletions globally — safe).
2. If `gc/state.round` > `view_round`: `GET` the current retired-list runs (per gc-shard).
   Any failure → ack does not advance this beat.
3. **Drain and swap:** take the view lock exclusive — this waits until every in-flight
   `mutateShard` call that started under the old view has fully completed (its CAS response
   received; mutations hold the lock shared for their entire call) — then install the new view.
4. `putOverwrite` heartbeat: lease extension + `min_active_build_seq` + `observed_gc_round =
   view_round`. Token-guarded as today; `PreconditionFailed` ⇒ foreign touch ⇒ `tripMountLost`
   (fail-closed, existing semantics — a GC fence-out lands here).

**Monotone-ack invariant:** `observed_gc_round` never decreases, and is never advertised above a
view that is actually installed and drained.

**Open ordering** (writable open): claim heartbeat (existing S13 claim protocol, `gc_fenced`
incarnations are simply superseded by the epoch bump) → load `gc/state` + retired list → stamp
`observed_gc_round` (immediate beat) → only then enable mutations. This closes the
new-mount-during-pass race: a mount the GC's enumeration missed necessarily finished its creation
PUT after the pass's round was already durable, so its first loaded view ≥ that round.

**Commit gate** (`promote` / direct committed publish; per blob leaf, inside `mutateShard` as
today):

- `HEAD` the blob (as today).
- **Absent** → re-upload from this build's own source (`putIfAbsent`, fresh token) — INV-1 path,
  as today.
- **Present, `(hash, current_token)` in the retired list** → **recreate**: `putOverwrite(blob_key,
  build-local bytes, expected = current_token)` → reference the fresh incarnation.
  `PreconditionFailed` ⇒ someone else recreated or GC deleted concurrently ⇒ re-HEAD and repeat.
  A retired token itself is **never referenced** (resurrect invariant: revival is a fresh
  re-upload from source only). This resolves the condemned-adoption gap (a plain `putIfAbsent` would
  adopt the condemned incarnation).

  (Amendment 2026-07-02, Task 6: recreation is performed by the EXISTING `Build::putBlob` cold-reuse
  rule — condemned ⇒ `uploadFromSource`, token-conditional — on the *retried* build, not a new gate
  code path. The promote gate itself stays fail-closed `ABORTED` because build-local sources are not
  retained at promote time; a promote-time in-place recreate is a possible follow-up.)
- **Present, not retired** → use (as today).

The write path gains **zero** S3 operations in the common case; the recreate PUT happens only in
the rare condemned-overlap window.

## GC round protocol (one pass)

Round K, leader under the gc lease (lease/attempt-scoping/resume machinery unchanged):

1. **Heartbeats:** LIST `gc/server-roots/` + GET each (O(servers), single-digit counts).
   Classify each heartbeat:
   - *terminated stamp* (graceful shutdown) → excluded (its own final write is causal proof no
     further mutations exist);
   - *live* (`expires_at_ms + skew_margin > now`) → contributes its `observed_gc_round` to the
     floor;
   - *expired* (`now > expires_at_ms + skew_margin`, no terminated stamp) → **fence-out**: one
     token-guarded `putOverwrite` preserving the body, setting `gc_fenced`, bumping seq. Success ⇒
     the sleeper's next renewal permanently fails (`tripMountLost`; only a full re-open — with a
     fresh view — can resume writing) ⇒ excluded. `PreconditionFailed` ⇒ it renewed concurrently
     ⇒ re-GET and reclassify as live.
   `min_ack := min(observed_gc_round over live)`; no live heartbeats ⇒ `min_ack = +∞`.
   Fence-outs must complete before step 2 (their writers' last commits are then all durable before
   the sweep).
2. **Discovery:** one LIST sweep of `cas/refs/` (paginated; O(universe/1000) requests); ETag-diff
   against the per-shard folded tokens (as the fold does today; `supportsListTokens = false`
   backends degrade to per-shard HEAD/GET — fail-closed, reads only) → changed-shard set D.
3. **Windows:** GET the D changed shard bodies; per shard take the journal window
   `(folded_cursor, end]`; GET the window's added/removed manifests; produce the sorted edge-delta
   list (existing fold machinery, unchanged).
4. **Three-cursor merge** per gc-shard — extends the existing two-cursor
   `foldDeltasIntoGeneration` with a third cursor over the prior retired run (all three inputs
   sorted by `blob_hash`); one streaming pass emits the new snapshot run, the new retired run, and
   the delete list:
   - in-degree > 0 **and** retired → drop the entry (**spare**; emit a B170 recheck-verdict
     event with the recovered in-degree);
   - in-degree = 0 **and** retired with `condemn_round < min_ack` → **graduate**: append
     `(hash, token)` to the delete list, drop the entry from the retired output;
   - in-degree = 0 **and** retired with `condemn_round ≥ min_ack` → keep the entry unchanged
     (not yet provably seen by every live writer);
   - in-degree = 0 **and** not retired → **condemn**: HEAD the blob (capture its current token;
     absent ⇒ nothing to delete, skip), append `(hash, token, K)` to the retired output.
   Note `min_ack ≤ K−1` always (acks cannot exceed the last published round), so an entry
   condemned in this pass structurally cannot graduate in the same pass — the two-round pipeline
   falls out of the arithmetic, with no explicit rule.
5. **Deletes:** `deleteExact(hash, token)` per graduated entry. `Deleted` → done; `TokenMismatch`
   → a writer recreated it → done (the fresh incarnation is a live object; a future round
   re-condemns it if unreferenced); `NotFound` → already deleted (crash-resume replay) → done.
   B170 outcome events for each. Manifest cleanup (`mfCleanup`, delete-after-sealed-decrements)
   rides the same phase unchanged.
6. **Seal:** PUT per-shard coverage/cursor updates for D (O(delta)); the single `gc/state` CAS
   publishes `round := K`, the adopted generation, the retired-list refs, and folded tokens.

**Crash-resume:** unchanged regime — attempt-scoped write-once artifacts; a new leader (or the
same one) re-runs the pass under a fresh attempt; already-executed deletes land on the
`NotFound` branch; only the adopted attempt is reader-visible.

**Steady-state request budget per round:** 1 LIST sweep + O(servers) heartbeat GETs (+ rare
fence-out PUT) + O(D) shard GETs + O(window) manifest GETs + O(gc_shards) run GET/PUTs + O(new
candidates) HEADs + O(graduated) DELETEs + O(D) coverage PUTs + 1 CAS. **No O(universe) GET/PUT
phase exists.** For the 100 000 × 8 slowly-changing example: ~2 000–3 000 requests
(≈ $0.001–0.01) and seconds of wall time, versus ~2 400 000 requests (≈ $4.6) today.

## Safety argument

The protected invariant is INV_NO_DANGLE / INV_NO_RETURN: a graduated delete `(b, t, r)` executed
at pass K must not race a commit that references `b@t`.

Case split over the committing writer's view `V` at gate evaluation:

- **V ≥ r:** its retired list (version V ⊇ entries with `condemn_round ≤ V`) contains `(b, t, r)`
  → the gate recreates (fresh token) or re-uploads; `b@t` is never referenced.
- **V < r:** graduation required `r < min_ack`, and `min_ack` was computed over live heartbeats at
  pass K **after** fence-outs — so this writer is not live: it is either gracefully terminated,
  or fenced-out after lease expiry. All its commits completed before its exclusion, exclusion
  completed before the pass's LIST sweep, therefore its commits are visible to the sweep
  (strongly consistent LIST/GET: a PUT completed before a LIST page is reflected in it), its
  shard reads as changed, its `+1` edges fold into this very merge → in-degree > 0 → the entry is
  **spared**, and no delete is issued. Contradiction with graduation.
- **In-flight commits during an ack advance:** the drain makes the ack-advertise happen strictly
  after every old-view commit's CAS response, so a heartbeat carrying `observed_gc_round = A`
  proves no commit with view < A is still in flight from that server.
- **New mounts during the pass:** open ordering (claim → load ≥ current durable round → ack →
  write). If the pass's heartbeat enumeration saw the mount, its ack participates in the floor;
  if pagination missed it, its creation PUT postdates the round-(K−1) publish, so its first view
  ≥ K−1 ≥ every graduating entry's `condemn_round` (graduation needs `r < min_ack ≤ K−1`), and
  its retired list contains every graduating entry.
- **Sleepers:** a crashed-but-not-dead process cannot resume writing: its local write-fence
  deadline (`mayMutate`, monotonic clock) lapses within the lease TTL (existing S13 contract, the
  single inherited timing assumption, margin-padded on the GC side), and the fence-out makes any
  later renewal fail permanently, so the fence cannot re-arm. Hardening note: `mayMutate`'s
  deadline uses `steady_clock` (`CLOCK_MONOTONIC`), which does not advance across a VM suspend —
  switch the fence deadline to a `CLOCK_BOOTTIME`-based clock as part of this work (container
  pause is already safe: the monotonic clock keeps ticking).
- **Stale writer recreating a spared blob:** harmless — a token-conditional overwrite of identical
  content; readers address by hash; no pending delete targets the new token.
- **Same-pass graduation:** impossible (`min_ack ≤ K−1 <` this pass's condemn round K).

Everything else (owner/epoch mount exclusivity, attempt scoping, deterministic artifacts,
manifest sweep eligibility via `process_epoch`/`min_active`) is inherited unchanged.

## What this removes or simplifies

- **Fence phase (R3)** — gone entirely (O(universe) GET+CAS-PUT per round → O(servers) GETs).
- **Recheck phase (R4)** — merged into the fold pass; the per-candidate `inDegreeInGeneration`
  whole-run re-reads (the quadratic hot spot that started this investigation) disappear
  structurally: the retired cursor rides the same streaming merge.
- **`fence_version`** in `gc/state`; **`PubFloor`**-style per-shard floors; per-round
  **`fence_round`** bumps (birth floor stays).
- **Per-round retired sets** and the multi-round `RetireView` fetch → one current sorted retired
  list.
- **The watermark object and `WatermarkKeeper`** → merged heartbeat (one slot, one thread,
  one PUT per beat; GC's heartbeat enumeration now also serves the orphan-sweep floors).
- **`ViewableRound` / retire-visibility barrier** → the publish-order invariant (retired runs
  durable before the round CAS).
- ~4 `gc/state` CASes per round → 1.

Deliberately **kept**: LIST + ETag-diff discovery (no write-path dirty declarations — no new PUT
on the writer's hot path); newborn-shard birth floor; B170 event coverage; the existing fold,
attempt-scoping, and manifest-cleanup machinery.

## Costs and trade-offs (stated honestly)

- **Deletion latency:** condemnation → deletion takes ≥ 1 full pass plus the ack latency
  (~1 beat). A live-but-stale-acking server stalls **all** graduations (the floor is a min) —
  this is a liveness property only, made observable (see below), and bounded for dead servers by
  lease expiry + fence-out.
- **gc/state dependency:** writers now GET `gc/state` every beat. Its unavailability stalls ack
  advance (deletes stall globally — safe) but does not take writers down (lease renewal is
  independent).
- **Snapshot rewrite bytes:** the pass still rewrites the full snapshot run per gc-shard —
  O(active edges) **bytes** (not requests) per round. With rounds now cheap and frequent this
  becomes the next dominant cost; the follow-up is delta-runs + periodic compaction, which is
  exactly the deferred O(buffer) streaming-merge work
  (`docs/superpowers/deferred_backlog/2026-07-01-cas-gc-runfile-obuffer-streaming.md`). Out of
  scope here.
- **One inherited timing assumption** (unchanged from today's mount exclusivity): a writer whose
  lease is expired by GC's clock + margin has already stopped mutating via its local fence.

## Observability

- Heartbeat ack lag: per-server `observed_gc_round` vs current round — ProfileEvents counter +
  a WARNING log when a live server lags > N rounds (it is holding the floor).
- B170 events: condemn / spare (with recovered in-degree) / graduate / delete outcome
  (`Deleted` / `TokenMismatch` / `NotFound`) / fence-out (who, which incarnation, why).
- Retired-list size and oldest-entry age in the round report.
- Periodic self-heal sweep (existing full-verify cadence): a changed shard whose events predate
  the folded cursor without having been folded is an invariant violation — loud alert.

## Migration

Pre-release, no persisted-data compatibility required
([[feedback_ca_no_compat_scaffolding_predev]]): remove the watermark object, `fence_version`,
per-round retired sets, and the fence/recheck code paths outright. First round under the new
scheme starts with an empty retired list; folded tokens/cursors carry over from the existing
seals. Mixed-version pools are not supported.

## TLA+ plan (gate before implementation)

New focused module `CaGcAckFloorCore` (companion regime to `CaCasMountCore`): writers with
`view` / `ack` / two-step commits (gate-evaluate, then land — exposing the in-flight window),
heartbeat liveness + fence-out, the retired list with per-entry `condemn_round`, the floor
arithmetic, and the one-pass merge as a single action family.

- **Positive stage:** INV_NO_DANGLE / INV_NO_RETURN / monotone-ack / publish-order hold.
- **Sabotage controls** (each MUST yield a counterexample):
  1. `SabotageDeleteAtFloor` — graduate with `condemn_round ≥ min_ack`;
  2. `SabotageAckWithoutRead` — ack advances without loading the retired list;
  3. `SabotageAckBeforeDrain` — ack advertised while an old-view commit is in flight;
  4. `SabotageSleeperRearm` — an expired heartbeat renews (no fence-out) and its writer commits;
  5. `SabotageSkipChangedShard` — the sweep skips a shard whose token advanced (analog of the
     existing Phase-2 control);
  6. `SabotageAdoptRetiredToken` — the commit gate references a listed token instead of
     recreating;
  7. `SabotageOpenWriteBeforeLoad` — a fresh mount mutates before loading the view.
- **Witnesses (reachability):** condemn → graduate → delete; condemn → recover → spare;
  recreate → `TokenMismatch` on the pending delete.

## Testing plan

- **Unit (gtest, `InMemoryBackend`, injected clocks/hooks):** the three-cursor merge rules
  including the `condemn_round = min_ack − 1 / = min_ack` boundary; drain (a hung in-flight
  mutation blocks the ack; completion releases it); fence-out (sleeper renewal permanently fails;
  concurrent renewal wins ⇒ reclassified live); commit-gate recreate (retired token → fresh
  incarnation referenced; `TokenMismatch` delete outcome); open ordering (mutation before view
  load must be impossible by construction); heartbeat merge (watermark floor + ack + lease in one
  body, terminated/fenced classification).
- **Integration / soak (`utils/ca-soak`):** hard-KILL a writer mid-commit-burst and verify the
  next rounds spare-then-recondemn correctly (no dangle in `fsck`); a paused (SIGSTOP) writer
  holds the floor, resumes, acks, floor advances; scenario asserting round request counts stay
  O(delta)+O(servers) (regression guard against reintroducing a universe sweep of GET/PUTs).
- **TLA+ regression:** the module above wired into the models' run script with per-cfg positive /
  sabotage / witness stages.

## Non-goals / follow-ups

- Delta-runs + compaction for the snapshot (bytes O(edges) per pass → amortized) — the deferred
  O(buffer) streaming work; becomes the next bottleneck after this lands.
- `process_epoch` → `writer_epoch` unification (touches manifest `writer_instance_id` stamps).
- Read-path 404-repair (B85) — unchanged by this design.
- Adaptive per-namespace `root_shards` (universe-size reduction) — orthogonal knob.
