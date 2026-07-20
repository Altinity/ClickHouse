# Umbrella review: CAS branch vs `altinity/antalya-26.6` merge-base

Date: 2026-07-20
Diff spec: `git diff $(git merge-base altinity/antalya-26.6 HEAD)..HEAD -- src ':!src/Disks/tests/**'`
Merge-base: `2ed6626a25ed0ecabff40adda18d4c2b48539ef5`, branch `cas-gc-rebuild`
Scope: 196 files, +37332 / −111, ~1084 commits.
Method: 14 parallel review subagents (UX/contract, architecture, YAGNI, security, performance, docs, correctness, operability, headers, concurrency, tests, compatibility, lifetime, deep audit of 4 state machines); all high-severity findings re-verified against source by the aggregator (✔). Supporting artifacts: `tmp/umbrella-review/` (diff.patch, diffstat.txt, shared_context.md).

## Summary

The core machinery (write txn, GC round, ref ledger, mount lease, formats) reviewed as remarkably solid — concurrency, header hygiene, and security came back essentially clean, and the deep audit confirmed the major invariants (EDGE-BEFORE-OBSERVE, one-pass GC commit, 404-record-and-continue, mixed-version relink gating) hold. The real issues cluster in three places: an acknowledged-but-unwired relink retention pin (the one data-integrity hole), inconsistent failure handling around `PartWriteTxn::abandon`, and the `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER` / read-only surface. Docs and test gaps are significant but expected for a pre-release branch.

## Blockers

1. **Fetch-by-relink has no retention pin — sender's part GC'd mid-relink can yield a committed manifest naming a deleted blob** ✔
   Risk score: 85 · Sources: deep-audit
   Files/lines: `src/Storages/MergeTree/DataPartsExchange.cpp:263-276` (fire-and-forget sender), `ContentAddressedMetadataStorage.cpp:1381-1389`
   Evidence: the code's own comment: *"There is a commit-before-release gap… A retention floor for read-replica snapshots is the intended protection… but that protocol is not wired here yet; the current same-token tail remains an acknowledged fsck-detectable risk."* The receiver adopts via `adoptEvidence` with no per-blob re-check at promote; if the source part goes Outdated and GC completes ≥2 folds while the receiver's precommit edge-PUT stalls, the blob is reclaimed under a build that still promotes successfully.
   Impact: silent read failure on a supposedly committed part, discovered only by fsck. The one finding that violates durability of committed data.
   Proposed fix: wire the sender-side retention pin (the epoch-floor design already recorded in the fetch-handoff notes) — or fail the relink path closed (fall back to byte fetch) until it exists.

## Major issues

2. **`PartWriteTxn::abandon` failure handling is systemically inconsistent across its call sites** ✔ (partially corrected)
   Risk score: 62 · Sources: lifetime, deep-audit (three independent sightings of one root cause)
   Files/lines: `CasPartWriteTxn.cpp:1128-1213`; `PartFolderAccess.cpp:308-322` (`publishEntries`); `ContentAddressedTransaction.cpp:92-119` (destructor)
   Three facets:
   - `abandon()`'s three `EventEmitter::emit` calls are unguarded, while `promote()` in the same file wraps its equivalent emits in `try/catch` ("CAS event emission after durable promote"). An emit throw after the durable `appendRefOps` misreports a durably-succeeded operation (repoint in `publishStaging`, ref-drop in `removeDirectory`) as failed. **Correction to the subagent's claim:** the "permanent GC watermark stall" does *not* hold — `~PartWriteTxn` idempotently retires `build_seq` (`CasPartWriteTxn.cpp:118-123`), so only the misreporting consequence survives.
   - `publishEntries` (used by relink adoption, `republishRef`, `repointRef`) never calls `abandon()` on exception; the destructor only retires the seq and never removes the precommit binding, so a failed promote leaks a durable live-epoch precommit that the stale-precommit sweep (epoch-scoped: `CasRefLedger.cpp:1665-1692`) and GC ("never detects or removes a dead precommit itself") both refuse to reclaim until remount/restart. Under repeated relink retries against a flaky peer this accumulates pinned debris.
   - `ContentAddressedTransaction`'s destructor swallows a thrown `abandon()` with a comment ("lingering debris is GC-reclaimed") that is inaccurate for the live-precommit case, and `abandon()` sets `alive = false` *before* the correctness-bearing `appendRefOps`, so a failed abandon can never be retried on the same object.
   Proposed fix: guard the emits in `abandon()` like `promote()` does; hold the `PartWriteTxnPtr` across the throwing calls in `publishEntries` and call `abandon()` on exception; move `alive = false` after the `appendRefOps` success; log at ERROR (not silently) when destructor-path abandon fails.

