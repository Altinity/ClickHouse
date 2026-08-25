# Gate M supplementary audit — verdicts (wave 2)

Supplementary wave covering the remaining corpus after the initial 21-file sample audit
(see `gate-m-audit-verdicts.md`). Missed claims appended to `extracted/audit-m2.jsonl`
(ids `AUDIT-M2-001`..`AUDIT-M2-450`). Includes two dedicated exhaustive re-passes on
`reviews.md` and `2026-07-12-cas-archaeology/00-REPORT.md` after their first-pass forks
flagged themselves as non-exhaustive on these high-stakes files, plus a re-dispatched
Group D bin 6 (7 files) that failed to launch on the first attempt.

Severity heuristic for this pass: OK = 0 missed claims, MINOR = 1-3, MATERIAL = 4+.
This is a coarse first-pass classification by count; a few borderline files may deserve
manual reclassification (noted where obvious).

## Group A — cas core docs (14 files)

The 02-11 numbered docs, README, ROADMAP, INTENT, how-we-got-here. 01-architecture.md and review1.md were already audited in wave 1.

| File | Missed | Severity | Top missed claim (truncated) |
|------|-------:|----------|-------------------------------|
| `docs/superpowers/cas/11-walkthrough.md` | 18 | MATERIAL | The CAS backend contract's `TOKEN ⟹ CONTENT` requirement (a repeated token implies unchanged bytes) is the one item that cannot be probed cheaply and is therefo... |
| `docs/superpowers/cas/03-writer-protocol.md` | 16 | MATERIAL | CAS's condemned detection is a per-hash `.meta` point-read (`BlobMeta{version, state, condemn_round, size}` with `MetaState::Clean`/`Condemned`), not a writer-s... |
| `docs/superpowers/cas/02-methodology.md` | 11 | MATERIAL | The GC design moved from an epoch-based reclamation (EBR) core that encoded generation in the object key (`blobs/<H>/<g>`) and required a `404→LIST` degraded re... |
| `docs/superpowers/cas/10-backups.md` | 10 | MATERIAL | The CAS backup threat model defines five threats: T1 bucket/region loss, T2 operator error (`DROP TABLE`, bad mutation, wrong `rm`), T3 a CAS-layer bug (notably... |
| `docs/superpowers/cas/06-tla-models.md` | 9 | MATERIAL | `CaIncarnationProofCore.tla` used Apalache 0.58.0 to verify an inductive invariant `IndInv` (19 conjuncts) for a pre-B91, token-only fragment of the GC core: ba... |
| `docs/superpowers/cas/08-testing-and-soak.md` | 8 | MATERIAL | A 4h active-workload chaos soak at WORKERS=6 fills the disk in ~60-90 min; root cause is rustfs#3231 (upstream, open): overwriting a >128 KiB object in an un-ve... |
| `docs/superpowers/cas/how-we-got-here.md` | 7 | MATERIAL | `CaGcIndegRefoldCore.tla` isolated an in-degree-underflow hazard the large safety model could not express: the C++ accumulated blob in-degree as a non-idempoten... |
| `docs/superpowers/cas/04-gc-protocol.md` | 5 | MATERIAL | GC's raw baseline rebuild condemns nothing: it only rebuilds cursors and edges. An earlier version ended the pass by LISTing `blobs/` and condemning every physi... |
| `docs/superpowers/cas/07-s3-budget.md` | 4 | MATERIAL | A 2026-07-19 post-fix live chaos-soak measurement (workers=6, per-operation-unit attribution via ProfileEvents/part_log/content_addressed_garbage_collection_log... |
| `docs/superpowers/cas/09-read-protocol.md` | 2 | MINOR | Both the shard decode cache and the `(ManifestId, Token)` manifest decode cache are memory-bounded by a wholesale clear rather than LRU eviction: once either ca... |
| `docs/superpowers/cas/INTENT.md` | 2 | MINOR | CAS design work treats a proof of correctness as requiring an executable, reproducible counterexample: a test must be able to fail. A relink proved only by a fl... |
| `docs/superpowers/cas/05-formats-and-backend.md` | 1 | MINOR | docs/superpowers/cas/05-formats-and-backend.md's own DONE/TODO/REJECTED summary table records GCS generation-token binding as DONE 2026-07-03 (via `http_client ... |
| `docs/superpowers/cas/README.md` | 0 | OK |  |
| `docs/superpowers/cas/ROADMAP.md` | 0 | OK |  |

## Group B — worklogs (20 files)

All worklogs minus the 2 already audited in wave 1 (2026-07-06-scenario-validation-night.md, 2026-07-17-unattended-r5-r6-round.md). Two duplicate/empty files were skipped (CURrent.md, 2026-07-21-unattended-consistency-f1f11.md — both 0 bytes, case/hyphen-collision artifacts of correctly-named files).

| File | Missed | Severity | Top missed claim (truncated) |
|------|-------:|----------|-------------------------------|
| `docs/superpowers/worklogs/CURRENT.md` | 14 | MATERIAL | CAS GC's `gc_fold_begin`/`gc_fold_end` audit rows write only an anomaly count (e.g. `{anomalies: '1'}`) as `detail`; `recordAnomaly` takes namespace, shard, man... |
| `docs/superpowers/worklogs/2026-07-21-unattended-reftablestate-experiments.md` | 12 | MATERIAL | E1 (relaxed replay of RefTableState) shipped as KEEP: the un-materialized overlay makes per-transaction scratch copies O(overlay) instead of O(N), reducing repl... |
| `docs/superpowers/worklogs/2026-07-03-real-s3-validation.md` | 6 | MATERIAL | On GCS's S3-compatible XML surface signed with sigv4, none of the conditional operations CAS needs are enforced (create-if-absent, conditional overwrite, and co... |
| `docs/superpowers/worklogs/2026-07-13-unattended-optimization-round.md` | 6 | MATERIAL | Task-8 review F1 (rev.6): Removed-lifecycle recoveries are not sealed — a late rebirth PUT from a dead epoch can transiently resurface in cold folds until GC na... |
| `docs/superpowers/worklogs/2026-07-03-scenarios-full-scale-campaign.md` | 4 | MATERIAL | S06/S07 (10000/20000-column wide parts) confirmed the manifest encode/decode works correctly at scale: a 10000-column wide part commits under the manifest hard ... |
| `docs/superpowers/worklogs/2026-07-12-unattended-refsnaplog-stabilization.md` | 4 | MATERIAL | A CA-s3 stateless-test baseline attribution ran the 38 CA-s3-lane FAILs on the normal non-CA job: 31 also failed there (local-env noise, not CA-caused), leaving... |
| `docs/superpowers/worklogs/2026-07-22-unattended-rev7-lifecycle.md` | 4 | MATERIAL | During rev.7/rev.8 disk-lifecycle implementation, T7's implementer review found a real hole (I1): the observe/FSCK path could mint `_pool_meta` over a residual ... |
| `docs/superpowers/worklogs/2026-07-03-unattended-night.md` | 3 | MINOR | Attribution of the night's 11 concurrent GC clamps was: RustFS returned false 404s on HEAD during a metacache storm caused by the `rustfs#3231` overwrite-leak's... |
| `docs/superpowers/worklogs/2026-07-11-unattended-cas-campaign.md` | 3 | MINOR | A pre-release v3 data-loss hole was found while modeling the deposed-leader path in TLA+: a deposed GC leader's pre-CAS `clearSparedMeta` call leaves stray-Clea... |
| `docs/superpowers/worklogs/2026-07-15-all-tree-part-files-t12-validation-gate.md` | 2 | MINOR | `REPLACE PARTITION`/`ATTACH PARTITION ... FROM` queue-clone relink on a Replicated CA table has no dedicated test: `test_cas_replicated_relink` only proves reli... |
| `docs/superpowers/worklogs/2026-07-16-unattended-codecs-txn-sourcelayout-part1.md` | 2 | MINOR | A source-layout reorg commit whose pathspec covered only the CA directory and `CMakeLists.txt` can strand fixes to external-consumer `#include` paths (e.g. `Dat... |
| `docs/superpowers/worklogs/2026-07-17-stabilization-pause-state.md` | 2 | MINOR | The acked-then-lost data-loss fix's regression test is `05014_insert_dedup_disk_commit_failpoint`, paired with the targeted failpoint `part_storage_fail_commit_... |
| `docs/superpowers/worklogs/2026-07-17-unattended-round2.md` | 2 | MINOR | S37's single failing verdict (22/23) was diagnosed as the scenario card's own oracle bug rather than a product defect: the card expected 100 rows but the true c... |
| `docs/superpowers/worklogs/2026-07-17-unattended-round2-part1.md` | 2 | MINOR | dedupTrace independently traced the CAS write ordering in `ReplicatedMergeTreeSink`: the CAS commit (`renameParts`→`stageManifest`, line 976) strictly precedes ... |
| `docs/superpowers/worklogs/2026-07-21-unattended-consistency-f1-f11.md` | 2 | MINOR | A one-shot `CORRUPTED_DATA` error from a temp-fetch with an absent binding was observed once during the definitive consistency soak and flagged for investigatio... |
| `docs/superpowers/worklogs/2026-07-06-p31-mount-lease-root-cause.md` | 1 | MINOR | A `LOGICAL_ERROR` "release before start" fires when Store teardown calls `stop()`/`doTerminate` on a mount-lease keeper whose `seq` is still 0 because `doStart`... |
| `docs/superpowers/worklogs/2026-07-08-unattended-cas-cache-and-stabilization.md` | 1 | MINOR | A full CA-s3 lane post-mortem exonerated the B86 log-policy removal for the observed timeout growth: ProfileEvents on every stalled query showed zero CAS/RustFS... |
| `docs/superpowers/worklogs/2026-07-16-unattended-codecs-txn-sourcelayout-part2.md` | 1 | MINOR | The root mechanism behind the #37 over-fencing defect is that `MountLeaseKeeper::renewOnce` (`CasServerRoot.cpp:913-931`) issues a single-shot direct `putOverwr... |
| `docs/superpowers/worklogs/2026-07-17-unattended-stabilization-resume.md` | 1 | MINOR | A CI fast-test build broke because `dbms` linked the dead `ch_contrib::crc32c` target left over from a phase-1a change, and the fast-test job does not initializ... |
| `docs/superpowers/worklogs/2026-07-16-unattended-codecs-txn-sourcelayout-part3.md` | 0 | OK |  |

## Group C — reports (69 files)

All reports/**/*.md minus the 3 already audited in wave 1. Includes a dedicated exhaustive follow-up pass on reviews.md and 2026-07-12-cas-archaeology/00-REPORT.md after their first-pass forks (C2, C4) flagged themselves as non-exhaustive on those two high-stakes files.

| File | Missed | Severity | Top missed claim (truncated) |
|------|-------:|----------|-------------------------------|
| `docs/superpowers/reports/reviews.md` | 31 | MATERIAL | `rebuildBaseline` (`cas-gc-rebuild`) has no mount-lease interlock: a live server's fresh mount lease does not stop rebuild from performing, because the tool's o... |
| `docs/superpowers/reports/2026-07-12-cas-archaeology/00-REPORT.md` | 30 | MATERIAL | D1 (definitely duplicated): 412/PreconditionFailed detection is reimplemented 3-4 times (`S3::Client::RetryStrategy`, `removeObjectIfTokenMatches`, `copyObjectC... |
| `docs/superpowers/reports/2026-07-12-cas-archaeology/06-tests.md` | 20 | MATERIAL | The "natural black-box equivalence" test oracle requires a CA table to be byte-for-byte identical to a normal MergeTree table on the same data under active GC (... |
| `docs/superpowers/reports/2026-07-12-cas-archaeology/03-gc-refs.md` | 14 | MATERIAL | The GC round's exact durable key layout: `gc/state` (controller state, one per pool, strict fail-closed CAGT-magic proto), `gc/hb` (advisory heartbeat, fixed 24... |
| `docs/superpowers/reports/2026-07-26-list-incompleteness-investigation.md` | 9 | MATERIAL | Probe A detects CAS ref-prefix LIST incompleteness via two independent enumerations per GC round (the pre-fold defer scan and fold's own enumeration); it compar... |
| `docs/superpowers/reports/2026-07-12-cas-archaeology/02-store-core.md` | 9 | MATERIAL | Every CAS key family is rooted at `POOL = pool_prefix`: blob body `POOL/blobs/<algo>/<S>/<hex>` (self-describing, `<S>`=first 2 hex chars); blob meta `POOL/blob... |
| `docs/superpowers/reports/2026-07-19-session-full-summary.md` | 6 | MATERIAL | The final whole-branch review (base `d57a41f353d` to head `7771bb60c70`, 38 commits) found a Critical `partAccess` use-after-free (the same class task T9 meant ... |
| `docs/superpowers/reports/2026-07-31-task5-removalready-proposal.md` | 6 | MATERIAL | Keeping a permanent `Retired` catalog row instead of the transient `RemovalReady` state was rejected because it grows the catalog with historical namespace birt... |
| `docs/superpowers/reports/2026-07-31-stage-b-strategic-review-codex-round2.md` | 6 | MATERIAL | The controller's Task 5 removal design gives the smallest catalog state set as `Creating`, `Live`, `Removing`, `Retired` (four persisted states), where `Absent`... |
| `docs/superpowers/reports/2026-07-31-task5-critique-codex.md` | 6 | MATERIAL | Blocking issue: cursor pruning is not monotone across crash-resume — if the process stops after the pruned seal is durable but before `_ckpt`/entry deletion, th... |
| `docs/superpowers/reports/2026-07-02-d2-scenario-triage.md` | 5 | MATERIAL | D2 classified S31 (ca-gc-dryrun under gc_shards>1) as INFRA: `docker-compose-gc_shards2.yml up -d` returned rc=1 and the cluster never became healthy within 240... |
| `docs/superpowers/reports/2026-07-31-removalready-review-fable.md` | 5 | MATERIAL | The `RemovalReady` catalog-state proposal's monotonicity claim requires the predicate to be enforced at five distinct cursor/walk producer sites: the catalog-on... |
| `docs/superpowers/reports/2026-07-11-cas-deposed-leader-stray-clean-meta.md` | 5 | MATERIAL | The chosen fix (Fix 4) for the deposed-leader stray-Clean defect makes GC freshness metadata add-only: GC never transitions Condemned to Clean on a spare (the s... |
| `docs/superpowers/reports/2026-07-20-umbrella-review-cas-vs-antalya-26.6.md` | 5 | MATERIAL | Fetch-by-relink has no retention pin: the sender is fire-and-forget and releases the source part while the receiver's `precommitAdd` edge-PUT can stall, so if t... |
| `docs/superpowers/reports/2026-07-12-cas-archaeology/05-integration.md` | 5 | MATERIAL | `DataPartsExchange.cpp:1084` calls `ContentAddressed::parseTableUuid` directly instead of going through the `IContentAddressedExchange` facade the relink design... |
| `docs/superpowers/reports/2026-07-12-cas-archaeology/01-frontend.md` | 5 | MATERIAL | For `ContentAddressedMetadataStorage`, the ctor default for `root_shards_` is 8 while the factory's config default is 32; a pool minted via the ctor default (e.... |
| `docs/superpowers/reports/2026-07-31-stage-b-strategic-review-codex.md` | 4 | MATERIAL | The Stage B strategic-review concentration proposal's replacement namespace-removal sequence is: (1) catalog transitions Live to Removing; (2) the fenced owner ... |
| `docs/superpowers/reports/2026-07-31-stage-b-strategic-review-fable-round3.md` | 4 | MATERIAL | The removal driver resumes entry deletion idempotently on the owning writer's next mount after a stop in the `Removing`-without-`_ckpt` window; it never re-crea... |
| `docs/superpowers/reports/2026-07-13-cas-soak-metrics-audit.md` | 4 | MATERIAL | An earlier revision of the soak audit had falsely declared `CasRootGet` (a backend counter of GETs on the `roots/` prefix) removed; it was in fact still live, a... |
| `docs/superpowers/reports/2026-07-31-stage-b-strategic-review-fable.md` | 3 | MINOR | The Stage B strategic review's concentration proposal, in dependency order: (1) a `FencedCatalogWriter` primitive constructed once per mount from `(admitted_gen... |
| `docs/superpowers/reports/reviews-todo.md` | 3 | MINOR | The emulated S3-compatible backend (rustfs) answers `TokenMismatch` (HTTP 412) rather than `NotFound` for an `If-Match` DELETE issued against an already-absent ... |
| `docs/superpowers/reports/2026-07-30-stage-b-critical-checkpoint-review.md` | 3 | MINOR | A reviewer sub-agent must deliver findings BOTH as a durable file AND in its final message; message-only delivery risks the findings existing nowhere durable an... |
| `docs/superpowers/reports/2026-07-31-stage-b-strategic-review-fable-round2.md` | 3 | MINOR | CAS should forbid the `Ordinary` database engine outright — its UUID-less `data/<db>/<tbl>` layout keeps a table's RootNamespace stable across drop-and-recreate... |
| `docs/superpowers/reports/2026-07-29-ca-transient-classifier-audit.md` | 3 | MINOR | In the CA transient-classifier audit, `CasBlobInDegree.cpp:289` versus `pruneSupersededGenerations` (`CasGc.cpp:3217`, called from `:802`) has a plausible resid... |
| `docs/superpowers/reports/2026-07-12-cas-archaeology/04-ops-mounts.md` | 3 | MINOR | The standalone `tree` `ObjectKind` was excised on 2026-07-03; `ObjectKind` is now Blob-only, and `toEventKind` only handles `Blob`. |
| `docs/superpowers/reports/2026-07-20-umbrella-review-fixes-deferred.md` | 2 | MINOR | Whether `~Pool()` destruction — deferred when an in-flight transaction holds the last `PoolPtr` — can issue farewell/lease I/O through an already-shut-down obje... |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_r2_findings.md` | 2 | MINOR | Round-1 remedy disposition verdicts for the ref-chain recovery redesign (from the round-2 review): seal high-water adoption sound but void precedence unusable w... |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_r4_findings.md` | 2 | MINOR | The claim that a rewritten CAS `fsck` oracle is "seal-aware" is overstated: current `fsck` discovers snapshots and logs solely via untrusted `LIST`, silently sk... |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_r6_findings.md` | 2 | MINOR | codex_r6_findings.md's 'Checks that held' section confirms: a ref-log body whose decoded sequence differs from its key is already rejected; under a true state-d... |
| `docs/superpowers/reports/2026-07-02-d1-soak-1h.md` | 1 | MINOR | The D1 1h chaos soak ran with `CA_SOAK_NO_HARD_KILL=1`, which deterministically downgrades a hard `docker kill` fault on a CH replica to a graceful RESTART, bec... |
| `docs/superpowers/reports/2026-07-24-codex-stage1-reviews.md` | 1 | MINOR | The stage-1 fix-verification pass found the writer-ledger memory regression guard incomplete: `LongTailReplaysUnderMemoryBound` exercises only the free `recover... |
| `docs/superpowers/reports/2026-07-18-asan-battery-exclusion-list-correction.md` | 1 | MINOR | After the ASan-battery grep fix, of the 28 verified distinct real culprits 7 were already-fixed memory-safety bugs and the remaining 21 needed LOGICAL_ERROR-abo... |
| `docs/superpowers/reports/2026-07-17-codex-review-triage.md` | 1 | MINOR | The 2026-07-17 codex-review triage's proposed fix-wave order prioritized finding №4 (condemn-marker load-bearing, the one reachable shared-pool data-loss class)... |
| `docs/superpowers/reports/2026-07-17-dataloss-traced-root-cause.md` | 1 | MINOR | The full set of code sites sharing the renameParts -> Keeper-decision -> parts-commit slot audited for the part-durability-before-Keeper-commit fix is: `Replica... |
| `docs/superpowers/reports/2026-07-18-s22-throttle-retry-rca.md` | 1 | MINOR | A regression test mirroring `CasPartWriteTxn.PutBlobWrongSizeFailsClosed` should arm a backend that throws `SlowDown` on the `.meta` key and assert the freshnes... |
| `docs/superpowers/reports/2026-07-18-s23-idle-rss-rca.md` | 1 | MINOR | S23's initial idle-RSS-growth failure correlated with elevated S3 error rates in the same window (read 8.6%, write 17.2% on one node); S3 retries allocate trans... |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_r9_findings.md` | 1 | MINOR | The ref-chain complete-cut design's generation-change safety was confirmed to have no durable double-apply path: if the old PUT wins, the successor adopts it; i... |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_r7_findings.md` | 1 | MINOR | The v7 ref-chain design's required go-red negative test controls under `HoleyListBackend` are: a wholly hidden A:+1 versus a visible B:-1 on the same blob; a ca... |
| `docs/superpowers/reports/2026-07-21-storageproxy-forwarding-audit.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-18-s31-dryrun-shards-rca.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-20-upstream-issue-draft-asyncloader-stuck-table.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-12-cas-refsnaplog-coverage-audit.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-18-asan-battery-rca.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-19-campaign-38-results.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-18-s23-tracked-growth-rca.md` | 0 | OK |  |
| `docs/superpowers/reports/20260717_codex_review.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-14-cas-gc-defense-audit.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-13-scenarios-stabilization-status.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-23-cas-wide-insert-baseline.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-18-s36-s37-placement-rca.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-18-s38-late-log-clamp-starvation.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-14-cas-s5-spare-clear-reopens-dataloss.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-29-gc-perf-audit-soak.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-30-stage-b-increment-review.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-14-cas-adoptevidence-relink-lifecycle-exposure.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-17-b199-recovery-seal-zstd-rca.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-24-cas-wide-insert-stage1-effect.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_plans_r4_findings.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_r3_findings.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_plans_r1_findings.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_plans_r2_findings.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_r8_findings.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_plans_r3_findings.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_r1_findings.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_r5_findings.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_simplify_design.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-26-s42-stale-edge-repro/PHASE1.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-12-cas-archaeology/07-history.md` | 0 | OK |  |
| `docs/superpowers/reports/2026-07-26-list-incompleteness-proof/README.md` | 0 | OK |  |

## Group D — targeted spec/plan side-section sweep (73 files, complete)

Not a full-file audit — grepped all 152 specs+plans files for side-section headers (Risks, Self-review, Deviations, Not in this plan, Out-of-band, Rejected alternatives, Open questions, Follow-ups); 76 matched, 73 after excluding files already fully audited in wave 1. Only the matched sections (+~20 lines context) were read and diffed against extracted records.

| File | Missed | Severity | Top missed claim (truncated) |
|------|-------:|----------|-------------------------------|
| `docs/superpowers/specs/2026-07-28-cas-merge-layout-preparation-design.md` | 8 | MATERIAL | The CAS merge-layout preparation's top identified risk (R1) is that an extraction changes lock discipline: the publish lane's correctness depends on the two-pha... |
| `docs/superpowers/specs/2026-07-25-cas-gc-observability-and-mount-force-design.md` | 7 | MATERIAL | In the CAS GC-observability skipped-transaction-detector design, probe A's independence from probe B rests on the two enumerations being separate physical LIST ... |
| `docs/superpowers/plans/2026-07-24-cas-publish-confirm-and-ref-lane-safety.md` | 3 | MINOR | A codex review of the publish-confirm/ref-lane-safety landing raised three footprint objections against the upstream-coupling-minimization rule; two were knowin... |
| `docs/superpowers/specs/2026-07-02-cas-gc-ack-floor-fence-redesign.md` | 3 | MINOR | The ack-floor GC redesign explicitly defers delta-runs plus compaction for the snapshot (amortizing snapshot bytes to O(edges) per pass instead of O(universe)) ... |
| `docs/superpowers/specs/2026-07-15-cas-txn-one-pipeline-design.md` | 3 | MINOR | The TXN-ONE-PIPELINE design flags a candidate upstream issue in its Backlog Observation: on plain object storage, carried-forward projections can be absent from... |
| `docs/superpowers/plans/2026-07-28-cas-fence-observability-and-write-grace.md` | 3 | MINOR | In the CAS mount-fence write-grace plan, no test may `EXPECT_THROW` a `LOGICAL_ERROR`; Part B failure paths must surface as `INVALID_STATE` or `NETWORK_ERROR`, ... |
| `docs/superpowers/plans/2026-07-29-cas-relink-seam-tla-gate.md` | 3 | MINOR | An early draft of the relink/seam TLA+ model omitted the WedgeRetryCreated action entirely, making the wedge protocol's own happy path unreachable: CasRefLedger... |
| `docs/superpowers/plans/2026-07-02-cas-gc-ack-floor-fence.md` | 2 | MINOR | The ack-floor GC redesign drops `process_epoch` as a separate merged-heartbeat field because the writable path already sets `process_epoch = writer_epoch` (`Cas... |
| `docs/superpowers/specs/2026-07-20-cas-ref-admits-incremental-budget-design.md` | 2 | MINOR | Reducing admits() call frequency (backlog fix #2) was rejected as an alternative to the incremental-counter design because each remaining call would still be O(... |
| `docs/superpowers/specs/2026-08-03-cas-naming-unification-design.md` | 2 | MATERIAL (content override — flags an undetected migration bug + cross-surface tag-rename sync risk) | For the CAS naming-unification migration, old `content_addressed`/`ca` names embedded in many `.reference` test files must be changed in the same commit as the ... |
| `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md` | 2 | MINOR | Whether `ApplyPending` can later become debug-only is recorded as a follow-up but deliberately not implemented in the Stage-B catalog plan; `RefApplyState` refa... |
| `docs/superpowers/specs/2026-07-09-cas-promote-resurrect-tokened-blob-design.md` | 2 | MINOR | The ideal, fully-invisible zero-re-upload fix for the promote/GC condemn race is a writer-triggerable synchronous fold that activates the precommit edge before ... |
| `docs/superpowers/plans/2026-07-10-cas-meta-descriptor-raw-body.md` | 2 | MINOR | In the meta-descriptor raw-body plan, the absent-meta AND absent-body retry mechanism in putBlob's step (Task 3 step 5) is left as an open implementation choice... |
| `docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md` | 2 | MINOR | Stage-B follow-up Task F1 (outside the Stage-B verdict) is a purely mechanical split of `CasGc.cpp` and `CasRefLedger.cpp` (each ~4000 lines) along their existi... |
| `docs/superpowers/plans/2026-07-16-cas-source-layout-refactoring.md` | 1 | MINOR | The source-layout refactor's phase-5 rename sweep (`Store`→`Pool`, `Build`→`PartWriteTxn`) must use word-boundary-anchored seds rather than blanket `s/Store/...... |
| `docs/superpowers/specs/2026-07-13-cas-ref-lease-exclusivity-rev6-proposal.md` | 1 | MINOR | The ref-lease-exclusivity rev.6 proposal left four open questions for review at draft time: whether a 30s `T_mat` unclean-handover penalty is acceptable in prod... |
| `docs/superpowers/specs/2026-07-29-cas-relink-reoffer-redesign.md` | 1 | MINOR | The relink protocol's defense against an actively-forged affirmative confirm answer relies on the request/response shape being structurally opaque to caches, no... |
| `docs/superpowers/plans/2026-07-11-cas-pluggable-hash-phase2-sha256.md` | 1 | MINOR | Phase 2 pluggable-hash `sha256` blobs are stored under the key shape `blobs/sha256/<shard>/<64-hex>`, distinct from the default 128-bit hash's `blobs/<aa>/<H>` ... |
| `docs/superpowers/plans/2026-07-16-cas-txn-one-pipeline.md` | 1 | MINOR | Under the one-pipeline transaction design, CA manifest build, blob upload, and promote (remote I/O) deliberately run inside `MergeTreeData::Transaction::commit`... |
| `docs/superpowers/specs/2026-07-02-cas-copy-forward-condemned-evidence.md` | 1 | MINOR | The condemned-evidence copy-forward design has a queued (not-yet-executed) validation task: a fresh soak run on a clean pool replaying the S13-adjacent kill-res... |
| `docs/superpowers/specs/2026-07-15-cas-source-layout-refactoring-design.md` | 1 | MINOR | The source-layout refactoring design defers a backlog of deliberately-not-done improvements: a constructor config struct for the pool facade's ~25 positional pa... |
| `docs/superpowers/specs/2026-08-02-cas-stage-b-remaining-design.md` | 1 | MINOR | Stage-B has two non-gating follow-ups scheduled after the T9 closeout: F1, a post-baseline mechanical split of `CasGc.cpp` and `CasRefLedger.cpp` along existing... |
| `docs/superpowers/plans/2026-07-03-cas-gc-rebuild.md` | 1 | MINOR | The GC-rebuild validation plan (queued outside this plan) is: mid-soak `mc rm` of `gc/state` should make the guard's `CORRUPTED_DATA` appear in the GC round log... |
| `docs/superpowers/plans/2026-07-11-cas-s3-native-staging.md` | 1 | MINOR | The S3-native staging plan's memory fast-path (spec §7) is explicitly deferred out of the plan as a separable optimization, noted for a follow-on. |
| `docs/superpowers/plans/2026-07-17-cas-reftable-cow-map.md` | 1 | MINOR | The reftable COW-map plan's self-review found the actual access-site inventory for `RefTableState::committed` was 18 sites across 5 files (verified against the ... |
| `docs/superpowers/plans/2026-07-26-overnight-s42-and-gc-perf-study.md` | 1 | MINOR | The S42/GC-perf overnight study plan is diagnostic-only: no task in the plan implements a fix, the only contemplated code changes are a counter or a log line, a... |
| `docs/superpowers/specs/2026-08-03-cas-branch-reconstruction-script-design.md` | 1 | MINOR | The branch-reconstruction carve script accepts, as a residual risk, that a file moved between source areas (e.g. a gtest moved into a layer directory) silently ... |
| `docs/superpowers/plans/2026-07-03-cas-gcs-generation-binding.md` | 1 | MINOR | GCS generation binding's OAuth leg (`gcp_oauth` dialect with ADC credentials) is deliberately not implemented as a task because no ADC credentials exist on the ... |
| `docs/superpowers/plans/2026-07-12-cas-ref-table-snapshot-log-phase1.md` | 1 | MINOR | The ref-table snapshot-log Phase 1 plan has zero deliberate deviations from its spec; Phase 2 items are excluded per the plan's Global Constraints, not silently... |
| `docs/superpowers/plans/2026-07-17-codex-triage-fix-wave.md` | 1 | MINOR | Codex triage finding №18 was not given its own task because the root fix in Task 2 collapses it; the affected list-branch change is called out explicitly inside... |
| `docs/superpowers/specs/2026-08-03-cas-docs-map-reduce-consolidation-design.md` | 1 | MINOR | The docs consolidation project's own risk register names four risks and mitigations: docs describing a moving target (mitigated by HEAD-time verification and an... |
| `docs/superpowers/plans/2026-07-09-cas-promote-tokenless-copyforward-race.md` | 1 | MINOR | An optional pre-pass refresh for the promote copy-forward tokenless-blob race fix (spec §3) was deliberately deferred rather than implemented: it is a fast-path... |
| `docs/superpowers/specs/2026-07-08-cas-part-folder-cache-design.md` | 1 | MINOR | A thread-local "active slot" design for the part-folder cache was rejected as unnecessary v1 complexity with no measurable benefit over a bounded shared map loo... |
| `docs/superpowers/plans/2026-07-09-cas-writer-gc-simplification-phase-a.md` | 1 | MINOR | The writer-GC-simplification Phase A plan deliberately leaves Phase B (further simplification) out of this plan, to be written as a separate plan after a meta-c... |
| `docs/superpowers/plans/2026-07-13-cas-introspection-first.md` | 1 | MINOR | The introspection-first plan's spec item §0.4 is deliberately left unimplemented in this plan; it lands later together with the opt-in levers work once the spec... |
| `docs/superpowers/plans/2026-07-20-umbrella-review-fixes.md` | 1 | MINOR | The umbrella-review-fixes plan explicitly leaves report findings 1 and 6 unaddressed (deferred by user instruction / covered by an existing separate plan) and f... |
| `docs/superpowers/plans/2026-07-06-cas-introspection-package.md` | 1 | MINOR | Widening the CAS GC-log to carry mount state was deliberately NOT done as part of the introspection package (design: YAGNI) because the new `system.content_addr... |
| `docs/superpowers/plans/2026-07-21-reftablestate-closed-class-experiments.md` | 1 | MINOR | The `RefTableState` closed-class experiments round's success criterion (Task 8 final gate) is: after a 20-minute soak, the hottest CAS-attributed CPU stack fami... |
| `docs/superpowers/plans/2026-07-28-cas-ref-chain-tla-phase.md` | 1 | MINOR | The ref-chain v9 TLA+ phase deliberately does not model the temporal lemma's writer-side arms (condemned-meta rematerialization, tokenless relink ordering); tho... |
| `docs/superpowers/specs/2026-07-24-cas-mount-lease-self-race-fix-proposal.md` | 1 | MINOR | Open question from the mount-lease self-race fix proposal: whether the existing `ProfileEvents::CasMountLeaseLost` counter (already incremented by `superseded`/... |
| `docs/superpowers/plans/2026-07-22-pr2073-sanitizer-aborts.md` | 1 | MINOR | PR #2073's sanitizer-abort plan deliberately leaves `SilkFiberSocketTest/1` unaddressed; if it later blocks a CI lane it must be escalated as a separate Altinit... |
| `docs/superpowers/specs/2026-07-12-cas-stabilization-cleanup-design.md` | 1 | MINOR | The stabilization-cleanup design's item A2 (widening CRC coverage) alters the on-disk run-file framing; this is accepted pre-release on the condition that write... |
| `docs/superpowers/specs/2026-07-28-cas-fence-observability-and-write-grace-design.md` | 1 | MINOR | `write_grace_ms` is independent of `cas_request_budget.operation_deadline_ms`, so a large grace plus a long remount can exceed a client's own request timeout; t... |
| `docs/superpowers/plans/2026-07-01-cas-shard-incarnation-and-registry-removal.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-11-cas-add-only-meta-fix.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-16-cas-merge-upload-retry-fix.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-23-cas-writepath-stage1.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-08-03-cas-docs-map-reduce-consolidation.md` | 0 | OK |  |
| `docs/superpowers/specs/2026-07-13-cas-memory-s3-budget-optimizations-design.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-08-cas-file-cache-disk-support.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-11-cas-mixed-algo-pools.md` | 0 | OK |  |
| `docs/superpowers/specs/2026-07-01-cas-shard-incarnation-and-registry-removal-design.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-02-cas-gc-snapshot-streaming.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-08-cas-observability-audit-and-inspect.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-25-cas-followups-detector-introspection-s42-forceclaim.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-08-cas-promote-over-committed-leak-fix.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-09-cas-promote-resurrect-tokened-blob.md` | 0 | OK |  |
| `docs/superpowers/specs/2026-07-03-cas-gcs-generation-binding-design.md` | 0 | OK |  |
| `docs/superpowers/specs/2026-07-17-part-durability-before-keeper-commit-design.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-06-cas-gc-round-skip-unchanged.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-20-cas-ref-admits-incremental-budget.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-06-cas-harness-honesty.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-10-cas-freshness-meta-v3.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-13-cas-ref-lease-exclusivity-rev6.md` | 0 | OK |  |
| `docs/superpowers/specs/2026-07-11-cas-mixed-algo-pools-design.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-06-cas-lease-view-sync-decouple.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-14-cas-opt-levers-s1-s5.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-22-cas-disk-lifecycle-rev7.md` | 0 | OK |  |
| `docs/superpowers/specs/2026-07-11-cas-pluggable-blob-hash-design.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-06-cas-mount-lease-fence-recovery.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-10-cas-retired-in-snapshot.md` | 0 | OK |  |
| `docs/superpowers/plans/2026-07-15-cas-all-tree-part-files.md` | 0 | OK |  |

