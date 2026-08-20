# CAS final checks before release {#final-checks-todo}

Working checklist for the pre-release pass. Items link to their authority; this file is the
short-lived TODO, not the record.

## 1. Fix #2173 — cross-disk `ATTACH PARTITION FROM` local -> CAS {#fix-2173}

- Issue: https://github.com/Altinity/ClickHouse/issues/2173 (CONFIRMED; reproduced on HEAD first-try)
- Full adjudication + fix shape: `docs/superpowers/cas/BACKLOG.md` `{#issue-2173-freezeremote-gap}`
- Plan: mirror `clonePart`'s CAS single-transaction branch inside
  `DataPartStorageOnDiskBase::freezeRemote` + stateless test from the issue's 3-statement repro.
- Scheduled: tomorrow, before the release.

## 2. Fix #2212 — FREEZE shadow namespace must be under server_root_id {#fix-2212}

- Issue: https://github.com/Altinity/ClickHouse/issues/2212 (CONFIRMED; CAS-001, data loss on backups)
- Full adjudication + fix shape: `docs/superpowers/cas/BACKLOG.md` `{#issue-2212-shadow-namespace}`
- Plan: prefix `shadowNamespace` with `server_root_id` + the two `"shadow/"` enumeration scopes
  (`ContentAddressedMetadataStorage.cpp:1281`, `:1513`, `:1700`) + stateless two-root isolation test.
- Scheduled: tomorrow, before the release.

## 3. Fix #2244 — lease/remount retry asymmetry {#fix-2244}

- Issue: https://github.com/Altinity/ClickHouse/issues/2244 (filed from the CI RCA of job 96307284077)
- Full record + fix directions (value order): `docs/superpowers/cas/BACKLOG.md`
  `{#issue-2244-lease-retry-asymmetry}` — (1) in-period renewal retries, (2) per-step remount-chain
  retries + own-ambiguous-claim window-reset check, (3) trip/remount observability + ProfileEvents,
  (4) rate-limit the snapshot-publication refusal loop.
- Minimum pre-release cut: (1) alone prevents the observed trip class; (3)-(4) are cheap; (2) can
  follow the release if time runs out.

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

## 6. Land the GCS request-isolation work (IN PROGRESS in a parallel session) {#gcs-request-isolation}

- Plan: `docs/superpowers/plans/2026-08-20-cas-gcs-request-isolation.md` (+ its spec in
  `docs/superpowers/specs/` — same date/topic).
- Owned by the parallel session; before release: confirm it landed (or record its cut-line), gate green.
