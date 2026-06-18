# CaB140DangleMerge — TLA+ results (2026-06-18)

First faithful model to **reproduce** B140-dangle, and to **prove** the fix closes it.

## Model

`CaB140DangleMerge.tla`. Models the THREE separately-durable structures that the prior
faithful model (`CaB140DangleFaithful.tla`, clean over 9.1M states) collapsed into one:

1. **snap EDGES** — `durSnap[gen]` (write-once, persisted).
2. **committed CURSOR** — `gcState.cursor` (set atomically with the committed gen).
3. **journal + trim** — `log` + `logBase` (the live tail; `logBase` = trimmed count).

Plus an **in-memory work-in-progress** fold per leader (`wip[l]`), **discarded on lease
loss** (`GEndRound`), and TWO leaders. Shared blob: `Trees={t1,t2}`, `Blobs={b1}`, both
trees reference `b1` (dedup).

Flag `TrimGated`:
- `FALSE` (buggy): the journal may be trimmed up to **any leader's in-memory fold cursor**
  (`MaxBuggyCursor`) — trim-before-durable.
- `TRUE` (fix): the journal may be trimmed only up to **`gcState.cursor`** (the committed
  snap's cursor).

## Results (MaxLog=3, MaxGen=3, two leaders, shared blob)

The fix has TWO flags: `TrimGated` (trim only up to the committed cursor) and `CursorInSnap`
(the cursor is part of the committed snap identity, committed atomically with its edges; when
FALSE the cursor is a SEPARATE durable object — the handoff KEY FACT — committed via
`GCommitCursor` independently of the edge-pointer `GCommitEdges`).

| `TrimGated` | `CursorInSnap` | cfg | result | distinct states |
|---|---|---|---|---|
| FALSE | FALSE | `m_both_buggy.cfg`  | **INV_NO_LOSS violated** | 0.71M (to CE) |
| TRUE  | FALSE | `m_cursorskip.cfg`  | **INV_NO_LOSS violated** | 0.85M (to CE) |
| FALSE | TRUE  | `m_trimonly.cfg`    | **INV_NO_LOSS violated** | 0.34M (to CE) |
| **TRUE** | **TRUE** | `m_merged.cfg`  | **No error; exhaustive** | **5.33M** |

**Conclusion: both halves of the merge are INDEPENDENTLY NECESSARY and JOINTLY SUFFICIENT.**
- Trim-gate alone (TRUE/FALSE) still dangles — the cursor-skip producer.
- Cursor-in-snap alone (FALSE/TRUE) still dangles — the trim-before-durable producer.
- Together (TRUE/TRUE) the model is clean over 5.33M states (exhaustive, non-vacuous).

## The reproduced counterexample (buggy, 17 states)

`log = [add t2, add t1, rem t1]`, `refs = {t2}` (t2 live, t1 dropped; both → shared `b1`):

1. L1 folds `add t2` (edge `t2→b1`) into its **in-memory** wip.
2. **`GTrim`** advances `logBase` to L1's in-memory cursor (drops `add t2`) — *before L1
   persists*.
3. L1 loses the lease → its wip (holding `t2→b1`) is **discarded**. L2 rebuilds from the
   **committed empty snap**.
4. L2 **GAP-skips** the trimmed `add t2` → `t2→b1` never enters any durable snap.
5. L2 folds `add t1`, `rem t1`, cascade-strips `t1` → `inDeg(b1)=0`.
6. L2 commits (`cursor = AbsLen`, "fully folded"), retires, **deletes `b1` while live `t2`
   references it** → `everLost = {b1}` (INV-NO-LOSS violation).

This matches the soak ground truth (§6, `p9_instr_correlation.txt`): cross-node **adopt**
of a shared blob, the live ref's edge never in the snap, exact-token delete of the
unchanged incarnation.

## The cursor-skip counterexample (`m_cursorskip.cfg`, trim already gated)

`log = [add t2, add t1, rem t1]`, `refs = {t2}` (both → shared `b1`):

1. L1 folds `add t2` (edge `t2→b1`) into its **in-memory** wip (cursor 1).
2. **`GCommitCursor`** publishes the durable cursor = 1 — while the committed edge-pointer
   still points at the empty gen 0 (edges have NOT caught up).
3. **`GTrim`** is gated by the committed cursor (=1), so it trims `add t2` — but the committed
   *edges* never reflected it. The cursor ran ahead of the edges, so the gate over-trims.
4. A leader rebuilds from the committed (empty) snap, **GAP-skips** the trimmed `add t2`,
   folds `add t1`, `rem t1`, cascade-strips `t1` → `inDeg(b1)=0`.
5. `GCommitEdges` publishes those edges; the gen's own extent = AbsLen, so retire fires and
   **deletes `b1` while live `t2` references it**.

This shows the two halves are **intertwined**: a trim-gate is only sound if the cursor it
trusts is coherent with the committed edges — which is exactly what `cursor-in-snap`
guarantees (the cursor is committed atomically with, and as part of, the snap).

## What the model proves

- **Both halves of the approved fix are load-bearing and together sufficient** (table above).
- Fix = (1) the durable snap is one write-once object carrying its own fold cursor, committed
  atomically with its edges (no separate `GCommitCursor`); (2) the journal may be trimmed only
  up to that committed cursor. Neither alone closes the dangle; together, clean over 5.33M
  states.

## Run

```
cd docs/superpowers/models
JAR=../../../tmp/tla2tools.jar
for cfg in m_both_buggy m_cursorskip m_trimonly m_merged ; do
  java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -workers auto -config $cfg.cfg CaB140DangleMerge.tla
done
# m_both_buggy / m_cursorskip / m_trimonly => INV_NO_LOSS violated ; m_merged => clean (5.33M states)
```
