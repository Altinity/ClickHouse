# CaRefDeltaIntakeCore — TLA+ gate results (v9)

Model: `CaRefDeltaIntakeCore.tla`. Gates the GC fold and deletion safety of spec
`2026-07-27-cas-ref-chain-complete-cut-design.md` (v9) — §1 (**blob in-degree is pool-wide**), §5
(fold rules, the **destructive-round frontier proof**, the **durable hold**) and §7 (REBUILD
preserves holds). Task 2 of the plan `2026-07-28-cas-ref-chain-tla-phase.md`; this is a phase-0
gate — it blocks the C++ work.

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
   enumeration: `_fix_ckptwitness` makes `_ckpt.checkpoint` one and is green.
6. **A latent TLA+ precedence bug was found in the module this rewrite replaces.**
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
| `_sab_skipquietprobe` (THE r7-1 blocker) | violation | **RED as required** — `NoAckedLoss` | 1,812,868 / 406,228 | 16 |
| `_sab_cleanupignorescursor` | violation | **RED as required** — `NoAckedLoss` | 66,695,799 / 12,028,740 | 22 |
| `_fix_ckptwitness` (the proposed mitigation) | green | **PASS** — `No error has been found` | 965,553,461 / 131,941,172 | 47 |
| `_sab_adoptbeforecommit` | violation | **RED as required** — `NoMissedFold` | 2,747 / 1,154 | 9 |
| `_sab_destroyunderhold` | violation | **RED as required** — `HoldSuppresses` | 5,465,086 / 1,096,291 | 17 |
| `_sab_rebuilddropshold` (THE r8 blocker) | violation | **RED as required** — `HoldSuppresses` | 10,146,299 / 1,962,570 | 19 |
| `_sab_clearholdonabsent` | violation | **RED as required** — `HoldSuppresses` | 5,655,585 / 1,142,785 | 17 |
| `_witness_corruptgap` (FINDING: violation = evidence) | violation = evidence | **VIOLATED as required** — `NoAckedLoss` | 50,003,552 / 8,929,285 | 22 |
| `_ctl_holdsuppresses` (control for the three hold sabotages) | green | **PASS** — `No error has been found` | 1,569,440,282 / 200,720,938 | 56 |
| `_sab_deleteignoresindeg` (the late-+1 window, second half) | violation | **RED as required** — `NoAckedLoss` | 32,256,210 / 6,224,494 | 23 |
| `_v9_safe` (THE GATE) | green | **PASS** — `No error has been found` | 304,739,033 / 46,930,936 | 44 |
| `_v9_hintomission` (the listing returns nothing, ever) | green | **PASS** — `No error has been found` | 36,648,716 / 6,011,454 | 42 |
| `_v9_hold` (corruption + an IDEALIZED detector; upper bound) | green | **PASS** — `No error has been found` | 503,634,646 / 71,461,063 | 46 |

Sabotage state counts are "explored before the violation was found" and vary a little between runs
with parallel workers; the verdict does not. Whole harness: **869 s, 13/13 expectations met.**

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

## The counterexamples, one per sabotage

Each row is the exact action sequence TLC reported.

**`_sab_skipquietprobe` -> `NoAckedLoss`**
`WAppendStart` -> `EpochSeal` -> `WAppendStart` -> `WAppendDurable` -> `WAppendDurable` -> `BeginRound` -> `WalkStep` -> `ScanComplete` -> `FoldCommitWin` -> `Condemn` -> `BeginRound` -> `ScanComplete` -> `FoldCommitWin` -> `Delete`

T1's slot 1 is an epoch seal and slot 2 is the acknowledged `+1`; T2 appends its `-1`. The round
walks T2 only — nothing forces it to touch T1, because the sabotage lets destruction proceed with
no frontier proof — so the fold sees the `-1` and not the `+1`, in-degree reaches zero, and the
next round deletes. This is r7-1 verbatim: per-namespace cursor immobility is worthless when
in-degree is pool-wide.

**`_sab_cleanupignorescursor` -> `NoAckedLoss`**
`WAppendStart` -> `WAppendStart` -> `WAppendDurable` -> `WAppendStart` -> `WRaiseSnap` -> `WAppendDurable` -> `Cleanup` -> `WAppendDurable` -> `BeginRound` -> `ProbeAbsent` -> `WalkStep` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Condemn` -> `BeginRound` -> `ProbeAbsent` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Delete`

Cleanup deletes T1's log 1 on snapshot coverage alone, while the cursor is still 0. That does not
merely lose a delta — it MANUFACTURES a quiet namespace: the exact `GET cursor+1` now answers
absent, the frontier proof is granted, and the acked `+1` at T1:2 sits behind a gap nothing will
ever walk.

**`_sab_adoptbeforecommit` -> `NoMissedFold`**
`WAppendStart` -> `BeginRound` -> `WAppendDurable` -> `WalkStep` -> `ScanComplete`

