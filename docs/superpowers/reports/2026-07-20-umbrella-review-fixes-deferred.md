# Umbrella-review fixes: what was deferred and why

Date: 2026-07-20
Companion documents:
- Review report (findings, evidence, line anchors): `docs/superpowers/reports/2026-07-20-umbrella-review-cas-vs-antalya-26.6.md`
- Fix plan (what IS being done): `docs/superpowers/plans/2026-07-20-umbrella-review-fixes.md`

The fix plan's scoping rule was: cover the maximum number of findings with the minimum number of behavior-changing interventions, and no new-feature work. This document is the tracked return-item list for everything that rule excluded — per the "no known reds without a tracked return item" policy, nothing here is silently dropped; every entry has an owner-decision, a deferral risk, and a return condition.

Categories:
- **DEFERRED** — should be done, parked with an explicit return condition.
- **REJECTED** — will not be done; the finding's proposed fix contradicts a project policy or its cost/risk exceeds the benefit. The underlying observation stays valid.
- **FEATURE** — valid improvement, but it adds new surface (schema, metrics, protocol) and therefore belongs to feature planning, not to a fix batch.
- **UPSTREAM-PREP** — only matters when preparing the branch for upstream submission.
- **VERIFY** — an open question from the review that needs investigation before deciding whether any fix is needed.

## Summary table {#summary}

| # | Item | Report finding | Category | Return condition / trigger |
|---|------|----------------|----------|----------------------------|
| 1 | Relink retention pin | 1 (blocker, 85) | DEFERRED (planned feature) | Before any release/soak that exercises replica fetches under GC pressure |
| 2 | Startup bounded retry for `Pool::open` | 6 (70) | DEFERRED (own recorded design) | Next stabilization batch; before any long soak with a flaky backend |
| 3 | Relink post-adopt failure → no byte-fetch fallback | deep-audit #5 (42) | VERIFY | Resolve together with item 1 (same code region) |
| 4 | `promote()` manifest GET-back skip | perf minor (40) | REJECTED | — (revisit only if commit-latency budgets demand it AND an equivalent re-proof is designed) |
| 5 | `head_first` adaptive dedup threshold | perf minor (30) | DEFERRED | After a bulk-load benchmark quantifies the HEAD+PUT tax |
| 6 | Orphan-sweep protection-view cache per pass | perf minor (45) | DEFERRED | If GC-round wall-clock or LIST budgets become a problem on large pools |
| 7 | `SingleWriterSlot::renewOnce` lock restructuring | concurrency (35) | REJECTED (comment lands instead) | Reopen if any new `state_mutex`-guarded accessor is ever added |
| 8 | `promote()` setting `alive = false` (lifecycle symmetry) | deep-audit nit (15) | REJECTED (document-only) | Reopen if `PartWriteTxn` ever gets a second caller pattern that reuses objects |
| 9 | `srid` column in the GC round log | operability note under finding 5 | ~~FEATURE~~ **DONE 2026-07-20** (`3b2c9abb822`) | — |
| 10 | Prometheus-scrapeable GC health (`AsynchronousMetrics`) | operability (42) | ~~FEATURE~~ **DONE 2026-07-20** (`3b2c9abb822`) | — |
| 11 | Result sets for `GARBAGE COLLECTION` / `GC REBUILD` | ux (45) | ~~FEATURE~~ **DONE 2026-07-20** (`cb111510c1a`) | — |
| 12 | Upstream diff split (4 separable changes) | compat finding 11 | UPSTREAM-PREP | When drafting upstream PRs |
| 13 | Non-CA regression coverage for `renameParts` durability reorder | compat finding 11 | UPSTREAM-PREP + VERIFY | MUST precede the upstream PR of that hunk; ideally sooner |
| 14 | `docs/superpowers/` worklog-path citations in permanent comments | ockham (25) | ~~UPSTREAM-PREP~~ **DONE 2026-07-20** (`11bf17fbf66`) | — |
| 15 | `CasLayout.h` parsers out-of-line move | headers (30) | ~~DEFERRED~~ **DONE 2026-07-20** (`4fafb4edb28`) | — |
| 16 | GCS end-to-end integration test (emulator) | tests (25) | DEFERRED | Before GCS is promoted out of experimental |
| 17 | Decommission owner-anchor tombstone race | security #4 (25) | DEFERRED (design exists) | Covered by `2026-07-18-t5-owner-tombstone-design.md`; execute with the next decommission work |
| 18 | `~Pool()` teardown vs `object_storage->shutdown()` ordering | concurrency #2 (25, low conf) | VERIFY | One-off investigation; escalate to fix only if the ordering guarantee is absent |
| 19 | Default-enabled CAS system logs zero-overhead claim | compat needs-verification | VERIFY | One-off check on a no-CA-disk server |
| 20 | `system.content_addressed_log`/`mounts` grant defaults for low-privilege users | security needs-verification | VERIFY | Before first multi-tenant deployment |

