# Unattended work log — B140-dangle real fix (build-root/precommit redesign)

Started: 2026-06-18 evening (user asleep, I am in charge; no questions, no stops).

**Mission:** Fix B140-dangle properly via the approved build-root/precommit design. Follow the
superpowers flow: design spec → TLA+ (brainstorm/spec/plan/impl/test) → C++ (spec/plan/impl/test) →
12h soak. Record all deferred/debt items in the backlog. Keep this log current.

**Approved design (from the brainstorming dialogue):** protection becomes *reachability from a durable
build root*, not a revocable hint. A build precommits its manifest tree under a build root (references
blobs by content-hash, published before/while blobs upload — local staging only for now); everything
reachable from the build root has in-degree ≥ 1 and GC cannot collect it. The real commit publishes
the table-namespace ref only when the full closure is present (fail-closed), then removes the
precommit. Abandoned precommits (dead builds) are reclaimed via the existing per-server watermark
(repurposed from per-object protection). The fragile `cas_owner` / `protectedByLiveBuild` per-candidate
machinery is DELETED. Four invariants: INV-NO-DANGLE-COMMITTED, INV-BUILDROOT-PROTECTS,
INV-BUILDROOT-RECLAIM, INV-COMMIT-FAILCLOSED.

Root cause that motivated this (report 2026-06-18-ca-b140-dangle-trigger-pinned.md): `reuseBlob` adopt
doesn't transfer `cas_owner`; an adopted blob's protection stays bound to its (retired) byte-writer, so
GC reclaims it under a still-in-flight adopter → publish dangles. Pinned live in the soak via B170
`system.content_addressed_log`.

---

## Timeline

- **T0** — Set up tasks #137–#141, this work log, backlog entries. Soak stopped, cluster down, dangle
  evidence preserved under `utils/ca-soak/tmp/b140_dangle_soak_20260618/`. Starting the design spec.
- **T1** — Design spec written + committed (`a208ab14d41`):
  `specs/2026-06-18-ca-build-root-precommit-design.md`. Backlog B171 (active) + B172 (deferred staging)
  added; B170 marked DONE. Tasks #137, #132, #136 closed.
- **T2** — TLA+ phase started (task #138): dispatched a background subagent to author
  `models/CaBuildRootPrecommit.tla` + buggy/fixed configs, with the two-config discipline (buggy MUST
  reproduce `INV_NO_DANGLE_COMMITTED`, fixed MUST exhaust clean; premature-reclaim + ordering
  interleavings covered). Awaiting completion before C++ implementation (ordering: TLA+ first).
- **T3** — While TLA+ runs: drafted C++ impl spec
  `specs/2026-06-18-ca-build-root-precommit-cpp-impl.md` (build-root namespace `_builds/<server>`,
  two-phase `Build::precommit`→fail-closed `publish`→remove precommit, GC fold + pending-tolerance +
  precommit reclaim via repurposed watermark, delete `cas_owner`/`protectedByLiveBuild`, failing-gtest
  list). Marked draft pending TLA+. Grounded in the actual layout/registry/fold/mutateShard code.
- **T4** — TLA+ GREEN + independently re-verified (task #138 done, subagent commit `fc3ac31447b`).
  `models/CaBuildRootPrecommit.tla` + 2×2 flag matrix. Results: buggy → `INV_NO_DANGLE_COMMITTED`
  violated (481 distinct states to CE; my re-run exit=12 confirms); fixed (UseBuildRoot ∧
  FailClosedCommit) → clean exhaustive 24205 states (my re-run "No error"). Non-vacuity witnesses
  confirm the dangerous interleavings are reachable+survived. **Sharp finding:** build-root ALONE
  still dangles (ordering window) and fail-closed-commit ALONE is clean only vacuously — both halves
  independently necessary, jointly sufficient (validates design §4.5/§4.6). Buggy CE matches the soak
  dangle exactly (write→adopt→BuildDie→GcDelete→Commit). Design CONFIRMED; C++ impl spec stands as-is.
- **T5** — Starting C++ implementation (task #140): writing-plans → subagent-driven TDD.
  Plan `plans/2026-06-18-ca-build-root-precommit.md` committed (`db0af38f506`). Build env: `build/`
  dir, `build/src/unit_tests_dbms`.
- **T6** — Task 1 RED (`b6e4f632456`): build-root taxonomy (`Precommit`/`PrecommitRemoved`/
  `PrecommitReclaim`), `Layout::isBuildRootNamespace` + `_builds` reserved, `Build::precommit` stub +
  `buildRootNs`/`buildShard`, failing dangle repro `CasBuildRootDangle.SharedBlobSurvivesSourceDrop…`
  (RED via ABORTED "dependency lost" — blob GC-deleted under retired owner). 122 others green.
- **T7** — Task 2 (`f3b1040f1fd`): real `Build::precommit` (build-root edge); GC folds the build root
  with pending-tolerance (skip the live-ref→missing-tree alarm for `_builds/` namespaces). **RED test
  now PASSES** — blob survives via reachability. Subagent CAUGHT+FIXED a latent bug: build-root shards
  are keyed by `build_seq` (beyond table shard fan-out), so GC needed `shardsToVisit(ns)` to LIST them.
  126/127 green.
- **T8** — Task 3 (`b0079ca1962`): fail-closed commit (unconditional `revalidateDeps`) + remove
  precommit after commit. New test `PrematureReclaimCommitFailsClosed` — commit ABORTs (never dangles)
  when the precommit was reclaimed mid-build. Caught a 2nd shard-keying bug (`dropRef` not shard-aware
  for build-root → shard-correct `mutateShard` drop). 119 green, 0 regressions.
- **T9** — Task 4 (`973a5e5ccbe`): GC auto-reclaims abandoned precommits (watermark liveness:
  `w==null ∨ !live ∨ build_seq<min_active`); DELETED `protectedByLiveBuild` + `cas_owner`/`ownerMeta`
  (watermark retained, repurposed). New test `AbandonedPrecommitReclaimed`. Translated 4 existing tests
  from the cas_owner model to the precommit/reachability model. Design-gap analysis: the putBlob→
  precommit window is exactly the design's documented residual, closed by fail-closed commit (lost
  work, never lost data). **161 pass, 1 intentional red.**
- **T10** — Task 5 (`acbc906074e`): wired `Build::precommit(tree)` into ALL THREE integration publish
  sites — `publishStaging` (INSERT/merge/mutation/hardlink), `republishRef` (RENAME/cross-engine
  repoint), `adoptPart` (replication relink). `ninja clickhouse` + `unit_tests_dbms` build CLEAN. 141
  pass, 1 intentional red. C++ implementation COMPLETE.
- **T11** — Final comprehensive review of the B171 change (6 commits) before the soak.