3. **`SYSTEM CONTENT ADDRESSED DROP POOL MEMBER` surface: read-only bypass, unrenewed admin lease, zero access-control tests** ✔
   Risk score: 72 · Sources: ux-contract, deep-audit, tests
   Files/lines: `InterpreterSystemQuery.cpp:1016-1048`; `CasPool.cpp:504-511` (`openForDecommission`); `ContentAddressedMetadataStorage.cpp:486`
   Evidence: (a) the handler goes straight from `ca->store()` to `Cas::decommissionPoolMember` with no `read_only` check, while `createTransaction`/GC-round/GC-rebuild all fail closed on `read_only` — an observe-only DR/audit node can destructively decommission a peer. (b) `openForDecommission` force-sets `config.read_only = false` and `skip_access_check = true` but *not* `background_watermark`, which a read-only host disk sets to `false` (`= (context != nullptr) && !read_only`); `mountWritable` gates lease renewal on that flag, so the admin claim (TTL ~30 s) is never renewed and any drain longer than the TTL aborts midway with no cleanup/report. (c) The most destructive of the three new verbs has no `GRANT`/`REVOKE`/`ACCESS_DENIED` test anywhere (unlike `GC REBUILD`, which has `05011_cas_gc_rebuild_access.sh`).
   Proposed fix: a shared `checkNotReadOnly()` used by all four mutating entry points; force `background_watermark = true` in `openForDecommission` alongside the two flags already forced; add an access-control test mirroring `05011`.

4. **`use_fake_transaction=true` is silently honored on a `content_addressed` disk, breaking the atomic manifest/ref publish** ✔
   Risk score: 65 · Sources: ux-contract
   Files/lines: `RegisterDiskObjectStorage.cpp:85-87`
   Evidence: `needs_real_transaction` only supplies the *default*; an explicit `<use_fake_transaction>true</use_fake_transaction>` is accepted with no validation, and `createTransaction()` then returns `FakeDiskTransaction` (per-file autocommit, no commit point) — exactly the scenario the adjacent comment warns against. A copy-pasted disk config silently loses part-commit atomicity.
   Proposed fix: throw `BAD_ARGUMENTS` at registration when the key is explicitly true and `metadata_type == ContentAddressed` (fail-close, consistent with how missing `server_root_id` is handled).

5. **`system.content_addressed_mounts` stamps the querying server's own GC health onto every peer's row** ✔
   Risk score: 68 · Sources: ux-contract + operability (independent)
   Files/lines: `StorageSystemContentAddressedMounts.cpp:144-163`
   Evidence: `ca->gcHealth()` is computed once per disk and `is_leader`/`pending_reclaim`/`last_success_age_seconds`/`wedged_namespace_count` are inserted identically for every mount row, including peer `srid`s. On the table explicitly built for incident triage, an operator will read "server B is GC leader" when the value describes server A. The GC round log also lacks an `srid` column, so there is no other durable round→mount mapping.
   Proposed fix: populate those four columns only on the local-`srid` row (NULL elsewhere), or split them into a per-disk `system.content_addressed_gc_health`.

