**B’s page settings first, then A, retaining the global backstop is a reasonable direction. B as written is not sound:** its phase reorder lacks the existing destructive gate, and its experiment cannot distinguish the proposed diagnosis.

**The checkpoint finding is correct conditionally:** without a validated recovery triple, covered-ref cleanup retains every log. Short lifetime alone does not establish that condition, and the logs remain reclaimable by the janitor.

Source paths below are relative to `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`.

## MAJOR findings

### 1. `retired_lives` does not prove this actor erased the row

> “`retired_lives` already carries the exact `NamespaceLifeId` of every row the drain erased this round.”

> “it was erased by this tenure, under this lease, moments earlier”

`CasRefCatalog::deleteCompletedRemovingAtSnapshot` produces `invalidated_life` whenever its resolution snapshot shows the old incarnation absent or replaced. It does not require proof that this actor’s write committed: see `Pool/CasRefCatalog.cpp:474–484`, `508`, and `525–536`.

`CatalogLifecycleReconciler::reconcile` appends that value to `retired_lives` independently of whether the outcome is `Deleted` (`Gc/CatalogLifecycleReconciler.cpp:99–118`). The result may also contain retirements when it returns `FencedOut`.

**Fix:** Describe these as exact lives **observed absent or replaced during reconciliation**. A should require `Authoritative`, `DrainComplete`, and confirmation of physical-life absence in an unambiguous `final_catalog_cut`. Remove the stronger claim of exclusive deletion provenance.

### 2. The proposed liveness callback does not prevent overlapping GC actors

> “wrap the drain in one `CasOperation` with the same liveness lambda the janitor uses, so a lost lease stops it mid-way”

> “Two GC actors cannot overlap”

The janitor refreshes authority **once per page**, then supplies `[this] { return authority_held; }` (`Gc/CasGc.cpp:364–367`). That flag changes only when `refreshAuthority` reads `gc/state` (`4692–4703`).

`CasOperation::gate` samples the callback; it does not refresh the underlying lease (`Backend/CasRequests.cpp:413–425`). The GC request plane has an open fence (`Pool/CasPool.h:761–765`).

Consequently, actor A can refresh successfully, pause, lose leadership to B, then continue deleting with its cached `true`. B can concurrently run its janitor against the same retired prefix. Reconciliation avoids carrying one authority observation across its catalog erase attempts by explicitly refreshing each attempt (`Gc/CasGc.cpp:4848–4858`); the proposed physical drain does not specify that.

Furthermore, sampling cannot cancel a delete already in flight.

**Fix:** Specify authority refresh boundaries outside the cheap, nonthrowing callback, and cover takeover between refresh and submission and during in-flight requests. If non-overlap is required, provide fencing or serialization that actually guarantees it; the existing callback is insufficient. Test real lease replacement, not merely a manually flipped callback.

### 3. B’s phase reorder changes the destructive gate

> “Invariants. Untouched — same license, same authority sampling, same cursor discipline per page.”

The current janitor receives `folded.suppress_destructive`, which becomes available only after `fold` (`Gc/CasGc.cpp:664–670`, `1176`). It includes anomalies, carried holds, and incomplete frontier coverage (`3203–3204`).

Deferred rounds explicitly suppress both deletion and cursor progress because they have no fold verdict (`625–631`). Reading the janitor’s own catalog cut does not reproduce this gate.

**Fix:** Make the first B change page settings and looping **at the existing phase position**. Treat earlier execution as a separate change requiring an explicit deletion policy and safety argument. Calling its invariants unchanged is factually incorrect.

### 4. `Removing` lives still have recovery readers

> “nothing will ever recover it”

> “Does the `_snap` tail of a dead life have any reader at all? Default: no”

`namespaceStillLogicallyPresent` explicitly recovers a `Removing` life to prove that its terminal transaction landed (`Pool/CasRefLedger.cpp:5051–5068`).

Likewise, `dropNamespaceImpl` acquires and recovers the observed life before deciding that a retry of removal is complete (`5143–5155`). `namespaceFilesLifeIfReadable` describes namespace-file access, not every recovery consumer.

