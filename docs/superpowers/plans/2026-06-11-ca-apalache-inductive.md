# CA Incarnation Core — Apalache Inductive-Invariant Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Find and machine-check a strengthened **inductive invariant** for the incarnation-token core with Apalache (SMT-backed, one-step induction) — removing the "explored prefix" limitation of TLC for the core no-dangle/no-return safety argument, and laying ~80% of the groundwork for a later TLAPS proof.

**Architecture:** A new, trimmed, fully type-annotated proof module `CaIncarnationProofCore.tla` (the stage-1/2 token core with the faithful `W-REVALIDATE` gate; no trees/debris/split/evidence), cross-validated against TLC first (cheap reachability pre-filter for every candidate lemma), then driven through Apalache's induction recipe with a disciplined counterexample-to-induction (CTI) loop. Negative controls: induction must FAIL when a load-bearing conjunct is removed.

**Tech Stack:** Apalache (latest release, JVM — OpenJDK 21 at `/usr/bin/java` satisfies it), Z3 (bundled), TLC (`tmp/tla2tools.jar`) for cross-validation. All runs from `docs/superpowers/models/`, logs under `tmp/`.

**What induction buys (state honestly everywhere):** the step check quantifies over ALL states satisfying `IndInv` — not just reachable ones — so a green induction is evidence for *every* execution at the fixed constant sizes, regardless of depth. Constants remain finite (parametric/unbounded-constant proofs are TLAPS territory); this is the middle rung, deliberately.

**Sequencing note:** Tasks 0–3 are light and can run anytime. The CTI loop (Task 5+) is SMT-heavy in bursts; if the 4h TLC hunt is still running (32 workers), prefer starting Task 5 after it completes.

---

## Task 0: Install Apalache + runner

**Files:**
- Create: `docs/superpowers/models/run_apalache.sh`

- [ ] **Step 1: Download the latest release** (network required):

```bash
mkdir -p /home/mfilimonov/workspace/ClickHouse/master/tmp/apalache
cd /home/mfilimonov/workspace/ClickHouse/master/tmp/apalache
curl -fL -o apalache.tgz https://github.com/apalache-mc/apalache/releases/latest/download/apalache.tgz \
  || gh release download --repo apalache-mc/apalache --pattern '*.tgz' -O apalache.tgz
tar xzf apalache.tgz --strip-components=1
bin/apalache-mc version
```

Expected: a version banner (0.4x+). If the asset name differs, list assets with `gh release view --repo apalache-mc/apalache` and fetch the `.tgz`. Record the exact version (goes into RESULTS).

- [ ] **Step 2: Runner script** `docs/superpowers/models/run_apalache.sh`:

```bash
#!/usr/bin/env bash
# Run apalache-mc with logging. Usage: ./run_apalache.sh <label> <apalache args...>
set -uo pipefail
if [[ $# -lt 2 ]]; then echo "usage: $0 <label> <apalache args...>" >&2; exit 2; fi
cd "$(dirname "$0")"
APA=../../../tmp/apalache/bin/apalache-mc
[[ -x "$APA" ]] || { echo "apalache not found: $APA" >&2; exit 3; }
LABEL="$1"; shift
LOG="../../../tmp/apa_${LABEL}.log"
"$APA" "$@" >"$LOG" 2>&1
RC=$?
grep -E "Checker reports|The outcome is|Error|error|Counterexample|PASS|FAIL|It took me" "$LOG" | tail -8
echo "exit=$RC log=$LOG"
exit $RC
```

`chmod +x` it. Smoke: `docs/superpowers/models/run_apalache.sh smoke version` → version in log, exit 0.

- [ ] **Step 3: Commit** — `git add docs/superpowers/models/run_apalache.sh && git commit -m "CA model: Apalache runner (jar fetched to tmp/, not committed)"` + the `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` trailer. Do NOT commit the tarball/binaries.

---

## Task 1: The proof-core module `CaIncarnationProofCore.tla`

