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

## `DiskEncrypted` over a CA disk hides `isContentAddressed` from every CA-aware branch {#encrypted-wrapper-hides-content-addressed}

Prerequisite detail for [B17] whenever encryption-at-rest × content-addressing is actually designed
(2031 triage, CAS-060). Two independent facts at HEAD:

1. `DiskEncryptedTransaction::writeFile` mints a fresh random IV for every rewrite-mode write
   (`src/Disks/DiskEncryptedTransaction.cpp:106-112`), and CAS hashes the bytes handed to it
   (`ContentAddressedTransaction.cpp:1814`, `:1876`). So every write through an encrypted wrapper
   is a unique blob: dedup does not merely narrow to per-key scope, it disappears entirely — even
   for the same server rewriting byte-identical plaintext.
2. `DiskEncrypted` does not forward `isContentAddressed` (only `ReadOnlyDiskWrapper` does —
   `src/Disks/ReadOnlyDiskWrapper.h:92`; the base returns false at `src/Disks/IDisk.h:477`). Every
   CA-aware branch therefore sees a non-CA disk: the whole-part transaction choices
   (`DataPartStorageOnDiskBase.cpp:422`, `:542`, `:735`), the relink fetch path
   (`DataPartsExchange.cpp:161`), the projection parent-transaction rule
   (`IMergeTreeDataPart.cpp:1364`, `MergeTask.cpp:567`) and the BACKUP-restore whole-part transaction
   (`MergeTreeData.cpp:7544`) all take the plain-object-storage path over a pool that is in fact
   content-addressed.

Neither is silent corruption — (1) is a space/cost regression and (2) lands on CAS's own per-file
autocommit rejections, i.e. loud failures — but any encryption work must start by deciding the
dedup-scope/key-derivation question and by making the wrapper CA-transparent (or refusing the
combination at config validation, which nothing does today).

## Every CA CLI/DR verb opens the pool through `_pool_meta`, so damage to that one object disables the instruments {#pool-meta-bootstrap-blocks-dr-tools}

