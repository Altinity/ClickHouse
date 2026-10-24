---
description: 'Product-level CAS MergeTree backlog for project management: main stories grouped by theme, with the value of each and the engineering ticket ids behind it.'
sidebar_label: 'CAS Backlog (product view)'
sidebar_position: 10
slug: /superpowers/cas/pm-backlog
title: 'CAS MergeTree — Product Backlog'
doc_type: 'guide'
---

# CAS MergeTree — Product Backlog {#cas-pm-backlog}

This is the product-level view of the CAS (content-addressed storage) MergeTree backlog, written for planning and prioritisation. Every story says what a user or the project gets from it. The numbers in parentheses are the engineering tickets in the live backlog (`docs/superpowers/cas/backlog`, prefix `CAS-`; `DRAFT-` ids are ideas that are not yet accepted as work). Small technical items are folded into a "technical debt" story under each theme and are not listed one by one.

Snapshot date: 2026-09-27. The live backlog has 451 items: 390 to do, 7 in progress, 54 drafts. Items that are done are not here. When a ticket moves, the live backlog wins.

Status marks: `[ ]` not started, `[~]` in progress (a pull request exists or work is under way).

## 1. First production deployment {#first-deployment}

Everything a customer needs before the first CAS pool goes to production. This theme gates every other one.

- [ ] **Safe defaults and an explicit opt-in.** Every CAS setting is reviewed with evidence before the first deployment, and a CAS disk cannot be created by accident from SQL. (CAS-297, CAS-108, CAS-110, CAS-154)
  - [ ] Review every default and record why it is right. (CAS-297)
  - [ ] Refuse SQL-defined CAS disks unless the operator opts in; decide whether the feature is marked experimental. (CAS-108, CAS-110)
  - [ ] Derive the request timeout from the lease budget instead of refusing the mount. (CAS-154)
