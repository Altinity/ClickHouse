---
description: 'Live backlog — operability, release gates, disk-error hardening, fsck/introspection surfaces, and the lazy_load_tables decision.'
sidebar_label: 'Operability & introspection'
sidebar_position: 7
slug: /superpowers/cas/backlog/operability-and-introspection
title: 'CAS Backlog — Operability and introspection'
doc_type: 'guide'
---

# CAS Backlog — Operability and introspection {#operability-and-introspection}

Part of the [CAS live backlog](/superpowers/cas/backlog). Topic file for operability, release gates,
disk-error hardening, fsck/introspection surfaces, and the `lazy_load_tables` decision.

## Operability & release gates {#operability}

- **[B197] `SYSTEM` control surface — START/STOP GC, POOL READONLY, CHECK** — GATE — Product-side GC stop is currently only a soak-harness workaround.
- **[B198] backup/restore runbook** — GATE — No runbook exists yet for CAS pool backup/restore; needed before the feature can be called operationally supported.
- **[B180 / format-freeze] pool-format version breadcrumb + first-release format freeze + rollout machinery** — GATE — Stamp the pool self-describingly; freeze the format on the first persisted-data release (schema-evolution framework is in place); durable roster + `max_content_addressable_pool_format` setting/rollout machinery not built (Part IV).
- **[B15/B99/B169/B159] `system.*` views for pool/blob/part refcounts + `clickhouse-disks` decode/introspect** — HARD (PARTIAL) — GC log + event log + `content_addressed_mounts` + ca-fsck/dryrun/rebuild/ca-inspect CLI done; per-part/ref `system.*` views + a top-down decode/traversal surface not yet. (INTROSPECTION-1/2 close signals.)
- **[B13] migration path for existing tables** — HARD — `ALTER TABLE … MOVE PARTITION` to a `content_addressed` disk re-packs; mixed-version rollout rule (read-new-before-write-new; format self-check fails closed) + a rollout-safety spec.
- **[F1-prod] read-only same-pool shadow disk (`ca_ro`) breaks table load on restart** — GATE (prod) — MergeTree part discovery finds every part twice → `UNKNOWN_DISK` on restart with CA tables. Stand workaround shipped (standalone `clickhouse-disks -C` fsck-only config; propagated to the default stand); PRODUCT fix (part discovery skips `readonly` same-pool disks, or a `hidden`/`introspection_only` disk flag) still open; `10replicas`/`gc_shards2`/`awss3` server configs may still embed `ca_ro`.
- **[B165] server OOM at hour-4 soak (~49 GiB RSS)** — VERIFY — Not reproduced since the `putBlob` streaming fix; re-run a long soak to confirm resolved.
- **[B14] expedited / GDPR right-to-erasure delete** — DESIRABLE — Under GC lock, confirm no live ref, then delete bypassing the two-phase graduation delay; no layout change.
- **[B17] encryption-at-rest × content-addressing** — DESIRABLE — Dedup scope per-encryption-key; local to key/hash derivation.
- **[B131] repo hygiene + M-W comment sweep** — GATE — 30 dangling `M-W`/`D-W1`/`2026-06-12-ca-core-m-w` comment references across 13 src files (incl. `ContentAddressedMetadataStorage.{h,cpp}`, `CasGcScheduler.h`, `DataPartsExchange.cpp:106`) reference the deleted plan — sweep to self-contained wording. Non-shippable files: `poc/cas_mergetree/` already deleted (F1 landed); the untracked empty `poc/` husk remains.

## Disk-error (ENOSPC / inode-exhaustion) audit follow-ups {#disk-error-audit-followups-2026-07-21}

Staging/target/GC disk-error audit verdict held (staging ENOSPC fail-loud, Native S3 corruption-free,
GC decision-durable-before-delete). Residual gaps, ordered by value:

- **HARD: size guard at dedup-admit** — `PartWriteTxn::observeAndAdmit` never compares the observed
  size against the caller's expected size, so a truncated object at a content-addressed key can be
  admitted as a dedup hit, producing a durably unreadable part.
- **HARD: temp-file + rename in the local blob write path** — tracked in formats-and-storage.md
  (`disk-error-audit`), paired with the guard above.
- **DESIRABLE: fsck physical-size check for blob bodies** — `runFsck` HEADs every blob but never
  compares physical size against the expected size, so a truncated blob passes as `Reachable`; the
  listing already carries the sizes, so this is free.