## Details {#details}

### 1. Relink retention pin (report finding 1 — the review's only blocker) {#relink-pin}

**What:** `DataPartsExchange` fetch-by-relink has a commit-before-release gap: the sender is fire-and-forget, so a source part GC'd mid-relink can leave the receiver with a committed manifest naming a deleted blob. The code itself documents the gap and names the intended fix ("a retention floor for read-replica snapshots… not wired here yet").
**Why not in the fix plan:** this is protocol design work (sender-side pin lifecycle, release-on-confirm/timeout), i.e. exactly the "new feature" class the plan excludes; it is already planned separately (fetch-handoff epoch-floor design).
**Deferral risk:** silent, fsck-detectable-only integrity violation for a committed part; window requires GC ≥2 folds during an in-flight relink of a blob with no other referencer — narrow, but nonzero on busy pools.
**Return condition:** must land (or relink must fail closed to byte-fetch) before any release or long soak that exercises replica fetches under GC pressure. Until then, treat `ca-fsck` dangling-manifest hits on fetched parts as this known cause first.

### 2. Startup bounded retry (report finding 6) {#startup-retry}

**What:** a transient backend failure during `Cas::Pool::open` inside `AsyncLoader`-driven table load strands the table permanently `FAILED`; `DETACH`/`ATTACH` confirmed not to retry; only a server restart recovers.
**Why not in the fix plan:** it changes startup semantics (retry/backoff classification of transient vs. protocol errors) — a "dangerous" intervention by the plan's rule — and it already has its own recorded design on this branch (bounded retry around the seal PUT in `ensureRefTableRecovered`, commit `3b9325f8029`; tracked in `docs/superpowers/cas/BACKLOG.md`).
**Deferral risk:** availability only (no integrity impact): one S3 blip at the wrong moment costs a production restart.
**Return condition:** next stabilization batch; mandatory before any long unattended soak against a deliberately flaky backend, since it converts one injected fault into a wedged phase (this is exactly how it was discovered).

### 3. Relink post-adopt failure handling (deep-audit #5) {#relink-post-adopt}

**What:** in `Fetcher::relinkPartToDisk`, once `adoptPartFromManifest` durably publishes the `tmp-fetch_` ref, a later local exception (format detection, checksum load) propagates instead of falling back to byte-fetch, possibly leaving a published ref with no loaded part.
**Why not in the fix plan:** unproven consequence — ordinary MergeTree startup cleanup of stale `tmp-fetch_` directories may already reap the orphan via `removeDirectory` → `dropRefIfPresent`; wrapping the tail in a compensating catch without knowing that would be blind engineering in the same region item 1 is about to rework.
**Return condition:** verify the startup-cleanup path during the retention-pin work (item 1); add the compensating `dropRefBestEffort` catch there if the reap is not guaranteed.

### 4. `promote()` manifest GET-back skip — REJECTED {#promote-getback}

**What:** the performance reviewer proposed passing the locally-built `PartManifest` into `promote` to skip the GET+decode of the manifest the same build just PUT (saves one S3 GET per part commit).
**Why rejected:** it removes a fail-closed re-proof (`RefMatchesBody`, `ManifestNamespaceMatches`) on the commit path. Project policy is explicit: no skip-read shortcuts in CA storage — re-reading/re-proving is the identity primitive, and "we just wrote it" is precisely the assumption the re-proof exists to not trust (partial writes, racing displacement, backend lies).
**Residual note:** the observation (one extra GET per part commit) stays true and is the accepted price of the invariant. Revisit only if per-part commit latency becomes a measured problem AND a design preserves an equivalent proof (e.g. conditional GET on the just-received ETag/generation — which is still a round trip, so the win may not exist).

### 5. `head_first` adaptive threshold {#head-first}

