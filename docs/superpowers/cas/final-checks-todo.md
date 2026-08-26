# CAS final checks before release {#final-checks-todo}

Working checklist for the pre-release pass. Items link to their authority; this file is the
short-lived TODO, not the record.

## 1. DONE — Fix #2173: cross-disk `ATTACH PARTITION FROM` local -> CAS {#fix-2173}

- Issue: https://github.com/Altinity/ClickHouse/issues/2173 (fixed on this branch; CAS-058)
- Adjudication: `docs/superpowers/cas/2031-triage.md` `{#cas-058}`; implementation plan:
  `docs/superpowers/plans/2026-08-21-cas-freezeremote-transaction.md`
- Implemented 2026-08-21: `c5467b8989b` adds local, same-pool CAS and replicated regression legs;
  `cfe9a6a3615` makes `freezeRemote` publish a cross-disk CAS clone through one transaction.
- Verified: new `05025_cas_attach_partition_cross_disk`, existing `05003_cas_freeze` and
  `05024_cas_freeze_two_roots`, the `clickhouse` and `unit_tests_dbms` builds, and the full
  `CAS*:Cas*:CA*` gate (2058 tests).

## 2. DONE — Fix #2212: `FREEZE` shadow namespace under `server_root_id` {#fix-2212}

- Issue: https://github.com/Altinity/ClickHouse/issues/2212 (fixed on this branch; CAS-001)
- Adjudication: `docs/superpowers/cas/2031-triage.md` `{#cas-001}`; implementation plan:
  `docs/superpowers/plans/2026-08-21-cas-shadow-namespace-server-root.md`
- Implemented 2026-08-21: `8e5ee61b6cc` adds the two-root regression and `11f5397a629`
  prefixes `shadowNamespace` plus all three shadow enumeration scopes; `7c4d4124133` updates the
  source contracts and examples.
- Verified: `05024_cas_freeze_two_roots`, existing self-release guard `05003_cas_freeze`, Release
  and Debug builds, and the full `CAS*:Cas*:CA*` gate (2058 tests).

## 3. DONE — Minimum #2244 renewal recovery cut {#fix-2244}

- Issue: https://github.com/Altinity/ClickHouse/issues/2244 (filed from the CI RCA of job 96307284077)
- Full record + fix directions (value order): `docs/superpowers/cas/BACKLOG.md`
  `{#issue-2244-lease-retry-asymmetry}` — (1) in-period renewal retries, (2) per-step remount-chain
  retries + own-ambiguous-claim window-reset check, (3) trip/remount observability + ProfileEvents,
  (4) rate-limit the snapshot-publication refusal loop.
- Implemented 2026-08-24: ambiguity-aware in-period renewal retries, exact-`GET` response-loss
  resolution, immutable `write_attempt_id`, lease-deadline enforcement, bounded renewal/remount
  observability, runtime-owned persistent workers, and snapshot-publication refusal backoff.
- Verified: focused renewal TLA+ 17/17, complete mount TLA+ 21/21, Release and Debug full `CAS*`
  gates, proxy integration 2/2, and the single exact-revision 15-minute S39 campaign (16/16 verdicts,
  50 recovered short pulses, one successful terminal remount, clean fsck).
- Deferred deliberately: per-step remount-chain retries, persistent step progress, and
  own-ambiguous-claim observation. These remain live under
  `BACKLOG.md` `{#issue-2244-remount-retry-follow-up}` and require a separate focused TLA+ design.

## 4. Fix CAS disk settings whitelist — valid S3 keys rejected {#fix-s3-key-whitelist}