- **DESIRABLE: free-space guard + orphan sweeper for `scratch_path`** — no `statvfs` check before a
  local staging write, and orphaned `*.tmp` files from an unclean restart are never swept (the S3
  staging prefix has a sweeper; local scratch does not).
- **MINOR: wrap the GC post-CAS cleanup in try/catch** — the post-CAS manifest-body delete loop and
  hand-off prefix wholesale delete aren't wrapped, so a genuine backend error escapes the round after
  its `gc/state` CAS already committed (data-safe, but reddens the round unnecessarily).
- **DESIRABLE: GC scheduler backoff + a distinct storage-full signal** — the pacing loop retries a
  failing round forever with no backoff and no ProfileEvent distinguishing target-storage-full from
  generic instability.
- **VERIFY: late-landing conditional PUT after fence loss** — same hazard class as the historical
  Late-Predecessor-PUT item (ref-protocol.md); confirm successor-side `writer_epoch` gating rejects it,
  fold into rev.6 lease work rather than tracking separately.
- **MINOR: destructor-`abandon` live-epoch precommit debris** — if `abandon` fails during a failed
  transaction's destruction, the live-epoch precommit binding persists until remount; bounded, but
  worth a periodic re-`abandon` retry under a persistently broken backend.

## `[CA-s3 Disk session pressure]` `ConnectionGroup: Too many active sessions in group Disk` {#ca-s3-disk-session-pressure}

On the asan CA-s3 lane (run for `e2d04bfe37e`), `00149_quantiles_timing_distributed` flipped on a
leaked stderr warning: `ConnectionGroup: Too many active sessions in group Disk, count 10400,
warning limit 8000`. The test's stdout was correct and reruns passed — the failure is warning noise,
but 10k+ concurrently active Disk-group sessions under parallel load is a real pressure signal for
the CA-s3 request fan-out (compare the write-path request-class findings in the disk-error audit
follow-ups above and the insert-slowness item). Worth a look at whether CA holds S3 sessions longer
than needed (e.g. across retry backoffs) before raising any limit.

## `lazy_load_tables` / `StorageTableProxy` — feature-level decision needed (consult audit 2026-07-21) {#lazy-load-tables-decision-2026-07-21}

Third incident of the same class (unforwarded `IStorage` virtual / direct cast through the proxy):
SYSTEM verbs (fixed, 05017), action-lock parking (open), mutations (`checkMutationIsPossible`,
fixed + 05021). A commissioned audit
(`docs/superpowers/reports/2026-07-21-storageproxy-forwarding-audit.md`) found **~60 unforwarded
virtuals, ~45 of class "must forward"**, including a critical one: `backupData`'s no-op default
means a BACKUP of a not-yet-materialized lazy table silently contributes NO data. Design findings:
no compile-time guard exists for "new virtual not forwarded"; swap-on-materialize does NOT fix the
class (escaped `StoragePtr`s in the UUID map/action locks + two lock domains); the clean long-term
shape is catalog-entry laziness (real refactor). Consultant recommendation: the feature as
implemented is net-negative — disable/quarantine rather than fix one virtual at a time.

- [ ] **USER DECISION**: quarantine/disable `lazy_load_tables` vs fund the full remediation
  (complete forwarding sweep + Clang-AST CI guard + backup regression test) vs catalog-entry
  laziness refactor. Until decided: treat every new lazy-table symptom as this class first.
- [ ] THIRD bug of the class found while validating the mutation fix (2026-07-22): `MATERIALIZE
  TTL` through a lazy proxy fails with `INCORRECT_QUERY` "no TTL set" even after the
  `checkMutationIsPossible` forward — the proxy's cached in-memory metadata carries columns only
  (no TTL/ORDER BY), and `getInMemoryMetadataPtr` deliberately does not forward (audit class C).
  Candidate rule if the feature stays: forward metadata to nested ONCE MATERIALIZED (no laziness
  left to preserve at that point); needs its own consult.
