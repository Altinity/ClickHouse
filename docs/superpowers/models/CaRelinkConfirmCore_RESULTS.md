# CaRelinkConfirmCore — TLA+ gate results

Model: `CaRelinkConfirmCore.tla`. Gates the **publish-then-confirm relink** protocol of spec
`2026-07-23-cas-fetch-handoff-publish-confirm-design.md` (rev.5), Part B — Task 9 of the plan
`2026-07-24-cas-publish-confirm-and-ref-lane-safety.md`. This is the gate that blocks all of Part B.

Runner: `./run_relinkconfirm.sh <cfg-basename>`. TLC 2 (tla2tools, Java 21),
`java -XX:+UseParallelGC -workers auto`, `CHECK_DEADLOCK FALSE` in every cfg. All fifteen
configurations were re-run on 2026-09-02 against the ref-scoped rule 3 revision (`SabotageTouchBlind`,
`sShape`; `_empty_receivers` runs too but, as before this revision, is not carried in the table below);
the numbers are the real TLC output, not estimates.

Constants unless the row says otherwise: `Receivers = {r1}`, `MaxId = 5`, `MaxRound = 5`,
`MaxHoles = 0`.

## Headline

1. **The confirm protocol is GREEN** — `ConfirmedRelinkNeverDangles` holds exhaustively, all four
   non-vacuity witnesses fire (so it is not green for free), and a fifth witness,
   `W_YesWhilePendingNoop`, is reachable — proving this revision's liveness gain, not the theorem's
   non-vacuity.
2. **Every sabotage violates.** Gate 1's exact-`ManifestRef` equality (rule 5), the touch-scoped
   pending check (rule 3), the poison state (rule 4), the mount-fence/current-writer check (rule 6)
   and the publish-BEFORE-confirm ordering are each *individually* load-bearing.
