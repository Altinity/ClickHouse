> **Closure note (2026-07-13 grooming).** The "Fix / Investigate First" items were re-triaged closed
> (GCR-1 mitigated by ack-floor; promote-over-committed landed via `republishRef` idempotency; J1/J2/J3
> addressed by shard-incarnation `writer_epoch` fencing; G-N1 clamp observability added; lost-ACK
> reconcile landed — see the 2026-07-11 re-triage sections below and P3-B1 CLOSED). Still-open residuals
> are consolidated in [`../cas/BACKLOG.md`](../cas/BACKLOG.md): the two RFC residuals (AWS SDK
> region-redirect retry bypass; `promoteStaged` `copyObjectConditional` retry semantics) → §1; SEC-1
> trust-domain doc → §7; AD-3 day-2 runbook → §7; the R1/X1 reader-vs-GC + replication (RPL-*) + W-N3
> write-batching-liveness verification items → §4/§10; low/minor cleanup (R2–R4, W-N4, G-N2–G-N4,
> SEC-4/SEC-5) → §13. This file is kept as the review-triage narrative.

Прочитал [reviews.md](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/reviews.md:1). Ниже уже deduplicated summary: не по всем исходным severity, а с учётом Mikhail comments внизу файла.

**Fix / Investigate First**
1. `GCR-1`: `rebuildBaseline` runs while a live mount lease exists.
Это выглядит реальным. Add mount-lease interlock: plain rebuild must refuse if any fresh writer mount exists; `force` only with explicit runbook semantics. Add the provided regression test and flip expectation to `rep.performed == false`. See [reviews.md](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/reviews.md:1190).

2. `Build::promote` overwrite leak via `republishRef` retry.
This is the most concrete storage leak: `promote(dst,T_b)` can overwrite existing `refs[dst]=T_a` without emitting `-1`, pinning `T_a` forever. Fix `Build::promote`: if `refs[R]` already has another manifest, either emit proper committed→committed repoint or fail closed with exception. Also move `Build::abandon` `retireBuildSeq` after the removal mutation. See [reviews.md](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/reviews.md:54).

3. Lost-ACK idempotency: `W-N1/W-N2/J5`.
On conditional-write conflict, reconcile by reading post-state instead of blindly replaying closure. This should cover `dropRef` false failure, `stageManifest` spurious abort, and duplicate publish/promote journal events. See [reviews.md](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/reviews.md:190).

4. `J1/J2/J3`: writer lease is not a storage fencing token.
Add `SIGSTOP/SIGCONT` test first. Short-term mitigation: re-check `mayMutate` immediately before shard `casPut`. Longer-term fix: include `writer_epoch` in the shard-write fence/precondition. See [reviews.md](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/reviews.md:426).

5. Persistent clamp freezes GC pool-wide: `G-N1`.
Keep fail-closed behavior, but add observability and operator escape: clamp age, clamped key/shard, alert, and documented `fsck`/rebuild path. Consider scoped suppression later. See [reviews.md](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/reviews.md:340).

**Verify / Add Tests**
1. `R1/X1`: reader vs GC race.
For normal `MergeTree`, this is likely covered by `DataPart` lifetime. Action: document that CAS read safety depends on `MergeTree` part lifetime; audit whether any ref-less/cross-node reader exists. Only implement ephemeral read pins if such path is real. See [reviews.md](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/reviews.md:88).

2. Replication paths.
`RPL-2` is mostly general `ReplicatedMergeTree` partial commit handling; still add CA-specific reconciliation tests if cheap. `RPL-3` likely covered by sender lifetime/ACK ordering, but add targeted relink-vs-GC test. `RPL-4` is a perf cliff to recheck: `to_detached` fetch may stream bytes instead of relinking. `RPL-5` is coverage debt. See [reviews.md](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/reviews.md:1148).

3. Write batching liveness: `W-N3`.
Add metrics first: queue wait, leader flush latency, batch size, batch-wide failures. Then make wait cancellation/timeout-aware if numbers show stalls. See [reviews.md](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/reviews.md:220).

**Document / Defer**
1. Security model.
`SEC-1` is the real security concern if pool crosses trust domains: `CityHash128` is not cryptographic. For release, explicitly document “one CAS pool = one trust domain”; for multi-tenant future, add crypto hash mode or trust-domain-scoped dedup. `SEC-2/SEC-3` are by design under that model. See [reviews.md](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/reviews.md:935).

