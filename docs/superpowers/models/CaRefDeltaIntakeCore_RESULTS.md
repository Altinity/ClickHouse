# CaRefDeltaIntakeCore — TLA+ gate results (v9)

Model: `CaRefDeltaIntakeCore.tla`. Gates the GC fold and deletion safety of spec
`2026-07-27-cas-ref-chain-complete-cut-design.md` (v9) — §1 (**blob in-degree is pool-wide**), §5
(fold rules, the **destructive-round frontier proof**, the **durable hold**) and §7 (REBUILD
preserves holds). Task 2 of the plan `2026-07-28-cas-ref-chain-tla-phase.md`; this is a phase-0
gate — it blocks the C++ work.

**2026-08-01 scope amendment.** This model starts at `BeginRound` with the catalog cut already
chosen. The catalog-only pre-fold drain specifies that input as the **fresh post-drain catalog cut**:
the actor resolves every eligible exact-row deletion justified by the authoritative adopted parent
before taking it. `CaRefPreFoldDrainCore` owns and sabotage-gates that cross-object ordering for
ordinary fold, `DEFER`, and `REBUILD`; `CaRefDeltaIntakeCore` assumes rather than independently proves
this provenance. Its fold/frontier/hold verdicts and state counts therefore must not be cited as a
fresh-cut provenance control.

Runner: `./run_deltaintake.sh` (runs every config and checks its expected verdict, including *which*
invariant a sabotage is required to break; sabotages run FIRST, because a green is only evidence
once the property it rests on has been seen red). TLC 2.19 (tla2tools, Java 21),
`java -XX:+UseParallelGC -workers auto`, 32 workers. Every number below is real TLC output from the
run of 2026-07-28, not an estimate.

Constants: `MaxSeq = 3` and `EdgeOf <- EdgeRoles` in every config — one bound everywhere, so
every sabotage/control pair is like-for-like (see Scoping for why 4 did not fit).

## Headline

1. **The r7-1 cross-namespace blocker is reproduced and closed.** `_sab_skipquietprobe` — a round
   that destroys without a frontier proof, i.e. "unhinted means quiet" — deletes a blob that an
   acknowledged `+1` in a namespace the listing omitted still names. Turn the proof back on and the
   same model is green. This is the defect the whole design exists for, and it is now an executable
   red/green pair rather than a paragraph.
2. **The r8 blocker is reproduced and closed.** `_sab_rebuilddropshold` — REBUILD rebuilding cursors
   while forgetting holds — destroys under a hold that should still have been carried. The hold is
   what turns a ONE-ROUND detection into a permanent fact, and the model shows why: the witness that
   makes the impossible shape detectable comes from the zero-trust listing, so a later round may
   simply not see it again.
3. **Spec §5's temporal lemma is stated with only half its mechanism.** For a `+1` landing after
   its namespace's frontier probe, §5 argues from two-phase pacing plus the writer-side
   rematerialization of a `Condemned` blob. `_sab_deleteignoresindeg` shows a window neither
   argument covers: the `+1` lands after the probe but **before** condemnation, so the writer sees a
   LIVE blob and rematerialization never applies; that round's fold misses it, in-degree reaches
   zero, the blob is condemned; the NEXT round folds it normally and in-degree is back above zero —
   and the pending exact-token delete fires anyway unless it **re-reads in-degree at the delete
   site**. That re-check is real code (the "`deleteExact` liveness re-check") but is not in §5's
   text, and this model says the lemma is incomplete without it. It is also why `NoAckedLoss` had to
   be strengthened: in this counterexample the acked `+1` IS folded, so the brief's
   "acked-but-unfolded" form would have reported nothing.
4. **`_v9_hintomission` — the listing returns nothing, ever — is GREEN**, because v9 consumes the
   hint nowhere for correctness. That is what retires rev.4's `_sab_resumeskip` by design rather
   than by omission.
