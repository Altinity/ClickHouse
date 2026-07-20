# Consistency-review findings F1-F11 — fix design

Status: user-approved design (2026-07-21). Source: codebase-consistency review of
`git diff $(git merge-base altinity/antalya-26.6 HEAD)..HEAD -- src` run 2026-07-20
(8 parallel precedent-lookup investigations; findings F1-F12, of which F12 — the absent
relink kill-switch setting — is explicitly out of scope here). All work lands on the
`cas-gc-rebuild` branch, one commit per finding cluster, no rebase/amend.

## Background {#background}

The review compared the CAS branch against host-codebase conventions (not correctness).
Verdict: reuse discipline is good; the actionable residue is five user-facing naming
inconsistencies (cheap now, expensive after release), three mechanism divergences, and
three polish items. This spec fixes F1-F11. Severity/evidence for each finding is
restated inline so this document is self-contained.

## Decisions taken during brainstorming {#decisions}

- F1 verb: `GC RUN` (verb-last, pairs with `GC REBUILD`; `START` would collide with the
  START/STOP=enable/disable SYSTEM convention, `COLLECT` is tautological).
- F5: opaque retry-profile token in `WriteSettings`; `S3ObjectStorage` owns the clone.
- F7: hybrid description style ("Number of X. Interpretive sentence.").
- F8: ship the standard log-config key set; `<ttl>` becomes a commented-out example.
- F4: full `BaseSettings`-macro settings struct (unknown-key rejection is the payoff).
- F6: thread names only; `BackgroundSchedulePool` migration deferred (see Non-goals).
- Packaging: stay on `cas-gc-rebuild`.

## Cluster A — user-facing renames {#cluster-a}

### F1 — SYSTEM command family spells "garbage collection" two ways {#f1-system-gc-run}

Finding: sibling commands `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION` and
`SYSTEM CONTENT ADDRESSED GC REBUILD` spell the same concept differently; no existing
SYSTEM family does that.

Fix: rename the enum entry `CONTENT_ADDRESSED_GARBAGE_COLLECTION` →
`CONTENT_ADDRESSED_GC_RUN` (`src/Parsers/ASTSystemQuery.h`). Rendered SQL becomes:

- `SYSTEM CONTENT ADDRESSED GC RUN [<disk>] [ON CLUSTER ...]` — optional disk, all
  CA disks when omitted (unchanged semantics).
- `SYSTEM CONTENT ADDRESSED GC REBUILD [FORCE] <disk> [ON CLUSTER ...]` — unchanged.
- `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER ...` — unchanged.

AccessType: `SYSTEM_CONTENT_ADDRESSED_GARBAGE_COLLECTION` →
`SYSTEM_CONTENT_ADDRESSED_GC_RUN` with grant string
`"SYSTEM CONTENT ADDRESSED GC RUN"` (`src/Access/Common/AccessType.h`).
`InterpreterSystemQuery`: case label + method rename
`runContentAddressedGarbageCollection` → `runContentAddressedGcRun`.

Sweep (full-tree grep for both the SQL phrase and the enum/AccessType identifiers):
parser comments, `src/Parsers/tests/gtest_Parser.cpp`, stateless/integration tests,
`utils/ca-soak/` scenarios and runbooks, the numbered CAS doc set, and
`tests/queries/0_stateless/01271_show_privileges.reference` (grant-name table) plus any
other `.reference` files carrying the old grant string.

### F2 — `CasGC*` vs `CasGc*` casing split {#f2-casgc-casing}

Finding: async metrics use `CasGC` (`ServerAsynchronousMetrics.cpp`) while ~20
ProfileEvents for the same subsystem use `CasGc`; the mounts-table doc string
references the lowercase spelling of a metric that exists only in uppercase form.

Fix: rename the four per-disk asynchronous metrics to the `CasGc` form (majority form
in the family): `CasGcIsLeader_{disk}`, `CasGcPendingReclaim_{disk}`,
`CasGcLastSuccessAgeSeconds_{disk}`, `CasGcWedgedNamespaces_{disk}`. Sweep in-repo
tests/docs/dashboards for the old spellings. The stale cross-reference in the mounts
table is handled by F3's description rewrite.

