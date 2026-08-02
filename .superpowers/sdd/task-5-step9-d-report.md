# Task 5 Step 9 Slice D report

## Scope and result

Base at the start of the slice: `be5a5c14e0ab96def6c8d2668c842bdc6db9a6da`.
The shared branch advanced through Step 10 to `d85a2b3e74814b44a3a1f07ea8e3abda` before this
slice's commit. The protected aggregate `.superpowers/sdd/task-5-report.md` was not edited or staged.

The retired namespace-cleanup marker/`Removed`-snapshot handshake has no executable producer,
consumer, DTO collection, or positive test left. The audit found and removed five kinds of residue:

- stale production comments that still routed `remove_txn_id` into a namespace-cleanup item;
- an obsolete namespace-shaped ref-intake test that was vacuous with opaque life ids;
- current system-table and walkthrough prose that still described marker publication, the old phase,
  namespace-bearing stream keys, `HEAD`+`GET` manifest intake, or an incomplete suppression predicate;
- S06 comments that still named `cas/refs/<ns>`;
- S38's executable old-layout namespace walk, also imported by S43, which made both cards
  inconclusive before reaching their injections.

S38/S43 now discover exactly one direct canonical lowercase 32-hex child below
`cas/ns/stream/`. Zero, multiple, malformed, uppercase, and nested children fail closed. S38's real
post-injection observation records that id directly. S43 freezes the no-absorption verdict before
performing one recognizable life-2 insert; only that write forces lazy namespace admission, after
which the card asserts the new opaque id differs from life 1 and the inserted row is the only visible
content. S43 remains a useful residual-data bootstrap guard; the same-pool old-byte/rebirth contract
remains covered by the Step 9 C++ tests.

## Executable inventory

Command:

```text
git grep -n -E 'namespaceIsRemoved|refCleanupMarkerKey|RefNsCleanupState|ns_cleanup_items|cleanup_markers|publishRemovedSnapshotNow|runNamespaceCleanupPasses' -- src tests programs utils base
```

Result: no matches.

Broader semantic command:

```text
git grep -n -i -E 'namespace.cleanup item|cleanup marker|removed snapshot|marker-driven' -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed src/Disks/tests
```

The sole match is `src/Disks/tests/gtest_cas_fold_seal_format.cpp:318`. It is intentional negative
coverage: `UnifiedCodecRejectsLegacyNamespaceCleanupRecord` constructs the generation-6 `nsc`
record and requires the generation-7 decoder to reject the marker-driven handshake.

Literal key command:

```text
git grep -n -E '(/_cleanup/|"_cleanup"|_cleanup/|/_cleanup)' -- \
  src/Disks/tests src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed utils/ca-soak
```

All four survivors are intentional:

- `src/Disks/tests/gtest_cas_gc_frontier_gate.cpp:1364` — asserts the removal path journals no
  `/_cleanup/` PUT.
- `src/Disks/tests/gtest_cas_ns_file_incarnation.cpp:125` — asserts logical absence while old bytes
  survive without writing a marker.
- `src/Disks/tests/gtest_cas_ref_writer.cpp:3895` — asserts same-name/same-writer-epoch rebirth writes
  no marker.
- `src/Disks/tests/gtest_cas_namespace_life_id.cpp:215` — deliberately constructs a generation-5
  namespace-bearing `_cleanup` key and asserts it is outside the final grammar.

The current `namespace_cleanup` phase is not residue. It is the one-page perpetual dead-life
janitor in `CasGc.cpp:461`; its phase assertions in `gtest_cas_gc_log.cpp`,
`gtest_cas_gc_round_defer.cpp`, `gtest_cas_gc_shard_incarnation.cpp`, and
`gtest_cas_namespace_janitor.cpp` are current behavior.

The following similarly named state is also intentional and retained:

- in-memory `remove_txn_id` in `CasRefProtocol.h`/`.cpp` identifies the terminal transaction while
  replaying a live runtime;
- `RefCleanupEvidence{remove_txn_id}` in `CasFoldSealFormat.h`/`.cpp` is the positive fold proof used
  by pre-fold catalog drain;
- in-memory `RefLifecycle::Removed` distinguishes a terminally replayed runtime from an empty one;
- mount lifecycle snapshots in `ContentAddressedMetadataStorage`, `CasMountRuntime`, `CasPool`, and
  `gtest_cas_lifecycle_snapshot.cpp` describe the pool mount, not a ref-table `Removed` snapshot.

## Historical documentation classification

The same exact-identifier grep under `docs/` has no hit in current user documentation or the current
walkthrough. Every remaining hit is a dated plan, design record, review, raw observation, backlog
item, or explicit deletion rationale. They are retained as historical evidence:

- archaeology/backlog records:
  `docs/superpowers/cas/2026-07-28-ref-rework-adjacent-findings.md:185`,
  `docs/superpowers/cas/2026-08-02-r1-verbatim-file-aliasing-closure.md:68,76`,
  `docs/superpowers/cas/BACKLOG.md:370`,
  `docs/superpowers/cas/deferred-docs-classification.md:19`,
  `docs/superpowers/cas/deferred-docs-fixes.md:192,204`, and
  `docs/superpowers/cas/review1.md:143`;