5. **A listing cannot be the witness source, and no listing discipline fixes that.** The hold fires
   on "an exact 404 below a witness", and spec §5 leaves the witness's provenance unstated. The only
   source v9 has is the round's enumeration — and an enumeration is a SNAPSHOT, so a witness that
   becomes durable after it was taken is invisible to the probe that follows. `_v9_hold` was red
   until its detector was made an explicit idealization that reads the store; `_witness_corruptgap`
   is the same shape with the real, omission-capable listing. The consequence: if an above-cursor
   record is lost to corruption before any round observed a witness above the gap, the fold cannot
   distinguish that namespace from a quiet one — the exact `GET cursor+1` answers absent, the
   frontier proof is granted, and a visible `-1` elsewhere deletes a blob that an INTACT acked `+1`
   above the gap still names. The precondition sits outside spec §1's trust model (exact point reads
   honest), but the damage exceeds the corruption itself, so both bounds are committed as runnable
   configurations rather than argued away. The fix direction is a DURABLE witness rather than an
   enumeration: `_fix_ckptwitness` makes `_ckpt.checkpoint` one and is green. **Spec §5 adopted
   this after the gate ran** (commits `abd77cd4738`, `33b301eacb8`), including its limit.
6. **Fix round 1 corrected two counterexample-FIDELITY defects** — both greens and reds were
   unchanged by them, but the traces were reaching their verdicts by shortcuts rather than by the
   scenarios the sabotages exist to model. (a) A hold did not revoke the frontier proof it
   contradicts, so a proof granted earlier in the SAME round, at the same position, before the hint
   disclosed the witness, survived and authorized destruction; `ProbeAbsent`'s hold branch now
   clears `frontier[t]`. (b) `Rebuild` inherited the round's `roundOutcome = "won"` and its proofs,
   so the r8 counterexample destroyed in the same idle tail rather than "one round later"; a rebuild
   now resets both. With the shortcuts closed, `_sab_destroyunderhold` and `_sab_rebuilddropshold`
   take the intended two-round path — detect via the hint's witness, carry the hold, then destroy in
   a later round whose hint omits it — and their state spaces grew from 1.1 M / 2.0 M distinct to
   5.3 M / 7.3 M at depth 21. The narratives below were rewritten against the new traces.
7. **A latent TLA+ precedence bug was found in the module this rewrite replaces.**
   `dupFlag' = dupFlag \/ (...)` parses as `(dupFlag' = dupFlag) \/ (...)`, so the moment the
   duplicate condition became true the ghost was simply not assigned. TLC reports that as
   `Successor state is not completely specified`, i.e. a harness *error*, not an invariant
   violation — and the retired runner's verdict test (`grep -qE "Invariant .* is violated|Error:"`)
   counted `Error:` as a violation, so a sabotage could have "passed" on a crash. Fixed here
   (parenthesised) and reported: the same shape is live at `CaBuildRootPrecommit.tla:300`
   (`everDangle' = everDangle \/ (...)`, invariant `~everDangle`), which is not this task's model.
   `run_deltaintake.sh` does not treat `Error:` as a violation and additionally asserts *which*
   invariant each sabotage broke.

## Summary table

| cfg | expected | TLC verdict | states (gen / distinct) | depth |
|---|---|---|---|---|
| `_sab_skipquietprobe` (THE r7-1 blocker) | violation | **RED as required** — `NoAckedLoss` | 1,956,320 / 440,447 | 17 |
| `_sab_cleanupignorescursor` | violation | **RED as required** — `NoAckedLoss` | 61,252,490 / 11,119,725 | 22 |
| `_fix_ckptwitness` (the `_ckpt` witness, now spec §5) | green | **PASS** — `No error has been found` | 869,656,192 / 117,412,972 | 47 |
| `_sab_adoptbeforecommit` | violation | **RED as required** — `NoMissedFold` | 2,508 / 1,052 | 9 |
| `_sab_destroyunderhold` | violation | **RED as required** — `HoldSuppresses` | 29,407,049 / 5,319,021 | 21 |
| `_sab_rebuilddropshold` (THE r8 blocker) | violation | **RED as required** — `HoldSuppresses` | 40,807,540 / 7,288,633 | 21 |
| `_sab_clearholdonabsent` | violation | **RED as required** — `HoldSuppresses` | 5,341,737 / 1,080,030 | 18 |
| `_witness_corruptgap` (FINDING: violation = evidence) | violation = evidence | **VIOLATED as required** — `NoAckedLoss` | 44,813,087 / 7,957,869 | 22 |
| `_ctl_holdsuppresses` (control for the three hold sabotages) | green | **PASS** — `No error has been found` | 1,563,304,998 / 199,946,482 | 56 |
| `_sab_deleteignoresindeg` (the late-+1 window, second half) | violation | **RED as required** — `NoAckedLoss` | 36,211,139 / 6,887,580 | 23 |
| `_v9_safe` (THE GATE) | green | **PASS** — `No error has been found` | 304,739,033 / 46,930,936 | 44 |
| `_v9_hintomission` (the listing returns nothing, ever) | green | **PASS** — `No error has been found` | 36,648,716 / 6,011,454 | 42 |
| `_v9_hold` (corruption + an IDEALIZED detector; upper bound) | green | **PASS** — `No error has been found` | 467,180,743 / 66,503,845 | 46 |