### F3 — `system.content_addressed_mounts` abbreviated columns {#f3-mounts-columns}

Finding: columns `srid`, `pid`, `seq`, `min_active` diverge from the spelled-out
convention of sibling system tables; descriptions mix two-word fragments with
multi-sentence essays.

Fix (`src/Storages/System/StorageSystemContentAddressedMounts.cpp`):

| old | new |
|---|---|
| `srid` | `server_root_id` (matches the config key) |
| `pid` | `process_id` |
| `seq` | `renewal_sequence` |
| `min_active` | `min_active_build_sequence` |

`started_at_ms` / `expires_at_ms` stay (not flagged; unit suffix is deliberate).
All descriptions normalized to 1-2 factual sentences. `is_leader` becomes: "1 if this
server's GC scheduler holds this disk's leadership lease. NULL on rows describing
other servers' mounts." — the retired-`CasGcIsLeader` history note moves to the header
comment or is dropped. The `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER` result-set
column `srid` (`InterpreterSystemQuery.cpp`) also renames to `server_root_id`.
Sweep tests/scenarios selecting the old column names.

## Cluster B — mechanism refactors {#cluster-b}

### F4 — `ContentAddressedSettings` struct {#f4-settings-struct}

Finding: the `content_addressed` factory registration hand-parses ~25 config keys
inline and feeds a ~25-positional-argument constructor; every default is duplicated
between the factory lambda and the constructor header. Codebase precedent at this key
count is the declarative `FileCacheSettings` pattern. A typo'd config key today is
silently ignored.

Fix: new `ContentAddressedSettings.{h,cpp}` under
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`, modeled on
`FileCacheSettings`:

- One macro table declaring every key: name, type, default, one-line description.
  This table becomes the single source of defaults.
- Generic `loadFromConfig(config, config_prefix)` iterating `config.keys()` with
  unknown-key rejection (`BAD_ARGUMENTS`), with a skip-list for non-setting subkeys
  present in the same disk block (`metadata_type`, `type`, `endpoint`, object-storage
  keys, etc. — enumerate during implementation from real configs, including the
  ca-soak and integration-test configs).
- `validate()` absorbing today's scattered checks: `gc_interval_sec >= 1`,
  `gc_shards >= 1`, `server_root_id` required (typed `NO_ELEMENTS_IN_CONFIG`) +
  macro expansion + `Cas::validateServerRootId`.
- Enum-valued knobs (`blob_hash`, `staging_backend`, `part_folder_validate`) are
  string settings in the table; `validate()` parses them once into typed accessors
  (fail-closed on unknown spellings, exactly as today).
- Macro expansion of `server_root_id` needs the global context; `validate(context)`
  takes it explicitly rather than reaching for a global.

`ContentAddressedMetadataStorage`'s constructor collapses to: object storage pointer,
key-compatibility prefix, server UUID, disk name, global context, and
`const ContentAddressedSettings &`. Header-side parameter defaults are deleted.
The settings object populates `Cas::PoolConfig` in one place. The factory lambda
shrinks to: build settings, load, validate, construct.

The existing per-key rationale comments in `MetadataStorageFactory.cpp` (design-doc
references, fail-closed notes) migrate to the macro table / settings header — they
must not be lost.

### F5 — `WriteSettings::s3_client_override` layering {#f5-retry-profile}

Finding: `WriteSettings` carries `std::shared_ptr<const S3::Client>` under
`#if USE_AWS_S3` — the first concrete backend client type and first preprocessor guard
in `WriteSettings`/`ReadSettings`. Precedent (`BackupIO_S3`) does client cloning at
client-construction level.

Fix: replace the pointer with a backend-neutral token in `WriteSettings`:

```cpp
enum class ObjectStorageRetryProfile : uint8_t { Default, SingleAttempt };
ObjectStorageRetryProfile object_storage_retry_profile = ObjectStorageRetryProfile::Default;
```