2. Day-2 runbook: `AD-3`.
Create a table: failure mode → signal/metric → diagnostic command → recovery command → test. Include stalled GC, persistent clamp, lost/corrupt `gc/state`, live mount conflict, orphan refs/manifests, pool meta corruption, and backup/restore of CAS control plane. See [reviews.md](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/reviews.md:1180).

3. Low/minor cleanup.
`R2-R4`, `W-N4`, `G-N2-G-N4`, `SEC-4/SEC-5` are not urgent correctness blockers. Good hardening: cache manifest under GET token, assert TTL vs GC latency, configure S3 multipart cleanup lifecycle, add decode-size caps, reject `..` in `checkNamespace`.

**Close As No-Action / By Design**
`G-N3` is a false positive: next GC round reads committed `gc/state`. Cross-shard serializability findings are by design. `RPL-2` should mostly follow existing `ReplicatedMergeTree` detached/recovery semantics rather than invent CAS-only transactionality.
---
## 2026-07-11 backlog re-triage (post ack-floor + Phase-2 sha256)

**GCR-1 (rebuildBaseline vs live mount lease) — RE-SCOPED, likely already mitigated.**
Phase-1 investigation: `Gc::rebuildBaseline` (CasGc.cpp:1729) DOES compute the ack-floor mount-lease
build-watermark floor (`computeHeartbeatFloor`, ~2100) during the disaster-recovery pass, AND its final
`gc/state` `casPut` is fail-closed against a competing writer (~2134 "gc/state changed under the rebuild
— re-run"). The review's GCR-1 predates the ack-floor merge (spec 2026-07-02). A concurrent writer's
in-flight/just-published blobs are protected by ITS mount-lease floor, not by refusing the rebuild — and
the originally-proposed fix ("plain rebuild must refuse if any fresh writer mount exists; flip
`rep.performed==false`") would BREAK the legitimate disaster-recovery use case (rebuild must be able to run
while writers exist). Before implementing anything: run a focused TWO-model consult on the specific
interleaving — a manifest published AFTER the rebuild's `discoverUniverse` snapshot whose blobs are below
the floor — to confirm the floor fully covers it (INV-NO-LOSS). Do NOT implement the blunt refusal.
Status: NOT a straightforward fix; needs consult-gated re-validation, not the review's proposed change.

**Other "Fix First" items (promote-over-committed, lost-ACK idempotency, writer-lease-fencing, GC clamp):**
promote-over-committed appears largely landed (Build::promote carries republishRef idempotency + unique-ref
fail-closed repoint, CasBuild.cpp:985-1094). The remainder are deep concurrency-correctness efforts of the
same class — each warrants its own systematic-debugging Phase-1 (confirm still-real post-recent-work) +
two-model consult + TDD + soak. Recorded here as dedicated future efforts per the campaign's
"too-complex/hard-decision → backlog" rule.

### GCR-1 CLOSED (2026-07-11 Phase-1 resolution — mitigated by ack-floor, no fix needed)
Code-grounded resolution (CasGc.cpp:2008-2016, the rebuild orphan-condemn comment + the zero_condemned
seeding at ~2020-2062): the rebuild DOES condemn an orphan blob B whose manifest M was published just after
its `discoverUniverse` snapshot — but it seeds a `kCondemned` row at round R; the DELETE (graduation) is
FLOOR-GATED ("Graduation still waits for every mount to ack past that round (the normal floor)"). The
ack-floor invariant (spec 2026-07-02, TLA+soak-validated) guarantees a live writer's mount lease cannot ack
past R until its <=R builds are committed AND visible — at which point the next regular round's
`discoverUniverse` sees M, B is edge-bearing again, and the retired-in-snapshot re-settlement resurrects B's
`kCondemned` row (never graduated). INV-NO-LOSS holds via the floor, NOT via refusing the rebuild. The
review's proposed "plain rebuild must refuse if a fresh writer mount exists; flip rep.performed==false" fix is
UNNECESSARY (the floor already protects) and HARMFUL (breaks the disaster-recovery use case — rebuild must be
able to run while writers exist). GCR-1's residual safety rests entirely on the already-validated ack-floor
invariant; it introduces no new concurrency obligation. NO CODE CHANGE. Closed.

### "Fix First" list re-triage (2026-07-11) — largely addressed by intervening work
Quick Phase-1 triage of the remaining review "Fix First" items (the review predates the shard-incarnation,
ack-floor, and reconcile-idempotency work). None is an open unaddressed bug; each has significant existing
mitigation and needs only a focused per-item Phase-1 CONFIRMATION (like the GCR-1 closure above), not a
from-scratch fix:
- GCR-1 (rebuild vs mount lease): CLOSED — floor-gated graduation (see above).
- promote-over-committed leak: largely landed — `republishRef` idempotency + unique-ref fail-closed repoint
  (CasBuild.cpp:985-1094).
- J1/J2/J3 (writer lease not a fencing token): addressed — shard-incarnation carries `writer_epoch`; a build
  from a superseded mount incarnation is fenced out (CasBuild.cpp:127; ShardIncarnation{.writer_epoch=...} at
  ~915). The review's longer-term fix (writer_epoch in the shard-write fence) exists.
- G-N1 (persistent clamp freezes GC pool-wide): observability added — `clampBefore` emits a `clamped` event row
  with the clamped key/shard (CasGc.cpp:940-956). The operator-visibility ask is met; scoped-suppression remains
  a future enhancement, not a bug.
- Lost-ACK idempotency (W-N1/W-N2/J5): addressed — Build's conditional-write conflict paths reconcile by reading
  the post-state and adopting the other party's result rather than blindly replaying (CasBuild.cpp:633,707,722).
TODO for a future focused session: a formal per-item confirmation + a targeted regression test for each, to
convert "largely addressed" into "closed with a test". None is release-blocking on current evidence.

## P3-B1 (2026-07-12, BLOCKER for Phase 3 sign-off): post-rolling-restart GC ack-wedge on the mid-switch soak
Mid-switch chaos soak (seed 3, Phase-3 binary 566bbfdbde6+, ch128->sha256 flip + rolling restart of BOTH nodes
at t=10m) WEDGED: both mounts' heartbeat ack stuck at 0 while published round raced to 854+ (~3s/round spin) ->
graduation floor pinned -> pipeline frozen (181k unreachable/pending/awaiting), delete_pending rows re-executed
every round (blob ch128:001db2... deleted once at 21:53:40Z, then endless outcome=replaced; rustfs returns
TokenMismatch not NotFound for If-Match DELETE on an ABSENT object - secondary finding, check the mapping).
Checker fail: "dryrun key previews deletion of a non-pipeline blob (absent/reachable)", dryrun_count=19815.
INV-NO-LOSS HELD throughout: dangling=0, unaccounted=0 - liveness wedge, NOT data loss. No view-refresh
exception in server logs (ack silently not advancing after remount). 34x "gc/state moved during the round"
(dual-leader fights) early after the restarts. Signature matches the S13-wedge family (P3.1 mount-fence
recovery, open tasks 5-6). Evidence: tmp/p3wedge/ (blob lifecycle TSV, gc log tail, both err logs, soak log).
DISCRIMINATOR (run FIRST next session): repeat the IDENTICAL both-nodes rolling restart mid-soak on the
PHASE-2 binary (c5a7c0409fb, no algo flip) - wedges too => pre-existing P3.1 gap; clean => Phase-3-caused
(then bisect P3 T1-T6, prime suspects: T5 Store::open changes / refreshAdmittedAlgos locking in the beat path).
Also note: the previous soaks today (Phase-2 binary, harness-driven single-node chaos) did NOT wedge.

### P3-B1 ROOT CAUSE (2026-07-12 debug) — reclassified: PRE-EXISTING GC lease/fold livelock, NOT a Phase-3 regression
Evidence hygiene first: the initial report was contaminated — the host ./logs dir is CUMULATIVE since Jul 6;
the "ack stuck at 0 / round 854" lines are from Jul 9 (pre-Phase-3, P3.1-era experiments). Tonight's CLEAN
window (2026-07-11 21:37-22:15Z) shows ZERO ack/fence problems and instead 10-11x "gc/state moved during the
round (ABORTED)" on EACH node (~every 2 min) — sustained DUAL-LEADER round fights.
Mechanism (livelock, self-sustaining):
  huge deletion pipeline (181k objects from the checkpoint's table DROPs) -> fold takes ~2 min ->
  GC lease expires MID-FOLD -> the other node legitimately adopts -> the first node's round CAS aborts
  ("gc/state moved") AFTER its pre-CAS redeletes already executed -> pipeline never shrinks -> folds stay
  slow -> repeat. The visible delete_pending loop (same blob re-deleted every round, outcome=replaced) is
  this re-execution; secondary finding: rustfs answers TokenMismatch (412), not NotFound, for an If-Match
  DELETE of an ABSENT object, so re-executions log "replaced" instead of "absent".
Why NOT Phase 3: `git diff c5a7c0409fb..HEAD -- CasGc.cpp` touches ZERO lease/round-CAS lines; the mechanism
is algo-agnostic; and the Jul-9 logs show the same wedge family on a pre-Phase-3 binary. Safety HELD
throughout (exact-token deletes idempotent; dangling=0, unaccounted=0).
Confirmatory step if desired (not yet run, ~15 min): no-algo-flip repro — big drop + both-nodes rolling
restart on any binary.
Fix directions (own track, consult-gated): (a) mid-fold lease renewal; (b) make pre-CAS redeletes durable/
post-commit so aborted rounds do not re-execute them; (c) map DELETE-If-Match-on-absent to Absent (rustfs 412
quirk); (d) soak checker: admit the executed-but-not-yet-folded dryrun class.
=> Phase 3 sign-off UNBLOCKED by this reclassification; P3-B1 continues as a standalone pre-existing bug.

### P3-B1 PRECISE ROOT CAUSE (2026-07-12, user question "разве лиза не продлевается mid-round?" led here)
B160's mid-round protection EXISTS (heartbeatLoop pulses gc/hb every H=interval/4; the steal check honors
hb_alive) BUT the gate `i_am_leader` is stored only AFTER `runRoundLogged` RETURNS (CasGcScheduler.cpp:195),
while the lease is acquired INSIDE the round (CasGc.cpp:210). => a NEW leader's FIRST round runs entirely
WITHOUT heartbeat cover — exactly the round B160 exists to protect. A follower's two ticks (10 s apart) see
frozen (seq, hb) => steals DETERMINISTICALLY whenever that first round is longer than ~2 ticks. The stealer's
first round is equally unprotected => mutual alternation at fold-duration cadence (tonight's ~2 min), locked
in by the big pipeline keeping folds slow. Steady state is safe (previous-round leaders carry the flag), which
is why ordinary soaks never tripped it: it needs leader CHANGE (restart) + a LONG first round (huge pipeline).
FIX: begin pulsing at LEASE ACQUISITION, not after the round — e.g. `runRound` invokes an `on_lease_acquired`
callback (scheduler sets `i_am_leader=true` + emits one immediate pulse) right after `acquireOrRenewLease`
succeeds; keep the abort path's `store(false)`. Deterministic-steal gtest: observer with two frozen
observations steals; with an in-round pulse between them it must back off.

### P3-B1 FIX LANDED (2026-07-12): 4ffe23bd7d2
`Gc::runRegularRound(on_lease_acquired)` fires the hook right after `acquireOrRenewLease`, before the fold;
the scheduler's callback sets `i_am_leader=true` + one immediate advisory `pulseHeartbeat`. 815/815 green incl.
2 new `CasGcLease` protocol tests (deterministic first-round steal; pulse-between-observations backs the
follower off). Scheduler thread wiring verified by review (no-sleep test convention). REMAINING on this track:
(b) pre-CAS redeletes re-execute on an aborted round — DISPOSITION 2026-07-12: NO CHANGE (by design). The
pre-CAS placement is the deliberate Task-9 ack-floor decision ("ONLY entries the PREVIOUS pass published as
delete_pending — justified by durable state, safe at any leader staleness"); re-execution is idempotent
(exact-token) and the (a) fix removes the amplification (aborts are rare again). Moving deletes post-commit
would be a consult+TLA-grade protocol change purchasing only a rare-path optimization. Revisit ONLY if a
post-fix soak still shows re-execution churn.
(c) rustfs 412-on-absent -> "replaced" outcome mapping — IN FIX (agent running; also unskips the .meta
cleanup that Replaced gates off — a real .meta leak for already-deleted blobs);
(d) soak-checker executed-but-not-folded class — IN FIX (agent running);
liveness TLA property (pending ~> settled, no-fairness lasso check). Validation TODO: repeat the both-nodes
rolling-restart mid-switch soak on this fix — the alternation must be gone.

### P3-B1 CLOSED (2026-07-12): validation soak GREEN
Exact replay of the failing mid-switch rolling-restart scenario on the fixed binary: PHASE3 OK, dangling=0,
0x "gc/state moved" on both nodes (was 10-11 each), no replaced-loops. Remaining on the track (non-blocking):
liveness TLA property (pending ~> settled lasso check) — nice-to-have formal gate for this bug class.
- [ ] RFC residual (from T4 review): AWS SDK region-redirect retry bypass — a client configured with region aws-global can retry past ShouldRetry (contrib AWSClient.cpp:330-357); CAS disks are not aws-global today; add a startup guard or probe if that ever changes.
- [ ] RFC residual (from T4 re-review): promoteStaged uses IObjectStorage::copyObjectConditional (server-side conditional copy) — a separate conditional-write mechanism NOT bounded by T4's single-attempt work; verify its underlying retry semantics before relying on write-once promote invariants under the retry-control RFC.
