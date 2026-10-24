**B first—now S1/S2—then A, retaining the global janitor, is a sound direction. Rev.2 still has five MAJOR findings.** The checkpoint gate is real; “short-lived tables leak every log” is only conditionally correct, and the new aggregate measurement does not establish that condition.

Source references below are relative to `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`, except where stated otherwise. Review was read-only; no builds, tests, edits, or commits.

## MAJOR findings

### 1. The zero-cleanup rows do not establish the checkpoint diagnosis; many reached a later gate

> “That is the signature of the checkpoint gate…”

> “a large share of this workload’s lives die without a validated triple, which the 66 zero rows support”

The **66 / 6 / 1 distribution reproduces for the first 73 cleanup rows**, but its proposed explanation does not.

`Gc::cleanupRefObjects` can produce zero deletions **after validating a recovery triple and constructing a nonempty cleanup chunk**. Its `authorityHolds` rejects any change to the **whole catalog’s ETag**, including unrelated namespace churn (`Gc/CasGc.cpp:3590–3605`), and the caller immediately returns (`3690–3691`). A valid triple can also yield an empty plan because of the durable cursor, checkpoint boundary, retained predecessor seal, or already-cleaned objects (`Pool/CasRefProtocol.cpp:837–852`).

There is direct evidence of the later gate in this sample: **35 of the 66 zero rows contain `CASOtherGet=1`**. In this function, the catalog revalidation supplies that read; it is reached only after triple validation and construction of a nonempty chunk. For example, [sample line 400](/home/mfilimonov/workspace/ClickHouse/lane-g/tmp/investigation/t4/msan_local/run8/samples/cas_gc_log.tsv:400) records two root GETs, one Other GET, and no deletions. Namespace objects classify as Root, the catalog as Other (`Backend/CasInstrumentedBackend.cpp:119–135`).

**Fix:** Retain the distribution as an observation, but withdraw its attribution to checkpoint absence. Measure separate reasons: no snapshot ID, invalid triple, empty plan, catalog revalidation failure, lease failure, and budget exhaustion. Keep §5’s per-life cohort inspection. This is a new unsupported inference introduced by the “upgraded to measurement” revision, not a rejection of the corrected cohort experiment.

### 2. A’s cold-recovery failure description is contradicted by the recovery code

> “a recovery that finds nothing yields `this_incarnation_terminal = false` and answers ‘present’”

> “The DROP retry … proceeds to a terminal append”

Neither follows when A deletes the recovery inputs.

Both consumers call `ensureRefTableRecovered` before inspecting terminal state (`Pool/CasRefLedger.cpp:5063–5071`, `5146–5155`). That recovery constructs authority from the captured life and calls `chooseRecoveryGrounding` (`829–849`). **A missing `_ckpt` throws `CORRUPTED_DATA`**, rather than producing an empty recovered state (`Pool/CasRefCkpt.cpp:142–151`).

Partial drainage also matters:

- Missing committed logs under an unchanged checkpoint throw `CORRUPTED_DATA` (`Pool/CasRefLedger.cpp:1067–1081`).
- A missing snapshot/witness can throw during base validation; checkpoint disappearance does **not** satisfy the base-restart condition (`863–878`; `Pool/CasRefCkpt.cpp:288–292`).
- Recovery’s outer retry loop treats corruption as non-transient (`Pool/CasRefLedger.cpp:1489–1494`).
- Even if recovery succeeds without terminal evidence, DROP rechecks the catalog and rejects an absent/replaced row **before** terminal append (`5179–5191`).

Concrete interleaving: a cold probe captures the `Removing` row; A erases the row and drains its objects; recovery then encounters an absent checkpoint or partially deleted chain. The probe throws before reaching its conservative `true` branch.

**Fix:** Replace the claimed fallback behavior with these actual outcomes. Test both presence probes and DROP retries against full and partial drainage, including a pause/crash between stream and state cleanup. Establish how callers handle the resulting non-transient corruption exception before accepting open question 3’s default. D1’s rejection is fixed; A’s newly added failure explanation is not.

### 3. Bulk deletion is not physically all-or-nothing

> “a batch is all-or-nothing per request”

> “a chunk that fails leaves its keys for a later page”

The API explicitly permits an attempt to delete some keys before failing: a retry resends the entire chunk, treating already-absent keys as success (`Backend/CasRequests.h:285–289`).

The production S3 implementation examines per-key errors after `DeleteObjects` and throws if failures remain. Other keys may already have been deleted: [S3ObjectStorage.cpp:755](/home/mfilimonov/workspace/ClickHouse/lane-g/src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:755), through line 769. The `NOT_IMPLEMENTED` fallback can likewise delete several individual keys before a later call fails (`Gc/CasGc.cpp:390–397`).