The shortest counterexample in the harness (9 states deep): the candidate cursor goes live at
`ScanComplete`, so a durable log is already below the cursor before any commit adopted it.

**`_sab_destroyunderhold` -> `HoldSuppresses`**
`WAppendStart` -> `WAppendStart` -> `WAppendDurable` -> `WAppendDurable` -> `WAppendStart` -> `WAppendDurable` -> `HideLog` -> `BeginRound` -> `ProbeAbsent` -> `HintReturn` -> `ProbeAbsent` -> `WalkStep` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Condemn`

The hold is established from the hint's witness, then a later probe in the same round grants a
frontier proof for the OTHER namespace and the sabotage lets `Condemn` ignore the live hold. The
honest gate refuses; `_ctl_holdsuppresses` is the identical configuration without the toggle.

**`_sab_rebuilddropshold` -> `HoldSuppresses`**
`WAppendStart` -> `WAppendStart` -> `WAppendDurable` -> `WAppendStart` -> `WAppendDurable` -> `HideLog` -> `WAppendDurable` -> `BeginRound` -> `ProbeAbsent` -> `HintReturn` -> `ProbeAbsent` -> `WalkStep` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Rebuild` -> `Condemn`

The r8 blocker. Round 1's hint returns the witness, the impossible shape is detected and T1 is
held. `Rebuild` then rebuilds cursors and drops the hold, and the very next destructive step runs
with the gap still there and nothing recording it. Spec §7's "REBUILD carries every hold
verbatim" is exactly what is missing here.

**`_sab_clearholdonabsent` -> `HoldSuppresses`**
`WAppendStart` -> `WAppendStart` -> `WAppendDurable` -> `WAppendStart` -> `WAppendDurable` -> `WAppendDurable` -> `HideLog` -> `BeginRound` -> `HintReturn` -> `ProbeAbsent` -> `WalkStep` -> `ProbeAbsent` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Condemn`

Two probes at the same offending position: the first sets the hold, the second finds it absent
AGAIN and the sabotage reads that as permission to clear. Spec §5 (r9-5) says the opposite — a
hold clears only by folding through the position, never by observing another absent.

**`_witness_corruptgap` -> `NoAckedLoss`**
`WAppendStart` -> `WAppendStart` -> `WAppendDurable` -> `WAppendStart` -> `WAppendDurable` -> `WAppendDurable` -> `HideLog` -> `BeginRound` -> `ProbeAbsent` -> `WalkStep` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Condemn` -> `BeginRound` -> `ProbeAbsent` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Delete`

No `HintReturn` appears in the trace at all: T1 is simply never mentioned by the listing. The
hidden log is unobservable, no witness is ever seen, and the namespace is indistinguishable from
a quiet one. This is the residual exposure, not a sabotage — every protocol rule is honest here.

**`_sab_deleteignoresindeg` -> `NoAckedLoss`**
`WAppendStart` -> `EpochSeal` -> `WAppendStart` -> `WAppendDurable` -> `BeginRound` -> `WalkStep` -> `ProbeAbsent` -> `WAppendDurable` -> `WalkStep` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Condemn` -> `BeginRound` -> `WalkStep` -> `ProbeAbsent` -> `ProbeAbsent` -> `ScanComplete` -> `FoldCommitWin` -> `Delete`

The late-`+1` window. `ProbeAbsent` grants T1's frontier proof, and only THEN does the acked `+1`
become durable — against a LIVE blob, so writer-side rematerialization never triggers. Round 1
condemns on an in-degree of zero; round 2 walks and folds the `+1` so in-degree is 1 again; the
pending delete fires anyway. The acked record is folded at the moment of deletion, which is why
only the strengthened `NoAckedLoss` sees it.

## Non-vacuity of the green runs

Action coverage (`-coverage 1`) on the two greens that matter, as `distinct-states-found : transitions`:

| action | `_v9_safe` | `_v9_hold` |
|---|---|---|
| `BeginRound` | 65,693 : 9,946,953 | 200,477 : 36,499,761 |
| `Cleanup` | 3,860,998 : 14,796,327 | 10,348,633 : 48,100,669 |
| `Condemn` | 313,662 : 357,795 | 463,313 : 660,657 |
| `Delete` | 22,019 : 24,229 | 153,046 : 198,447 |
| `EpochSeal` | 476,547 : 7,770,103 | 632,560 : 15,244,552 |
| `FoldCommitLose` | 4,651,879 : 4,854,410 | 15,744,765 : 17,480,651 |
| `FoldCommitWin` | 4,659,428 : 4,854,292 | 10,069,242 : 17,480,651 |
| `HideLog` | 0 : 0 | 7,823,335 : 32,259,603 |
| `HintReturn` | 1,306,798 : 9,771,548 | 0 : 0 |
| `Init` | 1 : 1 | 1 : 1 |
| `NoOp` | 0 : 20,493,091 | 0 : 71,461,063 |
| `ProbeAbsent` | 1,345,162 : 5,211,058 | 3,309,493 : 18,163,836 |
| `Rebuild` | 0 : 0 | 0 : 0 |
| `RevealLog` | 0 : 0 | 2,144,078 : 40,188,858 |
| `ScanComplete` | 4,837,583 : 5,690,421 | 13,279,920 : 17,480,651 |
| `WAppendAbandon` | 0 : 6,912,313 | 2 : 15,244,552 |
| `WAppendDurable` | 718,991 : 6,912,192 | 1,413,330 : 15,244,552 |
| `WAppendStart` | 164,908 : 7,769,340 | 216,011 : 15,244,552 |
| `WRaiseSnap` | 456,992 : 32,432,341 | 1,177,714 : 125,884,124 |
| `WalkStep` | 1,572,975 : 6,169,640 | 4,485,143 : 16,797,466 |

