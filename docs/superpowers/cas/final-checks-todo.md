# CAS final checks before release {#final-checks-todo}

Working checklist for the pre-release pass. Items link to their authority; this file is the
short-lived TODO, not the record.

## 1. Fix #2173 — cross-disk `ATTACH PARTITION FROM` local -> CAS {#fix-2173}

- Issue: https://github.com/Altinity/ClickHouse/issues/2173 (CONFIRMED; reproduced on HEAD first-try)
- Full adjudication + fix shape: `docs/superpowers/cas/BACKLOG.md` `{#issue-2173-freezeremote-gap}`
- Plan: mirror `clonePart`'s CAS single-transaction branch inside
  `DataPartStorageOnDiskBase::freezeRemote` + stateless test from the issue's 3-statement repro.
- Scheduled: tomorrow, before the release.

## 2. DONE — Fix #2212: `FREEZE` shadow namespace under `server_root_id` {#fix-2212}

- Issue: https://github.com/Altinity/ClickHouse/issues/2212 (fixed on this branch; CAS-001)
- Adjudication: `docs/superpowers/cas/2031-triage.md` `{#cas-001}`; implementation plan:
  `docs/superpowers/plans/2026-08-21-cas-shadow-namespace-server-root.md`
- Implemented 2026-08-21: `8e5ee61b6cc` adds the two-root regression and `11f5397a629`
  prefixes `shadowNamespace` plus all three shadow enumeration scopes; `7c4d4124133` updates the
  source contracts and examples.
- Verified: `05024_cas_freeze_two_roots`, existing self-release guard `05003_cas_freeze`, Release
  and Debug builds, and the full `CAS*:Cas*:CA*` gate (2058 tests).

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

## 6. Wire-key rename — full-word keys in all persisted formats {#fix-wire-keys}

- Reviewer ask; full record + fix shape: `docs/superpowers/cas/BACKLOG.md` `{#wire-keys-full-words}`
- Plan: rename all 71 short field keys to exact struct-field names across ~14 codecs in
  `Formats/*.cpp` + readers + golden tests + `Formats/README.md` convention line. Breaking change to
  every persisted format — only cheap pre-release, never again.
- Mechanical sweep: codex dispatch, review here.

## 7. Fix #2219 — relink refusal must not log Error + stack trace {#fix-2219}

- Issue: https://github.com/Altinity/ClickHouse/issues/2219 (CONFIRMED, cosmetic; misdirects triage)
- Full record + fix shape: `docs/superpowers/cas/BACKLOG.md` `{#issue-2219-relink-refusal-log-level}`
- Plan: switch both relink retry-later throw sites in `DataPartsExchange.cpp` from `NETWORK_ERROR`
  to `ABORTED` (already in the `processQueueEntry` demotion list → `LOG_INFO`, no stack trace).
  Zero upstream-code changes (user constraint).

## 8. Fix CAS-040 — a newline in a part-file path wedges GC pool-wide {#fix-cas-040}

- Source: issue #2031 finding CAS-040, adjudicated + REPRODUCED live on HEAD (2031-triage, 2026-08-21)
- Full record: `docs/superpowers/cas/2031-triage.md` `{#cas-040}` + BACKLOG
  `{#manifest-entry-path-newline-banner}`
- Mechanism: plain DDL (a projection whose name contains `\n`) makes `bannerFor` write the raw path
  into the manifest banner while path hygiene exists only on the DECODE side. The INSERT itself fails
  fail-closed (no committed part, no data loss), but the attempt leaves an undecodable ORPHAN
  manifest, and `planManifestCursorPage` decodes it unguarded (`Gc/CasOrphanManifestSweep.cpp:878`,
  no `try` at `Gc/CasGc.cpp:3124`) — so EVERY GC round in the pool fails forever, cursor never
  advances, nothing is ever reclaimed again.
- Fix: validate path hygiene on the ENCODE side (reject/escape at `bannerFor`) so such a manifest
  cannot be written, and make the sweep's decode of a foreign/undecodable manifest non-fatal to the
  round (record + continue, per [[feedback_ca_gc_never_throw_on_404]]'s principle). Both halves are
  needed: the first stops new occurrences, the second unwedges an existing pool.
- Pre-release: yes. A single unlucky DDL permanently disables reclamation for the whole pool.

## 9. Land the GCS request-isolation work (IN PROGRESS in a parallel session) {#gcs-request-isolation}

- Plan: `docs/superpowers/plans/2026-08-20-cas-gcs-request-isolation.md` (+ its spec in
  `docs/superpowers/specs/` — same date/topic).
- Owned by the parallel session; before release: confirm it landed (or record its cut-line), gate green.