**What:** blobs ≥1 MiB always pay HEAD-then-PUT even when genuinely new, doubling request count for cold bulk loads.
**Why deferred:** the current behavior is a documented, reasoned tradeoff (avoids large-body PUT-then-412 storms); "adaptive" needs data to not be guesswork.
**Return condition:** run a bulk-INSERT benchmark (cold pool, wide table) counting HEAD/PUT per blob; tune the default or gate on dedup-cache warmth only if the measured tax matters.

### 6. Orphan-sweep protection-view caching {#orphan-sweep-cache}

**What:** `CasOrphanManifestSweep` recomputes each namespace's full ref-log protection view (LIST + GETs) on every page/round instead of caching it per sweep pass.
**Why deferred:** correctness is fine (verified in review); the fix is churn inside a verified GC area for a cost that only shows on pools where one namespace's debris spans multiple listing pages.
**Return condition:** GC-round wall-clock or S3-op budgets regressing on large pools, or the round log showing the sweep dominating (`CasGcEnumerationPages` growth per round).

### 7. `renewOnce` lock restructuring — REJECTED (comment instead) {#renew-once}

**What:** `SingleWriterSlot::renewOnce` holds `state_mutex` across the heartbeat PUT, against the file's own documented discipline.
**Why rejected:** currently benign under two enforced invariants (terminate joins the renewal thread before locking; single driver). Restructuring lease-heartbeat locking is a hard-concurrency change — highest-risk class per project experience — to fix a purely hypothetical stall. The fix plan (Task 14) instead lands a loud comment on `state_mutex` documenting the invariant so no one adds a locked accessor blindly.
**Return condition:** reopen the restructuring the moment anyone needs a new `state_mutex`-guarded accessor on `SingleWriterSlot`.

### 8. `promote()` lifecycle symmetry (`alive = false`) — REJECTED (document-only) {#promote-alive}

**What:** `abandon()` deadens the object, `promote()` does not, so a promoted `PartWriteTxn` nominally accepts further calls.
**Why rejected:** the "symmetric" fix has hidden call-path risk: `publishStaging`'s repoint branch and the transaction destructor legitimately call `abandon()` on builds after promote-adjacent flows, relying on today's exact no-op/soft-fail semantics; flipping `alive` in `promote` converts some of those into new throw paths through a destructor. The hazard it guards against (a future caller reusing one `PartWriteTxn` across two builds) does not exist in the current call graph.
**Return condition:** if `PartWriteTxn` ever gains a caller that holds instances long-term, add the guard *together with* an audit of every `abandon()` call site.

### 9–10. GC-log `srid` column; Prometheus GC-health metrics — DONE 2026-07-20, `3b2c9abb822` {#observability-features}

> Executed same-day on user request: `srid` on both `Start`/`Finish` GC-log rows; `CasGCIsLeader_<disk>` / `CasGCPendingReclaim_<disk>` / `CasGCLastSuccessAgeSeconds_<disk>` / `CasGCWedgedNamespaces_<disk>` in asynchronous metrics; `tryFromDisk` helper added (existing 4 detection copies migrate in fix-plan Task 13). Original entry kept below for context.

**What:** (9) the GC round log has no `srid`, so rounds can't be durably attributed to a mount slot; (10) "GC stuck" / "mount lease lost" exist only as SQL-queryable state, invisible to standard `/metrics`-based alerting.
**Why not in the fix plan:** both are additive surface — a system-log schema extension and new `AsynchronousMetrics` entries — i.e. small features with naming/compat decisions, not fixes.
**Deferral risk:** operators of a production-like deployment cannot alert on the single most important CAS health signal without a custom SQL exporter.
**Return condition:** next CAS observability batch; item 10 should precede the first deployment anyone monitors with standard dashboards. Note Task 9 of the fix plan (local-row scoping in `system.content_addressed_mounts`) partially mitigates the triage confusion in the meantime.

### 11. Result sets for `GARBAGE COLLECTION` / `GC REBUILD` — DONE 2026-07-20, `cb111510c1a` {#verb-result-sets}

> Executed same-day on user request. Note learned during implementation: `runGarbageCollectionRoundNow`/`runGcRebuildNow` already returned `Cas::RoundReport`/`Cas::RebuildReport` (the interpreter was discarding them), and `SYSTEM` queries cannot take `FORMAT`, so tests invoking GC mid-script moved to `.sh` + `/dev/null` (05007, 05010 converted; 05008 redirect added). Original entry kept below for context.