**Files:**
- Create: `docs/superpowers/models/CaIncarnationProofCore.tla`

Derive it from `CaIncarnationCore.tla` by EXACT surgery — single leader, `W-REVALIDATE` gate only, token-only dependencies. The result is ~14 variables / ~16 actions: small enough for SMT, faithful to the mechanism the implementation will have.

- [ ] **Step 1: Variables kept (14) / dropped.**

KEEP (semantics identical to the main model): `present, tokOf, nextTok, obsoleteTok, man, retired, inflight, gcRound, gcPhase, fencePos, cursor, rootEdges, wDeps, wView` — with `gcPhase` a plain string (single leader) and `fencedSet` a plain `Set(SHARD)` (add it: 15th variable, single-leader form).

> **RENAME (proof core only):** the main model's `deadTok` is named **`obsoleteTok`** here — "tokens
> that stopped being current as a dependency" (by delete, resurrect, OR overwrite), NOT "physically
> deleted". Two reviewers independently misread the old name; the semantics are identical (MR-2).
> Everywhere the main model's docs say `deadTok`, the proof core reads `obsoleteTok`.

> **MR-2 completeness checklist (verify after writing the module):** every action that changes
> `tokOf[h]` or clears `present[h]` obeys the rule — `WResurrect` adds the displaced old token;
> `WOverwrite` adds the displaced old token; `Land` (real delete) adds the deleted current token;
> `WCreate` has no prior token (0 → t, nothing to add). No action may ever add `0` to the history —
> guaranteed because every displacing/deleting action guards on `present[h]` (⇒ `tokOf[h] >= 1`) and
> `TypeBounds` pins `obsoleteTok[h] \subseteq 1..(nextTok[h]-1)`.
DROP (out of proof-core scope, each a recorded residual): trees (`treeEdges, marker, pendCasc, TreeHashes`), debris/heartbeats (`creator, hbAlive, hbSeq, wedged, hbObs`), full-GC walk (`fgPhase, fgCut, fgRefs, fgSeen`), `trimBase` (+ `Trim`, `INV_JOURNAL_COVERAGE`), `roundOf` (single leader: `gcRound` is the active round), all `Enable*`/`Sabotage*` flags (proof core is flag-free; negative controls are invariant-side, Task 6), `Ev`/evidence deps (`wDeps` is token-only: `Set(<<HASH, Int>>)`).

- [ ] **Step 2: Actions kept (16), with these exact changes.**

Writers: `WCreate, WReuse, WResurrect, WReobserve, WRefreshView, WAbandon, WPublish, WDrop, WOverwrite` — `WResurrect`'s condemned-guard and `WPublish`'s gate use **`RetiredHit` only** (no obsolete-token consult anywhere in guards — the faithful re-observation world; `obsoleteTok` remains write-only history for `NoReturn`); `WPublish`'s gate = `\A <<h,t>> \in wDeps[w] : ~RetiredHit(h,t,wView[w]) /\ present[h] /\ tokOf[h] = t` plus `wView[w] >= man[s].fence`; drop `TreeDepsOK` and the `WCreate` child guard (no trees). `WOverwrite` and `WResurrect` keep the `obsoleteTok'` update (the model rule).

> **Why the proof-core gate needs no obsolete-token oracle (state this as a module comment too):**
> this differs from the main model's stage-1 oracle fix *intentionally*. The re-observation conjunct
> `present[h] /\ tokOf[h] = t` blocks a stale dependency on a deleted token (`present = FALSE`) and
> on a displaced/overwritten token (`tokOf # t`) directly — `obsoleteTok` is needed only as the
> `NoReturn` history variable, never as a gate input. This is exactly the spec's `W-REVALIDATE` claim.
GC (single leader, guards minus the `Leaders` quantifier): `GStartRound, GFold, GRetire, GFenceShard, GRecheckDelete, GEndRound, Land` — semantics identical (entry kept until landing; spared drops; cascade branch removed since no trees).

