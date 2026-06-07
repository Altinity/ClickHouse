# CA GC core — TLA+ model + TLC runbook

A TLA+ specification (`CaGcCore.tla`) of the **stabilized core** of the content-addressed (CA)
MergeTree garbage collector — the Keeper-only Epoch-Based Reclamation (EBR) profile, hardened with
the corrected hinges from the three adversarial reviews — model-checked with TLC to exhaustively
hunt safety violations within finite bounds.

This is **bounded model checking, not a proof**: TLC exhausts all interleavings up to the finite
bounds below. A clean check is strong evidence, not a theorem for unbounded epochs/writers.

## Source design

- `docs/superpowers/reports/2026-06-07-ca-gc-ebr-design-plan.md` — the EBR plan.
- `docs/superpowers/reports/2026-06-07-ca-gc-keeper-only-profile.md` — the Keeper-only profile.
- Reviews whose exact races we target:
  `…-simplified-correctness-review.md` (V1/V3/V4),
  `…-ebr-design-plan-review.md` (V-EBR-1/2/3),
  `…-keeper-only-review.md` (V-K1…K21).

## What is modelled (the stabilized core)

One fixed content hash `H`, so an object key `(H,e)` collapses to its epoch `e`. Each epoch's
object is in a lifecycle state `Absent | Present | Condemned | Deleted` (durable in S3, strongly
consistent). The safety-critical mechanics, all preserved:

- **`epoch_current`** — monotone, in S3; advanced only by the fenced leader, once per round (CLOSE).
- **`refs`** — set of epochs named by a live ref (the commit point, written last).
- **closed-epoch S3 fold barrier (RETAINED)** — the leader folds only CLOSED epochs; a `+` in an
  open epoch is not yet folded. (`FoldUnreferenced`.)
- **flush-`+`-then-advance** — a writer's `+` becomes durable in S3 *before* it advances `O_W`
  (`WriterAdvanceOW` is gated on the decided dependency being durably pinned). This is the
  cross-system ordering Keeper cannot supply (HB-CROSS does not exist).
- **per-writer pins** — each `+(H,e)` is per-writer so a drop nets only that writer's `+`
  (the fold sums by key; avoids the over-abstraction artifact of a single shared `+` boolean).
- **Keeper coordination** — a monotone fence; per-writer `O_W` + a server-side session
  (`Alive | ServerExpired`) and an independent client belief (`Connected | Disconnected`).
- **the session-expiry-vs-client-awareness gap** — `ServerExpire` drops a writer's `O_W` from the
  live set independently of the client, which keeps believing it is alive until it observes
  `Disconnected`. The corrected hinge: **writers and the leader fail-stop on `Disconnected`**
  (not on confirmed `Expired`) — `CanWrite` / `LeaderActive` gate on `Connected`.
- **GC round** — CLOSE → FOLD/CONDEMN → QUIESCE (`safe_epoch := min O_W` over live writers, else
  `epoch_current`) → RECLAIM (`epoch_current ≥ e+2 ∧ safe_epoch[l] > e ∧ unreferenced ∧
  retention-ok ∧ still-leader`). Every mutation gated on `LeaderActive(l)` (current fence + Connected).
- **two leader identities** — genuine split-brain: a successor `LeaderSteal` takes a higher fence
  while the old leader keeps its now-stale `heldFence`; `LeaderActive` (held = current fence) is the
  fencing that must block the stale leader.
- **failures, adversarially interleaved** — `ServerExpire`, `WriterDisconnect`/`Reconnect`,
  `LeaderDisconnect`/`Reconnect`, `LeaderSteal` (split-brain), `KeeperWipeRecover` (total Keeper
  wipe → re-elect, read epoch from S3, bump once, writers re-register; retention backstop honored).

## Invariants checked

- `INV_NO_LOSS` — every epoch in `refs` resolves to `Present`/`Condemned` bytes (never `Deleted`).
- `INV_NO_DANGLE` — no live ref names an `Absent`/`Deleted` object.
- `INV_NO_ABA` — an epoch key that was *ever* `Deleted` is never observed `Present` again
  (recreate uses a fresh epoch; `everDeleted` is the history set).
- `TypeOK` — type correctness.
- (`Liveness`, optional temporal) a genuinely-unreferenced condemned object is eventually `Deleted`.

## How to run

```bash
# from this directory (docs/superpowers/models)
JAR=/path/to/tla2tools.jar          # tlaplus/tlaplus GitHub release (single jar)
java -cp "$JAR" tlc2.TLC -workers auto -config CaGcCore_stage1.cfg       CaGcCore.tla
java -cp "$JAR" tlc2.TLC -workers auto -config CaGcCore_stage2_expiry.cfg CaGcCore.tla
java -cp "$JAR" tlc2.TLC -workers auto -config CaGcCore_stage3_split.cfg  CaGcCore.tla
java -cp "$JAR" tlc2.TLC -workers auto -config CaGcCore_stage4_full.cfg   CaGcCore.tla
```

The setup used here: OpenJDK 21, `tla2tools.jar` v2.19 (downloaded from the official
`tlaplus/tlaplus` GitHub releases).

## Finite bounds (the staged models)

All stages: `Writers = {w1, w2}`, `MaxEpoch = 3` (epochs `0..3`), `Retention = 1`. A
`StateConstraint` caps `|refs| ≤ 2`, per-writer `|pin| ≤ 2`, per-writer `|plusIssued| ≤ 2`, and
`fence ≤ MaxEpoch + 3` to keep the graph finite (the fence is otherwise unbounded under steals).

| Stage | Config | Leaders | Expiry | Split-brain | Wipe |
|---|---|---|---|---|---|
| 1 happy + dedup-reuse-vs-delete | `CaGcCore_stage1.cfg` | `{L1}` | off | off | off |
| 2 + session-expiry gap | `CaGcCore_stage2_expiry.cfg` | `{L1}` | on | off | off |
| 3 + split-brain | `CaGcCore_stage3_split.cfg` | `{L1,L2}` | on | on | off |
| 4 full (all failures) | `CaGcCore_stage4_full.cfg` | `{L1,L2}` | on | on | on |

## Results

See `RESULTS.md` (written after the runs) for the per-stage PASS/FAIL, state-space sizes, the
counterexamples found during model development (and whether each was a design bug or a modeling
artifact), and the residual things the bounded model does NOT cover.