6. **Transient backend failure during table/disk startup permanently strands the table (no retry, no non-restart recovery)**
   Risk score: 70 · Sources: operability (corroborated by this branch's own RCA commits `98bb1c0b7a6`…`3b9325f8029`)
   Files/lines: `ContentAddressedMetadataStorage.cpp:415-531` (`startup` → `Cas::Pool::open`), `CasPool.cpp:215-260` (probe unguarded by retry)
   Evidence/impact: one S3 blip during `AsyncLoader`-driven load caches the table as permanently `FAILED`; `DETACH`/`ATTACH` confirmed not to re-trigger the load; only a full server restart recovers. The branch has already recorded the proposed fix (bounded retry) but it is not landed in `src/`.
   Proposed fix: land the bounded-retry-with-backoff around the transient-classified failures of `Pool::open`/probe.

7. **`DiskObjectStorage::prepareRead` downcasts to the concrete `ContentAddressedMetadataStorage`, breaking the decorator seam the subsystem itself defines**
   Risk score: 60 · Sources: architecture
   Files/lines: `DiskObjectStorage.cpp:824-833`; contrast `ContentAddressedExchange.h` (the purpose-built narrow interface `DataPartsExchange.cpp` correctly casts to); fallout documented in `DiskObjectStorageCache.cpp:19-31` (generic cache wrapper must be bypassed for CA disks).
   Proposed fix: route `prepareInManifestRead`/`getBlobViewPlan` through `IContentAddressedExchange` (or a no-op-default virtual on `IMetadataStorage`), so generic decorators can forward.
   Related nit: two dead CAS includes in `DiskObjectStorageTransaction.cpp:3-4` left over from the pre-virtual-dispatch approach.

8. **Part-removal path pays one mandatory S3 HEAD per file under default `part_folder_validate=always`** ✔
   Risk score: 62 · Sources: performance
   Files/lines: `ContentAddressedTransaction.cpp:1387-1411` (`unlinkFile` → `getView(ForceFresh)`), `PartFolderAccess.cpp:190` (ForceFresh retained-view shortcut disabled when mode is `Always` — the default), `CasManifestReader.cpp:57-59` ("HEAD is mandatory even on a cache hit")
   Impact: every merge/mutation/TTL-move removal of a wide part issues hundreds of sequential HEADs before collapsing into one ref-drop — the same request-storm class as the historical B118 stall.
   Proposed fix: memoize "ForceFresh-validated this transaction" per (txn, ref) so a burst of `unlinkFile` calls pays the HEAD once.

9. **`RefResolve` audit event fires on every CA file access, even warm view-cache hits** ✔
   Risk score: 55 · Sources: performance
   Files/lines: `PartFolderAccess.cpp:156-166` (`getView` resolves before the cache lookup), `CasRefLedger.cpp:149-160`
   Impact: with `system.content_addressed_log` enabled by default, every `existsFile`/`getFileSize`/`readFile` allocates and enqueues a full `CasEvent` — cost scales with read traffic, not mutation rate; it is the only read-hot-path event emitter in the codebase.
   Proposed fix: emit `RefResolve` only when the resolve does real freshness work (miss / `ForceFresh` / `StrictValidate`).

10. **`RefCowMap::find` builds a non-mergeable iterator for overlay-only keys — forward iteration silently drops later base entries** ✔ (latent)
    Risk score: 55 · Sources: correctness
    Files/lines: `CasRefCowMap.cpp:79` — overlay branch uses `base->find(key)` where the sibling base branch deliberately uses `lower_bound` ("keeps the iterator mergeable"). With base={A,D}, overlay-only B: `find("B")` then `++` hits a state bitwise equal to `end()`, skipping D. Not reachable through current callers (all point-dereference), but `emplace`/`insert_or_assign` return the broken iterator from a primitive documented as a drop-in `std::map` replacement.
    Proposed fix: `it.base_it = base->lower_bound(key);` (strict improvement; identical when the key is shadowed) + a unit test iterating past an overlay-only key.

11. **Non-CA-gated behavior changes to shared paths need their own sign-off when upstreaming**
    Risk score: 55 · Sources: compatibility
    - `MergeTreeData::Transaction::renameParts` now unconditionally commits every part's disk-storage transaction before the Keeper commit decision (the R3 acked-then-lost fix) — deliberate and correct-looking, but it reorders durability for *all* `DiskObjectStorage`-backed MergeTree writes; needs dedicated non-CA regression coverage (plain S3 + `ReplicatedMergeTree`, zero-copy) and should be presented as a standalone change upstream.
    - `Client::RetryStrategy` now never retries HTTP 412 globally (`Client.cpp:104-111`); the message-substring fallback in `isPreconditionFailedError` can over-match on S3-compatible backends. Also affects pre-existing Iceberg conditional writes (probably beneficially).
    - `ReadBufferFromFileView` position fix (B115) also changes the pre-existing packed skip-index reader; confirm that suite ran.

## Minor issues / improvements

- `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION`/`GC REBUILD` return nothing to the client while sibling `DROP POOL MEMBER` returns a 10-column result row; `RebuildReport` goes only to `LOG_INFO`. Reuse the same result-set pattern. (ux-contract, 45)
- No Prometheus-scrapeable signal for "GC stuck" / "mount lease lost" — only system tables/logs; add an `AsynchronousMetrics` sample of `gcHealth()` per CA disk. (operability, 42)
- `CasGc.cpp` (and `CasBlobInDegree.cpp`) log via fixed `getLogger("CasGc")` with no disk/srid scoping — indistinguishable on multi-CA-disk nodes; `CasPool`/`CasServerRoot` already do this right. (docs, 45)
- `SingleWriterSlot::renewOnce` holds `state_mutex` across the heartbeat S3 PUT, against the file's own documented discipline (`CasServerRoot.cpp:942-969`); currently benign, a landmine for any future locked accessor. (concurrency, 35)
- `CasGcOutcomesFormat::decodeOutcomeLog` skips the digest-width pre-check, yielding `BAD_ARGUMENTS` instead of `CORRUPTED_DATA` on malformed input — the exact contract violation `CasPartManifestFormat.cpp:225-234` documents and guards against. (correctness, 32)
- `PartPathParser`'s 3-char-prefix Atomic-UUID anchor heuristic can misclassify legacy `Ordinary` layouts (`data/abc/abcxyz/...`); require hex-shaped prefix / well-formed UUID. (correctness, 38)
- `listRefs()` used for pure emptiness checks materializes the full ref map under the table lock (`existsDirectory` on `TableDir`); add `hasAnyRef`/`hasAnyRefWithPrefix`. (performance, 35)
- `promote()` GETs back and re-decodes the manifest it just staged on every part commit; pass the locally-built `PartManifest` through for the same-build case, keep the GET for adoption. (performance, 40)
- `CasOrphanManifestSweep` recomputes each namespace's full ref-log protection view per page/round; cache per sweep pass. (performance, 45)
- Interpreter lambda `content_addressed_storage_of` copy-pasted three times in `InterpreterSystemQuery.cpp`; factor out. (ockham, 30)
- Permanent source comments in `MergeTreeData.{h,cpp}` (and ~7 more files) cite dated `docs/superpowers/` worklog paths; keep the inline prose, drop or stabilize the citations before upstreaming. (ockham, 25)
- `CasLayout.h` keeps two ~50-line key parsers inline against its own out-of-line convention (26 direct includers). (headers, 30)
- `head_first` dedup gate triggers on size alone (≥1 MiB default), doubling S3 requests for genuinely-new blobs in cold bulk loads; consider adaptive gating. (performance, 30)
- Interserver-supplied `ManifestEntry.path` lacks `..`/empty-segment shape validation at decode — not exploitable today (used only as an in-memory key matched against locally-derived names), add the `checkNamespace`-style check as defense-in-depth. (security, 15)
- `promote()` doesn't set `alive = false` (asymmetric with `abandon()`); GC generation prune uses the post-fold generation as its retention floor (one-generation slack if the round's CAS loses). (deep-audit nits)