- [ ] **Step 3: Journal record type (the one Apalache-forced deviation).** Apalache rejects heterogeneous record unions, so the main model's `AddRec ∪ FenceRec` becomes a uniform record:

```tla
\* @typeAlias: rec = { op: Str, hs: Set(HASH) };
\* log entry: op \in {"add","rem","fence"}; hs is a singleton for add/rem, {} for fence.
```

`GFold`/`FoldRefs` read the hash via `\E h \in rec.hs : ...`. Semantics identical; assert this in the TLC cross-check (Task 2).

- [ ] **Step 4: Type annotations** (every CONSTANT and VARIABLE — Snowcat requires them). Uninterpreted sorts for identity sets:

```tla
CONSTANTS
    \* @type: Set(WRITER);
    Writers,
    \* @type: Set(SHARD);
    Shards,
    \* @type: Set(HASH);
    Hashes,
    \* @type: Int;
    MaxToken,
    \* @type: Int;
    MaxRound,
    \* @type: Int;
    MaxLog
VARIABLES
    \* @type: HASH -> Bool;
    present,
    \* @type: HASH -> Int;
    tokOf,
    \* @type: HASH -> Int;
    nextTok,
    \* @type: HASH -> Set(Int);
    obsoleteTok,   \* main model's deadTok: tokens that stopped being current (delete/resurrect/overwrite)
    \* @type: SHARD -> { fence: Int, refs: Set(HASH), log: Seq({ op: Str, hs: Set(HASH) }) };
    man,
    \* @type: Set({ h: HASH, t: Int, r: Int });
    retired,
    \* @type: Set({ h: HASH, t: Int });
    inflight,
    \* @type: Int;
    gcRound,
    \* @type: Str;
    gcPhase,
    \* @type: Set(SHARD);
    fencedSet,
    \* @type: SHARD -> Int;
    fencePos,
    \* @type: SHARD -> Int;
    cursor,
    \* @type: Set(<<SHARD, HASH>>);
    rootEdges,
    \* @type: WRITER -> Set(<<HASH, Int>>);
    wDeps,
    \* @type: WRITER -> Int;
    wView
```

- [ ] **Step 5: Constant initializer** (Apalache has no TLC model values). DEFAULT to the boring,
tool-friendly literal form — uninterpreted-type literals are **distinct by construction**:

```tla
CInit ==
    /\ Writers = { "w1_OF_WRITER", "w2_OF_WRITER" }
    /\ Shards  = { "s1_OF_SHARD" }
    /\ Hashes  = { "h1_OF_HASH", "h2_OF_HASH" }
    /\ MaxToken = 3 /\ MaxRound = 2 /\ MaxLog = 4
```