Sub-item of `BACKLOG.md`{#damaged-object-repair} (2031 triage, CAS-061), naming the one object kind
that item's list (`_ckpt`, fold seal, `gc/state`, catalog) does not: `_pool_meta`. All five CA tools
(`cas-fsck`, `cas-inspect`, `cas-gc-dryrun`, `cas-gc-rebuild`, `cas-drop-member`) reach the pool only
via `ca->store()`, i.e. via `Cas::Pool::open`, which ends at
`PoolMeta::createOrValidate(..., allow_mint=!read_only)` (`Pool/CasPool.cpp:494-496`). A read-only
open — which every tool is required to use (`programs/disks/CommandFsck.cpp:54`,
`CommandCaInspect.cpp:48`, `CommandCaGcRebuild.cpp:54`, `CommandCaGcDryRun.cpp:38`, `CommandCaDropMember.cpp:47`) — must never
mint, so an absent `_pool_meta` fails closed (`Pool/CasPoolMeta.cpp:143-146`) and an undecodable one
throws out of `decodePoolMeta` (`Formats/CasPoolMetaFormat.cpp:105-117`). Consequence: the single
damaged object locks out even `cas-inspect`, whose only use of the pool is a raw-key `GET` plus
`Layout` (`CommandCaInspect.cpp:52-56`) and which therefore does not need pool metadata at all.

Owed, in increasing cost: (a) let `cas-inspect` (and fsck's diagnose-only mode) work off a
pool-meta-less "raw backend + layout" open so the operator can read the damaged bytes; (b) an fsck
row that distinguishes `_pool_meta` present-and-undecodable from absent; (c) decide whether
`_pool_meta` is repairable at all — `pool_id` is a random u128 minted at creation, so it is
restorable from a backup copy of the object but not derivable, which makes the honest answer for
(c) "restore, not repair" and belongs in the runbook item 4 of {#damaged-object-repair}.

## The fsck meta/body pairing counters are computed and rendered nowhere (2031-triage CAS-062) {#fsck-meta-body-counters-unrendered}

P3, observability only — the two counters are ADVISORY by design and correctly excluded from
`FsckReport::clean` (pinned by `src/Disks/tests/gtest_cas_fsck.cpp:1224-1259`), so nothing here is a
missed hard finding.

`runFsck` counts `meta_without_body` and `body_without_meta`
(`ContentAddressed/Tools/CasFsck.cpp:1043,1046`), but no surface prints either one:
`formatFsckSummary` omits both (`Tools/CasFsck.cpp:1155-1174`), `contentAddressedFsckColumns` /
`appendContentAddressedFsckRow` omit both
(`src/Interpreters/InterpreterSystemQuery.cpp:2433-2478,2482-2504`), `programs/disks/CommandFsck.cpp`
never mentions them, and `detail` mode emits no per-object row for them either (the pairing loop only
increments). Outside the gtest, the only reader of these fields is nobody.

That makes the field comment in `Tools/CasFsck.h` (`meta_without_body`: "Counted and reported;
excluded from `clean()`") wrong on its "reported" half, which is exactly the shape
{#fsck-rule-restated-in-unfenceable-prose} is about: prose asserting a rendering that no surface
performs. Owed: either render both counters on the summary line (and, if rendered there, on the SQL
row for the same reason the other non-`clean` counters are on it) plus a `detail` row naming the
offending hash, or delete the counters and the comment together. Decide which — a counter no consumer
can read is not an audit signal.

The rest of CAS-062 is not new: the SQL FSCK's missing deadline / scoping / cancellation is
{#lifecycle-verbs-wait-out-uncancellable-scans}, per-object keys from SQL is the documented YAGNI at
`InterpreterSystemQuery.cpp:2426-2429` (the `clickhouse-disks cas-fsck --detail` applet is the
per-object surface), and "no repair path" is `gc.md`{#ckpt-damage-no-repair-path} for `_ckpt` — while
`SYSTEM CAS GC REBUILD` (`InterpreterSystemQuery.cpp:2545`) already is the repair path for the
in-degree/`stale_edge` class.

## `putIfAbsentControlled` discards the exception that decided the attempt's outcome (2031-triage CAS-068) {#putifabsent-swallowed-attempt-cause}

The byte-exact ref/manifest lane's classification point (`Backend/CasRequestControl.cpp:358-361`)
does `catch (const std::exception & e) { attempt_outcome = classifyConditionalWriteResult(e); }` — the
exception object is never logged and never carried out. This is the DELIBERATE half of the
`isDeterministicLocalFailure` decision (`4f4f93c6bc6`: "`putIfAbsentControlled` (the byte-exact
ref/manifest lane) is deliberately unchanged", because its resolve-by-identical-bytes makes retrying
any unproven error harmless), and it is not a correctness item: every exit is fail-closed
(`Pool/CasRefLedger.cpp:3869` wedges the lane on a sent-attempt `Unresolved`; `resolveByExactGet`
throws `CORRUPTED_DATA` on a genuine different-object conflict, `Backend/CasRequestControl.cpp:299`).
What is missing is the diagnosis: on a lane that wedges after burning `max_attempts`, the wedge
message names only `describeUnresolvedReason` (`CasRefLedger.cpp:3919`), so the actual per-attempt
failure — socket error, timeout, S3 code, or a deterministic local bug the sibling ops would have
rethrown — appears in no log line at all. Owed: a rate-limited `LOG_DEBUG`/`LOG_WARNING` of the
classified exception at the classification point (the `logCasWriteRetryLater` limiter already exists in
this file), so a wedged ref lane can be root-caused without a rebuild with instrumentation. Same class
as `{#fsck-meta-body-counters-unrendered}`: a signal computed and then not shown to anyone.

## Incomplete multipart uploads and `_probe/` debris are invisible to every CAS accounting surface (2031-triage CAS-082) {#mpu-and-probe-debris-unaccounted}

Two cost-only (never correctness) residuals, both P3:

**Incomplete multipart uploads.** CAS adds no multipart bookkeeping of its own — every remote body
goes out through `IObjectStorage::writeObject` → `WriteBufferFromS3`, which already aborts the upload
on cancel and in its destructor (`src/IO/WriteBufferFromS3.cpp:244`, `:313-316`,
`abortMultipartUpload` at `:469`), so an exception or a normal-shutdown teardown leaves nothing
behind. What survives is the process-kill case (SIGKILL/OOM/host loss) and a failed `AbortMultipartUpload`
call: the parts stay billed until the bucket's own lifecycle rule expires them. This is identical to
every other ClickHouse S3 disk and cannot be fixed inside the process, but CAS is the storage whose
docs promise a complete byte accounting for the pool (fsck `physical_bytes` sums HEAD sizes of visible
objects only — `Tools/CasFsck.cpp:744`, `:759`, `:1071`), so the gap is worth naming where the
operator reads. Owed: one line in the CAS operations docs recommending an
`AbortIncompleteMultipartUpload` lifecycle rule on the pool bucket, plus a note in the fsck docs that
`physical_bytes` counts committed objects and not in-flight multipart parts. Nothing in `docs/en` or
the CAS doc set mentions incomplete multipart uploads today.

**Capability-probe debris.** `runCapabilityProbe` cleans up on every exit path (`Backend/CasProbe.cpp:252`,
`:258`; the lambda HEADs then `deleteExact`s both keys, `:26-41`), and a mis-provisioned bucket fails at
step 0/0b (`:47-53`) before any object is written — so the audit's "left behind on exactly the
mis-provisioned buckets the probe exists to detect" is wrong. The real residual is a hard kill mid-probe:
two tiny objects under `<pool>/_probe/<u128hex>/` (`Pool/CasPool.cpp:466-467`), bounded by the number of
crashed mounts, never per-write. Nothing ever sweeps them: the bootstrap residual scan skips the whole
`_probe/` subtree deliberately (`Backend/CasSentinelProbe.cpp:18-30`, `:54-55`) — correct there, since
that scan decides whether a fresh `_pool_meta` may be minted and probe scratch must not fail-close a
healthy open — and fsck's unaccounted pipeline classifies only keys under the blob plane, so `_probe/`
keys are neither reported nor reclaimed. Owed (cheap): have `Pool::open` best-effort delete stale
`_probe/*` entries older than a threshold, or have fsck count them under a `probe_debris` line so the
class is at least visible. Not urgent — the bytes are negligible and cannot mask real data.

## `always_use_copy_instead_of_hardlinks=1` is accepted on a CA table and then breaks every mutation and same-disk partition clone (2031-triage CAS-085) {#always-copy-instead-of-hardlinks-no-gate}

Nothing rejects the MergeTree setting `always_use_copy_instead_of_hardlinks`
(`src/Storages/MergeTree/MergeTreeSettings.cpp:1902`, default `false`) on a content-addressed table
— neither at `CREATE`, nor at `ALTER ... MODIFY SETTING`. Once it is on, the copy variant of every
clone/hardlink site is taken and lands on `ContentAddressedTransaction::generateObjectKeyForPath`,
which is a `notYet` throw (`ContentAddressed/ContentAddressedTransaction.cpp:530-533`, message at
`:83-90`), because `DiskObjectStorageTransaction::copyFileImpl` derives destination keys from it
(`src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:522-524`).

Reachable paths at HEAD:
- Mutations (`ALTER ... UPDATE/DELETE`, `MATERIALIZE INDEX`, lightweight delete materialization):
  `MutateTask.cpp:2493-2496` and `:2516-2519` call
  `DataPartStorageOnDiskFull::copyFileFrom` → `disk->copyFile`
  (`DataPartStorageOnDiskFull.cpp:372-388` → `DiskObjectStorage.cpp:291-321`).
- The unchanged-part mutation clone: `MutateTask.cpp:3312` sets `copy_instead_of_hardlink`, reaching
  `DataPartStorageOnDiskBase::freeze` → `Backup` → `BackupImpl`, whose copy branch calls
  `transaction->copyFile` (`src/Storages/MergeTree/Backup.cpp:61-65`).
- Same-disk `ATTACH/REPLACE PARTITION FROM` and `MOVE PARTITION TO TABLE` on `StorageMergeTree`
  (`StorageMergeTree.cpp:3215`) reach the same `freeze` path.

Fail-closed, loud `NOT_IMPLEMENTED`, no silent corruption — but a mutation entry then retries
forever until the setting is reverted, and the thrown message ("the disk is wrapped by a layer that
bypasses the content-addressed write path") misdescribes this trigger. Fix direction: reject the
setting for a CA storage policy at `CREATE`/`ALTER MODIFY SETTING` (the same shape as the
`SUPPORT_IS_DISABLED` gates in `MergeTreeData::checkAlterIsPossible`), or teach the CA transaction
to serve `copyFile` as a manifest-level carry-forward like `createHardLink` already does.

Two claims from the source finding do NOT hold: `ALTER TABLE ... FREEZE` does not consult this
setting (`MergeTreeData.cpp:9988-9991` builds `ClonePartParams` with `make_source_readonly` only),
and neither does BACKUP/RESTORE cloning; the zero-copy implicit `copy_instead_of_hardlink` term
(`StorageReplicatedMergeTree.cpp:3357`, `:9220`) is dead on CA because
`DiskObjectStorage::supportZeroCopyReplication` returns false for `MetadataStorageType::CAS`
(`src/Disks/DiskObjectStorage/DiskObjectStorage.h:53-58`).

## `cas-inspect` decodes 10 of the 17 live object formats, and drops the fold seal's hold (2031-triage CAS-097) {#cas-inspect-format-coverage-and-hold}

P3, observability only — every gap is fail-loud (`BAD_ARGUMENTS`/`CORRUPTED_DATA`), never a silent
mis-read of a decodable object.

`caInspectToJson`'s dispatch (`ContentAddressed/Tools/CasInspect.cpp:566-635`) has a branch for
`PartManifest`, `RefCkpt`, `RefLog`, `RefSnapshot`, `GcState`, `MountLease`, `FoldSeal`, `RunFile`,
`BlobMeta` and the blob envelope. Seven live formats have no branch and land on the closing
`BAD_ARGUMENTS`: `PoolMeta` (`_pool_meta`), `RefCatalog` (`cas/ref_catalog`), `GcMaintenanceState`
(`gc/maintenance_state`), `GcHeartbeat` (`gc/hb`), `GcOutcomes`, `Owner` and `ServerEpoch`
(`Formats/CasFormat.h:98-128`; `Roster` is reserved and never written, `Formats/CasFormat.cpp:83`, so
the live set is 17, not 18). Two of them — `cas/ref_catalog` and `gc/maintenance_state` — are exactly
the control objects an operator reaches for when GC or a namespace lifecycle is stuck.

Two rendering residuals inside the branches that DO exist: (a) `renderRefCoverage`
(`Tools/CasInspect.cpp:357-363`) renders `classification` and `last_folded_ref_id` but omits
`RefCoverage::hold`, which by the format's own strict grammar is present iff `classification == 4`
(`Formats/CasFoldSealFormat.h:104-118`) — so a fold-seal dump shows `"classification":4` with no
reason and no offending position, dropping the one field that says WHY the fold refuses to advance
past that life; nothing pins the seal rendering (`src/Disks/tests/gtest_cas_inspect.cpp` has no
fold-seal test). (b) Sentinel and enumerated values render as bare numbers —
`"classification":4`, and a never-folded cursor as `{"writer_epoch":0,"ref_sequence":0}`
(`renderRefTxnIdObj`, `:121-127`) — so the reader must know the encoding.

Also real but harmless: a `cas/ns/state/<life>/_files/<name>` key falls through the namespace-state
branch (only `parseRefCkptKey` is tried, `:530-534`) and, for the two names `mount` and `fold_seal`,
is caught by the suffix branches at `:610-614` before the closing throw, so it is decoded as the
wrong format. The outcome is a `CORRUPTED_DATA` decode error instead of "unrecognized key layout" —
a worse message, not a wrong answer; a `parseNamespaceFileKey` branch (or a namespace-state throw
before the suffix checks) closes it.

Two claims from the source finding do NOT hold. Raw pool keys ARE enumerable: `cas-fsck --detail`
prints `class\tkey\tsize` per object (`programs/disks/CommandFsck.cpp:118-138`), which is exactly
what the shipped runbook tells the operator to feed `cas-inspect`
(`docs/en/antalya/cas/operations/debugging.md:185`), and the fixed control-object keys are documented
in `docs/en/antalya/cas/architecture/storage-layout.md`. And a wedged ref lane IS nameable at the
moment it wedges: the writer's own error names the namespace and the txn id
(`Pool/CasRefLedger.cpp:3934-3940`, `:2232-2234`) alongside `CASRefAppendWedged`. What is missing is
a QUERYABLE surface listing the currently-wedged namespaces —
`content_addressed_mounts.wedged_namespace_count` is an aggregate
(`src/Storages/System/StorageSystemContentAddressedMounts.cpp:55`, from `wedgedRefLaneCount`,
`Pool/CasRefLedger.cpp:1833-1851`, whose own map is keyed by namespace) — which is the
`per-part/ref system.* views` half of {#operability} `[B15/B99/B169/B159]`.

## ProfileEvents surface residuals {#profileevents-surface-residuals}

Two small, non-correctness residuals in the CAS `ProfileEvents` surface. Both are cosmetic
observability debt: nothing in the write or GC protocol depends on these counters, and neither can
lose or corrupt data.

**(a) The `CASServer*` row is unreachable, so eleven shipped counters are permanently zero.**
`classifyCasNs` returns only `Blob`, `Manifest`, `Root`, `Gc` and `Other`
(`ContentAddressed/Backend/CasInstrumentedBackend.cpp:113-130`); `CasNs::Server`
(`CasInstrumentedBackend.h:33`) still occupies a row of `cas_event_table`
(`CasInstrumentedBackend.cpp:103-106`), and the eleven `CASServer*` events it points at are declared
with operator-facing descriptions in `src/Common/ProfileEvents.cpp:844-854`. The per-server control
subtree moved under `<prefix>/gc/server-roots/<srid>/` (`Formats/CasLayout.h:388-417`), so owner
claims, epoch bumps, mount-lease claims and heartbeat renewals all classify as `Gc` — deliberately,
per the classifier cleanup in `44e41878ff0`, and documented at `CasInstrumentedBackend.h:18-20` and
pinned by `src/Disks/tests/gtest_cas_backend.cpp:304-308`. The residual is the user-visible half: an
operator reading `system.events` sees eleven documented counters that can never move. Close it by
either deleting the `Server` row plus its eleven descriptions, or classifying
`/gc/server-roots/` as `Server` before the `/gc/` rule (which would move mount/lease/epoch traffic
out of the `CASGC*` counters — a dashboard-visible change, so it needs a deliberate call). Mount and
lease activity itself is not unobservable meanwhile: `system.content_addressed_log` carries
`MountClaim`/`MountRelease`/`MountConflict`/`WatermarkRenew`/`GcLease*`
(`Primitives/CasEvent.h:30-33`) and `system.content_addressed_mounts` exposes the slots.

**(b) `CASBlobBodyPutAvoided` over-counts on the condemned HEAD-first hit.** In the HEAD-first
branch the counter (and `CASBlobDeduplicationCacheHit`) is incremented as soon as the `head` reports
present (`Pool/CasPartWriteTxn.cpp:210-214`), before `observeAndAdmit` decides whether that
incarnation is admissible. When it is condemned, `observeAndAdmit` throws `ABORTED`
(`:391`), the branch swallows exactly that code (`:222-228`) and falls through to
`uploadFromSource` (`:241`) — the body IS sent, yet the "body PUT avoided" event already fired. Only
`ABORTED` is swallowed, so this is drift on a rare race path, not a hidden failure; move both
increments below the successful `observeAndAdmit` to close it. `CASBlobHeadFirst` (`:208`) is
correctly placed — a HEAD really was issued. Related stale prose: `Pool/CasPool.cpp:250-253` still
says the dedup-cache seam is probed "up to twice ... once more just to attribute
`CASBlobBodyPutAvoided` to the cache" and names the caller `putBlob`; at HEAD the membership is read
exactly once into `cache_hit` (`CasPartWriteTxn.cpp:202`) and the caller is `uploadBlobDetached`.

## `FsckReport::clean` asserts more than the scan checked — no coverage flag for a skipped check family (2031-triage CAS-100) {#fsck-clean-verdict-has-no-coverage-flag}

P3, verdict-honesty only — no scan misclassifies anything, and every skipped family is skipped for a
stated cost reason. What is missing is the machine-readable "this family did not run" companion that
`FsckReport::partial` already is for the deadline case.

Three families are conditionally skipped, and in none of the three does the report say so:

1. The GC-snapshot run read — and with it the whole-file seal-checksum check that produces
   `corrupted_runs` — runs only when the pool has at least one present-but-unreferenced blob
   (`Tools/CasFsck.cpp:815`, checksum compare at `:877`). On a pool with none, `corrupted_runs` reads
   0 without a single run having been read. Mitigation, and why this is P3 rather than higher: the
   deletion-deriving consumers verify the same checksum fail-closed
   (`Gc/CasBlobInDegree.cpp:130`, `:718`, `Gc/CasGc.cpp:4478` → `SourceEdgeRunReader::verifyAgainst`,
   `Formats/CasRecordStreamFormat.cpp:316-322`), so a corrupt run stops GC loudly whether or not fsck
   looked.
2. `stale_edge` is computed only under `detail` (`Tools/CasFsck.cpp:905`), and the SQL path always
   passes `detail=false` (`src/Interpreters/InterpreterSystemQuery.cpp:2599`), so the `stale_edge`
   column is structurally 0 (acknowledged at `:2456-2458`). This one has a documented exception plus a
   compensating soak gate (`Tools/CasFsck.h:220-226`), so only the report-side "was it checked" bit is
   owed, not the gating decision.
3. A `--namespace`-scoped run skips the whole pool-wide physical/pipeline classification
   (`Tools/CasFsck.cpp:719`, scoped branch `:1048-1087`). The CLI help says so
   (`programs/disks/CommandFsck.cpp:29-31`), but the summary line, the report struct and `clean()`
   carry no scope marker, so a scoped run's `reachable=… dangling=0 …` line is byte-shaped exactly
   like a full run's.

Owed, cheapest first: a per-family coverage bit (or one `checked_families` bitmask) on `FsckReport`,
rendered on the summary line and the SQL row next to the counters it qualifies, so "0 because nothing
was found" and "0 because nothing was checked" stop being the same output — the same distinction
`partial` already makes for the deadline. Related: {#fsck-meta-body-counters-unrendered} (counters
computed and rendered nowhere) and {#lifecycle-verbs-wait-out-uncancellable-scans} (the SQL FSCK
passes none of the CLI's bounding parameters).

## No CAS disk setting is applied by `SYSTEM RELOAD CONFIG`, and the no-op is silent (2031-triage CAS-107) {#cas-settings-not-reloadable-silently}

`ContentAddressedSettings::loadFromConfig` runs exactly once, from the `cas` metadata-storage factory
lambda (`src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp:232-241`), i.e. only
when the disk object is CREATED. On a config reload `DiskSelector::updateFromConfig` calls
`disk->applyNewSettings(...)` for every disk that already exists
(`src/Disks/DiskSelector.cpp:180`), `DiskObjectStorage::applyNewSettings` forwards to
`metadata_storage->applyNewSettings(...)`
(`src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:985`), and
`ContentAddressedMetadataStorage` does not override that virtual — the base is an empty no-op
(`src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h:365-368`; only
`MetadataStorageFromCacheObjectStorage` overrides it, to forward to the underlying storage). So an
operator who edits `gc_interval_sec`, `deduplication_cache_bytes`, `part_folder_cache_bytes`,
`part_folder_validate`, `manifest_decode_cache_bytes`, any `gc_round_*` budget, ... and reloads gets a
successful reload, no log line, and no behaviour change. The S3 half of the same disk block DOES
reload (`DiskObjectStorage.cpp:988-989`), which makes the split especially surprising.

Not every setting is reloadable in principle — `server_root_id`, `gc_shards`, `blob_hash`,
`scratch_path`, `staging_backend` are pool-/mount-creation identities and must stay creation-time
only. Owed shape: an `applyNewSettings` override that (a) re-parses the block, (b) applies the
genuinely dynamic subset (GC cadence and the per-round budgets, the cache byte/entry budgets,
`part_folder_validate`, `gc_enabled` — the last already has runtime verbs, `SYSTEM CAS GC
STOP`/`START`), and (c) LOGS a warning naming any changed creation-time key as ignored-until-restart,
instead of today's silence. Fixing this also removes a second silent surface: the unknown-key gate
({#cas-disk-s3-key-whitelist-gap} in `BACKLOG.md`) is only ever evaluated at disk creation, so a typo
introduced by an edit-and-reload is not diagnosed until the next restart.

Second half, lower severity and mostly generic: removing a CAS disk from `storage_configuration` and
reloading only produces the upstream warning "disappeared from configuration, this change will be
applied after restart of ClickHouse" (`src/Disks/DiskSelector.cpp:215-218`) — no `shutdown()` on the
dropped disk. For a CAS disk this means the mount lease keeps being heartbeaten until the process
exits, so the `server_root_id` slot cannot be taken over by another server before a restart. This is
the same disk-registry-caches-forever class as the deferred disk-lifecycle leak
(`BACKLOG/mounts-and-lifecycle.md`{#disk-lifecycle-rev8-closure}) and is bounded by the restart, but
the warning does not say that a lease is still held; the honest short-term fix is a CAS-specific line
in that path (or in the mount log) naming the retained lease and the `FORGET` verb.

## The GC-health surface cannot express "never led", "GC stopped" or "backlog shed" (2031-triage CAS-098) {#gc-health-zero-is-ambiguous}

Four separate readings of the same per-disk GC-health snapshot
(`Gc/CasGcScheduler.cpp:392-407`, rendered by `StorageSystemContentAddressedMounts.cpp:52-56`,
`:196-209` and by the per-disk asynchronous metrics at
`src/Interpreters/ServerAsynchronousMetrics.cpp:378-392`):

1. **`last_success_age_seconds = 0` means BOTH "never led a round" and "succeeded within the last
   second."** `gcHealth` computes the discriminator — `ever_succeeded = last_ms != 0`
   (`Gc/CasGcScheduler.cpp:398`, field at `Gc/CasGcScheduler.h:126`) — and then neither surface
   exposes it: there is no `ever_succeeded` column and no `CASGCEverSucceeded_<disk>` metric. The
   operational bite is on the alerting side: an alert of the shape
   `CASGCLastSuccessAgeSeconds_<disk> > threshold` can NEVER fire for a disk whose GC never
   succeeded even once — precisely the silent failure the metric exists to catch. Cheapest honest
   fix: render `NULL` (and skip the metric) when `!ever_succeeded`, so "no data" is distinguishable
   from "fresh"; the alternative is to expose `ever_succeeded` alongside.
2. **`is_leader = 0` conflates "follower", "operator stopped GC here" and "scheduler self-exited".**
   `SYSTEM CAS GC STOP` is STOP-IN-PLACE — the scheduler object is retained deliberately so
   `gcHealth` keeps answering (`ContentAddressedMetadataStorage.cpp:971-1001`) — so a stopped GC
   presents exactly as a follower: `is_leader = 0`, health present. Nothing in `system.cas_mounts`,
   the metrics, or `system.cas_gc_log` says "this node's reclaimer is administratively off"; the only
   trace is the one-shot `LOG_INFO` at `src/Interpreters/InterpreterSystemQuery.cpp:2656-2661`. A
   `gc_running` (or `gc_state`) column, sampled from the scheduler's `stopping` latch, closes it.
   Not a defect: `GC STOP` being node-local and non-durable across restart matches the
   `SYSTEM STOP MERGES` precedent and is documented as such at the same site — the gap is
   observability, not persistence.
3. **`pending_reclaim` sheds nothing but executed deletes, so it drifts upward permanently.** The
   accumulator is `condemned - redeleted` (`Gc/CasGcScheduler.cpp:202-205`); an entry that leaves the
   retired list as `spared` or `replaced` (`Gc/CasGc.h:136-137`, counted at `Gc/CasGc.cpp:995-996`)
   is never subtracted. So the metric's own documented reading — "a persistently growing value
   indicates GC is not keeping up with reclaim"
   (`src/Interpreters/ServerAsynchronousMetrics.cpp:387-388`) — is satisfied by a perfectly healthy
   pool that spares a lot. The authoritative gauge already exists and is computed every round:
   `RoundReport::pending_condemned` / `pending_candidates` / `pending_retired`
   (`Gc/CasGc.h:152-155`), but it is rendered ONLY in the `SYSTEM CAS GC RUN` result set
   (`src/Interpreters/InterpreterSystemQuery.cpp:2356-2358`, `:2380-2382`) — neither
   `system.cas_mounts` nor `system.cas_gc_log` nor the metrics carry it. Owed: render
   `pending_condemned` where an operator watches (and either drop `pending_reclaim` or restate its
   description as "cumulative condemn-vs-delete flow", not a backlog).
4. **Our own docs read these columns as something they are not.** `operations/migration.md:200-203`
   tells the operator to check the victim's `state` and `last_success_age_seconds` before
   `SYSTEM CAS DROP POOL MEMBER` — but GC-health columns are `NULL` on every peer row by design
   (stated correctly in `architecture/mounts-and-leases.md:219` and
   `operations/monitoring.md:98-101`), so the victim's row never carries it; the liveness signals
   there are `state`/`expires_at`. `operations/troubleshooting.md:20` likewise offers
   "`last_success_age_seconds` not climbing" as evidence that the MOUNT LEASE is still renewing —
   two unrelated clocks. Both lines are wrong as written and both are cheap prose fixes.

NOT a defect, checked while triaging (the audit's fifth claim): `CASGCClampSuppressedPasses` no
longer "fires every round by construction". The destructive gate is conditional at HEAD —
`suppress_destructive = anomalies || carried_holds || frontier_incomplete`
(`Gc/CasGc.cpp:3065-3071`) with `UniversePolicy::kDefault = Authoritative`
(`Gc/CasGc.h:42-63`), flipped by `58fd482a800`. What IS stale is the counter's operator-facing
description (`src/Common/ProfileEvents.cpp:803`), still asserting "In the current stage this is EVERY
folding round by construction ... the round's destructive gate is shut unconditionally" — written
before the flip (`e337bb2c87d`) and never revisited. Same class as
`{#fsck-rule-restated-in-unfenceable-prose}`: a rule restated in prose no build can check. Fix the
sentence; the pointer to the fold seal's hold set and `tables_held` stays useful.

## The audit-event sink is installed even when `system.cas_log` is disabled, so the "disabled path is free" promise does not hold (2031-triage CAS-104) {#cas-event-sink-installed-when-log-disabled}

`makeCasEventSink` returns a non-empty `std::function` whenever the metadata storage has a `Context` —
its only early exit is `if (!context) return {}`
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:562-571`),
and the "is the log actually there" question is answered INSIDE the sink
(`:573-575`, `auto log = ctx->getContentAddressedLog(); if (!log) return;`). But
`createSystemLog` returns an empty pointer when the config section is missing
(`src/Interpreters/SystemLog.cpp:135-142`), and removing the section is the documented way to turn the
log off (`programs/server/config.xml:1197-1199`, "Remove the section to disable"). So with the log
disabled, `Pool::hasEventSink()` (`Pool/CasPool.h:784`) still answers `true`, every emission site still
builds a full `CasEvent` (7 `String`s plus a `std::map`, `Primitives/CasEvent.h:180-194`), still pays
the dispatcher's queue mutex (`Pool/CasEventDispatcher.cpp:19-22`), and the row is dropped at the very
end.

That contradicts two claims the code makes about itself: "disabled hot path still skips constructing
events entirely" / "a true no-op on the production hot path" (`Pool/CasPool.h:769-770,782-784`) and
"the query-frequency disabled hot path pays no mutex" (`Pool/CasEventDispatcher.h:99-101`). Nothing is
corrupted and no path fails — it is wasted work on paths that also do object-storage round trips, which
is why this is P3 and not a gate.

Fix direction: ask `context->getContentAddressedLog()` when BUILDING the sink and return an empty
`std::function` when the log is absent, so `hasEventSink` is again a truthful "delivery enabled"
predicate. The honest version of that needs the sink to be re-derivable on config reload, which is the
same missing plumbing as {#cas-settings-not-reloadable-silently} — a one-shot check at pool open is
still strictly better than today, because the disabled case is a static config choice in practice.

Related, and NOT this item: the per-emit event VOLUME (independent of whether the log is on) is already
tracked as `{#ca-log-tables-restart-cost}` in `gc.md` and in the audit-row note inside
`{#standalone-write-scratch-manifest-cost}` in `performance.md`. The dispatcher itself does not
serialize delivery under its mutex — it releases the lock around the sink call
(`Pool/CasEventDispatcher.cpp:36-51`), and the sink is the never-blocking `SystemLog::add`
(`src/Common/SystemLogBase.cpp:93-123`) — so there is no read-path mutex bottleneck to track here.

## The mount-lease / request-budget / snapshot-pacing knobs have no `ContentAddressedSettings` entry (2031-triage CAS-105) {#pool-pacing-knobs-no-config-surface}

`ContentAddressedSettings` declares 29 disk keys
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp:63-92`)
and `openPoolView` wires exactly those into `PoolConfig`
(`ContentAddressedMetadataStorage.cpp:747-775`). Everything else in `PoolConfig` is therefore a
struct default in production, reachable only from gtests:

- `mount_lease_ttl_ms` (30 s) and `mount_renew_period` (10 s) — `Pool/CasPool.h:182-183`. They set the
  write-fence deadline and the renewal cadence, and the user docs already quote both defaults as if
  they were tunable (`docs/en/antalya/cas/architecture/mounts-and-leases.md:73`,
  `docs/en/antalya/cas/operations/troubleshooting.md:19` even tells the operator to compare network
  latency against `mount_lease_ttl_ms`).
- the whole `CasRequestBudget` (`Backend/CasRequestControl.h:145-198`): `attempt_timeout_ms` 5 s,
  `operation_deadline_ms` 90 s, `max_attempts` 16, `lease_safety_margin_ms` 2 s, the two inter-attempt
  backoffs, and the three `recovery_retry_*` values (120 s / 1 s / 30 s).
- `snapshot_log_count_threshold` / `snapshot_log_bytes_threshold` and the publish- and
  precommit-sweep backoffs (`Pool/CasPool.h:234-253`) — these trade write-side full-snapshot `PUT`
  volume against read-side cold-fold `GET`s, i.e. exactly a per-workload dial.
- `gc_fold_threshold` (1) and `gc_fold_max_defer_rounds` (8) — `Pool/CasPool.h:159-165`; the
  skip-unchanged batching dial is fixed at "fold as soon as anything changed".
- `rebuild_edge_budget` (8 M edges ≈ 256 MB) — `Pool/CasPool.h:172`, an in-memory ceiling for
  `rebuildBaseline`.

Two members of the same struct are deliberately NOT part of this item: `gc_stuck_removal_rounds` is
self-documented as a test seam ("no user-facing setting is registered", `Pool/CasPool.h:166-168`), and
`gc_frontier_probe_budget` defaults to effectively unbounded with the explicit reasoning that a cap
there converts into a permanent GC stop (`Pool/CasPool.h:136-155`) — exposing that one needs the
argument for it, not just a `DECLARE`. `ref_table_cache_bytes` is the same class but already tracked
with its own two extra residuals under {#ref-table-cache-budget-admission-only} in `performance.md`.

Owed shape: `DECLARE` the pacing/budget subset (lease TTL + renew period, the request budget, the
snapshot thresholds/backoffs, `rebuild_edge_budget`, the fold-batching pair), keep `validate` extended
so the mount-lease inequality is checked against the CONFIGURED values rather than only the defaults,
and land it together with the reload gap ({#cas-settings-not-reloadable-silently}) so a configured
value is not silently ignored on `SYSTEM RELOAD CONFIG`. Not a gate: the defaults are the ones every
soak ran on, and `Pool::open` already refuses an inconsistent budget out loud.

## Byte accounting covers only blob bodies, and `previewDeletes` reports two different size units in one column (2031-triage CAS-123) {#byte-accounting-blobs-only-and-preview-size-units}

Two P3 observability residuals, neither of them a correctness issue.

**No per-object-class byte accounting.** `FsckReport::physical_bytes` is summed exclusively from the
`blobs/` listing, and even there only from BODY keys — `.meta` siblings are split out of
`present_blobs` before the sum (`Tools/CasFsck.cpp:727-744`, plus the two HEAD top-ups at `:759` and
`:1071`). Nothing anywhere sums the bytes of manifests, ref logs, ref snapshots, `_ckpt`, run files,
fold seals, GC state, or staging keys. The only non-blob byte counter in the whole surface is
`namespace_janitor_pending_bytes` (`Tools/CasFsck.h`, rendered at `Tools/CasFsck.cpp:1166` and exposed
as a SQL column at `src/Interpreters/InterpreterSystemQuery.cpp:2497`), which covers one debris class
only. The GC log has no byte columns at all — every counter in
`ContentAddressedGarbageCollectionLogElement` is an object count
(`src/Interpreters/ContentAddressedGarbageCollectionLog.h:32-43`), so "how many bytes did this round
reclaim" is not answerable from `system.content_addressed_garbage_collection_log` either. Consequence:
a bucket-total-versus-`physical_bytes` gap cannot be attributed to any object class. This is the
general form of the narrow case already named under {#mpu-and-probe-debris-unaccounted} (incomplete
MPU parts and `_probe/` debris); that item's owed doc line should say which classes `physical_bytes`
covers, and the cheap fix here is a per-prefix byte breakdown in the fsck report (it already walks
every plane) plus a `bytes_deleted` column on the GC-round row.

Related but distinct, and NOT owed here: there is no per-table reclaim forecast ("how much would
dropping table X free"), and on a deduplicating pool that number is not even well defined without
naming the sharing model (unique-to-this-table bytes vs its share of shared blobs). If it is ever
wanted, it needs a spec first, not a counter.

**`previewDeletes` mixes physical and logical sizes.** `Gc::PreviewEntry::size` carries the raw HEAD
size for zero-in-degree candidates (`Gc/CasGc.cpp:4441-4444`, envelope-inclusive) but the condemned
row's stored size for retired-in-snapshot rows (`:4470`), and that stored value was written through
`retiredLogicalSize` — i.e. already payload-only, `object_size - blob_header_len`
(`Gc/CasGc.cpp:320-329`, applied at `:1849` and `:1871`). `clickhouse-disks cas-gc-dryrun` prints that
column raw (`programs/disks/CommandCaGcDryRun.cpp:47`), so summing it mixes units by
`blob_header_len` (256 by default, `Pool/CasPool.h:54`) per condemned row. Small in absolute terms and
the command is documented diagnostic-only, but it is a one-line fix: either subtract the header in the
zero-in-degree branch too, or carry both fields.

Not defects in the same tool, for the record: rows the round will not delete are labeled in the
`reason` column (`unreachable` / `awaiting_graduation` / `delete_pending`), the non-quiescence
over-report is documented at the API (`Gc/CasGc.h:453-457`), and no blob is double-counted — the fold
emits at most one sentinel row per blob (`Gc/CasBlobInDegree.cpp:573-589`) and `zeroInDegree` skips
`kCondemned` rows (`:706-708`).

## fsck substitutes the default `BlobRef{}` for an unparsable blob key, and that value is the identity of an empty blob under the default algo (2031-triage CAS-124) {#fsck-unparsable-blob-key-sentinel-collides-with-the-empty-blob}

`Tools/CasFsck.cpp:953` classifies an unreachable listed object with
`layout.parseBlobKey(bkey).value_or(BlobRef{})`, and `BlobRef{}` is `{CityHash128, all-zero digest}`
(`Primitives/CasBlobDigest.h:207-214`). An empty blob hashes to exactly that: `IHashingBuffer` starts
at `state(0, 0)` and `getHash` returns it unchanged when nothing was hashed
(`src/IO/HashingWriteBuffer.h:20-29`), so `blobHashHexOneShot(CityHash128, "")` is `0…0`. Zero-length
blobs are creatable — files matching `partFileMustStayBlob` take the blob path regardless of size
(`ContentAddressedTransaction.cpp:860-917`) and neither `stageBlobPartFile` nor `CasPartWriteTxn`
rejects `size == 0`.

Consequence is one report label, not data: the `PendingGc` branch additionally requires a token match
(`:957-958`), which a foreign key cannot satisfy, but `in_run_hashes.contains(hash)` (`:968`) does not,
so a truly unparsable/foreign key can be labeled `AwaitingGc`/`StaleEdge` instead of `Unaccounted`
whenever the pool also holds an empty `cityhash128` blob in the GC snapshot. `report.unreachable` is
already incremented before the classification (`:946`), so nothing disappears from the report. Fix is
one line: keep the `std::optional<BlobRef>` and classify a parse failure as `Unaccounted` directly
instead of looking the sentinel up in `retired_by_hash`/`in_run_hashes`.

## `system.content_addressed_log` stamps time, `thread_id` and `query_id` on the DRAINING thread, not the emitter (2031-triage CAS-131) {#cas-log-drain-thread-attribution}

`CasEvent` is pure data with no time or caller identity fields (`Primitives/CasEvent.h:61-75`), and the
three columns that answer "when, and who did this" are filled inside the sink, at delivery:
`ContentAddressedMetadataStorage.cpp:580-581` (`event_time`/`event_time_microseconds` from
`system_clock::now()`) and `:594-595` (`e.thread_id = getThreadId(); e.query_id =
CurrentThread::getQueryId();`).

Delivery is not guaranteed to run on the emitting thread. `EventDispatcher::emit` enqueues under the
mutex and, if another thread is already draining, returns immediately — the queued event is delivered
by that other thread's loop (`Pool/CasEventDispatcher.cpp:19-30`, and the drain loop at `:31-52`
which releases the lock around the sink call). The same hand-off happens for an emission made from
inside a sink callback (the documented reentrancy case, `Pool/CasEventDispatcher.h:20-23`). So under
any concurrency — two queries writing, a query plus the GC/keeper background threads, the intra-part
upload fan-out the dispatcher was introduced for — a row can carry the `thread_id` of an unrelated
thread and the `query_id` of an unrelated query (or none), while its timestamp is the delivery
instant rather than the decision instant.

Nothing is corrupted and no path fails; the cost is that the audit table's own attribution columns
cannot be trusted for exactly the correlate-with-`system.query_log` triage they exist for. Fix
direction: stamp at emission — capture the three values in `EventDispatcher::emit` (or in the
emission helpers) into new `CasEvent` fields, and have the sink copy them instead of sampling its own
thread. Cheap, and it also makes `event_time` mean "when the decision happened", which is what every
existing analysis assumes.

Checked while triaging, NOT defects: the deliberate skip of the `RefResolve` row on a warm
`CachedForLoad` view-cache hit (`Parts/PartFolderAccess.cpp:161-186`, contract stated in
`Parts/PartFolderAccess.h:394` and `Pool/CasRefLedger.h:33`) — the hit is reported by
`CASPartFolderViewHits`; and the part-folder cache counters, which are all plain counts with
descriptions matching the code (`src/Common/ProfileEvents.cpp:919-924,943`), with the bytes/entries
gauges kept as separate `CurrentMetrics` (`src/Common/CurrentMetrics.cpp:234-235`).
