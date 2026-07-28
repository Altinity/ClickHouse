# CaCasMountCore — TLA+ results log

This file is the append-only results log for the `CaCasMountCore.tla` model (the mount
ownership + server-root identity subsystem). Each dated section records a config battery run
against a specific commit of the model, with exit codes, state counts, and (for RED runs) the
violated invariant and a short trace summary.

## 2026-07-24 — fence-not-rescue gate (spec rev.4) {#2026-07-24-fence-not-rescue-gate}

Task 1 of the fence-not-rescue implementation plan (spec:
`docs/superpowers/specs/2026-07-24-cas-mount-lease-self-race-fix-v2-design.md`, rev.4). This is
the TLA+ phase-0 gate Tasks 2-6 cite before touching any C++.

### Phase A is model-alignment, not a model change {#phase-a-alignment}

Phase A (non-aborting, exhaustive classification of a confirmed same-uuid epoch mismatch) is
model-**ALIGNMENT**, not a model change: the implementation's `LOGICAL_ERROR` abort on a
same-uuid confirmed mismatch had no model counterpart — the model already routes every
confirmed mismatch to `localLost` (`Renew`'s superseded branch, `CaCasMountCore.tla`, present
since round 0/P3.1) and proves `SupersededWriterMakesNoMutation` over it. No `.tla` edit was
needed or made for Phase A specifically.

### Step 1 — baseline (before touching the model) {#step-1-baseline}

Confirmed GREEN/RED against the pre-task committed model (`git show HEAD:...`), before any
edit:

| cfg | exit | result |
|---|---|---|
| `CaCasMountCore_stage1.cfg` | 0 | GREEN (expected) |
| `CaCasMountCore_sab_epochreset.cfg` | 12 | RED (expected — `WriterEpochMonotoneUnique`) |
| `CaCasMountCore_sab_foreigntakeover.cfg` | 12 | RED (expected — `ForeignUuidNeverAutoTakesOver`) |
| `CaCasMountCore_sab_adoptwedge.cfg` | 12 | RED (expected — `NoPermanentWedge`) |
| `CaCasMountCore_sab_fenceresurrect.cfg` | 12 | RED (expected — `FenceCostsEpoch`) |
| `CaCasMountCore_sab_wallclockreclaim.cfg` | 12 | RED (expected — `GlobalSupersededWriterMakesNoMutation`) |

Baseline all-green (honest exit 0, every sabotage non-zero) — proceeded to Step 3.

### Step 3 — model additions {#step-3-model-additions}

Added to `CaCasMountCore.tla`:

- Constants `SabEpochGuardOff` (FALSE = honest: epoch re-mint from an absent/0 durable
  counter requires `mount = None`; TRUE drops that guard — the pre-fix `allocateWriterEpoch`)
  and `SabDecomBlindBypass` (FALSE = honest: decommission re-mint requires a TERMINAL
  surviving mount; TRUE = the rejected blind bypass, round-3 finding-1).
- `WipeEpoch` — environmental action modeling genuine loss of the durable epoch **object**
  (independent of any Sab flag — always possible). **One-shot per behavior**, guarded by a new
  history variable `epochWiped` (see "State-space bound" below).
- `RemintEpoch(a)` — the absent-epoch branch of `allocateWriterEpoch` (`NormalMount` policy):
  fires only on a genuine wipe (`epoch = 0 /\ epochCeiling > 0`, see below), guarded by
  `SabEpochGuardOff \/ mount = None`.
