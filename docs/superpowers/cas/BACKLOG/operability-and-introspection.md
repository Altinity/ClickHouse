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