`S3ObjectStorage` owns the single-attempt clone: lazily built from the disk's current
client via `cloneWithConfigurationOverride` with `SingleAttemptRetryStrategy` and the
`expect_continue_min_bytes` floor (policy moves from `CasObjectStorageBackend.cpp`
into the S3 layer), guarded by the same mutex discipline as the main client, and
invalidated/rebuilt whenever the disk client rotates (`applyNewSettings` /
credentials refresh) — this also closes the latent staleness hole where the CAS-cached
clone could outlive a rotated client. `writeObject` selects the clone when the profile
is `SingleAttempt`. The CAS backend drops its cached clone and sets the profile on its
conditional writes. The `CasConditionalWriteSdkRetries` "must stay zero" tripwire and
the `s3_max_unexpected_write_error_retries_override = 1` companion knob keep their
semantics unchanged. `#if USE_AWS_S3`, the `S3::Client` forward declaration, and the
`<memory>` include leave `WriteSettings.h`. `gtest_cas_request_control.cpp`'s
override-mechanism test is reworked to assert profile selection.

Non-S3 backends ignore the profile (documented in the field comment); throwing on an
unsupported backend is not required because only CAS sets it today.

### F6 — background worker thread names {#f6-thread-names}

Finding: no CAS background thread calls `setThreadName` (convention appears in 144
files); the workers are invisible by name in `ps`, `system.stack_trace`, flamegraphs.

Fix: `setThreadName` at the top of each loop body (15-char limit):

| thread | name |
|---|---|
| `CasGcScheduler::loop` | `CasGcSched` |
| `CasGcScheduler::heartbeatLoop` | `CasGcHeartbeat` |
| `CasMountRuntime` remount thread | `CasRemount` |
| `MountLeaseKeeper::backgroundLoop` (`CasServerRoot.cpp`) | `CasLeaseKeeper` |

Also audit the two other raw `ThreadFromGlobalPool` spawn sites the review located
(`CasRefLedger.cpp` async publish, `CasPool.cpp`) and name them if they are loops
rather than one-shot tasks (one-shot tasks inherit an acceptable name).

## Cluster C — polish {#cluster-c}

### F7 — ProfileEvents description style {#f7-event-descriptions}

Finding: the 128 `Cas*` events use "Counts ...; growing values indicate ..." — the
trailing clause exists nowhere else in `ProfileEvents.cpp`; the dominant lead is
"Number of ...".

Fix: rewrite all 128 descriptions to the hybrid form — lead clause
"Number of <what>." followed by the interpretive tail as a separate sentence, e.g.
`"Number of CAS blob PUT requests. Growing values indicate storage write traffic."`
No event renames. Purely textual; ideal codex-delegation material with a spot-check
review pass.

### F8 — `content_addressed_log` default config section {#f8-log-config}

Finding: the section ships only `database/table/flush_interval` + a live `<ttl>`;
its own sibling (`content_addressed_garbage_collection_log`) and every other default
log carry the full standard key set, and convention ships `<ttl>` commented out.

Fix (`programs/server/config.xml`): add `partition_by`, `max_size_rows`,
`reserved_size_rows`, `buffer_size_rows_flush_threshold`, `flush_on_crash` with the
same values as the GC-log sibling; convert `<ttl>` to a commented-out example
(mirroring `query_log`); shrink the three-line marketing comment to the sibling's
factual one-liner style. Check `programs/server/config.yaml.example` and the ca-soak /
integration-test configs for copies of the section and align them.

### F9 — `s3_skip_check_objects_after_upload` polarity {#f9-upload-check-override}

Finding: the per-write knob inverts the polarity of the setting it overrides
(`s3_check_objects_after_upload`), unlike the two adjacent `*_override` fields.

Fix: replace with `std::optional<bool> s3_check_objects_after_upload_override`
(symmetric with `s3_max_unexpected_write_error_retries_override`); fold-in at
`S3ObjectStorage.cpp` becomes
`if (o) request_settings[check_objects_after_upload] = *o;`. CAS producer sets
`= false`. The rationale comment (CAS-mutable keys legitimately replaced between
upload and HEAD) moves with the field.