- [ ] If the feature stays: forward at least `backupData`/`restoreDataFromBackup`/
  `supportsBackupPartition`/`finalizeRestoreFromBackup`, `onActionLockRemove`,
  `supportsOptimizationToSubcolumns` (the audit's three most-urgent), then the rest of class B.
- [ ] FOURTH bug of the class + a REVERT (2026-07-22, xhigh review): the `checkTableCanBeRenamed`
  forward added on `StorageTableProxy` (7ab1fc15f4c) was REVERTED — it materializes the lazy table
  (`getNested`) while `DatabaseAtomic` holds its non-recursive database mutex (DatabaseAtomic.cpp:321/346),
  and a schema-inferred lazy `Buffer` resolves its destination via `DatabaseCatalog::getTable` in its
  constructor (StorageBuffer.cpp:180), re-entering the same database and self-deadlocking (cross-database
  RENAME/EXCHANGE can hold two database mutexes across the same work). So the nested engine's rename
  restriction is once again bypassed for a lazy (never-accessed) table — the pre-existing gap is REOPENED,
  not newly introduced. Correct fix (same shape as the other class-C bugs): materialize the proxy BEFORE
  any database mutex is taken, at the interpreter level, then re-fetch/verify identities under the lock and
  run the check on the materialized storage. NOTE for any upstream PR: the KEPT generic `checkMutationIsPossible`
  forward on `StorageProxy` also changes `StorageTableFunctionProxy` semantics (a table-function proxy now
  answers the mutation-possibility check from its nested storage rather than the `IStorage` default) — sound,
  but call it out explicitly (codex F5).
- [x] RESOLVED as misdiagnosis + REAL FIX LANDED (f1f11 soak 2026-07-21): the "post-kill CA table load takes minutes" finding was an artifact — the table sits in a lazy_load_tables=1 DB (706095958ea) and materializes in ~18 ms on first touch; nothing touched it post-kill, while SYSTEM SYNC REPLICA misreported the unmaterialized StorageTableProxy as "is not replicated". Fixed in 2ba28ac4b6f (unwrapTableProxy across single-table SYSTEM verbs + stateless test 05017). OPEN EMPIRICAL TAIL: measure post-fault getNested cost under churn at the next soak's first chaos checkpoint — if genuinely minutes, that is the real availability item.
- [ ] lazy_load_tables follow-ups (from T15 review, pre-existing): whole-db DROP REPLICA safety scan (InterpreterSystemQuery.cpp:~1687) and RESTART REPLICAS iteration skip unmaterialized proxies — a stale remote replica in ZK may stay uncleaned for lazy tables; STOP/START <action> on a single lazy table parks the ActionLock on the PROXY, invisible to the later-materialized nested storage.

## fsck and introspection surfaces {#fsck-surfaces}

- **[partb-review-findings] `eraseView`/`publishStaging` no-throw-after-commit residual** — {#partb-review-findings} — MINOR — The 2026-07-25 publish-confirm protocol review's two blockers and two majors are fixed (`8e6fe6ef0af`); the one residual window: `eraseView` still runs after the durable commit and can throw, and `ContentAddressedTransaction::publishStaging`'s `out_slot` is assigned only after `promoteBuild` returns — closing it means extending the no-throw-after-commit discipline one frame outward.
- **[fsck-large-pool-fixed] fsck large-pool reporting: three residuals after the 2026-07-26 fix** — {#fsck-large-pool-fixed} — MINOR — `corrupted_runs` visibility/fatality, inverted timeout budgets, and fabricated-clean-on-partial are fixed. Open: (a) `checker.py`/`run.py`/`plot.py` still print the old `M-F debris, B140` label for what the product now classifies as `AwaitingGc` (docstring cleanup only); (b) `FsckTimeout` still substitutes fabricated `{"dangling": 0, ...}` zeros on the remaining timeout path — landmine, not a live defect (every consumer guards on `not _detail_fsck_skipped`), fixing it needs auditing every downstream `f.get(...)`; (c) the GC-checkpoint entry-gate fsck still does not finish on a 5.5 GB pool within its 180s budget — options not yet chosen (scale the budget, use `--partial` as a lower bound, or make fsck cheaper).
- **[S42-ci-verdict] S42 memory-exhaustion card needs a clean re-run to certify** — {#s42-ci-verdict} — TEST — `ci`-scale S42 passed every safety signal (2,184 injected allocation faults fired, `CasRefApplyPoisoned=0`, both wedged ref lanes recovered, `CasGcUnmatchedRemoveDeltas=0`, 11,960 acked blocks survived) but failed 2/28 of its own strict verdicts on request-timeout cascades traced to a compromised host (still recovering from a prior `--scale full` attempt, load average 22-48). Not evidence of a defect, but not a certification either — re-run on a quiet host before calling S42 green.
- **[fsck-untestable-render-surfaces] the fsck exit set and SQL row still have no test that can fail for them** — {#fsck-untestable-render-surfaces} — The hard-finding rule now EXECUTES — `FsckReport::clean` is computed from `kFsckHardFindings`, and a `static_assert` on that list's deduced size trips in all three surfaces' translation units — so the rule no longer depends on a reader remembering it. What remains: an author who reads the assert as an arithmetic complaint, bumps the count, and does not visit the three surfaces. The summary line has a real test; the nonzero-exit set and the SQL row do not, because `contentAddressedFsckColumns` and `appendContentAddressedFsckRow` have internal linkage in an anonymous namespace, and `programs/disks` is not linked into `unit_tests_dbms`. Closing it means giving those functions external linkage plus a header — a structural change to `src/Interpreters/InterpreterSystemQuery.cpp`, a shared non-CAS file. **CONSULT ITEM, not a task**: per the standing rule that shared/upstream surfaces are not edited without consultation, this needs a decision before anyone implements it; the cheap alternative worth weighing first is whether the assert's message can be made harder to satisfy without visiting the surfaces at all. Also recorded: `05020_content_addressed_fsck.reference` pins the row via `TSVWithNames`, so it is a ONE-DIRECTIONAL fence — it fails on a column added without updating the reference, not on a `clean()` term added without a column; only the `static_assert` covers that direction.
- **[fsck-rule-restated-in-unfenceable-prose] the fsck exit-code rule is restated in prose the fence cannot reach** — {#fsck-rule-restated-in-unfenceable-prose} — The rule now EXECUTES in code (`FsckReport::clean` computed from `kFsckHardFindings`, `static_assert` tripping in three TUs), but the same rule is also RESTATED in prose that no build can check — `docs/superpowers/cas/08-testing-and-soak.md` and two harness files. Three of those restatements were found wrong in one round, one of them inside an operator-facing warning string. Nothing mechanical will catch a fourth. The real fix is structural and a documentation decision, not a code task: the doc should point at the code rather than restate the exit set — a reader who needs to know which findings exit nonzero should be sent to `kFsckHardFindings` and to `CommandFsck::executeImpl`, not handed a list that drifts; the same applies to the harness's comments. Related: `{#fsck-untestable-render-surfaces}` is the other half — together they bound what the fence does and does not reach.
- **[loose-mountpoint-object-as-corrupt-namespace-file] a loose mountpoint object under `_files/` is classified as a corrupt namespace file** — {#loose-mountpoint-object-as-corrupt-namespace-file} — `Layout::mountpointObjectKey` does not enforce the `_files` reservation its own doc comment claims, so a loose object at `roots/<srid>/_files/x` satisfies `parseNamespaceFileKey`'s necessary condition and gets treated as ours — `ca-decommission` refuses fail-close and `ca-fsck` posts a hard `lifeless_keys` finding against a key that is not damage. Direction is safe (refuse/report, never delete) so not urgent, but a hard finding against an undamaged key trains an operator to disbelieve hard findings. **Open decision**: enforce the reservation in `mountpointObjectKey` (makes the existing doc comment true) or narrow the classifier.

## New findings from the 2026-08-04 orphaned-open triage {#orphan-triage-2026-08-04}

- **[drop-replica-stop-proxy-forwarding-tails] `DROP REPLICA`/`STOP <action>` proxy-forwarding tails** — DESIRABLE — Distinct from the fixed `onActionLockRemove` half; a remaining `lazy_load_tables`-adjacent forwarding gap.
- **[server-root-id-macro-docs] server-config macro-expansion template for `server_root_id`** — DOC — A genuine deployment-docs gap: no documented macro-expansion template exists for configuring `server_root_id` across a fleet.
- **[gc-observability-field-list] GC observability field list (heartbeat lag, B170 event classes, retired-list age, invariant alert)** — DESIRABLE — A concrete dashboard/metrics gap, though partially covered by `system.cas_gc_log` — check the overlap before building anything new; only the uncovered fields are the real ask.
- **[fsck-partial-degrade-false-consistency] fsck `--partial` degrade path would fabricate a false consistency proof if wired in naively** — HARD — A concrete correctness guard needed before the `--partial` feature ships more broadly.
- **[fsck-crossepoch-life-omission] `CasFsck` omits life when calling `crossEpochFromSeal`, risking a false unconsumed-closing-seal report** — MINOR — Review finding NEW-3; narrow but concrete fsck-correctness bug with no evidence of a fix.
- **[storageproxy-mergetree-virtuals-not-forwarded] `StorageProxy` doesn't forward `isMergeTree`/`supportsTTL`** — DESIRABLE — A plausible functional bug: queries checking these virtuals on a not-yet-materialized lazy table would get wrong answers. Verify against HEAD `StorageProxy.h`; if still unforwarded, this affects any lazy-loaded MergeTree, not just CAS, so scope and file it generically.
- **[storageproxy-ast-interface-guard] Clang AST CI check for `StorageProxy` interface growth** — DESIRABLE — Compares virtual `IStorage` declarations against `StorageProxy` overrides, requires rationale for allowlisted omissions; concrete tooling proposal, no existing coverage.
- **[storageproxy-subcolumn-forwarding-bug] `StorageProxy` inherits `supportsOptimizationToSubcolumns`, over-reporting support for opted-out nested engines** — MINOR — A real, specific, scoped correctness finding from the storageproxy-forwarding-audit.

## Lifecycle verbs wait out an uncancellable GC round or FSCK scan (2031-triage CAS-049) {#lifecycle-verbs-wait-out-uncancellable-scans}

P2, operability only — nothing is corrupted or lost, and no data path is blocked
(`poolAccess`/`gcHealth` never take these mutexes; the snapshot at
`ContentAddressed/ContentAddressedMetadataStorage.cpp:432` is explicitly forbidden from waiting behind
`gc_scheduler_mutex`). What is missing is cooperative cancellation:

- A GC round has no stop hook. `CasGcScheduler::stop` (`Gc/CasGcScheduler.cpp:72-88`) sets `stopping`,
  notifies `wake`, and then `join()`s — an in-flight round runs to completion, and the loop comment at
  `:295-297` records the accepted extra round. `SYSTEM CAS GC STOP` (`:978`), `SYSTEM CAS FORGET`
  (`:926`) and `shutdown` (`:887`) therefore all wait it out (the synchronous round through
  `gc_scheduler_mutex`, held for the whole round at `:389`/`:619`/`:665`; the background one through
  the join). The round's destructive/recovery work IS capped (`GcRoundWorkBudget`,
  `Gc/CasBlobInDegree.h:251`, filled from the non-zero defaults at `ContentAddressedSettings.cpp:76-83`),
  so the wait is finite — but there is no time budget at all in the settings, and against a slow bucket
  the wall-clock wait is whatever the bucket makes it.
- `SYSTEM CAS FSCK` passes none of the bounding parameters the CLI passes and cannot be killed.
  `runFsckNow` calls `Cas::runFsck(*store(), detail)` (`:1063`) while holding `lifecycle_mutex` for the
  whole scan (`:1051`), whereas `programs/disks/CommandFsck.cpp:67` passes `on_progress`, `deadline`,
  `partial` and `namespace_prefix` (signature: `Tools/CasFsck.h:269-271`). The scan checks no query
  cancellation either, so the statement (`src/Interpreters/InterpreterSystemQuery.cpp:2599`) ignores
  `KILL QUERY` and `max_execution_time`, and `FORGET`/`GC STOP`/`GC START` block behind it for the
  duration. Serializing them against FSCK is deliberate (see the comment at `:1052-1053`); being
  unable to bound or interrupt the scan is not.

Owed: a stop token threaded through the round phases, and a SQL FSCK that derives a deadline from
`max_execution_time`, sets `partial_on_deadline`, and polls query cancellation. Related and already
tracked: `gc.md`{#fsck-scale-timeout} (fsck does not finish on a ~30 GiB pool) and
{#fsck-large-pool-fixed} item (c). Not tracked before this entry: the cancellation gap itself and the
SQL-vs-CLI parameter asymmetry.

Corrected while triaging: `shutdown` does NOT serialize behind an in-flight FSCK — it takes
`gc_scheduler_mutex` and `pointer_mutex` only (`:891`), never `lifecycle_mutex`, and the pool stays
alive through the `shared_ptr` the scan holds. Its only wait is on a GC round, which is the documented
priority choice at `:889-890` ("clean GC completion over fast shutdown").