What this rules out:

- **The greens are not green because nothing happens.** `_v9_safe` reaches `Delete` 22,019 times and
  `Condemn` 313,662 times: the pipeline runs to completion over and over, and `NoAckedLoss` holds
  anyway. `_v9_hold` destroys MORE (153,046 `Delete`), because the fault gives it more reachable
  states — so its green is not "the hold suppressed everything", it is "the hold suppressed exactly
  the rounds it had to".
- **Both commit outcomes are exercised in equal measure** (`FoldCommitWin` 4,659,428 vs
  `FoldCommitLose` 4,651,879 in `_v9_safe`), so `LosingCommitAdoptsNothing` is checked against real
  losing rounds rather than a corner.
- **The fault fires and un-fires.** In `_v9_hold`, `HideLog` contributes 7,823,335 distinct states
  and `RevealLog` 2,144,078 — so both the permanent-corruption path and the transient one (where the
  hold clears by folding through) are explored.
- **Sabotage-only actions are correctly unreachable.** `Rebuild` is 0 : 0 in both, `HideLog` /
  `RevealLog` are 0 : 0 in `_v9_safe`, and `HintReturn` is 0 : 0 in `_v9_hold` because `HintComplete`
  replaces the dribbling enumeration with the idealized detector.
- **The scan is exercised as a hint, not as truth.** `_v9_safe` takes `HintReturn` 1,306,798 times
  and `WalkStep` 1,572,975 times, and the fold's output depends only on the latter.

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
rather than a paragraph.

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
- **`_fix_ckptwitness` (cheap, partial, GREEN in this model).** Make `_ckpt.checkpoint` a second,
  hint-independent witness: a log above the cursor that `_ckpt` covers cannot have been legitimately
  cleaned, so its absence is the impossible shape. Free — the fold already reads `_ckpt` for cleanup
  ranges — and it converts the entire premature-cleanup class from data loss into a hold. It does
  NOT close the general case: a gap above `_ckpt.checkpoint` in a namespace the hint never mentions
  is still invisible.
- **The head-CAS alternative (§10).** An authoritative per-namespace head is the only witness that
  is both durable and always above the frontier. Already recorded there as the north star; this is
  one more entry on its side of the ledger.

Whichever is chosen, the transferable rule this model produced is: **a witness must be a durable
object, never an enumeration.** Both mitigations above satisfy it; no listing discipline can.

## Scoping — what this model deliberately does not cover

- **Two namespaces, one blob, one incarnation.** The catalog, `Creating`/`Removing` races and
  incarnations are `CaRefCatalogCore`'s subject (task 3). Namespaces here are always foldable.
- **The seal is an opaque slot occupant.** Task 1's hand-off note (`CaRefTableSnapshotLogCore.tla`
  lines 86-96) assigns `ckpt.seal`'s fold-side obligation to this module; what is discharged here is
  the *consumption* rule — a seal occupies a slot, the walk reads it and continues, and it applies as
  a no-op contributing no edge (spec §5 B1/B2, uniform consumption). Seal *grammar* (exactly one seal
  operation per transaction, `prev_epoch_seal` on exactly sequence 1) remains a C++ codec obligation,
  as Task 1 recorded.
- **`ExactlyOnce` has no dedicated sabotage.** It is a secondary oracle that no modelled rule
  breaks: `WalkStep` admits an id into `delta` only when it is above the round's starting cursor, and
  `csnap` is captured atomically at `BeginRound`. It is checked in nine configurations and is the
  property whose *update expression* carried the precedence bug in the previous module, so it is now
  at least correctly written. A sabotage for it would have to model a second concurrent folder, which
  needs per-actor round state this module does not have.
- **`TypeOK` has no sabotage** (house standard: it is a structural well-formedness check, including
  `Indeg >= 0` and `hold[t] <=> holdPos[t] # 0`).
- **Honest REBUILD is the identity on this state**, so it is modelled only through its sabotaged
  form. What REBUILD is required to preserve here — cursors and holds — is exactly what the sabotage
  drops.
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