- Field report: `Unknown setting 'http_keep_alive_timeout'` kills the server at startup when the
  #2243 mitigation is applied to a CAS disk block
  (https://github.com/Altinity/clickhouse-regression/actions/runs/32408309167/job/96552561919).
- Full record + fix shape: `docs/superpowers/cas/BACKLOG.md` `{#cas-disk-s3-key-whitelist-gap}` —
  skip builtin `S3AuthSettings`/`S3RequestSettings` names in `ContentAddressedSettings.cpp` instead
  of enumerating them into `non_cas_keys`; blocks the #2243 CI mitigation until fixed.

## 5. Fix #2211 — `GC RUN` follower row must name the outcome and the leader {#fix-2211}

- Issue: https://github.com/Altinity/ClickHouse/issues/2211 (CONFIRMED; contract decided 2026-08-21)
- Full adjudication + fix shape: `docs/superpowers/cas/BACKLOG.md` `{#issue-2211-gc-run-follower-noop}`
- Plan: keep the quiet idempotent OK (no exception — `ON CLUSTER` inversion); add `finish` column to
  the `RUN` result set; add advisory `hostname`/`server_uuid`/`pid` to `GcLease` (MountLease
  precedent, `owner` fencing token untouched) + `leader_host` in the row; populate
  `system.cas_mounts.is_leader` for all mounts; one-sentence leadership note in `{#sql-gc-run}` docs.
- No-steal on manual `RUN` stays untouched.

## 6. Wire-key rename — full-word keys in all persisted formats {#fix-wire-keys}

- Reviewer ask; full record + fix shape: `docs/superpowers/cas/BACKLOG.md` `{#wire-keys-full-words}`
- Plan: rename all 71 short field keys to exact struct-field names across ~14 codecs in
  `Formats/*.cpp` + readers + golden tests + `Formats/README.md` convention line. Breaking change to
  every persisted format — only cheap pre-release, never again.
- Mechanical sweep: codex dispatch, review here.

## 7. DONE — Fix #2219 — relink refusal must not log Error + stack trace {#fix-2219}

- Issue: https://github.com/Altinity/ClickHouse/issues/2219 (CONFIRMED, cosmetic; misdirects triage)
- Full record + fix shape: `docs/superpowers/cas/BACKLOG.md` `{#issue-2219-relink-refusal-log-level}`
- Implemented 2026-08-26 (`081c473904e` on this branch; `c5ded159345` on
  `cas-relink-refusal-classification` off `antalya-26.6`, PR #2159 having merged that morning): both
  relink retry-later throw sites switched from `NETWORK_ERROR` — but to `NO_REPLICA_HAS_PART`, NOT the
  planned `ABORTED`. Same demotion list (`LOG_INFO`, no stack trace, zero upstream-code changes), plus
  two properties `ABORTED` lacks: it keeps `need_to_save_exception` (a refusal storm stays visible in
  `system.replication_queue` — `ABORTED`'s save-nothing shape is the documented no-backoff pathology,
  see `[merge-progress-reset-mount-fence]`), and it is the one fetch-transient code the stateless
  corpus already tolerates in `part_log` checks — which also fixes the reproducible `02265_column_ttl`
  failure in the CAS lanes on PR #2159 (13/14 reruns under `prefer_fetch_merged_part_size_threshold=1`).
  `test_confirm_refuses_when_source_dropped_in_window` now pins the classification (failing-first
  proven against the old binary).

## 8. DONE — Fix CAS-040 — a newline in a part-file path wedges GC pool-wide {#fix-cas-040}

- Source: issue #2031 finding CAS-040, adjudicated + REPRODUCED live on HEAD (2031-triage, 2026-08-21)
- Full record: `docs/superpowers/cas/2031-triage.md` `{#cas-040}`; implementation plan:
  `docs/superpowers/plans/2026-08-21-cas-manifest-path-hygiene.md`.
- Implemented 2026-08-22: `6333a986af6905e59f538855f61caf6dc296d1cf` makes the entry record and
  `bannerFor` use one escaped path representation; `f738450415013c0f3da8b72746eed7edd04ed16d`
  records an undecodable orphan manifest and continues the sweep.
- The upstream `ProjectionsDescription::getDirectoryName` change is unnecessary: the manifest now
  carries arbitrary paths faithfully.

## 9. PARTLY DONE — Fix the three untracked P1s from the 2026-08-05 umbrella review {#fix-umbrella-p1}

Originally re-verified as open against HEAD on 2026-08-21
(`docs/superpowers/cas/fable-review-triage.md`). Current status follows.

- **DONE — B1, `~Gc` vs `meta_pool` drain (UAF class).** Implemented 2026-08-24: `7a376f141d3`
  gives every meta-pool job shared ownership of the state it touches, `e430917bb3c` drains the pool
  on every throwing round exit, and `683b68606e2` keeps that cleanup nonthrowing while preserving
  successful-path pool/framework exceptions. Focused lifetime and throwing-exit tests cover both
  condemn-marker and confirmed-meta-delete jobs.
- **DONE — B2a, `MountLeaseKeeper::claim` environment-change exceptions.** Implemented 2026-08-24:
  `98e49683ed4` reclassifies all claim-time slot races and foreign-owner conflicts from
  `LOGICAL_ERROR` to `ABORTED`, preserves `MountFencedException` for GC fencing, and adds focused
  coverage for all adoption windows. (2b, the GC delete-marker site, remains tracked separately as
  `{#versioning-enabled-after-mount}`.)
- **OPEN — B3, inline `disk(metadata_type='cas', …)` bypasses the whole `SYSTEM CAS` privilege model.**
  The factory still has no `custom_disk` gate, so any user who can `CREATE TABLE` mints a permanent
  pool member with a pool-wide view of other tenants' namespaces. The ready-made pattern is the
  `use_fake_transaction` rejection one file over. Decide: reject `custom_disk` for pool-joining
  metadata types, or require a dedicated grant.

## 10. DONE — Fix the shutdown-path null dereference: detached CAS work outliving `Context` {#fix-detached-pool-context}

- Source: the second 2026-08-05 umbrella review (opus), items B3 + B4, re-verified at HEAD
  2026-08-22; full record in `docs/superpowers/cas/opus-review-triage.md` `{#b3}`/`{#b4}` and BACKLOG
  `{#detached-pool-outlives-context}`. Untracked until now.
- Implemented 2026-08-25 in `e69b4d3c26f..e51affc6206`: all detached CAS work uses one tracked
  dispatcher; shutdown stops admission, interrupts recovery and drains before releasing the pool;
  every teardown phase is fail-soft; and CAS event sinks use a weak, null-safe `Context` path.
  Diagnostic dispatch failure also cannot replace the caller's fail-closed exception.
- Verified on the final revision: exact Debug `CAS*` gate 2170/2170, exact ASan `CAS*` gate
  2169/2169 with zero sanitizer reports, plus a real CAS-backed server shutdown after an
  approximately 18 MiB insert (`SIGTERM=0`, real wait status 0, no remaining process and no detached
  background-task timeout or fatal teardown diagnostics).

## 11. DONE — Land the GCS request-isolation work {#gcs-request-isolation}

- Plan: `docs/superpowers/plans/2026-08-20-cas-gcs-request-isolation.md` (+ its spec in
  `docs/superpowers/specs/` — same date/topic).
- Landed and reviewed on this branch: `faab6678d8f` isolates GCS generation adaptation to explicitly
  marked CAS requests, `12079eedd47` strengthens deterministic integration coverage, and
  `2c5a07f1288` applies the final caller-contract corrections. The current full Release and Debug
  `CAS*` gates are green.
- The credentialed three-group live-GCS run remains mandatory release-environment evidence; it is an
  external release gate, not unfinished request-isolation implementation.

## 12. Release hygiene: CHANGELOG entry and the pinned S3 image {#release-hygiene}

Both from the 2026-08-05 opus review, re-verified at HEAD 2026-08-24
(`docs/superpowers/cas/opus-review-triage.md` `{#m26}`, `{#m31}`).

- **No CHANGELOG entry exists for CAS anywhere on the branch**, for what the docs call a headline
  feature. Write one (category and wording per `.github/PULL_REQUEST_TEMPLATE.md`), and cover the
  user-visible upstream side effects this triage surfaced: the `clickhouse-disks --query` exit-code
  contract change, the `lazy_load_tables` SYSTEM-command behaviour change, and the `cas_log` /
  `cas_gc_log` tables shipping enabled.
- **Resolved:** the stateless binary, integration Compose service, and all nine CA soak Compose files
  use `1.0.0-rc.3`. Literal tag pins were deliberately retained to match existing third-party image
  practice.