Sabotage state counts are "explored before the violation was found" and vary a little between runs
with parallel workers; the verdict does not. Whole harness: **834 s, 13/13 expectations met.**

## The properties

```
NoMissedFold              == \A k \in everDurable : (k.i <= cursor[k.t]) => (k \in folded)
NoAckedLoss               == deleted => (\A k \in everDurable : EdgeOf[k] # "add")
HoldSuppresses            == ~destroyedUnderHold
ExactlyOnce               == ~dupFlag
LosingCommitAdoptsNothing == (roundOutcome = "lost") => (cursor = csnap)
```

`everDurable` — every ref record ever durably appended — is the oracle; it never shrinks when
cleanup deletes objects, which is what makes both loss properties decisive.

`NoAckedLoss` is deliberately **stronger** than the brief's
`deleted => (\A k \in everDurable : EdgeOf[k] = "add" => k \in folded)`. This model's `EdgeOf` gives
the shared blob exactly one removal — T2's, matching the pre-existing base edge — so an acked add is
never legitimately retracted, and "the blob was deleted while an acked `+1` named it" is the honest
statement of the damage. The weaker form cannot see the case where the `+1` landed after
condemnation and *was* folded yet a pending exact-token delete still fired — which is exactly
what `_sab_deleteignoresindeg` produces, and which the weaker form reports as clean.

`HoldSuppresses` is a sticky-ghost property rather than the brief's
`(\E t : hold[t]) => ~deleted`. That shape is not the property: a hold SET after a legitimate
deletion would falsify it, and a hold DROPPED before an illegitimate one — which is exactly the r8
blocker — would satisfy it. The ghost is set when a destructive step runs while a namespace is
**encumbered**: held, or carrying a *debt* (a hold whose offending position was released without
folding through it). Carrying the debt rather than flagging the drop is what makes the two
hold-dropping sabotages produce real multi-step behaviours instead of tautologies — each must
actually reach a `Condemn` or `Delete` to be caught.

Stated plainly so the greens are not over-read: **`HoldSuppresses` is structurally implied by
`DestructiveGate` in every honest configuration** — the gate's own `~AnyHold` conjunct, plus (since
the fix round) a hold revoking `frontier[t]`, means no honest run can reach a destructive step while
encumbered. Its green rows are therefore a consistency check, not a discovery. The property earns
its keep entirely through its three sabotages, each of which removes a different one of those
guards and immediately produces a counterexample.

## The counterexamples, one per sabotage

Each row is the exact action sequence TLC reported.

**`_sab_skipquietprobe` -> `NoAckedLoss`**
`WAppendStart` -> `EpochSeal` -> `WAppendStart` -> `WAppendDurable` -> `BeginRound` -> `WAppendDurable` -> `WalkStep` -> `ScanComplete` -> `FoldCommitWin` -> `Condemn` -> `BeginRound` -> `ScanComplete` -> `FoldCommitWin` -> `Delete`

T1's slot 1 is an epoch seal and slot 2 is the acknowledged `+1`; T2 appends its `-1`. The round
walks T2 only — nothing forces it to touch T1, because the sabotage lets destruction proceed with
no frontier proof — so the fold sees the `-1` and not the `+1`, in-degree reaches zero, and the
next round deletes. This is r7-1 verbatim: per-namespace cursor immobility is worthless when
in-degree is pool-wide.

**`_sab_cleanupignorescursor` -> `NoAckedLoss`**
`WAppendStart` -> `WAppendDurable` -> `WAppendStart` -> `WRaiseSnap` -> `WAppendDurable` -> `WAppendStart` -> `Cleanup` -> `BeginRound` -> `ProbeAbsent` -> `WAppendDurable` -> `WalkStep` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Condemn` -> `BeginRound` -> `ProbeAbsent` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Delete`

Cleanup deletes T1's log 1 on snapshot coverage alone, while the cursor is still 0. That does not
merely lose a delta — it MANUFACTURES a quiet namespace: the exact `GET cursor+1` now answers
absent, the frontier proof is granted, and the acked `+1` at T1:2 sits behind a gap nothing will
ever walk. `_fix_ckptwitness` is the same configuration with `_ckpt` coverage admitted as a
witness, and it is green — the loss becomes a hold.