D1 can therefore erase a required checkpoint/snapshot/log chain while a cold DROP retry or presence probe still needs it.

**Fix:** Reject D1 on this concrete recovery requirement. Preserve recovery evidence until catalog retirement, or separately redesign those consumers. For A, distinguish new lookups from callers already holding an old runtime: the latter have a documented stale-or-not-found contract (`Pool/CasRefLedger.h:233–237`).

### 5. C misstates the missing-log failure as silent

> “a permanent, silent storage leak … and a fold that may `CORRUPTED_DATA` on an expected-but-absent log”

The expected committed-log absence path sets a `GapBelowWitness` hold and stops intake (`Gc/CasGc.cpp:2565–2585`). The `hold` helper records an anomaly, emits `GcFoldClamp`, logs an error, and retains the cursor below the missing record (`2409–2429`). Holds persist in fold coverage (`Formats/CasFoldSealFormat.h:88–105`, `128–144`).

The current fold does not silently skip the missing decrements and declare completion.

**Fix:** Describe premature deletion as destroying required intake evidence and causing a **visible durable hold and stalled reclamation**. Incorrect arithmetic in a newly implemented snapshot bypass could introduce silent loss. C still requires a protocol change.

### 6. B’s failure criterion does not discriminate the capacity diagnosis

> “If §1b is wrong, debris keeps climbing at 10 pages and the limiter is elsewhere”

Ten pages still execute per-key exact deletes (`Gc/CasNamespaceJanitor.cpp:106–139`). More deletion work can lengthen rounds enough to reduce rounds per minute; the proposed time budget can also stop before ten pages. Debris can keep growing while insufficient reclamation capacity remains the correct explanation.

The suggested cursor diagnostic is also insufficient: `pages` and `keys` are recorded before suppression, ambiguity handling, and cursor publication (`49–50`, `148–159`). Repeated visits to the same page can report healthy-looking counts.

**Fix:** Measure actual cursor commits, completed pages, eligible dead keys, successful deletes per wall-clock second, and newly reclaimable objects per second under controlled workload. Non-convergence alone is inconclusive. Avoid stopping a multi-page experiment merely because one page contains no dead candidates.

### 7. The checkpoint prediction cannot distinguish the affected population

> “If instead `_snap` objects are plentiful and ref-cleanup counters are large, (d) is wrong”

Cleanup counters include both `Live` and `Removing` lives (`Gc/CasGc.cpp:3572–3574`, `3695–3697`). Long-lived tables can generate large cleanup counts while short-lived tables retain every log. A listed `_snap` also need not be checkpoint-authorized (`Pool/CasRefProtocol.cpp:830–835`).

The referenced measurements already contain `CASRefCleanupObjectsDeleted` values of **5,000, 5,000, and 4,368** in [run-8 samples](/home/mfilimonov/workspace/ClickHouse/lane-g/tmp/investigation/t4/msan_local/run8/samples/cas_gc_log.tsv:788), at lines 788, 808, and 828. That contradicts the prediction of near-zero aggregate cleanup, without falsifying the proposed short-lived-table mechanism.

**Fix:** Inspect a cohort of retired lives individually: their logs, snapshots, checkpoint snapshot IDs, validated recovery triples, and cleanup eligibility. Do not use pool-wide cleanup totals to reject a claim about that cohort.

## MINOR findings

- **Provisioning arithmetic is unverified.**

  > “~45 `_log` objects per dead life … roughly 5-6k deletes”

  [BACKLOG.md:1883](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/cas/BACKLOG.md:1883) supplies total logs and life counts, not logs per dead life. `ref_log_keys_listed` counts logs; `dead_life_debris` counts lives (`Gc/CasGc.cpp:569–577`, `4027–4029`). A dead-life fraction is not a dead-key fraction, especially for a janitor that also visits `state/`. Label the 18.8k inflow and 3–4× deficit as estimates pending direct measurement. Five-minute totals do not establish “from the first minute.”