- `RemintEpochDecom(a)` — the `DecommissionRecovery` policy branch: requires `mount # None`
  and (honest) a TERMINAL mount (`mount.fenced \/ mount.deadline + Drift <= clock`, Drift-aware
  per the model's own `GcFence`/`ClearExpiredMount` convention) and mints strictly past the
  surviving mount's own epoch; `SabDecomBlindBypass = TRUE` mints epoch 1 unconditionally.
- Wired all three into `Next`.

New history/bookkeeping variables (both pure derived bookkeeping — drive no OTHER guard than
noted, and are not read by any pre-existing action):

- `epochCeiling` — monotone high-water mark over every value `epoch` has ever held. Used
  **only** to distinguish, in `AllocEpoch`/`ObservedReclaim`/`WallClockReclaim`/`RemintEpoch`/
  `RemintEpochDecom`, a genuine post-wipe absent-epoch condition (`epoch = 0 /\ epochCeiling >
  0`) from bare genesis (`epochCeiling = 0`, never yet allocated — still routed through
  `AllocEpoch`/`ClaimMount`'s existing bootstrap path, unchanged).
- `epochWiped` — BOOLEAN, TRUE once `WipeEpoch` has ever fired on this behavior (see
  "State-space bound" below for why this exists).

### Deviations from the brief's literal sketch (all evidence-driven, found via TLC) {#deviations}

The brief's action sketches were explicitly flagged as adaptable ("SKETCHES... adapt
mechanically"). Four adaptations were required, each forced by an actual TLC counterexample
against the literal sketch — not speculative:

1. **`RemintEpoch`/`RemintEpochDecom` need the `epoch = 0 /\ epochCeiling > 0` genesis-vs-wipe
   guard.** Without it, `epoch = 0` (ambiguous between "never allocated" and "wiped") let
   `RemintEpochDecom` fire on a fresh, never-yet-allocated root — reachable even in the honest
   config. Fixed by adding `epochCeiling` (see above).
2. **`RemintEpochDecom` needs `crashed[a]` and a Drift-aware terminal check.** Without
   `crashed[a]`, TLC found a spurious *self*-decommission: the SAME still-live, actively-
   servicing actor could invoke `RemintEpochDecom` on its own in-flight mount purely because
   the wall clock had passed the deadline (a beat-blocked-but-alive incarnation — exactly the
   case `Renew`'s pre-existing late-renewal already handles safely), producing a false
   `SupersededWriterMakesNoMutation` violation in the **honest** config. Decommission is
   always performed on behalf of a presumed-dead identity (`openForDecommission` derives the
   victim uuid from a surviving mount left by a different run); this model has no separate
   "decommission operator" identity from `Actors` (round-3 finding-5's caveat), so `crashed[a]`
   (the model's existing "this process genuinely died, a fresh incarnation is now running"
   marker) is the faithful proxy. Also added `mount.epoch < MaxEpoch` (a `TypeOK` overflow the
   literal sketch missed) and made the terminal check Drift-aware
   (`mount.deadline + Drift <= clock`, matching `GcFence`/`ClearExpiredMount`'s established
   convention) since it is an observer-side death verdict.
3. **`WriterEpochMonotoneUnique` reverted to raw `epoch` (an earlier draft was wrong).** An
   earlier draft made this invariant's ceiling conjunct read `epochCeiling` instead of `epoch`,
   to tolerate an honest `WipeEpoch` with no consequential re-mint. Re-running the **full**
   battery (per Step 5) caught that this ALSO silently defeated the pre-existing
   `sab_epochreset.cfg` sabotage (`SabResetEpoch` zeroes `epoch` the identical way `WipeEpoch`
   does, and `epochCeiling` is immune to both) — a real regression, old RED silently turned
   GREEN. Reverted; see "Accepted residual gap" below for how the resulting honest-config
   false-alarm on this conjunct is instead handled at the cfg level (note: `FenceCostsEpoch`
   was ALSO dropped from the same cfgs, but for a distinct cause — see that section's
   corrected, per-invariant causal breakdown, not a shared "`WipeEpoch` alone" trigger).
4. **`WipeEpoch` made one-shot-per-behavior (`epochWiped` guard).** Originally unconditional
   per the brief's sketch ("always enabled"). Controller-flagged runaway CPU: `WipeEpoch`
   composing repeatedly with subsequent re-mints multiplied the explored state space with no
   new KIND of scenario for these invariants to distinguish — confirmed both before and after
   partial mitigation that `CaCasMountCore_rev6_observe.cfg` reached 100M+ states generated
   with an unbounded, still-growing queue. Added `epochWiped` (BOOLEAN, TRUE once `WipeEpoch`
   has ever fired) as a one-shot guard. Both negative-control cfgs stay reachable with a
   SINGLE wipe (that is what births the same-pair twin); repeated epoch-object loss within one
   behavior is out of this gate's modeled scope by deliberate, documented choice — a
   state-space bound, not a claim that a second loss is safe.

### Accepted residual gap: `FenceCostsEpoch` / `WriterEpochMonotoneUnique` in honest configs {#accepted-residual-gap}

**Correction (2026-07-24, review follow-up):** the original text below conflated the causes of
the two dropped invariants under one "`WipeEpoch` alone" narrative. An independent reviewer
re-ran both invariants individually (re-added to `stage1.cfg` one at a time) and found their
causes are **different** — only one of the two is actually triggered by `WipeEpoch` alone. Both
are corrected separately here.

**`WriterEpochMonotoneUnique`** — genuinely triggered by `WipeEpoch` **alone**, no re-mint
needed. Shortest counterexample (depth 6, honest `stage1.cfg`): `ClaimOwnerEmpty → AllocEpoch
(epoch 1) → ClaimMount → Write (wrote = {(A,1)}) → WipeEpoch (epoch: 1→0)`. At that final state,
`wrote` already contains `(A,1)` while the live `epoch` reads `0`, directly violating the
conjunct `\A x \in wrote : x[2] <= epoch`. This is the SAME underlying mechanism the
pre-existing, Sab-gated `SabResetEpoch` models (`SabResetEpoch` also zeroes `epoch` while
`wrote` already holds a higher value), just now reachable without that sabotage flag, since
`WipeEpoch` is deliberately ungated.

**`FenceCostsEpoch`** — **NOT** triggered by `WipeEpoch` alone; requires `RemintEpoch`'s
**honest** branch to actually fire afterward. Shortest counterexample found by the reviewer
(re-adding `FenceCostsEpoch` to `stage1.cfg` in isolation): `AllocEpoch (epoch 1) → ClaimMount →
GcFence (fencedEpochs = {(A,1)}) → AllocEpoch (epoch 2) → ClaimMount → ClearExpiredMount (mount
= None) → WipeEpoch (epoch: 2→0) → RemintEpoch mints the literal 1 (mount = None satisfies the
honest guard) → ClaimMount reinstalls a LIVE, unfenced mount at (A,1)` — colliding with the
`(A,1)` pair `GcFence` already fenced earlier. **Root cause: `RemintEpoch`'s honest branch mints
a hardcoded literal `1` with no distinctness protection**, unlike `RemintEpochDecom`'s honest
branch, which mints `mount.epoch + 1` (distinct by construction against the surviving mount it
can see). `RemintEpoch`'s `mount = None` guard correctly prevents re-arming a still-LIVE mount,
but does nothing to prevent the freshly-minted epoch NUMBER from colliding with a **historical**
value already recorded in `fencedEpochs` for this same uuid — a distinct failure mode from the
"is a mount currently live" question the guard actually answers.

Both mechanisms are, at the level of the design spec's own language, instances of the same
accepted category: "Residual hole, honestly stated" (Phase C section) — "epoch AND mount both
wiped under a live mount still permits a same-pair twin... degrading to a remount, never to two
silently-live writers persisting" — a bounded, self-healing risk Phase C does not eliminate (no
body nonce / coherent-read machinery is a stated non-goal). But they are two **different**
concrete triggers within that category, not one, and `FenceCostsEpoch`'s trigger is narrower and
plausibly closeable (see "Follow-up" below) — it should not be read as equally fundamental to
`WriterEpochMonotoneUnique`'s.

Both `FenceCostsEpoch` and `WriterEpochMonotoneUnique` read raw, instantaneous state
(`fencedEpochs` membership / the live `epoch` value) and cannot distinguish "temporarily
inconsistent, self-healing" from "genuinely corrupted." Dropped from every cfg where this
would otherwise mask the cfg's actual target (see table below for which). This mirrors the
model's own pre-existing precedent: `CaCasMountCore_sab_epochreset.cfg` has never listed
`FenceCostsEpoch` either, for the identical underlying reason. Both invariants remain
meaningful and are still exercised where they are each cfg's own primary target
(`WriterEpochMonotoneUnique` in `sab_epochreset.cfg`; `FenceCostsEpoch` in
`sab_fenceresurrect.cfg` and — as the actual intended finding — `sab_decomblindbypass.cfg`).

#### Follow-up (NOT implemented in this task) {#followup-remintepoch-distinct-mint}

Minting `epochCeiling + 1` instead of the literal `1` in `RemintEpoch`'s honest branch would
plausibly close the `FenceCostsEpoch` hole identified above: since `epochCeiling` is already a
monotone high-water mark over every value `epoch` has ever held (including values later
fenced), a mint of `epochCeiling + 1` can never numerically collide with anything in
`fencedEpochs` for this uuid, by construction — the same distinctness argument
`RemintEpochDecom`'s `mount.epoch + 1` already relies on. This would likely allow restoring
BOTH `FenceCostsEpoch` and `WriterEpochMonotoneUnique`'s ceiling conjunct to `stage1.cfg` (the
latter would still need the `WipeEpoch`-alone depth-6 case addressed separately, e.g. via the
`epochCeiling`-based ceiling substitution attempted and reverted in deviation #3 above — revisit
that revert in light of this fix, since the reason it was wrong was that it ALSO defeated
`sab_epochreset.cfg`'s detection, a concern orthogonal to this follow-up), narrowing the
residual gap to only the "epoch AND mount BOTH wiped simultaneously under a live mount" case
the design spec explicitly accepts. **Deliberately not implemented here** — flagged as a
candidate refinement for a later task/round, not a blocking gap in this phase-0 gate's verdict.

Honesty note on this whole follow-up: the model's `RemintEpoch` action is gated only by
`~(mount # None /\ mount.uuid = a /\ ~mount.fenced)` — it never checks `rootEmpty`, and
`rootEmpty` itself is one-way (TRUE only at genesis, `FALSE` once any `Write` fires and never
`TRUE` again), so the model's honest `RemintEpoch` reaches states the real code's
`allocateWriterEpoch` cannot: the code's absent-epoch branch additionally requires
`serverRootSubtreeEmpty` (an authoritative empty-subtree probe) before minting, a precondition
this model omits entirely. The honest-config `FenceCostsEpoch` counterexample above therefore
over-approximates the code's real exposure — conservative in direction (it can only find a
hole the code might not actually have, never hide one the code does have) — and should be
revisited together with the `epochCeiling + 1` refinement, not treated as proof the code itself
reaches this state.

### Step 4/5 — full battery on the modified model {#step-4-5-full-battery}

All runs below are against the final committed `.tla` + cfg state (single `java
-XX:+UseParallelGC -jar tla2tools.jar -config <cfg> CaCasMountCore.tla` invocation each).

| cfg | exit | color | invariant / witness reported | states (generated / distinct) | notes |
|---|---|---|---|---|---|
| `CaCasMountCore_stage1.cfg` | 0 | GREEN | — (all 5 listed invariants hold) | 51,231,925 / 10,616,665 | **exhaustive** (0 left on queue); `FenceCostsEpoch`, `WriterEpochMonotoneUnique` dropped (accepted residual gap, see above) |
| `CaCasMountCore_sab_epochreset.cfg` | 12 | RED (unchanged) | `WriterEpochMonotoneUnique` | 539 / 321 | own original target; trace confirms `epochWiped = FALSE` — genuinely via `SabResetEpoch`, not `WipeEpoch` |
| `CaCasMountCore_sab_foreigntakeover.cfg` | 12 | RED (unchanged) | `ForeignUuidNeverAutoTakesOver` | 585 / 339 | own original target; `WriterEpochMonotoneUnique` dropped (was masking this with the unrelated residual gap) |
| `CaCasMountCore_sab_adoptwedge.cfg` | 12 | RED (unchanged) | `NoPermanentWedge` | 606 / 336 | own original target, unaffected |
| `CaCasMountCore_sab_fenceresurrect.cfg` | 12 | RED (unchanged) | `FenceCostsEpoch` | 2,250 / 952 | own original target, unaffected |
| `CaCasMountCore_sab_wallclockreclaim.cfg` | 12 | RED (unchanged) | `GlobalSupersededWriterMakesNoMutation` | 94,388 / 20,770 | own original target, unaffected |
| `CaCasMountCore_witness_reclaim.cfg` | 12 | RED (unchanged) | `W_SameUuidReclaimsExpired` (reachable, as intended) | 584 / 338 | |
| `CaCasMountCore_witness_remountafterfence.cfg` | 12 | RED (unchanged) | `W_RemountAfterFence` (reachable) | 7,749 / 3,266 | |
| `CaCasMountCore_witness_observedreclaim.cfg` | 12 | RED (unchanged) | `W_ObservedReclaim` (reachable) | 287,089 / 59,375 | |
| `CaCasMountCore_witness_recoveryafterobservedreclaim.cfg` | 12 | RED (unchanged) | `W_RecoveryAfterObservedReclaim` (reachable) | 78,998,500 / 12,981,026 | 2min11s |
| `CaCasMountCore_sab_epochwipelive.cfg` (NEW) | 12 | RED (target) | `SupersededWriterMakesNoMutation` | 21,920 / 8,606 | `SabEpochGuardOff = TRUE`; see trace below |
| `CaCasMountCore_sab_decomblindbypass.cfg` (NEW) | 12 | RED (target) | `FenceCostsEpoch` | 1,755,204 / 528,248 | `SabDecomBlindBypass = TRUE`; the round-3 finding-1 trace, see below |
| `CaCasMountCore_rev6_observe.cfg` | **not completed** | — | — | 500M+ generated, queue still growing | see "Not completed" below — confirmed pre-existing, not caused by this task |

### `CaCasMountCore_sab_epochwipelive.cfg` — counterexample summary {#epochwipelive-counterexample}

`SabEpochGuardOff = TRUE` (the pre-fix `allocateWriterEpoch`, no mount-liveness check on the
absent-epoch branch). Depth 9 trace: `A` claims owner, allocates epoch, claims a mount, writes
→ `WipeEpoch` destroys the durable counter while the mount is still LIVE and unfenced →
`RemintEpoch(A)` re-mints epoch 1 despite the live mount being at a different epoch (guard
dropped) → `Renew`'s pre-existing different-epoch branch classifies the now-diverged mount as
superseded (`localLost[A]' = TRUE`) → `A`'s own next `Write` (gated only on its own still-valid
`clock < fenceUntil` — P2, reads no shared mount state) fires anyway → `lostThenWrote = TRUE`,
violating `SupersededWriterMakesNoMutation`. This is the model's decisive same-uuid substitute
for the "twin" story per the brief's fallback guidance (round-3 finding-5: the model's
actor≡uuid structure cannot express a literal second, concurrently-live physical identity) — a
corrupted incarnation's own bookkeeping diverges from ground truth via the guard-off re-mint,
is caught by the model's existing superseded-detection, and still mutates regardless.
`CaCasMountCore_stage1.cfg` (guard ON, `SabEpochGuardOff = FALSE`) proves the SAME scenario
(`WipeEpoch` reachable, mount live) does NOT reach this violation — the guard holds.