**`_sab_adoptbeforecommit` -> `NoMissedFold`**
`WAppendStart` -> `BeginRound` -> `WAppendDurable` -> `WalkStep` -> `ScanComplete`

The shortest counterexample in the harness (9 states deep): the candidate cursor goes live at
`ScanComplete`, so a durable log is already below the cursor before any commit adopted it.

**`_sab_destroyunderhold` -> `HoldSuppresses`**
`WAppendStart` -> `WAppendStart` -> `WAppendDurable` -> `WAppendStart` -> `WAppendDurable` -> `WAppendDurable` -> `HideLog` -> `BeginRound` -> `HintReturn` -> `ProbeAbsent` -> `WalkStep` -> `ScanComplete` -> `FoldCommitWin` -> `BeginRound` -> `ProbeAbsent` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Condemn`

**Two rounds, and that is the point.** Round 1 (states 9-14): the hint returns the witness, the
probe finds the gap below it and HOLDS the namespace — which also revokes the frontier proof that
same absent read had granted. Round 2 (15-20): the hint says nothing about that namespace, so the
identical absent probe now reads as an honest frontier and every namespace is "proven". The hold
is still live and is the only thing standing between that proof and the blob; this toggle removes
it. The hold's value is precisely that it OUTLIVES the round in which detection was possible.

**`_sab_rebuilddropshold` -> `HoldSuppresses`**
`WAppendStart` -> `WAppendStart` -> `WAppendDurable` -> `WAppendStart` -> `WAppendDurable` -> `HideLog` -> `WAppendDurable` -> `BeginRound` -> `HintReturn` -> `ProbeAbsent` -> `WalkStep` -> `ScanComplete` -> `FoldCommitWin` -> `Rebuild` -> `BeginRound` -> `ProbeAbsent` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Condemn`

**The r8 blocker, and now genuinely one round later.** Round 1 (9-14) detects the shape from the
hint's witness and holds. `Rebuild` (15) drops the hold — recording a debt — and, being its own
operation rather than a step inside a round, discards that round's destructive authorization.
Round 2 (16-21) starts clean, its hint no longer mentions the namespace, the same 404 reads as an
honest frontier, and `Condemn` runs over a gap nothing records any more. Spec §7's "REBUILD
carries every hold verbatim" is exactly the missing rule.

