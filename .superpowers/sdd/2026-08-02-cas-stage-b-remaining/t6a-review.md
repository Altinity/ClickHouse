# T6a review — commit `477fe702a7a` (`laneg/t6a`, base `f8df7d9a5e8`)

## Verdict: APPROVE-WITH-NONBLOCKING

The commit is sound as production code: the kept instrumentation is correct, exhaustive over the
current walk's exits, free of I/O and of hot-path cost, and the temporary probe is genuinely gone.
The reproduction is real and its numbers check out. The T6 flip prerequisite **is discharged** — but
on a narrower argument than the artifact states, and two pieces of the gate evidence need re-running
on the integrated tree before the flip itself.

Nothing here opens a fix round on `CasGc.cpp`. Findings 1, 4, 5 and 6 are PROSE and belong in
`docs/superpowers/cas/deferred-docs-fixes.md`. Findings 2 and 3 are TEST/EVIDENCE and are discharged
by the integration gate that T6 already runs on MAIN.

---

## 1. The attribution's logic — the enumeration is INCOMPLETE (PROSE, graded FALSE)

**Claim under review:** the post-LIST-append-above-frozen-tail break was the only unproven exit
recording neither an anomaly nor a hold, therefore the measured `0 anomaly(ies), 0 held namespace(s)`
deficits must be that exit.

I walked every exit of `Gc::fold`'s per-namespace walk as of `357cf7b963f^`. The anomaly/hold column
of the artifact's table is right for every exit it lists:

- `hold` itself calls `report.recordAnomaly` before anything else, so `GapBelowWitness`,
  `UnconsumedSealCrossing`, `WitnessDisappeared`, `BodyUndecodable` and `CheckpointUndecodable` all
  record one;
- the manifest-edge barrier records its anomaly inline before setting `fired`;
- the carried-hold arm records `"namespace still held below an unresolved position"`;
- the un-cataloged arm records its own anomaly;
- `intake_unprobed_budget` has its own `LOG_WARNING`;
- the fold abort records an anomaly.