The manifest comment itself acknowledges objects deleted by an exhausted chunk but omitted from accounting (`Gc/CasGc.cpp:1118–1121`).

**Fix:** Say **accounting is updated only after a chunk returns successfully; physical deletion can partially succeed**. Count `chunk.size()` as confirmed deleted-or-absent keys on success, not necessarily newly deleted objects. Test partial success followed by exhausted retries, later reclamation of remaining keys, and continued mutable-key handling when admission remains valid.

### 4. Incarnation uniqueness does not establish that retired prefixes receive no more writes

> “nothing can write under a dead incarnation”

This is stronger than the code guarantees.

A snapshot publisher captures live state, then performs a create at the captured life’s `_snap` key (`Pool/CasRefLedger.cpp:4481–4484`, `4543–4544`). Its admission checks mount generation and local runtime flags (`4451–4455`). Catalog retirement invalidates local runtime state (`691–725`); it cannot cancel a request already sent.

Thus a snapshot PUT can already be in flight when the terminal is folded and the life retired. A’s LIST/drain can finish before that PUT becomes visible. Its later admission failure cannot undo the landed object. The request engine explicitly distinguishes a write landing from reporting it committed (`Backend/CasRequests.cpp:827–835`).

This need not damage a successor: the incarnation-qualified prefixes correctly keep the bytes attached to the predecessor. But it invalidates “no more writes” and the unconditional emptiness expectation.

**Fix:** State the narrower guarantee: **a successor life uses a different prefix; delayed predecessor writes may still create debris**. Add that case to crash/concurrency analysis and backstop justification. Test a publisher landing after drainage; require publisher quiescence for immediate-empty assertions, otherwise assert eventual cleanup after publications settle.

### 5. S1’s proposed acceptance criterion still does not discriminate its diagnosis

> “a 1000-key page drops below 0.5 s”

> “This is a property of the change, not of the workload”

> “that cost is entirely the serial per-key DELETE”

A full page is not a page of 1,000 deletion candidates. The janitor skips catalog-resolvable keys (`Gc/CasNamespaceJanitor.cpp:103`), and the phase also performs authority refresh, maintenance-state reads, LIST, catalog reads, and cursor publication (`Gc/CasGc.cpp:364–367`; `Gc/CasNamespaceJanitor.cpp:28–52`, `148–159`). S1 also retains exact deletes for mutable keys.

The sample already contains full pages with **zero deletes** taking **0.738–0.835 seconds**; see [line 1199](/home/mfilimonov/workspace/ClickHouse/lane-g/tmp/investigation/t4/msan_local/run8/samples/cas_gc_log.tsv:1199) and [line 1527](/home/mfilimonov/workspace/ClickHouse/lane-g/tmp/investigation/t4/msan_local/run8/samples/cas_gc_log.tsv:1527). Batching no deletes cannot remove that cost.

Also, `CASBulkDeleteRequests` increments only after a backend call succeeds (`Backend/CasInstrumentedBackend.cpp:149–154`); it does not count failed physical attempts.

**Fix:** Use a controlled, eligible write-once cohort and compare request attempts and deletion latency for the same candidate mix. Measure LIST/control overhead separately. Treat 0.5 seconds as a workload-specific target, not a correctness or diagnostic threshold. Use phase-scoped attempt counts alongside successful batch counts.

## MINOR findings

### 1. Measurement boundaries and ranges need correction

> “85 … whole run”

> “phase duration, full page | 1.3 - 7.4 s”

> “16-75 namespaces planned”

The current file contains **87 janitor rows and 75 cleanup rows**. Restricting to its first 85/73 rows reproduces 55,285 visited keys, 22,888 deletions, 49 full pages, and the stated cleanup distribution. However, even within that subset:

- Full-page duration is **0.208–8.849 seconds**.
- `namespaces_planned` ranges from **1–268**.
- `namespaces_planned` means `folded.ref_tables.size()`, not namespaces with nonempty deletion plans (`Gc/CasGc.cpp:1186`).

**Fix:** Specify the sample cutoff and correct the ranges. Label ~7 ms as aggregate phase time divided by successful deletions, not isolated DELETE latency.

### 2. “Budget-saturated whenever it fires” overstates the table

> “when it does fire it is budget-saturated”

One nonzero row reports 4,368, below 5,000. Six rows reach the cap, but reaching it alone does not prove additional eligible work remained (`Gc/CasGc.cpp:3684–3701`).

**Fix:** Say “six of seven nonzero phases reached the configured cap”; establish remaining eligible work before calling it the binding limiter.

### 3. S2’s loop description conflicts with its corrected test

