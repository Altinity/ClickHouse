# Overnight: S42 OOM resilience + GC round-duration study

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** By morning, produce (a) a verdict on whether CAS survives memory-exhaustion faults, (b) an
evidence-backed answer to why a single GC round can take tens of minutes, and (c) draft fix proposals.

**Architecture:** Measurement-first. Every hypothesis gets a measurement that can REFUTE it, taken from
data the product already records where possible, and from a small added counter only where it cannot.

**Tech Stack:** ca-soak harness (`utils/ca-soak`), the S42 scenario card, `system.content_addressed_-
garbage_collection_log` per-phase rows, `system.blob_storage_log`, `ProfileEvents`.

## Global Constraints

- **Tests and investigations only.** Small edits — a counter, a log line, a test. NO large changes, no
  refactors, no fix implementation. Fix ideas go into a DRAFT document, not into code.
- Every added counter must be registered in `utils/ca-soak/soak/signals.py` or it will not be read.
- Every test written must be verified RED before being trusted.
- Do not touch the HEAD-before-GET protocol pair (standing veto).
- Do not change the in-degree reducer (it is correct; see the todo list §8).
- Redirect build/test output to a log file under `build/`; never dump it to the terminal.

---

## Part A — S42: do we survive memory exhaustion?

### Task A1: Run S42 at scale and record the verdict honestly

**Files:**
- Run: `utils/ca-soak/scenarios/cards/s42_alloc_faults.py`
- Output: `tmp/unattended/s42_scale.log`

**Interfaces:**
- Produces: a verdict of `green` / `inconclusive` / `red` plus the anti-vacuity counts, for Part C.

- [ ] **Step 1: Read the card's parameters and its anti-vacuity gate**

The card refuses to read green if no allocation fault actually occurred: `generic == 0` (client-visible
injected failures plus the `QueryMemoryLimitExceeded` delta) is `inconclusive`, never a pass. Confirm the
knobs: `memory_tracker_fault_probability` armed through driver URL parameters, paired with
`max_untracked_memory=0` so small allocations reach the tracker at all.

- [ ] **Step 2: Run the scenario**

```bash
cd utils/ca-soak/scenarios && python3 run.py --scenario S42 2>&1 | tee ../../../tmp/unattended/s42_scale.log
```

- [ ] **Step 3: Record the three oracles separately**

The verdict rests on a consistency oracle, and each part must be reported on its own — a single "green"
hides which oracle actually ran:
1. post-restart (journal-rebuilt) view identical to pre-restart;
2. fsck `dangling` / `unaccounted` / `stale_edge` clean both sides;
3. the snapshot integrity oracle — and `snapshot_oracle_checked == 0` is **inconclusive, not a pass**.

- [ ] **Step 4: Record the anti-vacuity count**

If `generic == 0`, the run proved nothing about OOM. Say so plainly rather than reporting green.

- [ ] **Step 5: Commit the findings**

```bash
git add docs/superpowers/cas/BACKLOG.md && git commit -m "ca: S42 at scale — OOM-resilience verdict"
```

---

## Part B — Why does a GC round take tens of minutes?

Baseline to explain: `fold_ref_intake` reached **1830 s** on a round folding 404,065 logs; `pending_deletes`
reached **77.2 s** in a single occurrence. Established already: intake cost is
`logs + 2 × edges` round trips at ~0.9 ms each, i.e. **request-bound, not CPU-bound**.

The user's three hypotheses, each with a discriminating measurement.

### Task B1: Hypothesis "repeated work" — how much of the round is re-reads?

**Files:**
- Query: `system.content_addressed_garbage_collection_log` on the ca-soak stand
- Output: `docs/superpowers/cas/BACKLOG.md`

- [ ] **Step 1: Read the seal-read redundancy the product ALREADY counts**

`fold_seal_read` carries a `redundant_reads` metric. Query it per round against `seal_reads`:

```sql
SELECT toUInt64(phase_metrics['seal_reads']) AS reads,
       toUInt64(phase_metrics['redundant_reads']) AS redundant,
       round(100*redundant/nullIf(reads,0),1) AS pct
FROM system.content_addressed_garbage_collection_log
WHERE event_type='Phase' AND phase='fold_seal_read' AND reads > 0
ORDER BY reads DESC LIMIT 20
```

- [ ] **Step 2: Restate the manifest-body redundancy already measured**

