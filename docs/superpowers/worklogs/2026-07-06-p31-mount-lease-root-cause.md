# P3.1 — mount-lease "foreign writer" race: root-cause investigation {#p31}

Status: CODE ANALYSIS COMPLETE (this section); repro evidence being collected to confirm
attribution. Method: systematic-debugging — no fix before the root cause is confirmed.

## The mechanism (code-derived) {#mechanism}

Key parameters: `mount_lease_ttl_ms = 30000`, `mount_renew_period = 10000` (ttl/3).

### The renewal path can block far past the TTL {#renewal-blocks}

`MountLeaseKeeper`'s background renewal (`SingleWriterSlot::renewOnce`, every 10 s) calls
`prepareRenew` FIRST, and `prepareRenew` runs the BEAT — `Store::refreshViewForBeat`
(`CasStore.cpp:542`): a `gc/state` GET, and when the published round advanced, an EXCLUSIVE
`view_gate` acquisition + `retire_view.refresh()` (S3 reads of retired sets). Two blocking hazards:

1. The exclusive `view_gate` waits out every in-flight `mutateShard` (shared holders). Under an S3
   retry storm (the campaign measured 19% read-error rates on RustFS; single requests observed
   retrying to attempt ~304/501 with backoff — minutes), one stuck in-flight mutation blocks the
   beat, and therefore the RENEWAL, for minutes.
2. `retire_view.refresh()` itself does S3 reads under the same congestion.

A renewal blocked > 30 s means the lease EXPIRES while its owner is alive and healthy.

### The "foreign writer" is the GC leader's legitimate fence-out {#fence-is-the-toucher}

Every GC round's heartbeat-floor pass (`computeHeartbeatFloor`, `CasServerRoot.cpp:470+`) fences any
EXPIRED, unterminated, unfenced mount: token-guarded `putOverwrite` stamping `gc_fenced = true`
(preserving uuid/epoch, seq+1). This is BY DESIGN — but it changes the slot's token. The blocked
renewal then resumes, its `putOverwrite(last_token)` gets `PreconditionFailed`, and
`SingleWriterSlot::renewOnce` throws the misleadingly-worded
`"touched by a foreign writer — failing closed, never re-minting"` (`CasSingleWriterSlot.cpp:75`).
`onRenewFailed` → `tripMountLost` + `scheduleRemount` → self-remount as a fresh incarnation →
recovers. **This explains the CHRONIC all-night collisions (~4x/hour/node, clustered in heavy-load
windows, mostly self-healing): the toucher is not foreign at all — it is the pool's own GC fencing
OUR expired lease after a beat-blocked late renewal.**

### The restart WEDGE (S13, exit 49) {#wedge}

`Store::open` sequence: `claimMountAwaitingExpiry` writes a fresh lease (expires now+30 s) → the
keeper's `doStart` runs `prepareRenew` (the BEAT — on a churned post-chaos pool the retired-view
load is slow) → only then `claim()` = HEAD → GET → uuid/epoch checks → `putOverwrite(got->token)`.
If the pre-adopt beat pushes past the TTL, the just-claimed lease expires MID-OPEN and the GC
leader may fence it at any moment. If the fence lands between `claim()`'s GET and PUT, the adopt
gets `PreconditionFailed` → `LOGICAL_ERROR` "touched while adopting our own mount slot" → thrown
during metadata load → `Application` aborts → exit 49. Cold open has NO retry (the self-remount
retry loop exists only for the RUNNING remount path), and the container has no restart policy —
the node stays down until manual intervention. Narrow window (needs the fence inside GET→PUT),
consistent with 1 wedge per ~26 collisions per night.

### Secondary bug: `release before start` at teardown {#release-before-start}

When `doStart` throws, the keeper's `seq` stays 0; Store teardown still calls `stop()` →
`doTerminate` → `seq == 0` → `LOGICAL_ERROR "release before start"` — pure noise during an
already-failing shutdown, but it is a real defect (terminate of a never-started slot should be a
quiet no-op).

### Design wrinkle discovered (needs TLA+ adjudication) {#adopt-over-fence}

`claimMount`'s refresh branch and `claim()`'s adopt branch check uuid+epoch but NOT `gc_fenced`:
if the GC fences the fresh lease BEFORE the adopt's GET (instead of inside the GET→PUT window),
the adopt re-reads the post-fence token and SUCCEEDS — resurrecting a FENCED incarnation with the
SAME epoch. The code comment on the reclaim branch says a fence-out is "terminal for that
incarnation"; same-epoch resurrection contradicts that. Whether it violates the ack-floor
guarantees (the fenced round dropped this ack from the floor) must be decided in the
`CaCasMountCore` model, not by inspection.

## Predictions for the repro (falsifiable) {#predictions}

- **P-1**: every `mount_conflict`/refusal on ch1 is preceded (≤ round interval) by a
  `gc_fence_out` event with `detail.srid = ca_soak_ch1` emitted by the current GC leader.
- **P-2**: the holder identity in every conflict is ch1's OWN `server_uuid` (fence preserves the
  body) — the "foreign writer" is never a foreign uuid.
- **P-3**: renewal failures correlate with renewal gaps > 30 s (beat-blocked), or with kill windows.
- **P-4**: a wedge, if reproduced, is the enriched "touched while adopting our own mount slot
  (server_uuid=<ch1's own>...)" during metadata load.

## Fix vectors (to be spec'd + TLA+-gated after evidence confirms) {#fix-vectors}

- **A (correctness of diagnosis)**: on `PreconditionFailed` in renewal/adopt, RE-READ and decide by
  BODY: same-uuid + `gc_fenced` = "fenced after lease expiry" (recoverable: remount / bounded
  re-claim), not "foreign writer". True foreign uuid stays fail-closed. Messages stop lying.
- **B (liveness)**: decouple the BEAT from the renewal deadline — a renewal must be O(1 PUT); when
  the beat cannot complete in time, renew with the last installed ack (ack LAG is what the floor
  is designed to tolerate; a lease EXPIRY is what it cannot).
- **C (cold-open resilience)**: bounded retry of the claim/adopt during `Store::open` (mirroring
  `claimMountAwaitingExpiry`'s existing loop) instead of one-shot fail-closed.
- **D**: `doTerminate` with `seq == 0` → quiet no-op.
- **E (TLA+)**: extend `CaCasMountCore` with: late-renewal expiry + fence + re-adopt interleavings;
  decide same-epoch-resurrection-after-fence (likely: a fence must cost an epoch — recovery is a
  NEW incarnation, and the adopt path must check `gc_fenced`).