**`_sab_clearholdonabsent` -> `HoldSuppresses`**
`WAppendStart` -> `WAppendStart` -> `WAppendDurable` -> `WAppendStart` -> `WAppendDurable` -> `WAppendDurable` -> `HideLog` -> `BeginRound` -> `HintReturn` -> `ProbeAbsent` -> `ProbeAbsent` -> `WalkStep` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Condemn`

Two probes at the same offending position within one round: the first sets the hold, the second
finds it absent AGAIN and the sabotage reads that as permission to clear — restoring the frontier
proof it had revoked. Spec §5 (r9-5) says the opposite: a hold clears only by folding through the
position, never by observing another absent.

**`_witness_corruptgap` -> `NoAckedLoss`**
`WAppendStart` -> `WAppendStart` -> `WAppendDurable` -> `WAppendStart` -> `WAppendDurable` -> `WAppendDurable` -> `HideLog` -> `BeginRound` -> `ProbeAbsent` -> `WalkStep` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Condemn` -> `BeginRound` -> `ProbeAbsent` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Delete`

No `HintReturn` appears in the trace at all: T1 is simply never mentioned by the listing. The
hidden log is unobservable, no witness is ever seen, no hold is ever set, and the namespace is
indistinguishable from a quiet one. This is the residual exposure, not a sabotage — every protocol
rule is honest here.

**`_sab_deleteignoresindeg` -> `NoAckedLoss`**
`WAppendStart` -> `EpochSeal` -> `WAppendStart` -> `BeginRound` -> `WalkStep` -> `ProbeAbsent` -> `WAppendDurable` -> `WAppendDurable` -> `WalkStep` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Condemn` -> `BeginRound` -> `WalkStep` -> `ProbeAbsent` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Delete`

The late-`+1` window. `ProbeAbsent` grants T1's frontier proof, and only THEN does the acked `+1`
become durable — against a LIVE blob, so writer-side rematerialization never triggers. Round 1
condemns on an in-degree of zero; round 2 walks and folds the `+1` so in-degree is 1 again; the
pending delete fires anyway. The acked record is folded at the moment of deletion, which is why
only the strengthened `NoAckedLoss` sees it.

## Non-vacuity of the green runs

Action coverage on the two greens that matter, as `distinct-states-found : transitions`. Reproduce
with `COVERAGE=1 bash docs/superpowers/models/run_deltaintake.sh`, which re-runs exactly these two
configurations under `-coverage 1` after the ordinary pass (off by default: it roughly doubles their
runtime and changes no verdict). Logs land in `tmp/tlc_cov_<cfg>.log`.

| action | `_v9_safe` | `_v9_hold` |
|---|---|---|
| `BeginRound` | 79,786 : 27,063,720 | 202,193 : 34,325,141 |
| `Cleanup` | 10,431,692 : 44,355,096 | 9,921,956 : 46,266,996 |
| `Condemn` | 590,312 : 895,672 | 461,596 : 660,657 |
| `Delete` | 224,221 : 291,688 | 151,333 : 198,447 |
| `EpochSeal` | 675,887 : 12,335,700 | 571,250 : 14,405,741 |
| `FoldCommitLose` | 9,222,404 : 9,933,608 | 14,743,365 : 16,089,352 |
| `FoldCommitWin` | 9,208,036 : 9,933,608 | 9,703,742 : 16,089,352 |
| `HideLog` | 0 : 0 | 6,873,090 : 28,649,063 |
| `HintReturn` | 1,781,512 : 16,824,484 | 0 : 0 |
| `Init` | 1 : 1 | 1 : 1 |
| `NoOp` | 0 : 46,930,936 | 0 : 66,503,845 |
| `ProbeAbsent` | 2,212,827 : 10,250,760 | 3,063,496 : 17,194,478 |
| `Rebuild` | 0 : 0 | 0 : 0 |
| `RevealLog` | 0 : 0 | 1,503,887 : 37,016,737 |
| `ScanComplete` | 8,110,000 : 9,933,608 | 12,681,401 : 16,089,352 |
| `WAppendAbandon` | 0 : 12,335,700 | 0 : 14,405,741 |
| `WAppendDurable` | 1,296,791 : 12,335,700 | 1,369,682 : 14,405,741 |
| `WAppendStart` | 225,635 : 12,335,700 | 178,603 : 14,405,741 |
| `WRaiseSnap` | 723,870 : 69,366,596 | 1,000,648 : 115,490,132 |
| `WalkStep` | 2,147,962 : 9,616,456 | 4,077,602 : 14,984,226 |

What this rules out:

- **The greens are not green because nothing happens.** `_v9_safe` reaches `Delete` 224,221 times
  and `Condemn` 590,312 times: the pipeline runs to completion over and over, and `NoAckedLoss`
  holds anyway. `_v9_hold` still destroys 151,333 times WITH corruption present — so its green is
  not "the hold suppressed everything", it is "the hold suppressed exactly the rounds it had to".
- **Both commit outcomes are exercised in equal measure** (`FoldCommitWin` 9,208,036 vs
  `FoldCommitLose` 9,222,404 in `_v9_safe`), so `LosingCommitAdoptsNothing` is checked against real
  losing rounds rather than a corner — even though no sabotage falsifies it.
- **The fault fires and un-fires.** In `_v9_hold`, `HideLog` contributes 6,873,090 distinct states
  and `RevealLog` 1,503,887 — both the permanent-corruption path and the transient one (where the
  hold clears by folding through) are explored.
- **Sabotage-only actions are correctly unreachable.** `Rebuild` is 0 : 0 in both, `HideLog` /
  `RevealLog` are 0 : 0 in `_v9_safe`, and `HintReturn` is 0 : 0 in `_v9_hold` because
  `HintComplete` replaces the dribbling enumeration with the idealized detector.
- **The scan is exercised as a hint, not as truth.** `_v9_safe` takes `HintReturn` 1,781,512 times
  and `WalkStep` 2,147,962 times, and the fold's output depends only on the latter.

## What the model is

Two namespaces (T1, T2) append contiguous ref records; ONE blob is shared. In-degree is pool-wide:

```
Indeg == BaseIndeg + Cardinality({k \in folded : EdgeOf[k] = "add"})
                   - Cardinality({k \in folded : EdgeOf[k] = "rem"})
