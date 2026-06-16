# CaBuildWatermark — TLC results (B167 per-server watermark)

Model: `CaBuildWatermark.tla`. Spec: `docs/superpowers/specs/2026-06-16-ca-build-watermark-design.md`.
Run: `java -XX:+UseParallelGC -cp tmp/tla2tools.jar tlc2.TLC -config <cfg> CaBuildWatermark.tla`
(from the repo root; add `-metadir states/<unique>` when launching several in the same second).

This replaces the abstract `HeartbeatGuard` boolean of `CaResurrectLiveness` with the **concrete watermark oracle** the design implements, and adds a second contending build (last-writer-wins). `Builds = {b1, b2}`, so the state space is tiny (≤34 distinct states per config).

## Oracle abstraction

`Protected == WatermarkGuard ∧ serverLive ∧ ¬gcDead ∧ owner ≠ NoOwner ∧ owner ∈ activeSet`, with:
- `owner ∈ activeSet` ≈ `build_seq ≥ min_active` for a live server (the owning build is still in-flight). This is a **conservative over-approximation** of the scalar floor — it unprotects a build's blobs the moment *that* build finishes, even if an older build keeps `min_active` low. That only makes GC *more* aggressive, so liveness proven here implies liveness under the real, more-protective floor.
- `serverLive ∧ ¬gcDead` = the per-server liveness signal + frozen-`seq` crashed-server verdict. `epoch` (stale incarnation ⇒ unprotected) is folded into `serverLive` (an old epoch is exactly "old server not live").
- **Publish does not require ownership**: a build references the present incarnation iff `present ∧ ¬condemned` (W-REVALIDATE adopt). Re-stamp-to-self is needed only to clear a condemnation / regain protection — which is why two builds do **not** ping-pong.

## Results table

| Config | Knobs | Property | Result |
|---|---|---|---|
| `_guard` | all sound, no crash | `Liveness == <>(published = Builds)` | **HOLDS** (10 states) |
| `_noguard` | `WatermarkGuard=FALSE` | `Liveness` | **VIOLATED** (12 states) |
| `_staleactive` | `ActiveSetCorrect=FALSE` | `Liveness` | **VIOLATED** (30 states) |
| `_unsounddetect` | `SoundDetection=FALSE` | `Liveness` | **VIOLATED** (22 states) |
| `_crash` | `CanCrash=TRUE`, all sound | `NoLeak == (serverLive=FALSE ∧ published={}) ~> <>¬present` | **HOLDS** (34 states) |

`TypeOK` holds in every config. The positive (`_guard`, `_crash`) and the three negative controls are all load-bearing: removing the watermark guard, the active-set discipline, or sound crash detection each reproduces a starvation lasso.

## Counterexample lassos (each negative control)

**`_noguard` — today's GC re-condemns the build's OWN fresh incarnation (the headline B167 lasso):**
```
S1 Init        present, condemned, owner=NoOwner
S2 BuildRestamp(b1)  owner=b1, condemned=FALSE      (body-in-hand fresh incarnation)
S3 GcCondemn         condemned=TRUE                 (~Protected because guard OFF — condemns b1's OWN blob)
S4 GcDelete          present=FALSE, owner=NoOwner
-> back to S2 (re-stream) -> S3 -> S4 -> ...  published never grows.
```
This is the body-in-hand writer, not the old bodyless GET path — proving writer-side re-stream alone is starvable and the watermark guard is load-bearing.

**`_staleactive` — `min_active` wrongly advances past a still-in-flight build:**
```
S2 BuildRestamp(b2)  owner=b2, activeSet={b1,b2}    (b2 protected)
S3 MinActiveSkip(b1) activeSet={b2}                 (floor wrongly advances)
S4 MinActiveSkip(b2) activeSet={}                   (floor passes b2, STILL in-flight)
S5 GcCondemn         condemned=TRUE                 (~Protected: owner=b2 ∉ activeSet)
S6 BuildRestamp(b1)  owner=b1 ...                   (re-stamp, but floor still empty)
S7 GcCondemn -> back to S4 ...  published never grows.
```
Confirms the in-memory active-set discipline (`min_active` = exact min of in-flight builds) is required.

