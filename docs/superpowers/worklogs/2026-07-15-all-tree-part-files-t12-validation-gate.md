# All-tree part files — Tasks 7-12 integration + T12 validation gate (2026-07-15)

Integrator pass over the tail of `docs/superpowers/plans/2026-07-15-cas-all-tree-part-files.md`
(12-task plan, `cb01c55da86`): Tasks 7-11, the codecs-v3 Phase 1 draft, then Task 9 (the mutable-file
deletion sweep) preempted the T12 pre-flight, and finally Task 12 itself — the full validation gate.
This entry is the dated worklog Task 12 §Step 5 asks for: landed tasks + validation evidence.

## Landed this pass {#landed}

- **Task 7** (`578a96cb0c5`) — relink is manifest-self-contained: dropped the `metadata_version` wire
  field + sidecar reconstruction on the `DataPartsExchange` fetch-by-relink path (cookie v2).
- **Task 8** (`3b4d0f20933`) — committed-file unlink stages removal marks, resolved via repoint unless
  superseded by the whole-part ref-drop (B123 evolution).
- **Task 10** (`c98c41e367c`) — dropped the freeze `metadata_version` special case in `MergeTreeData`:
  the byte-equal repoint no-op already absorbs the post-clone write.
- **Codecs-v3 Phase 1** (`74f06047b4a`, `50bbdd1a14f`, `065f6e97357`) — `Core/Formats/` bootstrap,
  `CasTextFormat` (JSON vocabulary, header/trailer, zstd arm), shared format test battery +
  `Formats/README.md` registry. Orthogonal to the all-tree work but integrated in the same pass.
- **Task 11** (`6e02e1f77e3`, `75c864de010`) — docs + backlog sync for the all-tree model (mutable set
  = ∅, repoint semantics) plus a review follow-up correcting §9.1's read-your-writes mechanism.
- **Task 9** (`8f74b727a2a`, review fix `4ed45f3dc50`) — the mutable-file concept deletion sweep: 31
  files, schema/API/staging/view/`ForceFresh`-branch/predicate removal. Caught and fixed 4 draft bugs
  during integration (a `publishWiredPart` manifest entry-ordering bug, 2 stale `entries.size()`
  assertions, an incidental byte-collision, an invalid direct `getBlobViewPlan` call) plus one
  post-review fix: `CaWiringOps.MoveDirectoryOntoExistingDestinationBuildSurvives`'s null-build
  precondition had silently drifted (Task 6/9 changed `writeFile`'s inline-candidate path to
  unconditionally call `buildFor`) — restaged via `unlinkFile` (Task 8's removal-mark staging is the
  one remaining shape that never calls `buildFor`) and made RED-able via `Store::setEventSink`
  (`BuildAbort` event discriminator).
- **T12 pre-flight closure**: `test_cas_replicated_relink` migrated from `with_minio` to `with_rustfs`
  (`3bd454bc4da` — `CasProbe`/B31 correctly refuses MinIO, which does not enforce conditional deletes;
  not a product bug). `repointRef`'s routine-repoint log downgraded `WARNING`→`LOG_DEBUG`
  (`6fb325d30e6`) — post-all-tree, a standalone write/remove on a committed part is the designed
  mechanism (Tasks 4/8/9), not an anomaly; the `!resolved` branch (BACKLOG "repointRef
  non-resolving-key audit gap", currently unreachable) stays `WARNING`. `ProfileEvents::CasRefRepoint`
  stays unconditional either way.
- **Task 12 §Step 4**: wired `CasRefRepoint == 0` into the ca-soak green-path assertion set for the
  cards with no `FREEZE`/`ATTACH`/`DETACH`/`MOVE`/`REPLACE PARTITION` (S03/S04/S05,
  `cabe38e4408`) — pattern-matched the existing `CasBlobList == 0` check; the attach/detach lifecycle
  cards (S15/S18) are deliberately excluded since they legitimately repoint.

## Task 12 validation-gate evidence {#t12-evidence}

All 4 gate steps ran against the final HEAD (through `6fb325d30e6`):

1. **Full CA unit sweep** — `ninja -C build unit_tests_dbms` clean;
   `--gtest_filter='Cas*:Ca*'`: 922 tests / 160 suites, **0 failures**, 2 pre-existing disabled.
2. **Server build + integration + txn coverage** — `ninja -C build clickhouse` clean;
   `test_cas_replicated_relink` (rustfs): `1 passed in 23.62s`; the B182-class CA-transactions oracle
   `05004_content_addressed_transactions.sh` under the CA-default stateless lane: `Passed: 1, Failed: 0`.
3. **CA-default stateless lane subset** — combined run of the transactions+freeze+attach family (7
   tests: `05003_content_addressed_freeze`, `04280_content_addressed_clone_partition_works`,
   `05002_content_addressed_fetch_partition`, `04283_content_addressed_replicated_rejected`,
   `02271_replace_partition_many_tables`, `01901_test_attach_partition_from`,
   `05004_content_addressed_transactions`): `Failed: 0, Passed: 7, Skipped: 0`.
4. **Soak assertion wiring** — `CasRefRepoint == 0` added to S03/S04/S05; functionally verified (not
   just syntax-checked) via a live dev-scale run (`scenarios.run --scenario S03 --seed 1 --scale
   dev`): new verdict recorded `pass` (observed 0) alongside the existing 16 verdicts.

**Coverage-gap decision** (Task 12 checklist item, scout-flagged): `REPLACE PARTITION`/`ATTACH
PARTITION ... FROM` queue-clone relink on a Replicated CA table (RPL-5 slice) has no dedicated test —
`test_cas_replicated_relink` only proves relink for the plain INSERT/merge fetch path, and RPL-4
documents `to_detached` relink as explicitly disabled, so it is not obvious which branch a
queue-cloned `REPLACE_RANGE` fetch takes without investigation. Judged NOT cheap (requires resolving
the relink-eligibility branch first, not a copy-paste test) — recorded as a scoped BACKLOG TEST-debt
line (`docs/superpowers/cas/BACKLOG.md` §10) instead of rushed into the gate.

All 4 gate steps: **PASS**. Full details in `.superpowers/sdd/all-tree/task-12-report.md`.