```

with `BaseIndeg = 1` — the blob's pre-existing folded edge, r7-1's step 1 — `EdgeOf[T2:1] = "rem"`
(the visible removal) and `EdgeOf[T1:2] = "add"` (the acknowledged `+1` that omission or a dropped
hold can hide). `AddId = 2 > 1` deliberately: proving T1's frontier requires walking *through* a
record below the add, which is the position the corruption fault attacks. Because at most
`BaseIndeg` removals exist in the whole key space, no fold order can drive in-degree negative;
`TypeOK` asserts `Indeg >= 0` rather than assuming it.

**`EdgeOf` is a CONSTANT bound by definition override** (`EdgeOf <- EdgeRoles` in every cfg). A TLC
cfg cannot spell a function literal, and fixing the roles in the module keeps the scenario byte-identical
across configurations, which is what makes each sabotage/green pair a controlled experiment. A
configuration wanting different roles overrides to a different operator.

The round is: `BeginRound` → (`HintReturn` | `WalkStep` | `ProbeAbsent`)\* → `ScanComplete` →
`FoldCommitWin`/`FoldCommitLose` → an idle tail in which `Condemn`/`Delete` may run. Destruction
requires `roundOutcome = "won"` (spec's one-pass round: a losing CAS adopted nothing, so it
accounted nothing and may destroy nothing), a frontier proof for EVERY namespace obtained in that
round, no encumbered namespace, and — for `Delete` — a round boundary since condemnation.

### The three adversaries, and why they are not sabotages

| constant | what it models | in green configs? |
|---|---|---|
| omission-capable hint (the default) | the LIST is a zero-trust hint: any subset, any prefix, may stop at once | yes — `_v9_safe` |
| `HintSilent` | the LIST returns nothing, ever | yes — `_v9_hintomission` |
| `HintComplete` | an IDEALIZED detector: the witness is read from the store, so it is observed the instant it exists | yes — `_v9_hold`, and only there |
| `EnableHiddenHole` | corruption: one above-cursor durable log stops answering point reads, bounded to one key, only where a higher durable id exists | yes — `_v9_hold` |

`HintComplete` earns its "idealized" label the hard way. It first meant "a listing that omits
nothing", seeded once per round — and `_v9_hold` was RED, because even a complete listing is a
SNAPSHOT: TLC found a behaviour where the witness (`T1:3`) became durable AFTER the round's
enumeration, so the probe at the hidden `T1:2` saw no witness and granted a frontier proof. No
listing, however honest, closes that. The constant now reads the store directly, which no
implementation can do, and `_v9_hold` is the resulting UPPER bound: *if* detection were perfect,
the hold makes corruption survivable. `_witness_corruptgap` is the lower bound. The gap between
them is the detection problem, and it is where `CkptWitness` (a durable, non-snapshot witness)
would buy real ground.

`EnableHiddenHole` is the only way the impossible shape is reachable at all, and that is the point:
under v9's own invariants no honest action can create a gap below a witness — contiguity forbids it
and the cursor∧snapshot cleanup gate forbids deleting above the cursor — which is exactly why the
protocol must DETECT one rather than tolerate it. `RevealLog` lets the fault be transient as well as
permanent, so both the "hold clears by folding through" path and the "hold never clears, destruction
suppressed forever" path are explored.

## Discrepancies with the task brief, and why the spec won

The plan's global constraint is that the spec wins over the plan text. Five places where this model
departs from the brief's sketch:

1. **The witness is what the round OBSERVED, not an oracle peek at the store.** The brief's trigger
   is "a probe finds `cursor[t]+1` absent while some durable id `> cursor[t]+1` exists" — omniscient
   about the witness. That detector is not implementable, and worse, it is inconsistent with the
   brief's own r8 control: with an omniscient witness the round after REBUILD would re-detect the
   shape and re-hold, so `_sab_rebuilddropshold` could never go red. The model therefore derives the
   witness from `maxSeen[t]`, the greatest id the round's hint returned — which is what spec §5 means
   by "a 404 below a same-epoch witness", and what makes the hold's durability load-bearing.
2. **`FrontierProof(t)` is a recorded observation, not an oracle peek either.** The brief's
   `([t, cursor[t]+1] \notin durable) \/ probedWalked[t]` is evaluated at `Condemn` time, so a `+1`
   landing *after* the probe would retroactively invalidate the proof and the model could never
   explore the temporal window that spec §5's r9-2 lemma is about. Here `frontier[t]` is set by the
   probe during the round and reset at `BeginRound`, so the late-arrival window is reachable — and
   closed, by the two mechanisms below.
3. **The hint feeds NOTHING into `delta`.** The brief says `PageStep` "feeds ONLY `delta`-candidates".
   Under v9 the fold advances by arithmetic and consumes the hint nowhere (§5); folding a hinted key
   directly would adopt a record whose predecessors are unproven, and it would also *mask* both
   load-bearing controls (T1's hidden add would fold straight out of the listing). `HintReturn`
   therefore only records `maxSeen`.
4. **The allocator derives ids from `everDurable`, not `durable`.** The brief's
   `MaxDurable(t) == Max(DurableIds(t) \cup SealedIds(t))` regresses after cleanup deletes a covered
   log, which would let the writer re-mint a used id. INV-1 says "next id =
   `greatest_applied.ref_sequence + 1`" — applied, not present.
5. **Spec §5's writer-side closure is modelled explicitly.** A writer whose add becomes durable
   against a condemned-or-deleted blob rematerializes it from source (`condemned' = FALSE`,
   `deleted' = FALSE`), per r9-2 variant (a). Without it the late-`+1` window is a false
   counterexample; with it, the window that remains is the one the frontier proof closes. Its side
   effect is finding 3 above: the delete-time in-degree re-check is redundant *in this model*.