- [ ] **No data loss from a stale delete.** GC deletes a blob's metadata object only by the etag it captured when the delete was scheduled, so a concurrent republish cannot be destroyed. (CAS-30)
- [ ] **Proven migration path.** Move partitions onto a CAS disk and back on a production-sized cluster, measure it, and document the procedure and its cost. (CAS-116, CAS-254)
- [ ] **Proven stability under chaos.** A 4-hour continuous chaos soak runs to the end with flat server memory, so we can say the system survives kills, faults and lease loss. (CAS-221, CAS-41)
- [ ] **Fast restart on a busy node.** Startup does not spend minutes loading outdated parts of system log tables or listing S3 for every table. (CAS-94, CAS-95, CAS-95.1 `[~]` PR #2440, CAS-95.3; CAS-95.2 is parked as doubtful)
- [ ] **Operator documentation for day one.** Bucket requirements, the trust boundary (the bucket credential is the whole security boundary), sizing guidance, release notes. (CAS-243, CAS-245, CAS-119, CAS-107, CAS-241, CAS-109, CAS-102)
- [ ] **Dashboards and alerts on day one.** A Grafana board and alert rules built from the CAS system tables, so an operator sees GC health, request errors and lease state without reading logs. (CAS-289, CAS-289.1, CAS-289.2, CAS-4)
- [ ] **Keep-alive and connection hygiene.** Port the S3 keep-alive recommendation and re-run its A/B over a real network path, so request latency and port usage match the measured recommendation. (CAS-283, CAS-74)

## 2. Insert and write-path performance {#insert-performance}

Goal: an insert on CAS costs about the same S3 requests as an insert on plain S3, and the write path never reads what it already knows.

- [ ] **Publish a part with zero GETs in the common path.** Today every insert pays several catalog and checkpoint reads; after this a part publish is PUT-only, which cuts request cost and latency per insert. (CAS-176, CAS-176.1, CAS-176.2, CAS-176.3, CAS-176.4, CAS-105)
- [ ] **Stop paying for part removal twice.** Removing a part today repoints its ref and then removes it; removing this extra write cuts about a fifth of the writer's PUTs on a busy node. (CAS-83, CAS-286)
- [ ] **Coalesce hot control-key writes.** The per-table checkpoint key and the pool catalog are rewritten on every flush; coalescing keeps them under the GCS mutation limit and cuts insert wait. (CAS-167, CAS-287, DRAFT-26, CAS-72, CAS-71, CAS-71.1, CAS-71.2, CAS-71.5, DRAFT-31)
- [ ] **Explain and remove unexplained manifest writes.** About 200k manifest PUTs per day on the demo stand are not explained by inserts or repoints; find and remove their source. (CAS-146, CAS-15)
- [ ] **Insert latency: publish outside the parts lock.** Upload blobs and stage the manifest before the table takes its parts lock, so a slow S3 does not block every other insert into the table. (CAS-13, CAS-14, DRAFT-2)
- [ ] **Memory per insert.** Size write buffers for tiny parts instead of allocating two full buffers per stream; publish small blobs from memory. (CAS-79, DRAFT-48, DRAFT-53)
- [ ] **Bulk operations on CAS.** `REPLACE PARTITION`, `MOVE PARTITION`, `ATTACH` and CAS-to-CAS moves carry references instead of re-uploading bytes one part at a time. (CAS-274, DRAFT-55, CAS-273)
- [ ] **Replicas share one merge.** On a shared pool one replica merges and the others relink the result, instead of every replica doing the same merge and uploading the same bytes. (CAS-103, CAS-288)
- [ ] **Read-path request reduction.** One GET to open a part, inline-by-size placement of small files, part-file route derived once per read. (CAS-16, CAS-25, CAS-75, CAS-24, CAS-80)
- [ ] **Technical debt: write path.** Re-measurements after the zero-GET change, benchmark of the mandatory blob HEAD, scratch growth attribution, staging-index and buffer nits, manifest cap check before encoding. (CAS-17, CAS-18, CAS-100, CAS-101, CAS-26, CAS-258, CAS-259, CAS-196, CAS-257, CAS-264, CAS-285, CAS-22, CAS-78, CAS-192, DRAFT-4, DRAFT-45, DRAFT-46, DRAFT-50, DRAFT-52)

## 3. Garbage-collection performance {#gc-performance}

Goal: a GC round finishes in minutes, not hours, and catches up at S3 speed after a backlog. Spec: `docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md`, issue #2429.

- [ ] **GC rounds in minutes (epic).** (CAS-29)
  - [~] Stage A, parallelism: round-scoped worker pools and parallel pending deletes (PR #2351), read every manifest body once per round, read condemn markers through the read-ahead, drop inline HEADs on mass removal. (CAS-29.2, CAS-29.12, CAS-29.3, CAS-29.4, CAS-29.5, DRAFT-14)
  - [ ] Stage B, discovery and cleanup cost proportional to new work, not to pool size: one exact probe per live table instead of a global LIST, per-life cleanup ranges and licences, batched janitor deletes. (CAS-29.1, CAS-29.6, CAS-29.7, CAS-29.8, CAS-29.9)
  - [ ] Stage C, a round deadline replaces the count budgets, and the next round starts at once while work remains. (CAS-29.10, CAS-29.11)
  - [ ] Listings cost one S3 request per 1000 keys, and one log line per round tells the operator what the round did. (CAS-29.13, CAS-29.14)
- [ ] **Bulk conditional deletes.** GC deletes blobs in batches of up to 1000 with a per-object etag check on S3, GCS and Azure, after a capability probe proves the store supports it. Cuts delete requests by up to 1000x. (CAS-284, CAS-284.1, CAS-284.2, CAS-284.3, CAS-284.4, CAS-284.5)
- [ ] **A round costs proportional to the change, not to the pool.** Log-structured in-degree runs and point-updatable counters so a small round stops rewriting the whole snapshot (1 to 2.6 GiB per day on the demo stand). (CAS-89, CAS-88, DRAFT-10, DRAFT-11, DRAFT-12, DRAFT-34)
- [ ] **One damaged table cannot stop GC for the whole pool.** A clamped or damaged namespace suppresses its own deletes only. (CAS-31, CAS-126, CAS-35)
- [ ] **Multi-node GC.** One coordinator round with work claimed by executors on several nodes, for pools too large for one node. (CAS-90)
- [ ] **Every byte is reclaimable.** Close the known shapes that leave a blob body no round revisits, and give operators an expedited delete for right-to-erasure requests. (CAS-33, CAS-33.1, CAS-33.2, CAS-33.3, CAS-46, DRAFT-24, DRAFT-5, DRAFT-9, DRAFT-8)
- [ ] **Technical debt: GC.** Bounded manifest cleanup, janitor cursor on transient failures, lease release on clean stop, orphan-sweep invariants and budgets, re-measurements on the sanitizer lane, minor comments and counters. (CAS-36, CAS-38, CAS-39, CAS-42, CAS-44, CAS-47, CAS-86, CAS-87, CAS-91, CAS-106, CAS-138, CAS-191, CAS-193, CAS-270, CAS-305, CAS-48, CAS-49, DRAFT-35)

## 4. Backup and restore {#backup}

Goal: `BACKUP` and `RESTORE` of CAS tables work with the standard ClickHouse tooling and with `clickhouse-backup`, and operators have a runbook.

- [~] **Native `BACKUP`/`RESTORE` of CAS tables.** Backup to S3 or disk copies payload bytes, not CAS envelopes; the existing backup integration suites pass on a CAS disk (PR #2415, PR #2437). (CAS-300, CAS-301)
- [~] **Operator runbook for backup and restore.** (CAS-115)
- [ ] **`FREEZE` safety.** A second `FREEZE WITH NAME` into an existing shadow is refused instead of silently overwriting. (CAS-260)
- [ ] **Next steps from the backup design.** `clickhouse-backup` embedded mode, local snapshots, attach API, CAS-to-CAS remote backup, and adopting objects uploaded by bulk loaders. Design: `docs/superpowers/cas/10-backups.md`. (DRAFT-16, DRAFT-47, DRAFT-51)

## 5. Alternative storage backends and encryption {#backends}

Goal: CAS runs on every object store our customers use, and inside the encrypted-disk wrapper.

- [ ] **Google Cloud Storage: release gate complete.** Run or descope every unrun arm of the live GCS gate (OAuth groups, ambiguity fault arms, `test_storage_s3` lane), so GCS is a supported backend, not a tested-in-parts one. (CAS-172, CAS-172.1, CAS-172.2, CAS-172.3, DRAFT-33, CAS-71.3, CAS-197)
- [ ] **Azure Blob Storage.** Implement and validate CAS on Azure, starting with the azurite emulator. (CAS-275)
- [ ] **Local and shared POSIX filesystems as a first-class backend.** Lets CAS run on NFS and local disks; single-process limits documented. (CAS-276, CAS-276.1, CAS-276.2)
- [ ] **CAS under the encrypted disk wrapper.** Design dedup scope per key and make the encrypted disk forward the CAS capability; refuse unsupported combinations at config load. (CAS-129, CAS-129.1, CAS-112, CAS-306)
- [ ] **Format upgrades across mixed-version servers.** Specify how a pool moves to a new on-S3 format version with a compatibility path and upgrade order (the format is frozen since 26.6.4). (CAS-117, CAS-265, CAS-272, DRAFT-44, DRAFT-3)
- [ ] **Technical debt: backends.** Region redirect budget, retry slowdown shared between clients, emulated-backend atomicity and token expiry, `SlowDown` policy, stack-trace capture on expected 412. (CAS-188, CAS-23, CAS-277, CAS-278, CAS-279, DRAFT-15, CAS-82, CAS-266, CAS-262, CAS-267, CAS-268, CAS-27, CAS-65, CAS-187, CAS-240, CAS-81, DRAFT-25, DRAFT-27, DRAFT-29, DRAFT-42)

## 6. Operations and robustness {#operations}

Goal: a CAS node survives memory pressure, lease loss, hard kills and operator mistakes, and every failure has a documented recovery.

- [ ] **Mount lease stays alive under pressure.** Lease renewal keeps running when the server is over its memory limit and does not fence early while most of the budget remains. Without this a memory spike turns into a read-only node. (CAS-292, CAS-298, CAS-152, CAS-153, CAS-151, CAS-155, CAS-178, CAS-179, CAS-185, DRAFT-28)
- [ ] **Replica re-add and pool membership.** A replica removed from a pool can be re-added with a new server uuid without hand-editing the object store; `DROP POOL MEMBER` cannot destroy a live sibling and says what a killed member must reach first. (CAS-156, CAS-156.1, CAS-156.2, CAS-156.3, CAS-157, CAS-158, CAS-163)
- [ ] **Repair instead of recreate.** Operators get a diagnose, repair and runbook path for a damaged control object, checkpoint or ref log, and `GC REBUILD` works over an undecodable GC state. (CAS-56, CAS-56.1, CAS-56.2, CAS-56.3, CAS-56.4, CAS-32, CAS-141, CAS-144, CAS-145)
- [ ] **CAS disk lifecycle.** Mount without a table, stop and eject a disk on `UNMOUNT` or last `DROP TABLE`, keep the server up when one pool cannot open at startup, apply dynamic settings on `SYSTEM RELOAD CONFIG`. (CAS-149, CAS-149.1, CAS-296.1, CAS-66, CAS-67)
- [ ] **Queries stay interruptible.** `KILL QUERY` and `max_execution_time` interrupt CAS waits and fsck; retry-later throws are counted. (CAS-181, CAS-55, CAS-248)
- [ ] **Crash safety of table operations.** A replica killed mid-`MOVE PARTITION` must not keep a duplicated partition; a part whose transaction marker vanished fails to load instead of silently changing meaning; a writer recovers from a truncated blob body. (CAS-253, CAS-255, CAS-308, CAS-113, CAS-180, CAS-183, CAS-184, CAS-256)
- [ ] **Non-MergeTree tables on CAS.** Persistent `Join` and `Set` tables fail to insert today because their files are parsed as part files; fix it and document what works. (CAS-318, CAS-194, CAS-261)
- [ ] **Merged-branch parity.** Fixes that landed only in `antalya-26.6` are brought back to the development branch and vice versa. (CAS-302, CAS-166)
- [ ] **Bring the CAS write failure inside MergeTree transactions to a clean error instead of a server abort.** (CAS-177 `[~]` PR #2396)
- [ ] **Technical debt: robustness.** Local scratch free-space checks, `clickhouse-local` teardown, ref-table cache budget, disk-setting validation, a few narrow races and ordering nits. (CAS-111, CAS-125, CAS-19, CAS-20, CAS-21, CAS-52, CAS-69, CAS-114, CAS-120, CAS-173, CAS-190, CAS-182, CAS-28, CAS-5, CAS-40, CAS-45, CAS-295, CAS-160, DRAFT-43, DRAFT-17, CAS-251)

## 7. Observability and transparency {#observability}

Goal: an operator can answer "is GC healthy", "why is this insert slow" and "what did that S3 error mean" from system tables and one log line, not from a debugger.

- [ ] **Honest GC health surface.** The per-disk GC state distinguishes never-led, stopped, disabled and shed; `last_success_age_seconds` is not 0 when no round ever succeeded; pending reclaim comes from the fold seal. (CAS-1, CAS-1.1, CAS-1.2, CAS-1.3, CAS-1.4, CAS-134, CAS-132, CAS-133, CAS-136, CAS-142, CAS-143, CAS-311, CAS-37)
- [ ] **Mount and lease timeline in the logs.** Fence, self-remount and terminal mount states are logged at the default level with cause and duration; terminal counters are documented. (CAS-3, CAS-150, CAS-159, CAS-271)
- [ ] **S3 request failures are readable.** Every CAS request failure names the key, verb and last transport error; throttling and after-all-retries failures are counted separately; expected 412/409 no longer look like errors in the log. (CAS-50, CAS-168, CAS-169, CAS-175, CAS-299, CAS-244, DRAFT-32, CAS-135, CAS-148)
- [ ] **Smaller, documented system tables.** Reduce and rename the columns of `system.cas_mounts`, `system.cas_gc_log` and `system.cas_log`, cut `cas_log` volume, document each table with example queries, and audit the 183 ProfileEvents so each has a reader. (CAS-291, CAS-290, CAS-2, CAS-2.1, CAS-2.2, CAS-229, CAS-51, CAS-7, CAS-8, CAS-6, CAS-137, CAS-92, DRAFT-1)
- [ ] **Complete `SYSTEM CAS` command set.** Every verb has one operator reference, aligned grammar and result rows, `GC RUN` once per pool, `GC STOP`/`FORGET` stop a round in flight. (CAS-296, CAS-296.2, CAS-304, CAS-54, DRAFT-6)
- [ ] **fsck that reaches a verdict.** A resumable or sharded scan on a 30 GiB pool, bytes per object class, reporting of what did not run, and why each object is kept. (CAS-140, CAS-139, CAS-9, CAS-10, CAS-11, CAS-12, CAS-34, CAS-62, CAS-64, CAS-124, DRAFT-54, DRAFT-7)
- [ ] **Capacity warnings before refusal.** Per-table ref-table budget use is exposed and warned about before a table hits the 64 MiB growth refusal; wedged ref lanes are queryable. (CAS-263, CAS-68, CAS-269, CAS-128)
- [ ] **Replication diagnostics.** Relink-confirm outcomes are counted and refusal is told apart from transport failure. (CAS-123, CAS-170.3, CAS-249, CAS-43, CAS-70.2)
- [ ] **Public docs and tooling.** Fix wrong command names in the public CAS docs, document `clickhouse-disks` exit codes and `cas-*` subcommands, decode every format in `cas-inspect`. (CAS-247, CAS-60, CAS-57, CAS-58, CAS-118, CAS-164, CAS-282, CAS-59)
- [ ] **Technical debt: observability.** Stale event descriptions, CI artifact dumps of `cas_gc_log`, text-log capture in soak dumps, GC view replay error class. (CAS-85, CAS-219, CAS-315, CAS-303, CAS-189)

## 8. Upstream contributions and fork hygiene {#upstream}

Goal: generic fixes live in upstream ClickHouse, so the fork's conflict surface shrinks with every release.

- [ ] **Carve generic fixes into upstream pull requests.** The S3 conditional-write stack (412 predicate, GCS dialect, `Expect: 100-continue`), and the standalone fixes (thread-group parent use-after-free, file-view read buffer, S3 read cancel, local-storage TOCTOU). (CAS-232, CAS-232.1, CAS-232.2, CAS-61, CAS-53, CAS-231, DRAFT-30, DRAFT-49)
- [ ] **Upstream correctness fixes found by CAS.** Concurrent mutations registering out of order and skipping one; `SYSTEM SHUTDOWN` under the watchdog is an immediate kill; a synchronous `DROP TABLE` waits for an unrelated table; a 0-byte upload falls back to multipart and throws; replication-queue attempts counted on a refused task; `checkSize` probes a directory for every checksum entry. (CAS-252, CAS-147, CAS-84, CAS-250, CAS-293, CAS-319, CAS-209, CAS-76)
- [ ] **`lazy_load_tables` proxy.** Decide whether to quarantine the feature or fund its remediation: forward backup and lock virtuals, answer metadata from the nested storage, materialise before locks, make whole-database `DROP REPLICA` see lazy tables, add a CI check for unforwarded virtuals. (DRAFT-18, DRAFT-19, DRAFT-20, DRAFT-21, DRAFT-22, DRAFT-23, CAS-230)

## 9. Technical debt, testing and CI/CD stabilisation {#tech-debt}

Goal: the code is split into units a reviewer can hold in context, the test API is out of production classes, and every CI lane and soak scenario is green or has a named owner.

- [~] **Split the three largest files.** `CasGc.cpp` into phase units behind equivalence fences (redoing PR #2286), `CasRefLedger.cpp` into recovery, append lane, snapshot publisher and lifecycle units, and the remount thread and namespace listing out of `Cas::Pool`. No rewrite, wire protocol untouched. (CAS-233, CAS-234, CAS-235)
- [ ] **Test API out of production classes.** Typed sub-configs, injectable clock, sleeper, executor and fault injector, and a test-control adapter, so production classes carry no `ForTest` members. (CAS-294, CAS-294.1, CAS-294.2, CAS-294.3, CAS-224, CAS-207, CAS-205, CAS-202, CAS-203, CAS-200, CAS-225, DRAFT-36, DRAFT-40, DRAFT-41)
- [ ] **CI lanes fit their budgets.** Sanitizer-only thread-pool profile so MSan and TSan shards fit 6 hours; the ASan CAS lane has no memory-limit failures; CA-S3 lanes stop holding 10,000 HTTP sessions. (CAS-223, CAS-222, CAS-127, DRAFT-37)
- [ ] **Test coverage that can fail.** Untag the 18 stateless tests still excluded from CAS, run production mutants against the gate, clear the three "known flaky" tests, add contract tests for real providers and partial writes, a perf-smoke gate on S3 request cost per insert. (CAS-210, CAS-208, CAS-211, CAS-198, CAS-199, CAS-77, CAS-70.1, CAS-70.6, CAS-165, CAS-281, CAS-212, CAS-226, CAS-206, DRAFT-39)
- [ ] **Soak scenarios reach a verdict.** Bring the five unowned scenarios to a full-scale result, fix the harness verdicts that misread designed behaviour, and size waits from measurements. (CAS-220, CAS-220.1, CAS-220.2, CAS-220.3, CAS-220.4, CAS-220.5, CAS-213, CAS-213.1, CAS-214, CAS-215, CAS-217, CAS-312, CAS-317, CAS-162, CAS-307, CAS-309, CAS-313, CAS-314, CAS-316, CAS-93, CAS-96, CAS-97, CAS-98, CAS-99, CAS-104, CAS-63, CAS-121, CAS-122, CAS-174, CAS-216, CAS-218, CAS-280, DRAFT-38)
- [ ] **Relink-confirm liveness closed on its own gate.** A ten-minute and a two-hour GCS soak prove every refusal counter is visible and the F11 livelock is gone. (CAS-170, CAS-170.1, CAS-170.2, CAS-170.4)
- [ ] **Hot-key lane follow-ups.** Measure phase A on the parallel stateless suite, then decide phase B; close the review nits. (CAS-70, CAS-70.3, CAS-70.4, CAS-70.5, CAS-71.4)
- [ ] **Formal models kept honest.** Per-invocation TLC metadirs, the orphan-sweep invariant in the manifest model, stale prose in model configs. (CAS-227, CAS-228, CAS-310, CAS-171)
- [ ] **Code hygiene.** Remove internal plan tags from comments, move the in-memory backend out of the production target, Darwin build, stale comments, parser constants tied to MergeTree names, typed identifiers. (CAS-130, CAS-131, CAS-161, CAS-236, CAS-237, CAS-238, CAS-239, CAS-242, CAS-246, CAS-201, CAS-204, CAS-195, CAS-186)

## Suggested order {#suggested-order}

1. Theme 1 (first deployment) and the in-progress items of themes 2 to 4, because they unblock the first customer.
2. Theme 3 stage A and bulk deletes, then theme 2 zero-GET publish: these are the two largest measured cost centres on the demo stand.
3. Theme 6 lease-under-pressure and theme 7 GC health surface, because both turn silent failures into visible ones.
4. Theme 5 backends in customer order (GCS gate first, Azure second), theme 8 upstream carve-outs alongside every release.
5. Theme 9 continuously, sized to keep CI green rather than as a project of its own.