3. *** **FINDING — `ConfirmedRelinkNeverDangles` is violable INDEPENDENTLY of the confirm
   protocol.** *** Modelling the GC fold cursor honestly (it advances over the records a round
   OBSERVED, and discovery is a paginated `LIST` with no completeness proof) breaks the theorem
   with **every confirm rule intact** and with the weakest possible adversary: **one** incomplete
   page, **once**, in the whole behaviour. See [`_sab_holeylist`](#sab-holeylist). This is the
   BACKLOG release-blocker `{#list-as-journal-dataloss-2026-07-25}` reproduced formally, and the
   confirm protocol cannot repair it. `_main` therefore runs with `MaxHoles = 0`, i.e. it
   **assumes** LIST completeness — an assumption the shipped code does not establish.
   *(2026-07-28: that assumption is what the v9 design removes; this model is deliberately not
   rewritten and `_sab_holeylist` stays red as the historical witness — see
   [What v9 does to this finding](#v9-holeylist-status).)*

## Summary table

| cfg | check | expected | TLC verdict | states (gen / distinct) | depth |
|---|---|---|---|---|---|
| `_main` | `TypeOK` `GraduationIsPhased` `ConfirmedRelinkNeverDangles` `PromotedNeverDangles` | PASS | **PASS** — `Model checking completed. No error has been found.` | 539,101 / 163,384 | 22 |
| `_main2r` (2 receivers, `MaxId = 7`, `MaxRound = 6`) | same four | PASS | **PASS** — `Model checking completed. No error has been found.` | 142,823,666 / 38,972,632 | 28 |
| `_sab_nogate1` | `ConfirmedRelinkNeverDangles` | violation | **VIOLATED (as required)** | 84,264 / 31,479 | 13 |
| `_sab_stalecache` | `ConfirmedRelinkNeverDangles` | violation | **VIOLATED (as required)** | 46,891 / 18,252 | 12 |
| `_sab_touchblind` | `ConfirmedRelinkNeverDangles` | violation | **VIOLATED (as required)** | 46,891 / 18,252 | 12 |
| `_sab_nopoison` | `ConfirmedRelinkNeverDangles` | violation | **VIOLATED (as required)** | 86,759 / 32,747 | 13 |
| `_sab_nofence` | `ConfirmedRelinkNeverDangles` | violation | **VIOLATED (as required)** | 78,818 / 29,716 | 12 |
| `_sab_publishafterconfirm` | `PromotedNeverDangles` | violation | **VIOLATED (as required)** | 77,689 / 30,607 | 11 |
| `_sab_holeylist` (`MaxHoles = 1`, **no confirm rule removed**) | `ConfirmedRelinkNeverDangles` | violation = FINDING | **VIOLATED** | 439,065 / 133,592 | 13 |
| `_witness_confirmno` | `W_ConfirmNo` | violation (reachable) | **VIOLATED (as required)** | 431 / 242 | 6 |
| `_witness_confirmunknown` | `W_ConfirmUnknown` | violation (reachable) | **VIOLATED (as required)** | 47 / 31 | 4 |
| `_witness_confirmyes` | `W_ConfirmYesPromoted` | violation (reachable) | **VIOLATED (as required)** | 106 / 72 | 4 |
| `_witness_delete` | `W_BlobDeleted` | violation (reachable) | **VIOLATED (as required)** | 7,844 / 3,572 | 9 |
| `_witness_yespendingnoop` | `W_YesWhilePendingNoop` | violation (reachable) | **VIOLATED (as required)** | 253 / 157 | 5 |

All runs finish in under 3 s except `_main2r` (10 min 34 s, 39.0M distinct states).

## The theorem

```
ConfirmedRelinkNeverDangles ==
    \A r \in Receivers :
        (Promoted(r) /\ rAnswer[r] = "yes" /\ rDurableBefore[r])
            => BlobsOf(Token) \subseteq LiveBlobs
```

`rDurableBefore[r]` is the model's `ActivationDurableBefore`: recorded AT CONFIRM TIME as
"this receiver's `+1` was already durable" (T1 < T2), not assumed.

`PromotedNeverDangles` is the same statement without the antecedent. In the honest model the two
coincide (a promote is only reachable through a *yes*, and the design's order makes the `+1`
durable first); they separate only under `_sab_publishafterconfirm`, which is precisely how that
sabotage shows the ordering is load-bearing rather than incidental.

## `_main` — GREEN

```
Model checking completed. No error has been found.
539101 states generated, 163384 distinct states found, 0 states left on queue.
```

`_main2r` re-runs the same four invariants with **two** receivers both relinking over the **same
deduplicated blob** (the dedup case is what turns a lost `+1` into a deletion rather than a leak),
plus wider id/round budgets:

```
Model checking completed. No error has been found.
142823666 states generated, 38972632 distinct states found, 0 states left on queue.
```

Why it holds, in one paragraph: a confirm *yes* requires (rules 3, 4, 6) that the sender's
in-memory committed row equals its durable binding at that instant, and (rule 5) that the binding
is exactly the token. So at T2 no `-1` against the token's source edge exists — the sender's edge
is still in the fold set, in-degree ≥ 1, and the blob was therefore never even condemned. The
receiver's `+1` is already durable at T1 < T2, and with complete observation no round can fold the
sender's later `-1` without folding that `+1` in the same round (both sit above their namespace
cursors), so in-degree never reaches 0 and three-phase graduation's sparing rule holds the blob
forever after.

## Non-vacuity (four witnesses, all reachable)

TLC reports a "violation" for a `_witness_*` config exactly when the negated state IS reachable.

- `W_ConfirmYesPromoted` violated ⇒ **the theorem's antecedent is reachable** (promoted, on a
  *yes*, with the activation durable first). Without this, `_main` would pass for free.
- `W_BlobDeleted` violated ⇒ **the consequent is not trivially true**: GC in this model really does
  run condemn → `delete_pending` → physical delete to completion.
- `W_ConfirmNo` violated ⇒ the exact-equality *no* branch really fires (not dead code).
- `W_ConfirmUnknown` violated ⇒ the touch-pending/poison/fence *unknown* branch really fires.

A fifth `_witness_*` row, `_witness_yespendingnoop`, is not a non-vacuity check on this theorem: it
proves reachable the *liveness* this revision buys — a *yes* while an unrelated ref's mutation is
open. See [`_witness_yespendingnoop`](#witness-yespendingnoop) below.

## Sabotages

### `_sab_nogate1` — gate 1's exact-`ManifestRef` equality is load-bearing {#sab-nogate1}

`ConfirmedRelinkNeverDangles is violated`, 13-state counterexample. Rule 5 is degraded to "the ref
NAME is bound to something" — the naive `resolveRef`-by-name confirm rev.2 rejected.

- **S2–S4** `SenderAdmit("m2")` → `SenderDurable` → `SenderApply`: the sender **repoints** the ref
  from the token manifest `m1` to a different manifest `m2`. The durable transaction is a `-1` on
  source edge `s_m1`; the committed row now reads `m2`; the lane is quiescent again.
- **S5–S9** two GC rounds on a complete fold (`GFold`/`GSettle` alternating): `folded = {}` →
  in-degree 0 → **condemn** (round 1, `GSettle` at S6) → **`delete_pending`** (round 2, `GSettle` at
  S8), then a third `GFold` at S9 with nothing new to observe. The blob is not physically deleted
  yet — round 3's `GSettle` is deferred to S13, after the promote.
- **S10** `RPublish`: the receiver's `+1` becomes durable — over a blob GC has already scheduled for
  deletion (condemned and `delete_pending`), though not yet deleted. (No presence probe: promotion
  does not probe tokenless adopted leaves.)
- **S11** `RConfirm` → **"yes"**, because the name is still bound (to `m2`). `rDurableBefore = TRUE`.
- **S12** `RPromote` → the receiver promotes on the false *yes*.
- **S13** `GSettle` (round 3): in-degree still 0 → **physical delete**, `present[b1] = FALSE` — a
  promoted manifest `m1` now references a deleted blob. **Violation.**

With rule 5 intact the same trace answers **"no"** at S11 (`sCacheRef = "m2" ≠ Token`) and the
receiver aborts. This is the ABA the A3 `precommitAdd` mint-tightening exists to make impossible to
re-create at the same identity.

### `_sab_stalecache` — gate 1 rule 3 (the touch-scoped pending check) is load-bearing {#sab-stalecache}

`ConfirmedRelinkNeverDangles is violated`. Rule 3 is dropped, so the confirm reads the committed row
without checking whether an admitted mutation touches the ref.

- **S2–S3** `SenderAdmit` → `SenderDurable`: the removal is **durable**; `sDurableRef = "m2"` while
  `sCacheRef` is still `"m1"` and `sPending = sLeader = TRUE`. This is the post-durable-PUT window
  (spec §Problem 2) — the object is on S3 and GC can fold it, but the in-memory row has not moved.
- **S4–S8** two GC rounds on a complete fold (`GFold`/`GSettle` alternating): `folded = {}` →
  **condemn** → **`delete_pending`**, then a third `GFold` at S8 with nothing new to observe — the
  physical delete is not reached yet (its `GSettle` is deferred to S12). The sender's apply has
  still not happened — the tenure is simply slow (a chunked flush commits several durable
  transactions inside one tenure).
- **S9** `RPublish`: `+1` durable, over a blob GC has already condemned.
- **S10** `RConfirm` → **"yes"** off the stale row (`sCacheRef = "m1" = Token`).
- **S11** `RPromote` → the receiver promotes on the false *yes*.
- **S12** `GSettle` (round 3) → **physical delete**, `present[b1] = FALSE`. **Violation.**

With rule 3 intact S10 answers **"unknown"**: the admitted mutation is `touching` and `sPending` spans
admission to apply, which is `pending` plus `carved` in the code. Rule 3 is written against the
mutation's *scope*, not against the tenure: `_witness_yespendingnoop` reaches a *yes* during a tenure
whose mutation does not touch the ref, and `_sab_touchblind` shows that ignoring the scope re-opens
this very trace.

### `_sab_touchblind` — rule 3 must read the mutation's scope {#sab-touchblind}

`ConfirmedRelinkNeverDangles is violated`, 12-state counterexample, structurally identical to
`_sab_stalecache`'s: rule 3's `quiescent` predicate stays `TRUE` unconditionally under
`SabotageTouchBlind` just as it does under `SabotageStaleCache`, because with only two admitted
shapes a confirm blind to shape can never see a touching mutation as touching.

- **S2–S3** `SenderAdmit("m2")` → `SenderDurable`: a **touching** mutation (`sShape = "touching"`) is
  admitted and its removal becomes durable; `sDurableRef = "m2"` while `sCacheRef` is still `"m1"` and
  the tenure stays open (`sPending = sLeader = TRUE`).
- **S4–S8** two GC rounds on a complete fold — `folded = {}` → **condemn** → **`delete_pending`**,
  then a third `GFold` at S8 with nothing new to observe — the physical delete is not reached yet
  (its `GSettle` is deferred to S12).
- **S9** `RPublish`: the receiver's `+1` becomes durable over a blob GC has already condemned.
- **S10** `RConfirm` → **"yes"**: `sTouches = sShape = "touching" /\ ~SabotageTouchBlind` is `FALSE`
  regardless of `sShape`, so `quiescent` reads `TRUE` and the stale row confirms.
- **S11** `RPromote` → the receiver promotes on the false *yes*.
- **S12** `GSettle` (round 3) → **physical delete**, `present[b1] = FALSE`, while `r1` is `promoted`
  with `rAnswer = "yes"` and `rDurableBefore = TRUE`. **Violation.**

With `SabotageTouchBlind = FALSE` the same admission sequence sets `sTouches = TRUE` while `sPending`,
so S10 answers **"unknown"** instead — this is the counterexample `_sab_stalecache`'s closing
paragraph points to as "re-opened" by blindness to scope.

### `_witness_yespendingnoop` — the liveness gain is reachable {#witness-yespendingnoop}

`W_YesWhilePendingNoop is violated` (a TLC "violation" here means the negated state IS reachable),
5-state witness trace — **the liveness this revision buys**, and without it a green `_main` could
still be the old table-wide rule in disguise:

- **S2** `SenderAdmitNoop`: a mutation of **another** ref is admitted — `sShape' = "noop"`,
  `sPending' = TRUE`, `sLeader' = TRUE` — while the queried ref's `sDurableRef` stays `Token`.
- **S3** `SenderDurable`: the noop tenure's transaction becomes durable (`NsNoise`'s edge-neutral
  `op = "noop"` record, `src = "noise"`); `sDurableRef` is unchanged because the shape is `noop`. The
  tenure is still open — this is the post-PUT, pre-install window the design opens for an untouched
  ref.
- **S4** `RPublish(r1)`: the receiver's `+1` becomes durable.
- **S5** `RConfirm(r1)` → **"yes"**: `sTouches = FALSE` because `sShape = "noop"`, so rule 3's
  `quiescent` is `TRUE` although a tenure is open and durable-but-unapplied.
  `sawYesWhilePendingNoop' = TRUE` because `ans = "yes" /\ sPending /\ sShape = "noop" /\ NoopDurable`
  all hold at this instant.

Under the pre-revision table-wide rule 3 (`~sPending /\ ~sLeader`, no shape distinction) this state is
unreachable: any open tenure — touching or not — refuses the confirm. This trace is the formal record
that the ref-scoped rule 3 admits strictly more legitimate *yes* answers than the table-wide rule did,
without opening `_sab_touchblind`'s hazard.

### `_sab_nopoison` — gate 1 rule 4 is separately load-bearing {#sab-nopoison}

`ConfirmedRelinkNeverDangles is violated`, 13-state counterexample. This is **not** covered by
rule 3, which is why it needs its own sabotage:

- **S2–S3** admit → durable removal.
- **S4** `SenderPoison`: the in-memory apply **throws** on a durable transaction. The tenure closes
  — `sPending = FALSE`, `sLeader = FALSE`, so **the lane is genuinely quiescent** and rule 3 sees
  nothing wrong — but `sCacheRef` stays permanently `"m1"`.
- **S5–S9** GC folds the `-1`, condemns, graduates.
- **S10–S12** publish, confirm → **"yes"** off the poisoned row, promote.
- **S13** `GSettle` physically deletes `b1`. **Violation.**

With rule 4 intact S11 answers **"unknown"**. Note the ordering caveat the spec states: the marker
is only a safety net for the *confirm*; it is A1 (no-throw install) that stops the state arising.

### `_sab_nofence` — gate 1 rule 6 is load-bearing {#sab-nofence}

`ConfirmedRelinkNeverDangles is violated`, 12-state counterexample.

- **S2** `FenceLoss` — this instance is no longer the namespace's single writer.
- **S3** `ForeignRemove` — the namespace's *new* writer durably removes the binding. The deposed
  instance's committed row never learns: `sCacheRef = "m1"`, `sDurableRef = "none"`, and the lane is
  perfectly quiescent and unpoisoned.
- **S4–S8** GC folds the foreign `-1` through two rounds (**condemn**, **`delete_pending`**), with a
  third `GFold` at S8 observing nothing new. **S9** publish; **S10** confirm → **"yes"** off a view
  that is authoritative for nobody; **S11** promote; **S12** `GSettle` (round 3) → physical delete.
  **Violation.**

With rule 6 intact S10 answers **"unknown"**.

### `_sab_publishafterconfirm` — "publish, THEN confirm" is load-bearing {#sab-publishafterconfirm}

`PromotedNeverDangles is violated`, 11-state counterexample. The order is inverted: confirm and
promote first, publish the `+1` afterwards.

- **S2** `RConfirm` from `init` → **"yes"** (the sender is genuinely still bound to `m1`, everything
  quiescent). `rDurableBefore = FALSE` — the receiver has no durable evidence yet.
- **S3–S4** the sender admits and durably repoints (no apply/poison step follows in this trace; the
  tenure stays open).
- **S5** `RPromote`: the receiver promotes on the earlier *yes* — nothing about the sender's lane
  gates a promote once confirmed.
- **S6–S11** three GC rounds on a complete fold: **condemn** (`GSettle` at S7), **`delete_pending`**
  (`GSettle` at S9), **physical delete** (`GSettle` at S11), `present[b1] = FALSE`. **Violation** of
  `PromotedNeverDangles`.

Note the *guarded* theorem is NOT violated here — its `rDurableBefore` antecedent is false — which
is exactly the point: a *yes* is only worth anything if the receiver's evidence was already durable
when it was given. A confirm taken before T1 proves liveness at a moment the receiver was not yet
protecting anything, and the protection gap is real.

### `_sab_holeylist` — *** THE FINDING: not a confirm sabotage *** {#sab-holeylist}

**Every confirm rule is intact in this configuration.** What changes is `MaxHoles = 0 → 1`: exactly
**one** round in the whole behaviour may return an incomplete `LIST` page; every other round is
perfectly complete. That is the weakest adversary that models the shipped discovery mechanism
(`CasGc.cpp:829,1033`, "the durable cursor advances per FULLY folded log" at `:1035-1037`; no
contiguity or gap check exists anywhere in the fold; the listing API offers a continuation cursor
but no snapshot token or high-water mark).

`ConfirmedRelinkNeverDangles is violated`, 13-state counterexample:

| state | action | effect |
|---|---|---|
| S2 | `RPublish(r1)` | receiver's `+1` on the deduplicated blob `b1` is **durable**, txn id **1**, namespace `r1` |
| S3 | `NsNoise(r1)` | a later, **edge-neutral** transaction in the SAME namespace, txn id **2** (an owner transition on some other ref — the RCA's record `H`) |
| S4 | `RConfirm(r1)` | **"yes"** — legitimately: the sender is still bound to `m1`, lane quiescent, unpoisoned, fenced. `rDurableBefore = TRUE` |
| S5–S6 | `SenderAdmit` / `SenderDurable` | the sender's `-1` on `s_m1`, txn id **3** |
| S7 | `RPromote(r1)` | the receiver promotes on a correct *yes* |
| **S8** | **`GFold` — the one holey page** (`holes: 0 → 1`) | observes **{id 2, id 3}** and **omits id 1**. `cursor[r1]: 0 → 2` — **past the omitted `+1`** — and `cursor[ns_s]: 0 → 3`. `folded = {}`: the sender's edge is removed, the receiver's edge was never added |
| S9 | `GSettle` (round 1) | in-degree 0 → **condemn** `b1` |
| S10–S11 | `GFold` (complete) / `GSettle` (round 2) | id 1 is now **below** `cursor[r1] = 2`, so it is never in `Avail` again — the complete pages cannot recover it. → **`delete_pending`** |
| S12–S13 | `GFold` (complete) / `GSettle` (round 3) | → **physical delete**, `present[b1] = FALSE`, while `r1` is `promoted` with `rAnswer = "yes"` and `rDurableBefore = TRUE`. **Violation** |

This is the BACKLOG blast-radius scenario mechanised: a durable `+1` on a **deduplicated** blob is
skipped permanently, the other owner's `-1` folds normally, GC sees zero edges and deletes by exact
token a blob a committed manifest still references. Three-phase graduation does **not** help — its
sparing rule can only spare what a fold shows it, and this `+1` will never be folded again.

Two things this trace establishes beyond the BACKLOG prose:

- **One page, once, is enough.** The `MaxHoles = 1` budget rules out every "GC was merely lazy"
  trace, so the counterexample is forced through the permanent skip. There is no "a later round
  will catch it" recovery.
- **The confirm protocol is orthogonal to it.** The *yes* at S4 was correct at the moment it was
  given; the deletion is caused entirely below the protocol, by GC's journal discovery. No
  strengthening of gate 0 or gate 1 changes this trace.

Consequences for the plan: `_main` (and therefore Part B's TLA+ gate) is **conditional on LIST
completeness**. Until codex's fix items 1–2 land (an authoritative per-namespace `prev_txn_id`
chain, and destructive GC conditional on an exact cut meet with global suppression on any
unprovable namespace), `_main` should be read as "the confirm protocol adds no new dangle path",
not as "a confirmed relink cannot dangle".

### What v9 does to this finding — and why this model is NOT rewritten {#v9-holeylist-status}

*Added 2026-07-28 by the v9 ref-chain TLA phase audit (`2026-07-28-v9-phase-RESULTS.md`).*

The v9 design `2026-07-27-cas-ref-chain-complete-cut-design.md` is the fix for exactly the defect
`_sab_holeylist` reproduces. **This model is deliberately left as it stands**, for two reasons:

1. **Its subject is unchanged.** What `CaRelinkConfirmCore` gates is the publish-then-confirm relink
   protocol — a durable `+1` before the confirm, gate 1's six rules, three-phase graduation. v9
   changes none of that. Rewriting the fold cursor here would re-verify v9's fold in a model whose
   confirm machinery makes the state space large, and would delete the one place where the
   historical defect is written down as an executable trace.
2. **`_sab_holeylist` is the historical defect witness and stays red.** It is the formal record of
   what the shipped GC does today, kept so the regression can never be re-litigated as prose. Its
   `MaxHoles = 1` adversary is the *pre-v9* discovery mechanism by construction, so its red says
   nothing about v9 and must not be read as a v9 failure.

**The fold-side flip lives in the two new-in-phase models, and they form the regression pair:**

| config | colour | what it pins |
|---|---|---|
| `CaRefTableSnapshotLogCore_sab_scanistruth` | **RED** (`INV_RECOVERY`) | the pre-v9 premise — a listing/scan treated as truth — still breaks recovery. The defect class is still detectable. |
| `CaRefDeltaIntakeCore_v9_hintomission` | **GREEN** (all invariants incl. `NoAckedLoss`) | under v9 the same omission is harmless: the enumeration is a zero-trust HINT, fold work advances by arithmetic (`cursor + 1`), and destructive work needs the per-namespace frontier proof. |

Read together: the red proves the property is not vacuous and the green proves v9 closes it. Neither
alone would be evidence — a green whose sabotage was never seen red is a green for free, and a red
with no green beside it is a defect with no fix. The `_main` caveat recorded above ("read as *the
confirm protocol adds no new dangle path*, not as *a confirmed relink cannot dangle*") therefore
stands **for the shipped code** and is lifted only for the pool the v9 chain governs, once the v9
implementation lands — the models gate the design, not the code.

## Modelling decisions, and what the bounds cost

- **One deduplicated blob.** `Blobs = {"b1"}`; the sender's manifest `m1` and the receiver's
  relinked manifest are two owners of the same content-addressed token — which is exactly the
  configuration in which a lost `+1` becomes a deletion rather than a leak. `m2` (the repoint
  target) has no blobs in the universe: all that matters about it is that it does not reference
  `b1`. Modelling more blobs would multiply states without adding a distinct failure mode.
- **The initial edge is pre-folded history.** `folded` starts as `{[b1, s_m1]}` with an empty
  journal, so the model starts from a steady state rather than spending rounds establishing one.
- **In-degree is a set-presence merge keyed by (blob, source_id)**, applied in strict txn-id order
  (`ApplyOrdered`), matching `CasBlobInDegree.cpp:585-597` — including that a `del` whose key is
  absent is a silent no-op.
- **`GFold` and `GSettle` are separate steps**, so a `+1` that becomes durable after a round's fold
  cut is invisible to that round's settlement. That window is real and the model keeps it.
- **The sender's ref lane is split into five steps** (`SenderAdmit` | `SenderAdmitNoop`,
  `SenderDurable`, `SenderApply` | `SenderPoison`) precisely so the post-durable-PUT window and the
  poisoned-apply state are reachable and separately observable by the gate, for both admitted shapes.
- **`NsNoise` is edge-neutral** (`op = "noop"`): it changes nothing about reachability. Its only
  role is to give the fold cursor a higher same-namespace id to advance to, which is what converts
  an omitted page entry into a permanent skip. It exists in **all** configs, including `_main` —
  it is not machinery added only to make the sabotage fire.
- **The receiver's abort is durable and releases protection** (a `-1` on its own edge), so a
  refused relink does not accidentally keep blobs alive and mask the theorem.
- **Bounds** `MaxId = 5`, `MaxRound = 5`, one receiver in `_main`. `MaxRound = 5` gives one round of
  slack over the three needed for condemn → `delete_pending` → delete. `_main2r` re-checks the
  universal quantifier at two receivers with `MaxId = 7`, `MaxRound = 6` (39.0M distinct states);
  nothing was shrunk to make a config pass.

### Deliberately out of scope

- **Gate 0** (part-anchored fast filter). Rev.5 demoted it to an availability filter that authorizes
  nothing — every *yes* is gate 1's — so it has no safety content to gate. Modelling it could only
  produce a false sense that a *yes* has two independent sources.
- **Gate 1 rule 2** (warm / `recovered` / not `superseded_by_remount`). Streaming recovery publishes
  atomically (`CasRefLedger.h:492`), so a half-recovered view is not observable; the rule's residual
  content is availability (answer *unknown* rather than doing I/O), not safety.
- **Multi-leader GC**, the condemn-marker durability gate, and fold clamping. Gated elsewhere
  (`CaRetiredInRunFoldAbortWitness.tla`, `CaGcCondemnMarkerGate.tla`,
  `CaRefFoldClampRecoveryCore.tla`); folding them in here would multiply the state space without
  touching the confirm predicate.
- **Liveness.** No fairness constraints and no temporal properties: every claim in the spec's
  failure taxonomy is about what a *yes* authorizes, and the abort-uncertain wedge semantics are
  explicitly "fail-long over-protection", which is a safety-preserving outcome by construction.