**What:** `DROP POOL MEMBER` returns a 10-column report; the other two verbs return nothing (`RebuildReport` goes to `LOG_INFO` only).
**Why not in the fix plan:** additive client-visible output; harmless but zero-risk-budget was reserved for actual defects.
**Return condition:** next UX pass; trivial to implement by reusing the existing `SourceFromSingleChunk` pattern from the `DROP POOL MEMBER` handler.

### 12–14. Upstream-preparation items {#upstream-prep}

**What:**
- (12) Four changes bundled in this branch are independently reviewable and should be separate upstream PRs: GCS conditional-write support (`GCSConditionalDialect`/`GOOG4Signer`/HMAC client), the `renameParts` disk-transaction-close durability fix, the `ReadBufferFromFileView` B115 position fix + gtests, and the global S3 412-no-retry policy.
- (13) The `renameParts` reorder is unconditional for ALL `DiskObjectStorage`-backed MergeTree writes and is currently validated only by CAS-side soaks; it needs dedicated non-CA regression coverage (plain S3 + `ReplicatedMergeTree`, cached object storage, zero-copy) — this doubles as a VERIFY item and should not wait for the upstream draft if cheap to run.
- (14) **DONE 2026-07-20, `11bf17fbf66`**: all ten `docs/superpowers/` citations removed from `src/` (nine files); essential rationale folded into the prose, stable tags (B37/R3/S22/T5) kept.
**Why not in the fix plan:** none of this changes behavior on this branch; it is packaging work for the upstream submission.
**Return condition:** when drafting upstream PRs — except the non-CA `renameParts` test run (13), which is worth doing at the next convenient CI window.

### 15. `CasLayout.h` parsers out-of-line — DONE 2026-07-20, `4fafb4edb28` {#caslayout-inline}

**What:** `parseRefObjectKey`/`parseManifestKey`/`checkNamespace` (~50 lines each) live inline in a header with 26 direct includers, against the file's own out-of-line precedent.
**Why deferred:** CAS-internal build-time cost only; zero runtime impact; pure churn best batched with other mechanical moves.
**Return condition:** fold into the next CAS-wide mechanical cleanup or the upstream-prep pass.

### 16. GCS end-to-end integration test {#gcs-e2e}

**What:** `GCSConditionalDialect`/`GOOG4Signer` are unit-tested at the request-transformation level only; no CI job exercises real GCS wire behavior (all CAS integration tests are RustFS-backed).
**Why deferred:** needs new test infrastructure (fake-gcs-server or real-bucket credentials); GCS support is explicitly experimental and config-gated (`gcs_max_conditional_put_bytes`).
**Return condition:** hard prerequisite for promoting GCS out of experimental status.

### 17. Decommission owner-anchor tombstone race {#tombstone-race}

**What:** a same-UUID successor recreating its mount in the narrow gap between decommission's liveness recheck and its owner CAS gets its anchor tombstoned; a later restart of that identity refuses to reclaim. Documented and consciously scoped in `CasDecommission.cpp:262-269`.
**Why deferred:** the design covering it already exists (`docs/superpowers/specs/2026-07-18-t5-owner-tombstone-design.md`); availability-only impact, triggerable solely by a privileged operator racing the victim's own restart.
**Return condition:** execute the T5 design with the next decommission-related work; until then the operational rule is "don't run `DROP POOL MEMBER` while the victim may be restarting".

### 18–20. Verification debt (investigate before deciding) {#verify}

- **(18) `~Pool()` vs `object_storage->shutdown()` ordering** (concurrency #2, low confidence): can a deferred `Pool` destruction (in-flight transaction holding the last `PoolPtr`) issue its farewell/lease I/O through an already-shut-down object storage? Best-effort try/catch bounds the blast radius to a swallowed error, but the ordering contract was never confirmed. One-off trace of `S3ObjectStorage::shutdown` semantics + the table-shutdown drain guarantee; fix only if the guarantee is absent.
- **(19) Default-enabled CAS system logs on non-CA servers**: the code claims zero cost when no CA disk exists; not independently traced through the SystemLog flush path. One-off check (row counts + background flush activity on a vanilla server); matters for upstreaming the default config.
- **(20) Grant defaults for the new system tables**: whether low-privilege users see `system.content_addressed_log` (`token`, `detail` map) and `content_addressed_mounts` (`hostname`, `pid`) by default, consistent with peers like `system.replicas`. No literal secrets found in the schemas; confirm before any multi-tenant deployment.
