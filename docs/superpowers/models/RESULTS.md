> **HISTORICAL.** These are results for the SUPERSEDED EBR/epoch/generation model (`CaGcCore.tla`).
> The current model is `CaIncarnationCore.tla` — see `CaIncarnationCore_RESULTS.md`. Kept as the
> record of EBR-era checking. (Banner added 2026-06-23; mirrors the one already in `README.md`.)

# CA GC core — TLC model-checking results

TLC (v2.19, OpenJDK 21) was run on `CaGcCore.tla` against the four staged configs. This is
**bounded model checking**: TLC exhaustively explored every interleaving up to the finite bounds
(`Writers={w1,w2}`, `Leaders` per stage, `MaxEpoch=2` → epochs `0..2`, `Retention=1`, and a
`StateConstraint` capping `|refs|≤2`, per-writer `|pin|≤1`, `|plusIssued|≤1`, `fence≤MaxEpoch+2`).
A clean run is strong evidence of safety within those bounds, **not** a proof for unbounded
epochs/writers/leaders.

## Final results (all invariants)

| Stage | Failures enabled | Distinct states | Wall time | Result |
|---|---|---|---|---|
| 1 happy + dedup-reuse-vs-delete | none | 478,870 | 1s | **PASS** (no error) |
| 2 + session-expiry gap | expiry | 6,251,604 | 10s | **PASS** (no error) |
| 3 + split-brain | expiry + split-brain | 101,227,648 | 2m39s | **PASS** (no error) |
| 4 full | expiry + split-brain + Keeper wipe | 135,125,268 | 3m37s | **PASS** (no error) |

All runs exhausted their queue (`0 states left on queue`) = complete BFS within the bounds
(stage 4 reached graph depth 41). Invariants checked in every stage: `TypeOK`, `INV_NO_LOSS`,
`INV_NO_DANGLE`, `INV_NO_ABA`. java/TLC actually ran (OpenJDK 21, `tla2tools.jar` v2.19).

## Counterexamples found DURING model development (each = a real design constraint)

TLC found genuine `INV_NO_LOSS` violations on intermediate, deliberately-incomplete encodings.
Each one corresponds to a load-bearing rule the reviews identified; fixing the model to encode the
rule (i.e. testing the *intended* hardened design) made the violation disappear. This is the
model-checker doing its job: it refuses to let a too-weak version pass.

### CE-1 — flush-`+`-then-advance must cover the decide→`+`-durable window (V-EBR-1 / V-EBR-2(i))

First run (no failures) lost data: a writer decided to reuse epoch `e`, advanced `O_W` past `e`
*before* its `+(e)` was durable, the leader's closed-epoch fold then saw `e` unreferenced and
condemned+deleted it, and the writer published a ref to the deleted `e`. **Design rule it encodes:**
a writer may not advance `O_W` past `e` while it holds a decided-but-not-yet-durably-pinned
dependency at `≤ e` (`WriterAdvanceOW` gates on `decided[w] ∈ pin[w]`). This is the reviews'
must-fix #1/#2 "flush-`+`-then-advance, covering the decide→`+`-durable window," and it is the
S3-side ordering Keeper cannot supply (HB-CROSS does not exist). **Verdict: real design constraint,
not an artifact.** Encoding it = green.

### CE-2 — the reuse decision must be made under a fresh, non-lagged `O_W` (E21)

Next run lost data: a writer reused epoch `0` while its `O_W` was already `1` (it had advanced
past `0`), so its lease no longer pinned `safe_epoch ≤ 0`; the leader quiesced `safe_epoch=1>0`
and deleted `0`. **Design rule:** the reuse decision (`W.head`) must target an epoch the writer
currently observes (`e ≥ O_W[w]`), so its live lease covers the dependency until the `+` is durable
(`WriterReuse`/`WriterResurrect` require `e ≥ O_W[w]`). **Verdict: real design constraint** (the
≤1-lag / reuse-under-fresh-`O_W` rule). Encoding it = green.

### CE-3 — per-`+` accounting (a drop must net only that writer's `+`) — MODELING ARTIFACT

A run lost data because a single shared `plusDurable` set collapsed all `+(e)` into one boolean: a
writer's drop of its *first* commit removed the pin that a *second* in-flight reuse of the same
epoch depended on. **This was an over-abstraction in the model, not a design bug** — the real design
uses event-id-keyed `+`/`-` deltas whose fold sums by key. Fixed by making `pin` per-writer
(`pin ∈ [Writers → SUBSET Epochs]`); a drop removes only that writer's `+`. **Verdict: modeling
artifact**, corrected.