> “looping pages while dead candidates and a budget remain”

The proposed test correctly says a page without dead candidates must not stop the loop. The current janitor can advance through an entirely live page (`Gc/CasNamespaceJanitor.cpp:103`, `142–153`).

**Fix:** Remove dead-candidate presence from the continuation condition. Use the page/time budget and explicitly defined cursor-progress/end-of-pass conditions.

### 4. Catalog erasure does not destroy every pointer to the life

> “After that erase the only way to find the bytes is a LIST of the whole namespace root.”

The reconciler returns `retired_lives`, and the adopted seal contains life-keyed rows (`Gc/CatalogLifecycleReconciler.h:32–34`; `Gc/CatalogLifecycleReconciler.cpp:36–39`). Given an incarnation, both per-life prefixes remain constructible (`Formats/CasLayout.h:134–143`).

**Fix:** Describe the actual gap: today’s implementation does not use those retirement candidates for physical cleanup, and loses the transient handoff after the invocation. Similarly, aggregate visited-key totals do not establish “roughly one” traversal; count committed cursor wraps.

## Verified rev.1 fixes and requested contracts

- **Catalog resolution:** `CatalogLifeIndex` indexes **all lifecycle states**. `resolve` returns the unique `NamespaceLifeId`, returns absent only when the physical ID is missing, and throws for duplicate ownership (`Pool/CasRefProtocol.cpp:23–70`). The janitor rejects an ambiguous whole cut before deletion. Missing mandatory catalog storage also throws (`Pool/CasRefCatalog.cpp:64–69`).

- **Retirement provenance:** Correctly fixed. `retired_lives` records observed absence/replacement, not exclusive erase provenance; it can accompany `FencedOut`. `CatalogLifecycleReconcileResult` contains authority status, resolution, retired lives, optional final cut, and deleted count. Generation zero returns `DrainComplete` with no final cut and no retirements (`Gc/CasGc.cpp:4831–4839`).

- **Drain timing and irreversible work:** Correct. It runs before heartbeat work, DEFER, enumeration, and fold (`Gc/CasGc.cpp:469–481`). Eligibility requires `Removing` plus adopted-parent cleanup evidence without a hold (`Gc/CatalogLifecycleReconciler.cpp:28–43`). It already irreversibly erases catalog rows via conditional catalog replacement (`Pool/CasRefCatalog.cpp:514–517`), but performs no namespace-object cleanup.

- **Liveness and leadership:** The withdrawal of guaranteed non-overlap is correct. `admit` captures generation and callback; `gate` samples the callback without refreshing the lease (`Backend/CasRequests.cpp:304–306`, `413–425`). Refreshes must remain outside the cheap, nonthrowing callback. Per-attempt/per-chunk refresh improves stopping behavior; it is not cancellation of an in-flight request.

- **Janitor position and cursor:** Correctly fixed. Suppression derives from the fold; DEFER suppresses deletion and valid-page progress. Ambiguity/suppression retain the cursor; malformed keys, absence, mismatches, and ordinary caught deletion failures do not necessarily prevent advancement. Earlier committed pages remain committed after later liveness loss (`Gc/CasNamespaceJanitor.cpp:63–72`, `134–159`).

- **`deletePrefixWholesale`:** Correctly fixed. Its counter measures visited keys, including absent/mismatched ones; `out_fully_drained` certifies enumeration completion, not emptiness (`Gc/CasGc.cpp:3733–3757`).

- **C and D1:** The substantive corrections stand. Removing lives have recovery consumers. Missing required fold logs produce holds and diagnostics; coverage persists them. `snapshotOf` rejects terminal state, and divergent deterministic fold artifacts are rejected. Those old findings should not be re-raised.

## Answers

**Recommended order:** Yes, as an engineering sequence: S1, then bounded S2 at the existing phase position, then A after correcting its failure analysis and validating the races. Keep the global janitor for historical orphans, interrupted/budget-limited drains, and delayed publications. The measurements do not yet prove that S1 removes the binding capacity limit.

**Checkpoint finding:** `planRefCleanup` really returns an empty plan without its checkpoint argument (`Pool/CasRefProtocol.cpp:824–835`). Production already skips absent snapshot IDs and failed triple validation before calling it (`Gc/CasGc.cpp:3644–3665`). Therefore **a life without a validated recovery triple retains every log through this cleanup path**. Short lifetime does not establish that condition: snapshot triggering uses transaction count or bytes (`Pool/CasRefLedger.cpp:4166–4167`; defaults at `Pool/CasPool.h:290–291`). Nor is this permanent leakage: the dead-life janitor remains another reclaimer.

VERDICT: MAJOR: 5