(Do NOT use declared `CONSTANTS w1, w2, ...` of uninterpreted type as the identity values — declared
uninterpreted constants are *not* automatically distinct in Apalache; the literal syntax is. `Gen(n)`
is the alternative if the installed version's manual prefers it — record whichever is used.)

- [ ] **Step 6: Fold operators** (needed by the structural lemmas; Apalache forbids unbounded recursion — use its folds):

```tla
EXTENDS Apalache   \* provides ApaFoldSeqLeft
\* @type: (Set(HASH), { op: Str, hs: Set(HASH) }) => Set(HASH);
ApplyRec(acc, rec) == IF rec.op = "add" THEN acc \cup rec.hs
                      ELSE IF rec.op = "rem" THEN acc \ rec.hs ELSE acc
\* refs derived from a full log
\* @type: Seq({ op: Str, hs: Set(HASH) }) => Set(HASH);
FoldRefs(log) == ApaFoldSeqLeft(ApplyRec, {}, log)
\* prefix fold for the snap lemma
\* @type: (Seq({ op: Str, hs: Set(HASH) }), Int) => Set(HASH);
FoldPrefix(log, n) == ApaFoldSeqLeft(ApplyRec, {}, SubSeq(log, 1, n))
```

These folds are the most likely SMT bottleneck. Known escape hatch (pre-authorized, detailed in
Task 5): if the step check spends its time in `FoldRefs`/`FoldPrefix`, replace the `Seq` journal
with an abstract per-shard published/folded counter pair + an unfolded-adds set — try the sequence
version first, do not spend days optimizing it.

- [ ] **Step 7: SANY-parse** (`tla2sany.SANY`) — must be clean. Commit: `CA model: proof core module for Apalache induction (single-leader token core, W-REVALIDATE gate)` + trailer.

---

## Task 2: TLC cross-validation of the proof core

The proof core must agree with the main model before any SMT effort. TLC is also the cheap pre-filter for every candidate lemma: a conjunct that fails on *reachable* states is simply false — no point asking SMT about it.

**Files:**
- Create: `docs/superpowers/models/CaIncarnationProofCore_tlc.cfg`

- [ ] **Step 1: Config** — stage-2-equivalent bounds, TLC model values: `Writers={w1,w2}, Shards={s1}, Hashes={h1}, MaxToken=3, MaxRound=2, MaxLog=4`; `CONSTRAINT StateConstraint` (add the same `Len(log) <= MaxLog` constraint operator to the module); INVARIANTs: `TypeBounds, NoDangle, NoReturn` (defined in Task 4 — for this task, temporary minimal versions: `NoDangle == \A s \in Shards : \A h \in man[s].refs : present[h]`, `NoReturn == \A h \in Hashes : present[h] => tokOf[h] \notin obsoleteTok[h]`).
- [ ] **Step 2: Run** `timeout 360 docs/superpowers/models/run_tlc.sh ...` — wait: the runner hardcodes `CaIncarnationCore.tla`. Run TLC directly for the proof core:

```bash
cd docs/superpowers/models && /usr/bin/java -XX:+UseParallelGC -cp ../../../tmp/tla2tools.jar tlc2.TLC \
  -metadir ../../../tmp/tlc-meta -workers auto -config CaIncarnationProofCore_tlc.cfg \
  CaIncarnationProofCore.tla > ../../../tmp/tlc_ProofCore.log 2>&1; tail -5 ../../../tmp/tlc_ProofCore.log
```

Expected: PASS, state count in the same order as `reval_stage2` (913,278 ±, the trimmed actions change it — record it). If it VIOLATES: the surgery broke semantics — fix before proceeding (compare against the main model's behavior; do NOT weaken).
- [ ] **Step 3: Commit** config + any module fix.

---

## Task 3: Apalache typecheck + bounded smoke

- [ ] **Step 1:** `docs/superpowers/models/run_apalache.sh typecheck typecheck CaIncarnationProofCore.tla` → exit 0. Fix annotation errors until clean (common: record-access on union types, missing alias, `Gen` placement).
- [ ] **Step 2: Bounded-model-checking smoke** — Apalache and TLC must agree on shallow behavior:

```bash
docs/superpowers/models/run_apalache.sh bmc check --cinit=CInit --inv=NoDangle --length=6 CaIncarnationProofCore.tla
```

Expected: no counterexample (TLC already verified far deeper). A counterexample here = encoding divergence between TLC and Apalache semantics → fix before induction.
- [ ] **Step 3: Commit** any fixes: `CA model: proof core typechecks under Snowcat; BMC smoke agrees with TLC`.

---

## Task 4: IndInv v1 — the candidate inductive invariant

**Files:**
- Modify: `docs/superpowers/models/CaIncarnationProofCore.tla` (add the invariant section)

- [ ] **Step 1: Add the named conjuncts** (verbatim starting set — every one is a fact argued informally in the spec/model work; the CTI loop will strengthen):

```tla
TypeBounds ==   \* domains the types don't carry (Apalache types are unbounded Int/Seq)
    /\ \A h \in Hashes : /\ nextTok[h] \in 1..(MaxToken+1)
                         /\ tokOf[h] \in 0..MaxToken /\ tokOf[h] < nextTok[h]
                         /\ obsoleteTok[h] \subseteq 1..(nextTok[h]-1)
                         /\ (present[h] => tokOf[h] >= 1)
    /\ gcRound \in 0..MaxRound /\ gcPhase \in {"idle","retiring","fencing","fenced"}
    /\ fencedSet \subseteq Shards
    /\ \A s \in Shards : /\ Len(man[s].log) <= MaxLog
                         /\ cursor[s] \in 0..Len(man[s].log)
                         /\ fencePos[s] \in 0..(Len(man[s].log) + 1)
                         /\ man[s].fence \in 0..MaxRound /\ man[s].refs \subseteq Hashes
                         /\ \A i \in DOMAIN man[s].log : /\ man[s].log[i].op \in {"add","rem","fence"}
                                                         /\ man[s].log[i].hs \subseteq Hashes
    /\ \A e \in retired : e.h \in Hashes /\ e.t \in 1..MaxToken /\ e.r \in 1..MaxRound
                          /\ e.t < nextTok[e.h]      \* a retired token was allocated
    /\ \A d \in inflight : d.h \in Hashes /\ d.t \in 1..MaxToken
    /\ \A w \in Writers : wView[w] \in 0..MaxRound
                          /\ \A p \in wDeps[w] : p[1] \in Hashes /\ p[2] \in 1..MaxToken

NoDangle  == \A s \in Shards : \A h \in man[s].refs : present[h]              \* TARGET
NoReturn  == \A h \in Hashes : present[h] => tokOf[h] \notin obsoleteTok[h]   \* TARGET

InflightHeld ==        \* THE lemma: a delete can be in flight only for a held entry.
                       \* NOTE: this depends on the modeling choice that outcome consumption is
                       \* ATOMIC with the delete-message landing (Land drops the matching retired
                       \* entry in the same step). A later refactor to separate outcome logs
                       \* breaks this conjunct's inductiveness — revisit it together.
    \A d \in inflight : \E e \in retired : e.h = d.h /\ e.t = d.t

RetiredCurrentOrDead ==   \* a retired token is still current, or it STOPPED BEING CURRENT and is
                          \* therefore in obsoleteTok (displaced-or-deleted history — the name says
                          \* what it means: obsolete as a dependency, not necessarily deleted).
                          \* WResurrect/WOverwrite add the displaced old token atomically (MR-2) —
                          \* exactly why the resurrect-displacement state does not falsify this.
    \A e \in retired : e.t = tokOf[e.h] \/ e.t \in obsoleteTok[e.h]

InflightVsRefs ==      \* the heart: an in-flight-deletable token is never a referenced current token
    \A d \in inflight : \A s \in Shards : d.h \in man[s].refs => tokOf[d.h] # d.t

SendImpliesFenced ==   \* a delete is sent only after every shard's fence covers its entry's round
    \A d \in inflight : \E e \in retired :
        e.h = d.h /\ e.t = d.t /\ \A s \in Shards : man[s].fence >= e.r

RefsFromLog ==         \* manifest CAS keeps refs and journal atomic — refs IS the full-log fold.
                       \* SCOPE: this is the LOGICAL / pre-compaction manifest (the proof core has
                       \* no Trim). Production manifests trim folded tails: there, refs = fold of
                       \* (checkpoint base + tail). Physical trimming is covered by the TLC model
                       \* (INV_JOURNAL_COVERAGE), not by this proof core — a recorded residual.
    \A s \in Shards : man[s].refs = FoldRefs(man[s].log)

SnapFromPrefix ==      \* the snap is exactly the cursor-prefix fold
    \A s \in Shards : { h \in Hashes : <<s, h>> \in rootEdges } = FoldPrefix(man[s].log, cursor[s])

IndInv == TypeBounds /\ NoDangle /\ NoReturn /\ InflightHeld /\ RetiredCurrentOrDead
          /\ InflightVsRefs /\ SendImpliesFenced /\ RefsFromLog /\ SnapFromPrefix
```

- [ ] **Step 2: TLC pre-filter** — add every conjunct as `INVARIANT` lines to `CaIncarnationProofCore_tlc.cfg` and re-run TLC (Task 2 command). Every conjunct MUST hold on reachable states; one that fails is a false lemma — fix it (the trace tells you how) before any SMT. Record which needed correction.
- [ ] **Step 3: Apalache base case:**

```bash
docs/superpowers/models/run_apalache.sh base check --cinit=CInit --init=Init --inv=IndInv --length=0 CaIncarnationProofCore.tla
```

Expected: PASS (Init satisfies IndInv).
- [ ] **Step 4: Commit**: `CA model: IndInv v1 candidates (TLC-prefiltered) + Apalache base case`.

---

## Task 5: The CTI loop (the real work — iterative, disciplined)

- [ ] **Step 1: The step check:**

```bash
timeout 1800 docs/superpowers/models/run_apalache.sh step check --cinit=CInit --init=IndInv --inv=IndInv --length=1 CaIncarnationProofCore.tla
```

Three outcomes, each with a fixed response:
- **PASS** → IndInv is inductive. Go to Task 6.
- **Counterexample (a CTI)**: a state satisfying IndInv whose successor violates it. Read `tmp/apa_step.log` + the generated counterexample files (Apalache writes `_apalache-out/.../violation.tla`). Classify:
  1. **Missing fact** (the CTI state is unreachable but IndInv doesn't exclude it — by far the most common): identify the fact that excludes it, ADD a named conjunct, TLC-pre-filter the new conjunct (Task 4 Step 2 — mandatory before re-running SMT), re-run the step check.
  2. **Real bug** (the CTI state IS reachable — confirm by TLC-checking the violated conjunct as an invariant; if TLC also violates it, you mis-transcribed a lemma — case 1 in disguise; if the violated conjunct is `NoDangle`/`NoReturn` AND TLC at proof-core bounds passes them, the CTI is unreachable — case 1): a genuine reachable safety violation should usually have been reproduced by TLC within the proof-core bounds — but only if the TLC config exercises the same actions and bounds as `CInit`; compare the two configs before classifying. Treat any apparent "real bug" with extreme suspicion and escalate with the trace rather than patching.
  3. **Abstraction artifact / SMT timeout**: if the step check times out (30 min), first try `--smt-encoding=funArrays`, then shrink `MaxLog = 3` in `CInit`; if the journal folds are the bottleneck, the PRE-AUTHORIZED fallback is to replace the `Seq` journal with an abstract per-shard counter pair (published-count / folded-count) + an unfolded-adds set, re-deriving `RefsFromLog`/`SnapFromPrefix` over it — record the abstraction and re-run Task 2's TLC cross-validation on the abstracted module first.
- **Budget**: up to ~15 CTI iterations or ~3 working hours of SMT. If not inductive by then, STOP and checkpoint: commit the current IndInv state, write up the remaining CTI class honestly (a partially-strengthened invariant + the open CTI is still a deliverable — it documents exactly what a TLAPS effort would face).

- [ ] **Step 2: Keep a CTI journal** — for each iteration, one line in a scratch list (iteration #, violated conjunct, CTI summary, fact added). This goes into RESULTS (Task 7); it is the most valuable artifact for the future TLAPS work.
- [ ] **Step 3: Commit on success or checkpoint**: `CA model: IndInv inductive under Apalache (N CTI iterations)` or `CA model: IndInv checkpoint — inductive modulo <open item>`.

---

## Task 6: Negative controls + implication checks

An inductive invariant can be "inductive" by being wrong (too strong somewhere irrelevant) — prove it has teeth.

- [ ] **Step 1: Implication checks** (the targets follow from IndInv):

```bash
docs/superpowers/models/run_apalache.sh impl1 check --cinit=CInit --init=IndInv --inv=NoDangle --length=0 CaIncarnationProofCore.tla
docs/superpowers/models/run_apalache.sh impl2 check --cinit=CInit --init=IndInv --inv=NoReturn --length=0 CaIncarnationProofCore.tla
```

Both must PASS (trivially — they're conjuncts; this guards against a later refactor splitting them out).
- [ ] **Step 2: Drop-a-lemma controls.** Define in the module two weakened variants, each `IndInv` with exactly one conjunct removed: `IndInv_NoHeld` (minus `InflightHeld`) and `IndInv_NoRefs` (minus `InflightVsRefs`). Step-check each:

```bash
docs/superpowers/models/run_apalache.sh ctlheld check --cinit=CInit --init=IndInv_NoHeld --inv=IndInv_NoHeld --length=1 CaIncarnationProofCore.tla
docs/superpowers/models/run_apalache.sh ctlrefs check --cinit=CInit --init=IndInv_NoRefs --inv=IndInv_NoRefs --length=1 CaIncarnationProofCore.tla
```

EACH must FAIL the step check (a CTI appears) — proving the removed lemma is load-bearing for the induction, not decorative. If one still passes, either the remaining conjuncts imply it (fine — document the implication) or the invariant is over-strong somewhere (investigate before accepting).

- [ ] **Step 2b: Gate negative control (the F1 bug class, the most important control).** Define in the module a weakened publish action and next-relation — flag-free, selected via Apalache's `--next`:

```tla
\* NEGATIVE CONTROL ONLY: WPublish without the re-observation conjunct (the F1 bug class).
WPublishNoReval(w, s, h) ==
    <copy of WPublish with the per-dep conjunct reduced to ~RetiredHit(h, t, wView[w]) —
     the present[h] /\ tokOf[h] = t conjunct REMOVED; everything else identical>
NextNoReval == <copy of Next with WPublish replaced by WPublishNoReval>
```

(Write both out in full in the module — they are mechanical copies with one conjunct removed; mark them with a comment banner so nobody mistakes them for the real protocol.) Then:

```bash
docs/superpowers/models/run_apalache.sh ctlreval check --cinit=CInit --init=IndInv --next=NextNoReval --inv=IndInv --length=1 CaIncarnationProofCore.tla
```

MUST FAIL with a CTI — expected shape: a stale token-bearing dependency (object deleted or displaced after observation) passes the weakened gate and publishes, violating `NoDangle`/`InflightVsRefs`. This is the direct machine-checked witness that the `W-REVALIDATE` re-observation conjunct is what carries F1 in the inductive argument.
- [ ] **Step 3: Sanity that Init is satisfiable** (a contradictory IndInv passes everything vacuously): `--init=IndInv --inv=FalseInv --length=0` with `FalseInv == FALSE` MUST produce a counterexample (= an IndInv state exists). 
- [ ] **Step 4: Commit**: `CA model: induction negative controls — InflightHeld/InflightVsRefs load-bearing; IndInv satisfiable`.

---

## Task 7: Docs + spec linkage

- [ ] **Step 1:** Add an "Apalache induction" section to `CaIncarnationCore_README.md` (how to run: Task 0 install + the three commands) and to `CaIncarnationCore_RESULTS.md`: Apalache version, the final IndInv conjunct list with one-line meanings, the CTI journal table, negative-control outcomes, and the honest scope statement (induction over all IndInv states at fixed constants `|Writers|=2, |Shards|=1, |Hashes|=2, MaxToken=3`; constant-parametric generality = TLAPS, for which IndInv is the prepared input). Respect doc conventions (anchors on any new headers).
- [ ] **Step 2:** Append one sentence to spec §12 "Model status and findings": the inductive-invariant rung (Apalache) result + pointer.
- [ ] **Step 3: Final commit**: `CA model: Apalache induction results — README/RESULTS + spec §12 pointer`.
