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

## 7. Fix #2219 — relink refusal must not log Error + stack trace {#fix-2219}

- Issue: https://github.com/Altinity/ClickHouse/issues/2219 (CONFIRMED, cosmetic; misdirects triage)
- Full record + fix shape: `docs/superpowers/cas/BACKLOG.md` `{#issue-2219-relink-refusal-log-level}`
- Plan: switch both relink retry-later throw sites in `DataPartsExchange.cpp` from `NETWORK_ERROR`
  to `ABORTED` (already in the `processQueueEntry` demotion list → `LOG_INFO`, no stack trace).
  Zero upstream-code changes (user constraint).

## 8. DONE — Fix CAS-040 — a newline in a part-file path wedges GC pool-wide {#fix-cas-040}

- Source: issue #2031 finding CAS-040, adjudicated + REPRODUCED live on HEAD (2031-triage, 2026-08-21)
- Full record: `docs/superpowers/cas/2031-triage.md` `{#cas-040}`; implementation plan:
  `docs/superpowers/plans/2026-08-21-cas-manifest-path-hygiene.md`.
- Implemented 2026-08-22: `6333a986af6905e59f538855f61caf6dc296d1cf` makes the entry record and
  `bannerFor` use one escaped path representation; `f738450415013c0f3da8b72746eed7edd04ed16d`
  records an undecodable orphan manifest and continues the sweep.
- The upstream `ProjectionsDescription::getDirectoryName` change is unnecessary: the manifest now
  carries arbitrary paths faithfully.

## 9. Fix the three untracked P1s from the 2026-08-05 umbrella review {#fix-umbrella-p1}

Re-verified against HEAD on 2026-08-21 (`docs/superpowers/cas/fable-review-triage.md`): still open,
and none of the three was tracked anywhere until now.

- **B1 — `~Gc` vs `meta_pool` drain (UAF class).** No explicit `~Gc` exists and `meta_pool` is
  declared BEFORE `condemn_marker_mutex`/`condemn_markers_confirmed`, so members destroy in an order
  that leaves a live pool worker locking a destroyed mutex when a round throws after scheduling a
  condemn-marker write. rev.8 made `Gc` destruction a routine event (UNMOUNT, GC STOP), not just
  shutdown — which widens the window rather than closing it. Fix: explicit destructor that
  waits `meta_pool` first (or declare it last), plus a drain on the scheduler's exception path.
- **B2a — `MountLeaseKeeper::claim()` throws `LOGICAL_ERROR`** on four "environment changed under us"
  branches reachable from the background self-remount thread; in debug/ASan that is `abort()` on the
  very lanes that certify the feature. The sibling renewal path was already reclassified to
  `ABORTED`/`MountFencedException`; apply the same to `claim()` and update the death test that pins
  the current behaviour. (2b, the GC delete-marker site, is tracked separately as
  `{#versioning-enabled-after-mount}`.)
- **B3 — inline `disk(metadata_type='cas', …)` bypasses the whole `SYSTEM CAS` privilege model.**
  The factory still has no `custom_disk` gate, so any user who can `CREATE TABLE` mints a permanent
  pool member with a pool-wide view of other tenants' namespaces. The ready-made pattern is the
  `use_fake_transaction` rejection one file over. Decide: reject `custom_disk` for pool-joining
  metadata types, or require a dedicated grant.

## 10. Fix the shutdown-path null dereference: detached CAS work outliving `Context` {#fix-detached-pool-context}

- Source: the second 2026-08-05 umbrella review (opus), items B3 + B4, re-verified at HEAD
  2026-08-22; full record in `docs/superpowers/cas/opus-review-triage.md` `{#b3}`/`{#b4}` and BACKLOG
  `{#detached-pool-outlives-context}`. Untracked until now.
- One coupled chain: `shutdown()` does not drain the detached CAS dispatches, which hold a strong
  `Pool` reference, so `~Pool`'s durable farewell write and mount-event emit can run arbitrarily late
  — and CAS logs through a strong `ContextPtr` whose `shared` is already nulled by
  `resetSharedContext()`. The snapshot publisher is a routine path, so this is not an exotic race.
- Fix both halves: a tracked drain `shutdown()` waits on (so `~Pool` runs while the world exists),
  and an event-emit path that tolerates an absent log/context instead of dereferencing it.
- Pre-release: yes. It is a crash at shutdown on any server that ever mounted a CAS disk.

## 11. Land the GCS request-isolation work (IN PROGRESS in a parallel session) {#gcs-request-isolation}

- Plan: `docs/superpowers/plans/2026-08-20-cas-gcs-request-isolation.md` (+ its spec in
  `docs/superpowers/specs/` — same date/topic).
- Owned by the parallel session; before release: confirm it landed (or record its cut-line), gate green.