### CE-4 — the session-expiry-vs-awareness hinge: fail-stop on `Disconnected` is INSUFFICIENT (K1/K2)

With the expiry failure enabled (stage 2), TLC lost data even though writers fail-stopped on
`Disconnected`: a writer whose Keeper session the *server* had already expired (`ServerExpired`,
dropped from `safe_epoch`) but which still *believed* it was `Connected` kept committing — it pinned
`+` and published a ref into an epoch the leader had condemned+deleted while computing
`safe_epoch := epoch_current` over zero live writers. This is **exactly the K1/K2 violation the
Keeper-only review flagged**, and it confirms the review's must-fix #2: *"fail-stop on `Disconnected`"
alone does not close the `[t_expire, t_aware]` gap.* The corrected hinge is a writer self-fence on a
**local-elapsed-time deadline strictly inside `T_session`**, so the writer goes read-only *before* the
server can expire it. Modelled faithfully as: a consequential writer op requires
`WriterMayAct(w) == conn[w]=Connected ∧ sess[w]=SessAlive`. **Verdict: real design constraint** (the
make-or-break hinge). Encoding it = green (stage 2 passes).

## What the bounded model does NOT cover (residual untested surface)

The model abstracts aggressively. These are deliberately out of scope and remain UNTESTED here:

1. **A single content hash `H`.** Cross-hash interactions (manifests referencing many blobs, a part
   pinning a *set* of `(H,e)`) are not modelled. The reviews argue manifests share the blob lifecycle,
   but multi-object atomicity of a commit naming several hashes is not checked.
2. **The fold/snap data plane internals.** `snap` carry-forward retention, signed/clamped cross-window
   `+`/`-` counts (F14), LIST pagination misses (F15/E17), torn/partial `snap` writes (F16/E16/K13),
   and `event_id` dedup (F12/E13) are abstracted into the boolean "closed-epoch durable `+`" fold.
   The barrier's *correctness* is modelled; its *implementation* (streaming merge-sort, atomic publish)
   is not.
3. **Clock-skew arithmetic (S3-only mode).** This spec models the **Keeper-only** profile, where the
   hinge is the session-timeout assumption. The S3-only `2·Δ_skew` lease math, directional skew bound,
   and read-back-durable renewals (V-EBR-3 / E3/E4) are NOT modelled — Keeper is assumed.
4. **`active/<H>` reorder + reader `404→LIST` retry** (E20/§8). Reads are not modelled at all; the
   spec checks only that a live ref's bytes are not deleted (`INV_NO_LOSS`/`INV_NO_DANGLE`), not the
   in-flight-read restart rule.
5. **Reconcile (the full scan).** The retention backstop is modelled as a guard on `LeaderReclaim`
   (`e ≤ epoch_current − Retention`), but the reconcile job's own scan-window races (K7/K14/E18) and
   the post-restore grace are NOT separately modelled; the wipe action relies on the same retention
   guard.
6. **Liveness/leak invariants.** Only safety (no loss / no dangle / no ABA) is checked. Over-count
   leaks (V2/F5/E23/K19 — resurrection leaving a stale `+`), `INV_NO_LEAK_FOREVER`, and the optional
   `Liveness` temporal property are not exhaustively verified (the resurrection-leak path is a known
   liveness-only issue the reviews accept, cleaned by reconcile).
7. **Finite bounds.** `MaxEpoch=2`, 2 writers, ≤2 leaders, fence ≤ 4. Schedules requiring a third
   epoch generation, a third concurrent writer, or three contending leaders are outside the explored
   space. The `e+2` limbo, the reuse-vs-delete race, the expiry gap, split-brain, and wipe all
   manifest within these bounds, but deeper combinatorial schedules are not proven absent.
8. **Multipart-upload invisibility / S3 lifecycle abort** (E9/K12) — modelled implicitly by an object
   only becoming `Present` atomically; the incomplete-multipart lifecycle rule is assumed as infra.

**Bottom line:** within the finite bounds, the stabilized core — closed-epoch fold barrier +
flush-`+`-then-advance + reuse-under-fresh-`O_W` + session-Alive self-fence + fenced/fail-close
leader + `e+2`/`safe_epoch` reclaim + retention backstop — holds `INV_NO_LOSS`, `INV_NO_DANGLE`,
and `INV_NO_ABA` under the full adversarial interleaving (expiry gap, split-brain, total Keeper
wipe). The four counterexamples found during development each pinpoint a load-bearing rule; three
are real design constraints (already named by the reviews) and one was a modeling artifact.