- **The duration and debris figures combine different runs.**

  > “Round duration 24 s -> 597 s, backlog 81 -> 35,351.”

  Those are run-7 figures; the enumeration figures are run 8 ([BACKLOG.md:1865–1884](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/cas/BACKLOG.md:1865)). Identify the runs explicitly rather than presenting one aligned time series.

- **`deletePrefixWholesale` does not certify an empty prefix.**

  > “with its exact-token removes and `bounded_remaining` / `out_fully_drained`”

  Its counter advances for every visited key, including `Gone`, `Mismatch`, or an absent HEAD. `out_fully_drained` means enumeration exhausted without stopping for the budget; mismatched objects may remain (`Gc/CasGc.cpp:3733–3757`). State these semantics explicitly when specifying budgets and tests.

- **A’s request estimate omits namespace files.**

  > “1 LIST + ~1 exact DELETE for the state prefix”

  The state tree also contains namespace files, which the janitor recognizes (`Gc/CasNamespaceJanitor.cpp:84–89`). Four requests is an idealized estimate for the stated small stream and checkpoint-only state, excluding retries and authority reads. Qualify the estimate and the “empty within two rounds” test with sufficient budget and successful, unsuppressed execution.

- **Multi-page cursor tests need page-local expectations.**

  > “a page that loses liveness stops and publishes nothing”

  Earlier pages can already have committed their cursors. The current final admission check protects the current page’s publication; it does not roll back earlier progress (`Gc/CasNamespaceJanitor.cpp:142–159`). Specify which page loses liveness and retain previously committed progress.

- **The incarnation citation points to an alias, not minting.**

  > “Incarnations are minted fresh `UInt128` (`CasNamespaceLifeId.h:22`)”

  That line defines the type alias. Cite `mintFreshIncarnation`, `Pool/CasRefCatalog.cpp:231–242`, and its use at `688`. The incarnation-qualified prefix construction is correctly cited.

## Confirmed claims and recommendation

- **Checkpoint gate:** `planRefCleanup` returns an empty plan without a validated checkpoint (`Pool/CasRefProtocol.cpp:824–835`). The production caller already skips missing checkpoint snapshot IDs and failed triple validation (`Gc/CasGc.cpp:3644–3661`). Logs at or after the checkpoint remain, along with any required predecessor seal (`CasRefProtocol.cpp:837–844`). This establishes retention by this cleanup path, not permanent leakage.
- **Short-lived does not imply checkpoint-free:** snapshot triggering uses transaction count **or bytes**, with defaults of 256 transactions and 1 MiB (`Pool/CasRefLedger.cpp:4159–4167`; `Pool/CasPool.h:290–291`).
- **Janitor license:** `CatalogLifeIndex::resolve` protects every cataloged lifecycle state, including `Creating` and `Removing`; duplicate physical IDs throw rather than resolve absent (`Pool/CasRefProtocol.cpp:23–60`). The janitor reads its own post-LIST cut and suppresses an ambiguous cut (`Gc/CasNamespaceJanitor.cpp:42–72`, `103`).
- **Drain timing and irreversibility:** reconciliation runs before DEFER, the hot enumeration, and the fold (`Gc/CasGc.cpp:469–481`). It already performs irreversible catalog row removal through a conditional catalog replacement (`Pool/CasRefCatalog.cpp:514–517`). The reconciler currently performs no physical namespace cleanup.
- **Fold and C:** keeping A outside `Gc::fold` preserves its lack of physical deletion. C requires new protocol support: `snapshotOf` explicitly rejects terminal state (`Pool/CasRefProtocol.cpp:610–615`), and deterministic fold artifacts reject divergent replay bytes (`Gc/CasGc.cpp:3445–3450`).
- **Backstop:** retain it. A’s crash window, failed or budget-limited drains, historical orphans, and absent candidates on subsequent reconciliation all require independent rediscovery.

**Recommended order:** B’s settings and bounded paging at the current phase position; measure actual service rate; then A with corrected retirement and leadership contracts. Keep the global janitor. Separate phase reordering from that first change.

Read-only review; no edits, build, tests, or commits.

VERDICT: MAJOR: 7