### Docs debt (pre-release, but track)

- `content_addressed_garbage_collection_log.md` lists a nonexistent column (`children_cascaded`) and omits six real ones including `anomalies`/`fence_outs`.
- `GC REBUILD`, `DROP POOL MEMBER`, `content_addressed_mounts`, `content_addressed_log`, the `content_addressed` metadata type, and all ~23 config keys have no user docs.
- The 129 `Cas*` ProfileEvents descriptions embed internal review citations ("closes artifact #3", "Round-B §2", "rev.6 §…") that must be stripped before shipping — they're permanent `system.events` surface.

## Needs verification

- Whether an exception after a successful relink `adoptPartFromManifest` (post-adopt local build/load failure in `Fetcher::relinkPartToDisk`) leaves an orphaned `tmp-fetch_` ref, or whether MergeTree's startup temp-dir cleanup reaches it via `removeDirectory` → `dropRefIfPresent`. (deep-audit, 42)
- Whether a deferred `~Pool()` (mount-lease farewell PUT) can run after `object_storage->shutdown()` when an in-flight transaction holds the last `PoolPtr` — depends on whether the framework drains disk transactions before `IDisk::shutdown`. (concurrency, low confidence)
- Whether `system.content_addressed_log`/`_garbage_collection_log` enabled-by-default is truly zero-overhead on servers with no CA disk (the code claims it; not traced end-to-end).
- Real per-query multiplier for finding 9 (calls to `getView` per column per query) — determines its true severity.
- `.claude/tools/cppexpr.sh` appears broken in this environment (`-Werror,-Wmissing-prototypes` even on the CLAUDE.md example) — the `RefCowMap` repro was hand-traced (and aggregator-verified against source) rather than executed.