Two further departures are additive rather than corrective: `_sab_destroyunderhold` and
`_ctl_holdsuppresses` exist because `HoldSuppresses` would otherwise be a guard nobody had seen
fail, and `_fix_ckptwitness` exists because the finding below deserves a demonstrated mitigation
rather than a paragraph — the spec has since adopted that mitigation.

## The residual exposure, stated plainly

`_witness_corruptgap` is red, and it is the honest boundary of what v9 buys:

> The impossible shape is detectable only in a round whose listing returns an id above the gap.
> A namespace whose above-cursor record was lost to corruption before any such round ran, and which
> the listing then never mentions, is indistinguishable from a quiet one.

`_v9_hold` proves this is about the WITNESS, not about the hold: with detection idealized to a
store read, the identical corruption is survived. And it is not fixable by demanding a better
listing — an enumeration is a snapshot, so even an omission-free one misses a witness that becomes
durable after it was taken. That was the first `_v9_hold` counterexample.

The hold does not help — there is nothing to hold on. The frontier proof does not help — one exact
`GET cursor+1` is exactly what spec §5 prescribes, and it answers absent. Three dispositions, in
increasing cost:

- **Accept.** The precondition is corruption, which already lost a ref record. Spec §1 states the
  trust model as "exact point reads are honest"; a record that is *gone* is not a dishonest read.
  The extra damage — a blob deleted out from under an intact acked `+1` above the gap — is the part
  that is not covered by that argument.
- **`_fix_ckptwitness` (cheap, partial, GREEN in this model) — now spec §5.** Make
  `_ckpt.checkpoint` a second, hint-independent witness: a log above the cursor that `_ckpt` covers
  cannot have been legitimately cleaned, so its absence is the impossible shape. Free — the fold
  already reads `_ckpt` for cleanup ranges — and it converts the entire premature-cleanup class from
  data loss into a hold. It does NOT close the general case: a gap above `_ckpt.checkpoint` in a
  namespace the hint never mentions is still invisible. This model proposed it; the spec adopted it
  in `abd77cd4738` / `33b301eacb8`, limit included. The constant stays so both arms remain separable
  evidence: `_sab_cleanupignorescursor` (off) RED, `_fix_ckptwitness` (on) GREEN.
- **The head-CAS alternative (§10).** An authoritative per-namespace head is the only witness that
  is both durable and always above the frontier. Already recorded there as the north star; this is
  one more entry on its side of the ledger.

Whichever is chosen, the transferable rule this model produced is: **a witness must be a durable
object, never an enumeration.** Both mitigations above satisfy it; no listing discipline can.

## Scoping — what this model deliberately does not cover

- **Two namespaces, one blob, one incarnation.** The catalog, `Creating`/`Removing` races and
  incarnations are `CaRefCatalogCore`'s subject (task 3), while the required pre-fold drain and
  fresh-cut ordering are `CaRefPreFoldDrainCore`'s subject. Namespaces here are always foldable and
  the model consumes an already-fresh post-drain cut.