39.6% of intake manifest fetches are re-reads (7,565 edges / 4,573 distinct manifests), all
cross-transaction, zero intra-transaction. Convert that to round-time: at 2 round trips per edge and
~0.9 ms per trip, compute the seconds a round-scoped cache would have saved on the 404k-log round.

- [ ] **Step 3: Record both as a single "repeated work" total**

- [ ] **Step 4: Commit**

### Task B2: Hypothesis "unnecessary work" — is any request avoidable at all?

**Files:**
- Read: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp` (`foldManifestEdges`)

- [ ] **Step 1: Establish what each request is FOR**

Per log: 1 GET of the log body — irreducible, the transaction must be read.
Per edge: 1 HEAD + 1 GET of the manifest body. Determine from source WHY the body is needed for a `-1`
edge as well as a `+1`: the reducer needs the blob list to emit per-blob deltas, and that list lives only
in the body.

- [ ] **Step 2: Check whether the `-1` case could use already-known data**

A `-1` edge cancels a `+1` that was folded earlier. If the earlier fold recorded the blob set for that
`(manifest, path)` edge, the `-1` may not need the body at all. Determine whether the in-degree run
already carries enough to cancel without re-reading. **Record the answer either way** — if it does not,
that is a fact about the design, not a defect.

- [ ] **Step 3: Record the HEAD question WITHOUT proposing removal**

The HEAD-before-GET pair is a protocol step under standing veto. Measure its share of round time
(edges × 0.9 ms) and record it as a cost, explicitly not as a proposal.

- [ ] **Step 4: Commit**

### Task B3: Hypothesis "serial where it could be parallel" — is the fold single-threaded?

**Files:**
- Read: `src/Disks/.../Gc/CasGc.cpp` intake loop; `Backend/CasBackend.h` `forEachListedKey`

- [ ] **Step 1: Establish serialism from source**

The intake loop walks logs one at a time, issuing a synchronous GET per log and per edge. Confirm there is
no thread pool, no prefetch, no batching. Quote the loop.

- [ ] **Step 2: Establish it from data**

At ~0.9 ms per round trip and 1.9M requests, a perfectly serial round costs ~1710 s. The observed
`fold_ref_intake` maximum was 1830 s. **If those match, the phase is ~100% serial request latency** and
there is no significant CPU or lock term to find. State the arithmetic.

- [ ] **Step 3: Identify what is independent and therefore parallelisable**

Per-log GETs are independent reads; per-edge manifest GETs are independent reads; the ORDER of application
matters but the FETCHING does not. Record the concurrency limit that already exists elsewhere in the pool
for comparison.

- [ ] **Step 4: Record the ceiling**

If fetching parallelised to N-way, the phase floor becomes ~1710/N seconds plus the merge cost. Give the
number for N = 8 and N = 16 so the proposal in Part C has a size.

- [ ] **Step 5: Commit**

### Task B4: The unexplained phase — `pending_deletes` at 77 seconds

**Files:**
- Query: the per-phase rows; read `CasGc.cpp` `pending_deletes` region

- [ ] **Step 1: Get its request profile from the phase rows' ProfileEvents**

Same decomposition as intake: which counters move, and how many requests per deleted object.

- [ ] **Step 2: Compare against the number of objects deleted that round**

- [ ] **Step 3: Say whether it is the same request-bound shape or something else**

- [ ] **Step 4: Commit**

---

## Part C — Draft fix proposals

### Task C1: Write the draft proposals document

**Files:**
- Create: `docs/superpowers/cas/draft-fixes-20260726.md`

- [ ] **Step 1: One section per proposal**

Each must carry: the measurement that motivates it, the expected saving with its arithmetic, the risk, and
what would REFUTE the proposal. A proposal without a refutation condition is a wish.

- [ ] **Step 2: Mark clearly that these are DRAFTS**

No implementation tonight. The user decides in the morning.

- [ ] **Step 3: Commit**

---

## Self-review notes

- **Spec coverage:** S42/OOM → Part A. GC round duration with the three named hypotheses → B1 (repeated),
  B2 (unnecessary), B3 (serial). `pending_deletes` surprise → B4. Draft fixes → Part C.
- **Constraint check:** no task implements a fix; the only code changes contemplated are a counter or a log
  line, and none is required by the current tasks.
- **Refutability:** B3 step 2 can refute the serialism hypothesis outright if the arithmetic does not
  match; B1 can refute "repeated work matters" if `redundant_reads` is zero and the manifest share is
  small.