- superseded plans:
  `docs/superpowers/plans/2026-07-12-cas-ref-table-snapshot-log-phase1.md:254`,
  `2026-07-12-cas-stabilization-cleanup.md:955,959,994,995,1031,1033,1036,1037,1042,1045,1058,1060`,
  `2026-07-13-cas-pool-member-decommission.md:295,353,354,425`,
  `2026-07-13-cas-ref-lease-exclusivity-rev6.md:1346,1427`,
  `2026-07-15-cas-codecs-v3-phase2-control-plane.md:107,1424,1429,1459,1485,1489,1490,1495,1497,1498,1573,1686,1698`,
  `2026-07-16-cas-source-layout-refactoring.md:521`,
  `2026-07-20-cas-table-load-stuck-asyncloader.md:52`, and
  `2026-07-25-cas-followups-detector-introspection-s42-forceclaim.md:1704,1767`;
- the active Stage B plan's history and deletion obligations:
  `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md:701,1079,1494,1536,1763,1828,2082,2618`;
- dated reviews and telemetry:
  `docs/superpowers/reports/2026-07-14-cas-gc-defense-audit.md:39,43,56`,
  `2026-07-28-ref-rework-reviews/codex_r6_findings.md:690,694,774,1314,1315,1357,1382-1386,1394,1402,1403,1445,1450,1453,1461,1469,2314`,
  `2026-07-30-stage-b-critical-checkpoint-review.md:52`,
  `2026-07-31-removalready-review-fable.md:225,229`, and
  `2026-07-31-stage-b-strategic-review-fable-round3.md:95,167`;
- raw immutable telemetry:
  `docs/superpowers/reports/2026-07-26-s42-stale-edge-repro/raw/gc_log_ca-soak-ch1-1.tsv:8,12,28,32,48,52,68,72,88,92,108,112,128,132,148,152,168,172,188,192,208,212,228,232,248,252,268,272,293,305,309,325,329,345,349,365,369,395,399,415,419,440,444,460,464,485,489,545,549,605,609,659,665`;
- superseded specifications:
  `docs/superpowers/specs/2026-07-12-cas-stabilization-cleanup-design.md:72`,
  `2026-07-15-cas-codecs-v3-design.md:80`,
  `2026-07-15-cas-source-layout-refactoring-design.md:195`, and
  `2026-07-25-cas-gc-observability-and-mount-force-design.md:287,297`;
- authoritative deletion rationale:
  `docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:126,153,346`.

Literal `_cleanup` references in the active Stage B plan at lines
`28,80,96,1082,1092,1191,1283,1759,1762,1771,1855` describe the transitional layout, its deletion,
or negative no-PUT obligation. The authoritative spec's lines `126,341,455` explicitly record the
old class's deletion and format-generation boundary. These are load-bearing rationale, not live API.
Unrelated ClickHouse `_cleanup` names outside the CAS subtree are generic cleanup methods, settings,
or other engines and were not changed.

## Tests and independent evidence

- `build_debug/step9_d_build.log`: `ninja -C build_debug unit_tests_dbms`, exit 0; final line links
  `src/unit_tests_dbms`.
- `build_debug/step9_d_exact.log`: five marker/rebirth/legacy pins, 5/5 passed.
- `build_debug/step9_d_suite.log`: `CasRefIntake.*`, `CasGcFold.*`, `CasGcFrontierGate.*`,
  `CasNsFileIncarnation.*`, `RefWriterNamespaceRemoval.*`, and `CasNamespaceLifeId.*`, 86/86 passed.
- `build_debug/step9_d_s38_red.log`: expected TDD RED; collection fails because
  `_discover_single_life_id` does not exist yet.
- `build_debug/step9_d_s38_green_review2.log`: discovery, post-injection observation, S43 ordering,
  and counter-probe unit tests, 13/13 passed.
- `build_debug/step9_d_pycompile_review2.log`: `py_compile` for S06, S38, S43 and their unit test,
  exit 0 and empty log.
- `build_debug/step9_d_diff_check_review2.log`: `git diff --check`, exit 0 and empty log.
- `build_debug/step9_d_blocker_scan_review2.log`: no deleted `ns` observation, retired helper,
  stale suppression conjunct, or false "entire durable state" wording, exit 0 and empty log.
- `build_debug/step9_d_diff_check_review3.log` and
  `build_debug/step9_d_blocker_scan_review3.log`: the final maintenance-state key spelling fix is
  whitespace-clean and no stale hyphenated key or prior blocker remains; both logs are empty.

The C++ changes made after the dedicated build are comments only; no production or test behavior was
changed. The behavior-changing revision after review is Python-only and has its own RED/GREEN test.

## Adjacent observations

- S43 cannot simultaneously prove the residual-data bootstrap guard and old-object/new-life isolation:
  the former deliberately refuses before a catalog life exists, then deletes the planted object to
  prove causation. Its post-absence forced insert and distinct-life-id control prevent the old
  path-alias premise from returning; same-pool rebirth with predecessor bytes is covered by the
  Step 9 C++ tests.
- The phase-number comments in `CasGc.cpp` were not renumbered when `pre_fold_ref_drain` was inserted:
  `heartbeat_floor` still says `PHASE 2/19` although it executes third. This is unrelated to the
  retired marker surface and should be handled as a separate mechanical observability cleanup.