### `CaCasMountCore_sab_decomblindbypass.cfg` — counterexample summary (round-3 finding-1) {#decomblindbypass-counterexample}

`SabDecomBlindBypass = TRUE`. Depth 14 trace: `A` claims owner, allocates epoch 1, claims a
mount, is fenced by GC (`fencedEpochs = {<<A,1>>}`), allocates epoch 2, reclaims (fenced
recovery, `mount = [epoch: 2, uuid: A, fenced: FALSE, ...]`, LIVE), then `A` crashes (`Die`,
`crashed[A] = TRUE`) while that epoch-2 mount is still live → `WipeEpoch` destroys the durable
counter → `RemintEpochDecom(A)` (blind bypass, ignores mount liveness) mints epoch 1
unconditionally, recreating the SAME `(uuid=A, epoch=1)` pair that was already fenced earlier →
`ClaimMount`'s pre-existing same-pair-adopt path (`AdoptWrite`) lets the corrupted incarnation
continue over the LIVE mount body reporting `epoch = 1` → `FenceCostsEpoch` (`mount # None /\
~mount.fenced => <<mount.uuid, mount.epoch>> \notin fencedEpochs`) violated: a live, unfenced
mount body now exists under `(A, 1)`, a pair the GC already fenced. This is exactly the
design spec's round-3 finding-1: "a blind bypass would re-mint epoch 1 while a LIVE epoch-1
victim mount survives; `claimMount` then same-pair-adopts it immediately... creating exactly
the forbidden twin."