### F10 — shared S3 error-name predicates {#f10-error-predicates}

Finding: `CasRequestControl.cpp` maintains its own error-name whitelists
(`AccessDenied`, `InvalidAccessKeyId`, `SignatureDoesNotMatch`, malformed-request
family) overlapping the unretryable set in `S3Exception::isRetryableError` — two
hand-maintained lists that will drift.

Fix: move the three predicates `isMalformedRequestError`, `isEntityTooLargeError`,
`isAccessDeniedError` (each taking the error name string + modeled
`Aws::S3::S3Errors` value) from `CasRequestControl.cpp` into `S3Common.{h,cpp}`
beside `isRetryableError` / `isPreconditionFailedError`, with a comment tying the
name lists together. `CasRequestControl` keeps only the CAS-specific tri-state
mapping (`Committed/DefiniteFailure/Unresolved`). `isRetryableError` itself is NOT
modified in this pass — different contract, zero behavior change allowed here.

### F11 — unwired cache CurrentMetrics {#f11-cache-metrics}

Finding: the manifest-decode cache (`CasManifestReader.cpp`) and the dedup presence
cache (`CasPool.cpp`) pass `CurrentMetrics::end()` to `CacheBase`; every established
`CacheBase` user registers a `*Bytes`/`*Entries` pair.

Fix: declare `CasManifestDecodeCacheBytes`/`CasManifestDecodeCacheEntries` and
`CasDedupCacheBytes`/`CasDedupCacheEntries` in `CurrentMetrics.cpp` (descriptions in
the same style as the existing `CasPartFolderCache*` pair) and pass them in the two
constructors. No behavior change.

## Non-goals {#non-goals}

- **F12 (relink kill-switch setting)** — flagged for a conscious product decision,
  not part of this fix pass.
- **`BackgroundSchedulePool` migration of CAS background loops** — considered and
  deferred. The raw-thread choice is documented in `CasGcScheduler.h` (attached
  `ThreadStatus` for per-round `ProfileEventsScope`; Disks-layer construction without
  `Context`), and migrating soak-hardened lease/fence timing machinery is a
  behavioral risk with purely stylistic payoff. Revisit only if a real scheduling
  problem appears.
- **Rewriting `S3Exception::isRetryableError` semantics** (see F10).
- **Renaming `started_at_ms`/`expires_at_ms`** (see F3).
- No compatibility aliases for any renamed surface: the feature is pre-release with
  no persisted deployments, so old spellings are removed outright (per the standing
  no-compat-scaffolding rule).

## Testing {#testing}

- F1: `gtest_Parser.cpp` cases updated; `01271_show_privileges.reference` (and any
  other grant-table references) regenerated; full-tree grep proves zero occurrences
  of the old SQL phrase / identifiers outside historical worklogs and reports
  (historical docs under `docs/superpowers/reports|worklogs` are NOT rewritten).
- F3: any test selecting renamed columns updated; grep proves zero live references
  to `srid|pid|seq|min_active` in the mounts-table context.
- F4: new gtest — unknown key rejected, defaults land, `validate()` failures
  (`gc_shards=0`, missing `server_root_id`, unknown `blob_hash`) produce the same
  typed exceptions as today.
- F5: `gtest_cas_request_control.cpp` reworked to the profile token; a case
  asserting the clone is rebuilt after `applyNewSettings` (client rotation).
- F7/F8/F11: compile + existing gates; no dedicated tests.
- Whole pass: full `Cas*:CA*` gtest gate, the CA stateless/integration lanes, and a
  phase-1 ca-soak smoke (`--ops` mode) as the final regression gate.

## Execution notes {#execution-notes}

One commit per finding cluster (A: F1+F2+F3, B: F4, then F5, then F6, C: F7-F11 —
F4 and F5 are large enough to stand alone). Mechanical sweeps (F1 grep-rename, F7
description rewrite) are codex-delegation candidates per the standing policy;
F4 and F5 stay with the controller.