## Suggested commit / diff split

For eventual upstreaming, independently reviewable pieces:
1. GCS conditional-write support (`GCSConditionalDialect`, `GOOG4Signer`, `PocoHTTPClient` HMAC) — self-contained, tested, consumable standalone.
2. `renameParts` disk-transaction-close durability fix — its own PR with non-CA regression tests.
3. `ReadBufferFromFileView` B115 position fix + gtest battery.
4. S3 412-no-retry policy + `isPreconditionFailedError`.
5. Core CAS backend (the rest).

## Tests to add or strengthen

- Access-control negative tests for `DROP POOL MEMBER` (and plain-`GARBAGE COLLECTION` denial, currently only the allow-path is proven).
- Mock-S3 unit tests for `removeObjectIfTokenMatches`/`copyObjectConditional` error mapping (412→TokenMismatch, 404→NotFound) — currently only `LocalObjectStorage` fakes + RustFS integration cover the CAS concurrency primitive.
- `copyS3File` conditional-copy tests: losing conditional copy surfaces as precondition-failed and never falls back to unconditional copy (the mock client has no `CopyObject` handler at all today).
- `RefCowMap` iteration-past-overlay-only-key unit test (finding 10).
- `DROP POOL MEMBER` from a `<readonly>` host disk, and a drain longer than the lease TTL (finding 3).
- `MergeTreeDeduplicationLog` B37 regression test for a disk that genuinely hits the new null-writer exception.
- `abandon()`-throws-mid-unwind fault injection for `publishEntries`/`publishStaging` (finding 2).

## Coverage summary

- Entry points reviewed: disk registration/config, part write/read/remove, merge/mutate/fetch (relink + byte), GC round + rebuild + orphan sweep, mount/lease lifecycle, decommission, all three SYSTEM verbs, three system tables, S3/IO shared-path hunks.
- Deep audit fully traced 4 state machines (part-write commit, GC round, mount/lease, fetch-by-relink) through success/failure/cancel/crash/concurrency paths; GC round and part-commit came back defect-free apart from nits.
- Verified clean: in-process locking across the whole CAS tree (one minor), header hygiene of high-fan-out headers, SQL access-check wiring, `srid` key-injection defense, mixed-version relink gating, non-CA hot-path cost of shared hunks.
- Not covered: `src/Disks/tests/**` (excluded by spec; ~1059 tests exist there), `CasGcShardPlan.cpp` reducer internals, `rebuildBaseline` tail, GOOG4 signature vectors, runtime fault injection.
- Main assumption: the extensive inline invariant documentation accurately states intent; code was checked against it.

## Final verdict

Status: **request changes**
Minimum required actions before this ships (per the branch's own no-known-reds policy):
1. Wire the relink retention pin or fail relink closed until it exists (finding 1).
2. Fix the `abandon()` failure-handling cluster: guarded emits, abandon-on-exception in `publishEntries`, `alive=false` ordering (finding 2).
3. Close the `DROP POOL MEMBER` read-only bypass + force `background_watermark` in `openForDecommission` + add its access test (finding 3).
4. Reject explicit `use_fake_transaction=true` on CAS disks (finding 4).
5. Scope the GC-health columns in `system.content_addressed_mounts` to the local row (finding 5).
6. Land the bounded startup retry already designed on this branch (finding 6).
7. `RefCowMap::find` → `lower_bound` one-liner + test (finding 10).

Everything else (perf items 8–9, architecture seam 7, docs/ProfileEvents cleanup, upstream diff split) is important but can be sequenced after these.