- **The seal is an opaque slot occupant.** Task 1's hand-off note (`CaRefTableSnapshotLogCore.tla`
  lines 86-96) assigns `ckpt.seal`'s fold-side obligation to this module; what is discharged here is
  the *consumption* rule — a seal occupies a slot, the walk reads it and continues, and it applies as
  a no-op contributing no edge (spec §5 B1/B2, uniform consumption). Seal *grammar* (exactly one seal
  operation per transaction, `prev_epoch_seal` on exactly sequence 1) remains a C++ codec obligation,
  as Task 1 recorded.
- **Two properties are UNFALSIFIED and must be cited as such.** `ExactlyOnce` and
  `LosingCommitAdoptsNothing` are checked in nine and thirteen configurations respectively and have
  never been reported violated — no sabotage in this harness breaks either. `LosingCommitAdoptsNothing`
  is *almost* falsified: `_sab_adoptbeforecommit` does violate it, but `NoMissedFold` breaks one step
  shallower and is what TLC reports, so the red is never attributed to it. Treat both as consistency
  checks rather than as evidence, and do not let the main plan cite them as proven-load-bearing.
- **`ExactlyOnce` has no dedicated sabotage.** It is a secondary oracle that no modelled rule
  breaks: `WalkStep` admits an id into `delta` only when it is above the round's starting cursor, and
  `csnap` is captured atomically at `BeginRound`. It is checked in nine configurations and is the
  property whose *update expression* carried the precedence bug in the previous module, so it is now
  at least correctly written. A sabotage for it would have to model a second concurrent folder, which
  needs per-actor round state this module does not have.
- **`TypeOK` has no sabotage** (house standard: it is a structural well-formedness check, including
  `Indeg >= 0` and `hold[t] <=> holdPos[t] # 0`).
- **Honest REBUILD is the identity on this model's state**, so it is modelled only through its
  sabotaged form. What REBUILD is required to preserve here — cursors and holds — is exactly what
  the sabotage drops. Acquiring the lease, draining against the authoritative adopted parent, and
  taking the later fresh rebuild cut are separate obligations of `CaRefPreFoldDrainCore`.
- **Bounds: `MaxSeq = 3` uniformly.** `MaxSeq = 4` was tried first and the greens did not fit the
  plan's ~10 min ceiling — `_v9_safe` reached 205 M distinct states with a still-GROWING queue at
  the 4-minute mark, and `_ctl_holdsuppresses` took 363 s. The plan's rule is to shrink bounds
  rather than drop properties, so every configuration now runs at 3, which also means every
  sabotage and its control share a bound and no verdict difference can be attributed to one. The
  scenario needs `MaxSeq >= 2` (T1 ids 1 and 2, T2 id 1); 3 leaves one slot of headroom, which the
  counterexamples use for an `EpochSeal`. Every red reported here was ALSO reproduced at
  `MaxSeq = 4` during the search for a workable bound.

## History — the configs this rewrite replaces

| deleted cfg | what it proved | why it is gone |
|---|---|---|
| `_safe` | rev.4 honest intake green: ordered scan, resume-after-returned-key pagination, cleanup gated on cursor ∧ snapshot | superseded by `_v9_safe`. The ordered scan it validated is the thing v9 removes: the fold no longer consumes the listing at all |
| `_sab_resumeskip` | RED on `NoMissedFold` — a page advancing past a durable key without returning it (the opaque-continuation-token over-scan) | **retired by design.** Pagination honesty is no longer load-bearing: hint omission is the modelled NORM, unconditionally, and `_v9_hintomission` proves the fold is correct when the listing returns *nothing at all*. A sabotage cannot break a rule the protocol no longer relies on |
| `_latepred` | RED on `NoMissedFold` — the documented cross-epoch late-predecessor PUT, retained as an expected-fail | **moved to Task 1's flip.** `LatePredecessorPut` is deleted from this module: under INV-1 a straggler is arithmetically at the frontier, so "an old-epoch log appears below an advanced cursor" is unrepresentable here. `CaRefTableSnapshotLogCore_v9_flip_latepred` is where it now lives, and it is GREEN |

`_sab_adoptbeforecommit` and `_sab_cleanupignorescursor` are retained: cursor-adoption atomicity and
the cursor∧snapshot cleanup gate are unchanged rules and both are still load-bearing — the second
one more sharply than before, since under v9 premature cleanup does not merely lose a delta, it
manufactures a quiet-looking namespace.

## Reproduce

```bash
bash docs/superpowers/models/run_deltaintake.sh    # 13 configs, exits nonzero on surprise
```

Logs land in `tmp/tlc_CaRefDeltaIntakeCore_<cfg>.log`.