**But the enumeration omits one exit: the walk that never starts.** In the old code `expected` is
left `nullopt` when `cursor == RefTxnId{}`, `listing.logs` is empty, and `checkpoints.life_epochs`
has no entry for the namespace. `while (expected)` is then skipped entirely, `frontier_proven` stays
false, and **no anomaly, no hold and no warning is emitted** — exactly the `0 anomalies, 0 holds`
shape the artifact attributes solely to the frozen tail. Such a namespace *is* a walk target: the
target loop pushes every `live_incarnation` entry, and the `listing->logs.empty()` branch pushes it
with `frozen_tail = nullopt`. The old code's own `frontier_proven` doc comment names this case
("the walk that never started ... no probe is taken, nothing is proved, and fail-closed says
unproven"), so it was visible in the code the spike read.

The claim is therefore **FALSE as written**, and an impossibility claim about a set does not survive
a missing member. What would rescue it is a discriminator the artifact never states:
`readCheckpointWitnesses` GETs `_ckpt` by exact key for every `Live`/`Removing` catalog row (not from
the LIST), and `completeCreation` publishes `_ckpt` strictly before the `Creating -> Live` CAS — so a
`Live` row implies a durable `_ckpt`, and the silent exit needs the residual shapes (a `Removing` row
whose `_ckpt` was already reclaimed, a decoded `_ckpt` without `life_epoch`, or the test-only
`casAdmitEntry` bridge, which `CasGc.h` explicitly says has no `life_epochs` entry). Rare, not
impossible, and unquantified.

**The operative conclusion survives.** That second exit is *also* structurally closed, by the same
commit: in current code `expected` is `nullopt` at loop entry only when `!grounding`, and that arm
emits `LOG_WARNING` plus a hold or an anomaly and sets `CheckpointUnusable`. So under the corrected
enumeration both silent exits are gone, and BENIGN-TRANSIENT still holds. What changes is a T8
criterion the artifact does not list: after the flip, a namespace in that shape now suppresses via an
**anomaly** ("no usable checkpoint") rather than silently — T8 should assert that count is zero too,
not only `unproven` and `probe_budget`.

## 2. Structural closure — CONFIRMED (CODE, no finding)

`Gc::fold`'s walk now opens each iteration with the ceiling test against
`*grounding->committed_through`, and when the ceiling is reached with
`resolved_through == *grounding->committed_through` it sets `frontier_proven = true` — it proves
rather than abandons. `witnessAbove` additionally considers `committed_through`, so a committed
record can no longer be invisible to the witness set. `grounding` is non-null inside the loop by
construction: the two arms above it (`!grounding`, and `grounding && !grounding->committed_through`)
both `expected.reset()`, so the loop cannot be entered with either optional empty. Round work stays
finite because `committed_through` is a round-start snapshot of `_ckpt`.

## 3. The kept instrumentation — CORRECT and effectively free (CODE, no finding)

What it adds: `FoldResult::FrontierUnproven` (a reason per namespace) and
`FoldResult::FrontierDeficit` (nine counters, `count`/`total`/`describe`), plus a `; unproven: …`
clause on the existing suppression log line.

- **Exhaustive over the current walk.** Every break that leaves `frontier_proven` false sets a
  reason: the ceiling arm (`CommittedBelowCursor`), the frozen-tail arm
  (`AppendAboveFrozenTail`), and every `hold`/`fired` path via the single
  `unproven_reason = Held` at the effective-hold site. The pre-loop arms set `NoCatalogEntry`,
  `CheckpointUnusable`, `CheckpointFrontierEmpty`. The loop has no fall-through exit — `while
  (expected)` only leaves via `break`. So `unattributed` should indeed be identically zero, which is
  what makes it a usable fence rather than a catch-all.
- **The sum invariant holds.** `frontier_namespaces` is incremented at exactly two sites (per walked
  namespace, and `+= intake_unprobed_budget`); each contributes to exactly one of `frontier_proven`
  or a bucket. On the abort path the deficit is reset and `fold_aborted` set to the whole count with
  `frontier_proven = 0`, and that reset happens *after* the probe-budget block, so the invariant is
  not broken by ordering. Not a vacuous check: 935 rounds of live evidence exercise it with nonzero
  denominators.
- **Cost.** Zero additional backend requests. Per namespace: one `uint8_t` store. Per round: one
  `total()` (nine adds) and, only when the deficit is nonzero, one `describe()` + one `fmt::format`
  — inside the `if (suppress_destructive)` block, so nothing is allocated on a healthy round. After
  the flip the clause is unreachable on an unsuppressed round by definition (`suppress_destructive`
  is false only when the frontier is complete, i.e. the deficit is zero).
- **Comment policy.** The three added comments state invariant and reason only; no task ids, plan
  refs, review ids or BACKLOG anchors. (Neighbouring pre-existing comments do carry such refs —
  `Review C3`, `Final review F1`, `Stage B (Task 4-C)` — but this commit adds none.)
- **The counterfactual probe is genuinely removed.** `git grep -n "T6A-TEMP" 477fe702a7a -- src/`
  returns nothing, and the diff contains no `T6A` token. The commit touches exactly three files.

## 4. Reproduction — CONFIRMED, with one gap in the proxy (PROSE, graded IMPRECISE)

Re-run against `/home/mfilimonov/workspace/ClickHouse/lane-g/build/t6a_server3.log`:

```
935  "destructive work SUPPRESSED"    all with "— 0 anomaly(ies), 0 held"
705  "(1 of 1 namespace(s) proven"
230  "(80 of 80 namespace(s) proven"
  0  "unproven:"            0  "frontier-probe budget"
935  "T6A-TEMP"            12  T6A-TEMP lines with a nonzero count
```

Every number in the artifact reproduces exactly. The workload is as described: `t6a_create.sql`
creates 80 tables on one CA-local pool (`object_storage_type = local`,
`metadata_type = content_addressed`, `gc_interval_sec = 1`), `t6a_insert.sql` holds 2000 `INSERT`s
over all 80, and `t6a_load.sh` runs four concurrent clients over it.

**The gap:** the 80-namespace pool has zero namespace *creation* churn during the measured rounds —
all 80 tables are created before the load, and `t6a_insert.sql` contains no `CREATE`/`DROP`/
`TRUNCATE`. The reproduction therefore cannot exercise the second silent exit of finding 1, which is
precisely the catalog-row-versus-`_ckpt` race that concurrent creation would produce. This does not
weaken the frozen-tail counterfactual (that predicate demonstrably fires, 12 rounds of it, largest
76 of 80); it means the reproduction is a fair proxy for *append* concurrency and not for *lifecycle*
concurrency, and the artifact presents it as covering the whole question.

## 5. The dead arms — CONFIRMED unreachable, CONFIRMED not fixed (CODE, no finding; disposition correct)

The ceiling test is the first statement of `while (expected)`; between it and both sites, `expected`
is only reassigned on the crossing path, which `continue`s back through the ceiling test, and
`grounding` is never reassigned. So both sites run under `*expected <= *grounding->committed_through`:

- the frozen-tail break's second conjunct `*grounding->committed_through < *expected` is always
  false — the break is dead, and with it the `append_above_frozen_tail` bucket;
- the absent-record arm's `if (*expected <= *grounding->committed_through) hold(...) else
  frontier_proven = true;` — the condition is always true, so the `else` is dead.

Both are still present in the commit, as claimed. Neither is a safety defect: the surviving path is
the stricter one in both cases.

**A third dead arm the artifact does not report:** `const bool catalog_names_this_namespace = true;`
immediately above `if (!catalog_names_this_namespace)`. That arm is unconditionally dead in both the
old and the new code, so `no_catalog_entry` is a second bucket that can never be nonzero. The
artifact's table presents it as a live cause. (PROSE, IMPRECISE.)

## 6. Two further prose imprecisions (PROSE)

- **`checkpoint_unusable` is not the bucket the table describes.** The `!grounding` arm sets
  `CheckpointUnusable` *and*, whenever a walk position exists, calls `hold` — and the effective-hold
  site later overwrites the reason with `Held`. So `checkpoint_unusable` counts only the
  no-cursor variant; the ordinary unreadable-`_ckpt`-with-a-cursor case is reported as `held`. The
  cause is not lost (the hold reason is `CheckpointUndecodable` in the seal, and the arm logs a
  warning naming the namespace), but "every unproven namespace gets exactly one bucket" reads as if
  the bucket names the cause, and here it names the mechanism.
- **Header comment placement.** In `CasGc.h` the paragraph describing `FrontierDeficit` and
  `unattributed` is attached to `enum class FrontierUnproven`, and its last two sentences (the ones
  that actually describe the enum) sit at the bottom of that same block. `struct FrontierDeficit`
  and the `frontier_deficit` member carry no doc of their own, so code intelligence surfaces the
  bucket contract at the wrong symbol.

## 7. Gates (TEST/EVIDENCE — both discharged by T6's own gate, but do not carry these forward)

`t6a_gtest_ca.log` really does report `1826 tests from 253 test suites ran. [ PASSED ] 1826 tests.`
Two record notes, both of which mean this run is **not** the plan's full CA gate:

- **Wrong filter.** `Note: Google Test filter = Cas*:CA*` — the old prefix filter, not
  `utils/cas-gate/generate_cas_suites.sh`'s generated suite list. `CascadeWriteBuffer` appearing in
  the run confirms it is prefix matching. The Global Constraints define the gate as the generated
  filter; the dispatch already anticipated this.
- **Wrong binary — the more serious of the two.** By mtime: `t6a_build2.log` 01:10 (rebuilt
  `CasGc.cpp` + `clickhouse`), `t6a_server3.log` ends 01:21 (and contains 935 `T6A-TEMP` lines, so
  build2 carried the probe), `t6a_ut_build.log` 01:23:25, `unit_tests_dbms` linked 01:23:24, gtest
  01:23–01:25, `t6a_build3.log` 01:26:46. `t6a_ut_build.log`'s 123 steps compile only `gtest_*.cpp`
  and link `unit_tests_dbms` — it never rebuilds `CasGc.cpp`, so the test binary was linked against
  the build2 `libdbms.a`, i.e. **the probe-carrying tree**. `build3` recompiled `CasGc.cpp` (probe
  removed) and relinked only `programs/clickhouse`; `unit_tests_dbms` was never relinked. The
  1826/1826 result is evidence about a tree that differs from the committed one.

The delta is a counter plus a per-round `LOG_WARNING`, so the risk is that the committed tree is
*untested by this run*, not that the run hid a failure. It is discharged for free by T6's own gate
run on MAIN with the generated filter — which must happen on the integrated tree **before** the
flip, not after.

## Disposition

- Integrate `477fe702a7a` into `cas-gc-rebuild` as is.
- Before T6's flip: one full CA gate (`generate_cas_suites.sh` + generated filter) on the integrated
  tree. That single run closes finding 2 and finding 3 together.
- Findings 1, 4, 5 (third dead arm), 6 → `docs/superpowers/cas/deferred-docs-fixes.md`.
- Add to T8's soak criteria: zero `no usable checkpoint` anomalies, alongside the artifact's own
  `unproven == 0` / `probe_budget == 0` / stable-drain criteria.