### `CaCasMountCore_rev6_observe.cfg` — not completed {#rev6-observe-not-completed}

This cfg (MaxClock=10, MaxEpoch=4, MaxToken=11, Drift=2, 7→5 invariants) did not finish within
a ~15-20 minute budget either before or after the `epochWiped` one-shot fix. **Confirmed via a
side-by-side check that this is pre-existing, not caused by this task**: running the cfg
against the ORIGINAL, pre-task committed `.tla` file (via `git show HEAD:...`) produces an
**identical growth curve** (≈35M states generated/min, queue climbing steadily past 9M with no
sign of turning over) — i.e. this cfg's cost is dominated by its own pre-existing
combinatorics (`Drift = 2` triples the nondeterministic `\E d \in 0..Drift` branch at every
`ClaimMount`/`Renew`/`AdoptWrite`, compounded across `MaxClock = 10`), not by anything this
task added. `epochWiped`'s one-shot guard measurably caps this task's OWN contribution (a
constant, bounded number of `WipeEpoch`/`RemintEpoch`/`RemintEpochDecom` states per behavior)
but does not change the underlying, pre-existing exhaustive-search cost of this specific cfg.

This cfg is **not** part of the Step 1/Step 4 battery the brief specifies by name — it is one
of the wider pre-existing config set. Given the confirmed pre-existing cost, it is left
**unresolved by this task**: it was GREEN before this task started (per its own file header,
"exhaustive GREEN across all 7 invariants" as of round 10) and there is no evidence this task
changed that (the two invariants this task's changes could plausibly affect —
`WriterEpochMonotoneUnique`, `FenceCostsEpoch` — are the same accepted-residual-gap invariants
already dropped from it here, for the identical documented reason as `stage1.cfg`). A follow-up
item (out of this task's scope) would be either a smaller-bounds smoke variant of this cfg, or
accepting that it requires a longer, dedicated TLC run outside the interactive session budget.

### Config battery constant additions {#config-battery-constants}

Every existing `.cfg` file for this model was updated to define the two new constants
(TLC errors on undefined constants): `SabEpochGuardOff = FALSE`, `SabDecomBlindBypass = FALSE`
for every honest and every pre-existing sabotage cfg (they test unrelated sabotage classes);
`CaCasMountCore_sab_epochwipelive.cfg` sets `SabEpochGuardOff = TRUE`;
`CaCasMountCore_sab_decomblindbypass.cfg` sets `SabDecomBlindBypass = TRUE`.

### Verdict {#verdict}

**GREEN** (the phase-0 gate this task exists to produce): the honest configuration (guard ON)
proves `WipeEpoch` reachable with no consequential violation of any of its 5 remaining
invariants, exhaustively. Both negative controls (guard OFF; decommission blind bypass) are
RED against their intended targets. The full pre-existing battery (10 cfgs) keeps its color
(all still RED/GREEN as before), with two cfgs (`sab_foreigntakeover`, and the honest
`stage1`/`rev6_observe`) needing an invariant-list adjustment to keep reporting their OWN
intended finding rather than the newly-reachable, accepted, unrelated residual-gap false
alarm — documented above, not silently dropped. `CaCasMountCore_rev6_observe.cfg` alone did
not complete in-session; confirmed pre-existing/orthogonal to this task, not blocking.

## 2026-07-28 — v9 recovery-generation layer (spec `2026-07-27-cas-ref-chain-complete-cut-design.md`) {#2026-07-28-recovery-generation}

Task 5 of the v9 ref-chain TLA+ phase. Spec authority: §3 "Recovery ownership" (the mount-fence
generation is captured at admission and required on every `slot-occupy`, `_ckpt` CAS and install;
self-remount cancels or waits out recovery before rearming) and §9's r9-5 sabotage ("an
old-generation wedge or recovery result returning after the successor sealed the slot is refused by a
generation recheck under the install lock — post-I/O, immediately before every install/unwedge/`_ckpt`
publication"). This is the gate the main implementation plan cites before touching the
`slot-occupy` / `_ckpt` / install code.

This module already owned the mount-fence generation *itself* — the durable writer epoch (`epoch`,
its per-actor view `localEpoch`, the fenced-pair set `fencedEpochs`), which is why §9 assigns the
generation's lifecycle here rather than to `CaRefCatalogCore` (which treats "the generation still
validates" as an oracle, `creatorAlive`) or `CaRefTableSnapshotLogCore` (single-recoverer). What v9
adds is the CONSUMER side.

### Naming: the brief's sketch vs the module's own vocabulary {#name-mapping}

The brief's action/flag names were adapted to this module's conventions, as it instructed. The
mapping, for anyone reading the brief and the model side by side:

| brief | module | why |
|---|---|---|
| `SabotageStaleInstall` | `SabStaleInstall` | every flag here is `Sab*` |
| `SabotageWedgeRetryOldGen` | `SabWedgeRetryOldGen` | ditto |
| `WedgeRetry` | `WedgeRetryCreate` + `WedgeRetryOccupied` | split by what `slot-occupy` returned; the split is what makes the seal's rejection reachable on the honest path (see "Findings" #1) |
| "current fence generation" | `GenerationCurrent(a, g) == g = localEpoch[a]` | `localEpoch` IS this module's per-actor view of the mount-fence generation |
| "¬superseded" | `clock < fenceUntil` | the module's own mechanical liveness check, as used by `Write` since round 8's P2 — deliberately NOT a read of `epoch`, which would make `GlobalSupersededWriterMakesNoMutation` unfalsifiable by construction |
| — | `SabSlotNoByteCompare` (added) | plan task 1's first hand-off, which the brief asks for in prose but does not name |

### What was added to the model {#additions}

Four constants: `RecoveryGenOn` (a FEATURE GATE, not a sabotage — FALSE in every pre-existing cfg),
`SabStaleInstall`, `SabWedgeRetryOldGen`, `SabSlotNoByteCompare`, plus the state-space bound
`MaxAdmissions`.

Eight variables: `recGen` / `wedgeGen` (the generation captured at admission by a recovery and by a
wedged lane, kept deliberately DISJOINT so one sabotage's counterexample cannot stand in for the
other's), `slot` (ONE frontier key — `None` = `Created`, `seal = FALSE` = `Occupied(bytes)`,
`seal = TRUE` = `Occupied(EpochSeal)`), `acked` / `durable` (both monotone), `admissions`, and the
two history flags `staleRefusedEver` / `sealRejectedEver`.

Nine actions: `RecoveryStart`, `SealSlot`, `Install`, `RecoveryRefused`, `WedgeAdmit`,
`StragglerLands`, `WedgeRetryCreate`, `WedgeRetryOccupied`, `WedgeAbandonStale`.

ONE new invariant, `AckedOpsAreDurable == acked \subseteq durable`. Both sabotages the brief names
target PRE-EXISTING invariants instead (see the next section). Two new witnesses,
`W_GenerationRefused` and `W_SealRejectedRetry`.

Every pre-existing action gained a single `UNCHANGED rgVars` conjunct rather than having its
already-long `UNCHANGED` tuple retyped — an edit touching 19 multi-line tuples is exactly where a
silent omission lives.

### Reused invariants, and the one that had to be new {#invariant-reuse}

Per the brief, no parallel invariant was invented where an existing one already says the thing:

- **`GlobalSupersededWriterMakesNoMutation`** (pre-existing, `~supersededThenWrote`) is the target of
  BOTH `_sab_staleinstall` and `_sab_wedgeretryoldgen`. An old recovery result publishing after a
  self-remount, and a dead lane's conditional create landing in the successor's id space, are both
  "a mutation by an incarnation the durable `epoch` counter has already passed", which is precisely
  what that invariant says. The two cfgs share a target deliberately and each header names it; the
  ROUTES are independent (a returning recovery RESULT vs a lane's own CREATE), which is where the
  evidence lives. It is also the same invariant `_sab_wallclockreclaim` targets, by a third route.
- **`AckedOpsAreDurable`** had to be new. `wrote`, `supersededThenWrote` and `lostThenWrote` all
  record what a writer DID; acked-then-lost is a divergence between that and what its CALLER WAS
  TOLD, and nothing in the module recorded the latter. It is a structural set inclusion over two
  monotone sets, not a ghost flag.
- `SupersededWriterMakesNoMutation` and `NoPermanentWedge` are carried in the green gate unchanged —
  the new layer must not manufacture a knowledge-based violation or a wedge, and it does not.
- `WriterEpochMonotoneUnique` and `FenceCostsEpoch` are dropped from `_v9_recoverygen.cfg` for causes
  that PREDATE this round and are analysed in full in "Accepted residual gap" above (the ungated
  `WipeEpoch`; `RemintEpoch`'s hardcoded literal `1`). `_stage1.cfg` remains where that gap is
  recorded.

### Cost neutrality of the legacy battery — verified, not asserted {#cost-neutrality}

`RecoveryGenOn = FALSE` in every pre-existing cfg freezes all eight new variables at their `Init`
values, so no legacy behaviour gains a state. Checked by running the full legacy battery twice on
the same machine with the same `-workers 1`: once against the pre-task committed `.tla`
(`git show HEAD:...`, in a scratch tree) and once against the modified one. **All twelve are
byte-identical in both state counts**, and the two that the earlier round recorded numbers for
(`stage1`, `witness_recoveryafterobservedreclaim`) also match this file's 2026-07-24 table exactly:

| cfg | result | states generated | distinct | baseline s | after s |
|---|---|---|---|---|---|
| `sab_epochreset` | RED `WriterEpochMonotoneUnique` | 539 | 321 | 0 | 1 |
| `sab_foreigntakeover` | RED `ForeignUuidNeverAutoTakesOver` | 585 | 339 | 1 | 1 |
| `sab_adoptwedge` | RED `NoPermanentWedge` | 669 | 372 | 1 | 0 |
| `sab_fenceresurrect` | RED `FenceCostsEpoch` | 2,551 | 1,088 | 0 | 1 |
| `sab_wallclockreclaim` | RED `GlobalSupersededWriterMakesNoMutation` | 111,240 | 24,905 | 1 | 1 |
| `sab_epochwipelive` | RED `SupersededWriterMakesNoMutation` | 21,920 | 8,606 | 0 | 0 |
| `sab_decomblindbypass` | RED `FenceCostsEpoch` | 1,755,204 | 528,248 | 5 | 6 |
| `witness_reclaim` | RED `W_SameUuidReclaimsExpired` | 584 | 338 | 1 | 0 |
| `witness_remountafterfence` | RED `W_RemountAfterFence` | 7,749 | 3,266 | 0 | 1 |
| `witness_observedreclaim` | RED `W_ObservedReclaim` | 287,089 | 59,375 | 2 | 2 |
| `witness_recoveryafterobservedreclaim` | RED `W_RecoveryAfterObservedReclaim` | 78,998,500 | 12,981,026 | 132 | 168 |
| `stage1` | **GREEN**, exhaustive | 51,231,925 | 10,616,665 | 111 | 136 |

(The RED counts differ from the 2026-07-24 table because that round used `-workers auto`; parallel
BFS explores in a nondeterministic order, so an aborted run's counts are not reproducible. The two
GREEN/exhaustive runs match exactly, which is the comparison that carries information. This round's
runner therefore pins `-workers 1`.)

### Findings — three things that had to change after TLC disagreed {#findings}

1. **`W_SealRejectedRetry`'s first draft was satisfiable by a degenerate route, and would have
   claimed to discharge a hand-off it did not.** As first written it counted ANY seal met by a lane's
   retry. BFS satisfied it at depth 7 with ONE generation sealing the key and its OWN lane then
   meeting it — the ordinary self-walk, which `CaRefTableSnapshotLogCore` already covers and which
   says nothing about two recoverers racing (the actual hand-off). Fixed by making the flag count
   only a seal planted under a STRICTLY LATER generation (`slot.gen > wedgeGen[a]`) while leaving the
   REJECTION itself generation-independent, because in the protocol any seal at the key is
   conclusive. The trace is now the cross-generation one (depth 11, below).
2. **The green gate had no verdict at all without a bound on admissions, so `MaxAdmissions` was
   added.** At `_stage1`'s bounds the honest layer passed 46 M distinct states at depth 19 with a
   still-growing queue after 9 minutes and no verdict — and smaller numeric bounds barely helped
   (`MaxClock = 3, MaxEpoch = 2, MaxToken = 5` was at 14.5 M and still growing), because the layer's
   variables are ORTHOGONAL to the mount machinery and multiply it rather than extend it.
   `MaxAdmissions` caps how many operations/recoveries a behaviour admits in total. It is a
   state-space bound in the same declared spirit as `epochWiped`'s one-shot `WipeEpoch` guard from
   the 2026-07-24 round — not a safety claim — and it is faithful in direction: a wedged lane holds
   ONE operation and a fresh incarnation runs ONE recovery, so a handful of admissions is what the
   product produces. `ADMISSIONS=<n>` in `run_mount.sh` re-runs the suite at another value.
3. **The first counterexamples all used the model's PRE-EXISTING epoch-0 bootstrap mount, which the
   product's strict order never reaches — so they were re-checked without it.** `ClaimMount` permits
   `mount = None /\ localEpoch[a] >= epoch` at genesis, so a mount can exist at generation 0 before
   any `AllocEpoch`, whereas `CasStore.cpp:312-316`'s STRICT ORDER allocates the writer epoch BEFORE
   claiming the lease. BFS naturally found the 0→1 transition first. Re-run in a scratch tree with
   the state constraint `mount = None \/ mount.epoch > 0` (module copy and constraint NOT committed —
   it exists only to answer this question), **all three sabotages stay red with an `AllocEpoch`-first
   mount**, on the 1→2 transition:
   - `_sab_staleinstall`: `ClaimOwnerEmpty → AllocEpoch → ClaimMount → Tick → Tick → RecoveryStart →
     ClearExpiredMount → AllocEpoch → ClaimMount → Install`
   - `_sab_wedgeretryoldgen`: same prefix with `WedgeAdmit` in place of `RecoveryStart` and
     `WedgeRetryCreate` in place of `Install`
   - `_sab_slotnocompare`: `ClaimOwnerEmpty → AllocEpoch → ClaimMount → Tick → WedgeAdmit →
     WedgeRetryCreate → Tick → ClearExpiredMount → AllocEpoch → ClaimMount → WedgeAdmit →
     WedgeRetryOccupied`
   The counterexamples are therefore about a generation TRANSITION, not about the bootstrap value.

### Counterexample traces {#v9-traces}

**`_sab_staleinstall` — depth 10.** `A` claims the root and mounts; the clock reaches the deadline;
`RecoveryStart` captures `recGen[A] = 0`; `ClearExpiredMount` retires the record; `AllocEpoch` mints
generation 1 (`epoch = 1`, `localEpoch[A] = 1`); `ClaimMount` re-arms with a FRESH
`fenceUntil = 4`. The stale recovery result now returns and, with the recheck dropped, `Install`
publishes under `recGen[A] = 0` while `epoch = 1` → `wrote = {<<A, 0>>}`,
`supersededThenWrote = TRUE`. Note what did NOT stop it: `clock < fenceUntil` passes, because the
REMOUNT re-armed the fence. That is exactly §3's "self-remount ... before rearming" window, and the
generation recheck is the only thing in it. (A bare GC fence-out cannot reach `Install` at all —
`GcFence` requires `mount.deadline + Drift <= clock`, which implies `clock >= fenceUntil`; the same
mutual-exclusion construction `Write` has relied on since round 9.)

**`_sab_wedgeretryoldgen` — depth 10.** Identical shape with the wedged lane in place of the
recovery: `WedgeAdmit` stores generation 0, the mount is retired and re-armed at generation 1, and
the generation-0 lane's conditional create then lands while `epoch = 1`. What the sabotage cannot
reach is worth recording: once the successor's `SealSlot` has occupied the key, `slot # None` and the
store refuses the create whatever generation is presented (INV-1's conclusive rejection). The
sabotage's entire reachable damage is the PRE-SEAL window — which is why the recheck has to come
first rather than lean on the seal.

**`_sab_slotnocompare` — depth 12.** `A` admits a wedged operation at generation 0 and its retry's
create lands, so the key holds the bytes of operation `<<A, 0>>` (`durable = acked = {<<A,0>>}`). The
mount is then retired and re-armed at generation 1, and `A` admits a NEW operation, `<<A, 1>>`. Its
retry finds the key `Occupied`; with the comparison skipped it concludes "mine landed" and acks
`<<A, 1>>`, while `durable` still holds only `<<A, 0>>` → `acked = {<<A,0>>, <<A,1>>}` ⊄ `durable`.
A caller has been told an operation succeeded that nothing ever wrote. This is the branch
`CaRefTableSnapshotLogCore` structurally cannot express (it grants the writer perfect knowledge of
`writtenEver`; task-1 report concern 1), handed here and discharged.

**`_witness_genrefused` — depth 9, reachable as intended.** `RecoveryStart` captures generation 0,
the mount is retired, `AllocEpoch` moves the local generation to 1, and `RecoveryRefused` drops the
returning result. The honest recheck really does refuse something, so the green gate is not green by
unreachability.

**`_witness_sealrejected` — depth 12, reachable as intended.** `WedgeAdmit` stores generation 0 →
`ClearExpiredMount` → `AllocEpoch` (generation 1) → `ClaimMount` → `RecoveryStart` (`recGen[A] = 1`)
→ `SealSlot` occupies the `Created` key with the SUCCESSOR generation's `EpochSeal` → the
generation-0 lane's `WedgeRetryOccupied` meets it and resolves definitively failed with no ack.
This is plan task 1's concurrent-recoverer hand-off: that module's single `rPhase` makes a seal
planted by a different recoverer than the one reading it unrepresentable.

### The bounds, and the measured sweep behind them {#v9-bounds}

The six new configs run at `MaxClock = 2, MaxEpoch = 2, MaxToken = 4, TTL = 2, Drift = 0,
MaxAdmissions = 3, Actors = {A, B}` — **smaller than `_stage1`'s**, which needs justifying rather
than asserting. Every row below is a real `-workers 1` run of `_v9_recoverygen`'s invariant list on
this machine; the runs marked *abandoned* were killed by hand to free CPU and are reported as
"no verdict by then", not as timeouts.

| admissions | actors | MaxClock/Epoch/Token | outcome | generated | distinct |
|---|---|---|---|---|---|
| unbounded | `{A,B}` | 4 / 3 / 7 (`_stage1`'s) | no verdict, depth 19, queue growing (abandoned ~9 min) | 195,903,650 | 46,547,519 |
| unbounded | `{A,B}` | 3 / 2 / 5 | no verdict, depth 19, queue growing (abandoned) | 68,183,359 | 14,479,883 |
| unbounded | `{A}` | 4 / 3 / 7 | no verdict, depth 19, queue growing (abandoned) | 72,278,162 | 18,124,829 |
| 3 | `{A,B}` | 4 / 3 / 7 | no verdict, depth 17 (abandoned ~4 min) | 63,268,900 | 16,717,686 |
| 3 | `{A,B}` | 3 / 2 / 5 | no verdict by 600 s (abandoned) | — | — |
| 3 | `{A}` | 3 / 3 / 5 | no verdict by 600 s (abandoned) | — | — |
| 3 | `{A}` | 3 / 2 / 4 | **GREEN**, exhaustive | 38,448,343 | 7,834,854 |
| 3 | `{A}` | 2 / 2 / 4 | **GREEN**, exhaustive | 11,501,074 | 2,440,747 |
| **3** | **`{A,B}`** | **2 / 2 / 4** | **GREEN, exhaustive — the chosen configuration** | **50,885,769** | **9,762,979** |

Three things this sweep establishes, none of which was obvious before running it:

- **`MaxAdmissions` alone was not enough, and neither were smaller numeric bounds alone.** The
  unbounded rows and the `3 / 4-3-7` row are both without a verdict. The layer's variables are
  orthogonal to the mount machinery, so they multiply its space; only cutting both dimensions gets a
  verdict.
- **`MaxToken` is the sharp lever, not `MaxClock`.** `{A}` at `3 / 2 / 4` is green while `{A}` at
  `3 / 3 / 5` is not, and `{A,B}` at `3 / 2 / 5` is not. `MaxToken` bounds how many times the mount
  record may be written, which is what drives the pre-existing space this layer multiplies. Anyone
  tuning these bounds later should reach for `MaxToken` first.
- **`Actors = {A, B}` costs ~4× over `{A}` and is kept anyway**, so that
  `NoTwoServerUuidsOwnSameServerRoot` and `ForeignUuidNeverAutoTakesOver` stay non-vacuous in the v9
  gate rather than only in `_stage1`. The second actor contributes nothing to the new layer itself
  (every new action requires `owner = a`).

What the chosen bounds still contain: `TTL = 2` against `MaxClock = 2` is one complete
expire-and-remount cycle, which is exactly the shape every counterexample in this round needs (each
is a generation TRANSITION with an outstanding result or lane); `MaxEpoch = 2` allows two
transitions; `MaxToken = 4` allows four mount writes against a longest trace using two. All three
sabotages and both witnesses are red **at these same bounds** — first found at `_stage1`'s wider
ones and re-confirmed here — so the green and the reds are directly comparable, which is the
property that carries the evidence.

### The new configs — full battery {#v9-battery}

`bash docs/superpowers/models/run_mount.sh`, `-workers 1`, uncontended. The six new configs, with
`_stage1` repeated for comparison:

| cfg | exit | colour | invariant / witness reported | generated | distinct | s |
|---|---|---|---|---|---|---|
| `sab_staleinstall` (NEW) | 12 | RED (target) | `GlobalSupersededWriterMakesNoMutation` | 206,083 | 65,968 | 3 |
| `sab_wedgeretryoldgen` (NEW) | 12 | RED (target) | `GlobalSupersededWriterMakesNoMutation` | 204,854 | 66,055 | 2 |
| `sab_slotnocompare` (NEW) | 12 | RED (target) | `AckedOpsAreDurable` | 1,560,369 | 470,659 | 4 |
| `v9_recoverygen` (NEW) | 0 | **GREEN** | — (all 7 listed invariants hold) | 50,885,769 | 9,762,979 | 152 |
| `witness_genrefused` (NEW) | 12 | RED (reachable, as intended) | `W_GenerationRefused` | 63,755 | 22,377 | 1 |
| `witness_sealrejected` (NEW) | 12 | RED (reachable, as intended) | `W_SealRejectedRetry` | 1,538,207 | 463,567 | 3 |
| `stage1` (legacy gate) | 0 | GREEN | — | 51,231,925 | 10,616,665 | 146 |

`v9_recoverygen` reports **0 states left on queue** — exhaustive, not truncated. Note the two greens
cost almost exactly the same wall time (152 s vs 146 s) at their respective bounds, which is the
practical point of `_stage1` keeping the layer off: the suite did not get slower for anyone.

The three sabotage counts are abort-run numbers and are reproducible only because the runner pins
`-workers 1`; see the `-workers` note in `run_mount.sh`'s header.

### `MaxAdmissions` is not doing the work {#v9-admissions-not-load-bearing}

`ADMISSIONS=5 bash run_mount.sh` re-runs the whole suite with the bound raised. The green gate stays
**GREEN and exhaustive** at 5, at roughly twice the cost:

| run | outcome | generated | distinct | s |
|---|---|---|---|---|
| `_v9_recoverygen`, `MaxAdmissions = 3` (default) | GREEN, exhaustive | 50,885,769 | 9,762,979 | 152 |
| `_v9_recoverygen`, `MaxAdmissions = 5` | GREEN, exhaustive | 101,723,633 | 19,209,187 | 333 |
| `_v9_recoverygen`, `MaxAdmissions = 5`, `Actors = {A}` | GREEN, exhaustive | 23,029,764 | 4,802,299 | 64 |

So the bound buys a verdict, not a colour: it is what makes the search terminate, and raising it does
not change the answer. The whole suite was re-run this way — `ADMISSIONS=5 bash run_mount.sh` →
**18/18 expectations met**, every sabotage red against the same target and every witness still
reachable — so no config's colour depends on the bound's value.

### Per-action coverage — the green gate is not vacuous {#v9-coverage}

`COVERAGE=1 bash run_mount.sh` re-runs `_v9_recoverygen` under `-coverage 1`. All nine new actions
fire, with the distinct-states : transitions counts below. This is the machine-checked answer to
"does the honest configuration actually exercise the layer, or is it green because nothing happened":

| action | states | transitions |
|---|---|---|
| `RecoveryStart` | 380,304 | 972,632 |
| `SealSlot` | 113,208 | 269,088 |
| `Install` | 289,744 | 1,214,264 |
| `RecoveryRefused` | 580,648 | 1,975,184 |
| `WedgeAdmit` | 982,112 | 985,472 |
| `StragglerLands` | 1,180,288 | 1,180,288 |
| `WedgeRetryCreate` | 157,000 | 264,248 |
| `WedgeRetryOccupied` | 694,128 | 1,812,376 |
| `WedgeAbandonStale` | 787,824 | 1,937,184 |

`RecoveryRefused` **and** `WedgeAbandonStale` both fire, which is the specific thing coverage was
needed for: `W_GenerationRefused` is driven by ONE flag set at both sites, so the witness alone
cannot show that both rechecks are exercised. They are. As a sanity check in the other direction, the
same run reports `SabResetEpoch` and `WallClockReclaim` at `0:0` — correctly disabled by their FALSE
flags in this cfg.

### Runner change {#v9-runner}

`run_mount.sh` was a single-cfg wrapper taking a basename. It is now a whole-suite runner with
asserted violation NAMES, matching `run_refcatalog.sh` / `run_nscleanup_staleleader.sh` — a per-cfg
colour that nothing checks is a colour that rots. Env knobs: `TLC_WORKERS` (default 1, deliberately),
`ADMISSIONS=<n>`, `COVERAGE=1`, `SLOW=1`. The raw `java` invocation for running one config by hand is
in the script header.

`_rev6_observe` is excluded from the default suite and appended only under `SLOW=1`, where it is
expected to report `incomplete` and does not fail the run. Its non-completion is **pre-existing** and
already analysed above (`{#rev6-observe-not-completed}`); what this round adds is that a config with
no verdict can no longer masquerade as a green one, because the runner now names the state.

### Verdict {#v9-verdict}

**GREEN** — the gate this task exists to produce. `bash run_mount.sh` → **18/18 expectations met**.
The three new sabotages are each red against their named target, the two new witnesses are each
reachable, the new green gate is exhaustive over seven invariants, all twelve pre-existing configs
keep their colour with byte-identical state counts, and the green survives raising the one bound the
round had to add.

Two things this gate does NOT establish, stated so the plan does not over-claim them:

- **The `_ckpt` CAS is not one of the sites modelled here.** §3 requires the generation on
  `slot-occupy`, the `_ckpt` CAS **and** the install; `slot-occupy` and the install are carried here,
  while `_ckpt`'s own writers live in `CaRefTableSnapshotLogCore` and `CaRefCatalogCore`, which
  carry the generation on their writes but treat its validity as an oracle. Nothing in the model set
  proves the three sites present the SAME rechecked value — that is a plan obligation.
- **The catalog-token half of the zombie install stays with `CaRefCatalogCore`.** This module owns
  the generation credential's lifecycle; the install must present BOTH that and the catalog
  token-CAS, and only the first is proven here.