**`_unsounddetect` — crash detection declares a LIVE server dead:**
```
S2 GcDeclareDead     gcDead=TRUE                    (fires while serverLive — K too small)
S3 BuildRestamp(b2)  owner=b2, condemned=FALSE      (b2 active, but...)
S4 GcCondemn         condemned=TRUE                 (~Protected: gcDead masks a live owner)
S5 BuildRestamp(b1) -> S6 GcCondemn -> back to S3 ...  published never grows.
```
Confirms sound detection (frozen `seq` across K≥2 passes, the B160 discipline) is load-bearing: a false-positive death is as fatal to liveness as having no guard.

## Relationship to `CaResurrectLiveness`

`CaResurrectLiveness` (kept) proved the *abstract* heartbeat guard is load-bearing: guard ON → `<>published` holds; guard OFF → starvable. `CaBuildWatermark` discharges that abstraction onto the concrete oracle and shows the three concrete ways the oracle can be broken (no guard / stale active set / unsound death) each reproduce the same starvation, while the sound oracle converges and a genuine crash still reclaims (no leak). The publish-gate safety backstop and `INV-NO-LOSS/NO-DANGLE/NO-RETURN` remain `CaIncarnationCore`'s job (safety), not checked here.

## Safety invariants (checked in EVERY config, sabotages included)

Two state invariants are checked alongside the temporal properties, at `Builds = {b1, b2, b3}`:
- `Inv_ProtectedNeverCondemned == Protected ⇒ ¬condemned` — the guard never leaves a protected incarnation condemned (hence never exact-token-deletable): a protected blob is never lost.
- `Inv_NoDangle == (published ≠ {}) ⇒ present` — a published reference never dangles.

Both **HOLD in all five configs, including the three sabotages.** That is the B167 thesis made machine-checkable: the sabotages break **liveness only**, never safety (no data loss — consistent with the soak evidence that the active table stayed intact).

## Companion: `CaBuildWatermarkNum` — concrete numeric validation of the abstraction

`CaBuildWatermark` abstracts the scalar floor as `owner ∈ activeSet`, folds `epoch` into `serverLive`, and uses one server. `CaBuildWatermarkNum.tla` discharges those abstractions with a **numeric** model: a real per-server scalar `MinActive(s) = min(active build_seqs)`, **two servers** each with its own `epoch` watermark, real `epoch` counters a restart bumps, and monotone `build_seq` allocation. It checks SAFETY (`Inv_ProtectedNeverCondemned`, `Inv_NoDangle`) exhaustively (`MaxSeq=2`, `MaxEpoch=1`).

| Config | Knob | Result |
|---|---|---|
| `_correct` | sound guard, monotone `build_seq` | **No error** (4616 distinct states) — safety holds with the real scalar floor, 2 servers, restart/epoch |
| `_confused` | guard checks the WRONG server's watermark | **VIOLATED** — `Inv_ProtectedNeverCondemned`; per-server scoping is load-bearing |
| `_nonmonotonic` | `build_seq` unique but NOT monotone | **VIOLATED** — see finding below |

**Finding — monotone `build_seq` allocation is load-bearing, not just uniqueness.** The first `_correct` run (before this constraint) failed: build `seq=2` re-stamped the blob, finished without referencing it (GC condemned it correctly), then a build `seq=1` *started* → `min_active` dropped to 1 → `ownerSeq=2 ≥ 1` → the scalar floor **re-protected a finished build's condemned blob** (a leak). The fix is the discipline the spec already names — `build_seq` is allocated from a **monotone counter**, so a new build's seq always *exceeds* every prior one and `min_active` can never be pulled back below a finished build's seq. `_nonmonotonic` (uniqueness without monotonicity) reproduces the violation, confirming the counter is required. The implementation MUST allocate `build_seq` strictly increasing per `(server, epoch)`, never reuse, never out-of-order.

## Residuals

- The boolean `CaBuildWatermark` uses two builds, one hash, one server; the numeric `CaBuildWatermarkNum` uses two servers / numeric floor / epoch but checks safety only. Together they cover liveness (boolean) and the floor/scoping/epoch safety (numeric). Multi-server *liveness* and >2 servers are not enumerated; the per-server oracle is independent by construction (a blob is governed by exactly one server's watermark, its `server_id`).
- Crashed-server detection is modeled as a `serverLive`/`gcDead` flag pair with a soundness knob; the actual frozen-`seq`-across-K-passes mechanism is proven in `CaGcLeaseCore` (B160) and reused.
