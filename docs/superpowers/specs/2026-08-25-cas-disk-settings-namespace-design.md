---
description: 'Design for letting a CAS disk carry any underlying object-storage setting, by giving the CAS settings their own config-key namespace instead of an enumerated skip-list'
sidebar_label: 'CAS disk settings namespace'
sidebar_position: 7
slug: /superpowers/specs/cas-disk-settings-namespace-design
title: 'CAS disk settings: which keys the CAS layer owns'
doc_type: 'design'
---

# CAS disk settings: which keys the CAS layer owns {#cas-disk-settings-namespace-design}

**Status:** DRAFT for review, rev.1 (2026-08-25).

This specification covers item 4 of `docs/superpowers/cas/final-checks-todo.md`
(`{#fix-s3-key-whitelist}`) and the BACKLOG record `{#cas-disk-s3-key-whitelist-gap}`. The field
report behind it: adding `<http_keep_alive_timeout>60</http_keep_alive_timeout>` to a CAS disk block
— the mitigation suggested in Altinity/ClickHouse#2243 — kills the server at startup with
`Unknown setting 'http_keep_alive_timeout' (UNKNOWN_SETTING)`
(https://github.com/Altinity/clickhouse-regression/actions/runs/32408309167/job/96552561919).

**Statements about the current code were checked against `cas-gc-rebuild` at `e51affc6206`.**
Functions are named rather than cited by line: line numbers in this subtree move weekly, and a stale
line number reads as authority it has not earned.

## The defect {#defect}

A disk block is a **shared namespace**. Its keys are read by several independent consumers: the
object-storage factory, the S3 auth/client settings, the S3 request settings, the Azure settings,
the proxy resolver, `IDisk`, `DiskFromAST`, the fake-transaction gate — and the metadata storage.

Every one of those consumers *pulls*: it walks its own list of fields and reads
`config_prefix + "." + name`, ignoring whatever else is in the block. `S3AuthSettings`'s
config constructor iterates `impl->allMutable()`; `S3RequestSettings`'s does the same with a
`setting_name_prefix` of `s3_`; `MetadataStorageFactory` reads its handful of keys by name.

`ContentAddressedSettings::loadFromConfig` is the only consumer that *scans*. It enumerates every
key in the block and feeds each one to `BaseSettings::set` unless the key appears in a hand-written
`non_cas_keys` set of 22 string literals. `set` throws `UNKNOWN_SETTING` for anything it does not
recognise, so **any legal disk key nobody thought to enumerate takes the server down at startup.**

The skip-list carries a 31-line comment describing the four-source scan of in-repo configs that
produced it, and a standing instruction to repeat that scan whenever a new config family is added.
That instruction is the defect in prose form: correctness of a CAS disk depends on someone
re-enumerating a namespace CAS does not own.

What this costs today, on a CAS disk:

- every `s3_*` request setting — `s3_retry_attempts`, `s3_max_put_rps`, `s3_max_single_read_retries`,
  `s3_storage_class_name`, … (on the disk path request settings are read *only* under the `s3_`
  prefix, and not one of them is in the skip-list);
- most S3 client/auth keys: `connect_timeout_ms`, `request_timeout_ms`, `max_connections`,
  `session_token`, `role_arn`, `no_sign_request`, `disable_checksum`, `http_keep_alive_timeout`,
  `http_keep_alive_max_requests`, …;
- custom HTTP headers (`header`, `access_header`), per-user scoping (`user*`), the `<proxy>`
  subtree, and the `<server_side_encryption_kms_config>` subtree;
- **Azure in its entirety** — `account_name`, `account_key`, `connection_string`, `container_name`,
  `use_workload_identity`, `endpoint_contains_account_name` are read ad-hoc and unprefixed by
  `AzureBlobStorageCommon`, and none of them is in the skip-list.

## Why the enumerated skip-list cannot be repaired {#unenumerable}

The BACKLOG's recorded fix shape was: skip any key whose name is a builtin `S3AuthSettings` or
`S3RequestSettings` name, using `BaseSettings::hasBuiltin`, and keep `non_cas_keys` only for the
ad-hoc generic-layer keys. That closes the reported case and leaves the class open, because **the
foreign key space is not a finite set of names**:

- `getHTTPHeaders` matches any key that *starts with* `header` / `access_header`; repeated XML
  elements reach Poco as `header`, `header[1]`, `header[2]`. `hasBuiltin("header[1]")` is false.
- `S3AuthSettings`'s constructor collects any key starting with `user`. The suffix is arbitrary.
- `<proxy>` and `<server_side_encryption_kms_config>` are subtrees, not keys.
- `endpoint`, `path`, `type`, `name`, `object_storage_type`, `metadata_type`, `use_fake_transaction`,
  `key_compatibility_prefix` live in no settings object at all — the generic disk layer has no
  registry to enumerate.
- The *spelling convention* differs per backend: the same upload-size concept is `s3_`-prefixed on
  an S3 disk and unprefixed on an Azure one. A CAS-side enumeration would have to track every
  backend's naming convention, not merely its names — including backends that do not exist yet.

There is direct evidence in the current skip-list that this is already going wrong:
`max_single_part_upload_size` is listed, but on the S3 disk path that key is inert (request settings
need the `s3_` prefix there); it is the correct spelling for Azure, and it entered the list from an
Azure-shaped config.

## Part 1 — CAS owns the `cas_` namespace {#part-1-prefix}

Two properties are wanted, and only one arrangement delivers both:

1. **Any underlying setting must work**, for every backend, now and for backends added later. This
   requires that CAS never inspect a key it does not own.
2. **A mis-spelled CAS setting must fail closed.** You can only fail closed over a namespace you
   own; CAS does not own the disk block.

Therefore the CAS settings get their own config-key prefix, `cas_`, and CAS reads only keys that
carry it. The disk block already contains a precedent for exactly this disambiguation: S3 request
settings are read there under the `s3_` prefix.

### The loader {#loader}

`ContentAddressedSettings::loadFromConfig` keeps scanning the block, but consumes only its own
namespace:

```cpp
static constexpr std::string_view CAS_KEY_PREFIX = "cas_";

for (const std::string & key : config_keys)
{
    if (!key.starts_with(CAS_KEY_PREFIX))
    {
        /// A bare CAS setting name in the shared disk block is a mis-spelling of the prefixed
        /// form, never a foreign key: reject it instead of ignoring it.
        if (ContentAddressedSettingsImpl::hasBuiltin(key))
            throw Exception(ErrorCodes::UNKNOWN_SETTING,
                "content_addressed disk: `{}` must be spelled `cas_{}`", key, key);
        continue;
    }
    impl->set(key.substr(CAS_KEY_PREFIX.size()), config.getString(config_prefix + "." + key));
}
```

`non_cas_keys` and its 31-line comment are deleted outright. Nothing replaces them: after this
change there is no list of foreign keys anywhere in the CAS subtree, because CAS no longer has an
opinion about foreign keys.

Consequences worth stating:

- Typo detection is preserved and becomes exact: `cas_gc_shardz` is unknown *within the `cas_`
  namespace*, so `BaseSettings::set` throws `UNKNOWN_SETTING` as it does today, hints included.
- `.changed` keeps meaning "the config carried this key", because `set` is still called only for
  keys that were present. The `NO_ELEMENTS_IN_CONFIG`-vs-`BAD_ARGUMENTS` distinction for
  `server_root_id` in `validate` is unaffected.
- A duplicated element reaches Poco as `cas_gc_enabled` and `cas_gc_enabled[1]`; the second strips
  to `gc_enabled[1]` and is rejected. A duplicate key being a loud error is the wanted behaviour.
- The C++ side is untouched: `ContentAddressedSetting::gc_enabled`, the `DECLARE` list, and the
  per-TU `extern` declarations all keep their present names. The prefix exists only in the config
  spelling, exactly as `s3_` does for `S3RequestSettings`.

### The bare-name guard {#bare-name-guard}

Without the guard, an operator who writes the pre-rename `<gc_enabled>false</gc_enabled>` gets a
*silently ignored* key and a GC that keeps running. The guard converts that into a startup error
naming the correct spelling.

The guard is a permanent rule, not migration scaffolding: a bare CAS setting name inside a CAS disk
block is never a legitimate foreign key. The CAS names are distinctive enough
(`gc_round_redelete_budget`, `manifest_sweep_list_budget_keys`) that a future backend colliding with
one is not a practical risk — and the two plausibly generic names, `skip_access_check` and
`gcs_max_conditional_put_bytes`, both leave the CAS settings list in Parts 2 and 3, so the guard has
no exceptions to carve out.

### What the guard buys {#guard-value}

CAS carries settings whose silent loss is permanent rather than merely wrong:

- `blob_hash` is fixed at pool creation. A silently ignored value means a pool built with
  `cityhash128` when `sha256` was intended, and no way to change it afterwards.
- `gc_shards` is creation-time-only; on reopen the pool's persisted GC state is authoritative.
- `server_root_id` is required, so a mis-spelling of *that* key already self-detects.

Three existing regression tests pin the loud-rejection posture — `RemovedCacheSettingsAreRejected`,
`LegacyTokenProducingPutCapNameRejected`, `UnknownKeyRejected` in `gtest_cas_settings.cpp`. This is
also why the change ships as one commit rather than "stop scanning now, add the prefix later": the
intermediate state would require deleting those three tests and restoring them afterwards.

## Part 2 — `skip_access_check` leaves the settings list {#part-2-skip-access-check}

`skip_access_check` is deliberately shared: `registerDiskObjectStorage` reads it from the same disk
block and ORs it with the server-level `global_skip_access_check`, and CAS reads it to decide whether
to run its boot-time capability probe. One key, two consumers, one meaning.

Renaming it to `cas_skip_access_check` would split that into two keys with two meanings, so it keeps
its bare spelling. To keep the "every CAS config key starts with `cas_`" rule free of exceptions, it
stops being a `ContentAddressedSettings` entry: it is removed from
`LIST_OF_CONTENT_ADDRESSED_SETTINGS` and becomes a plain member of `ContentAddressedSettingsImpl`
alongside `blob_hash_algo_cached`, populated in `loadFromConfig` by reading the unprefixed key
directly, and exposed through an accessor in the same shape as `blobHashAlgo`.

`ContentAddressedMetadataStorage` keeps its `skip_access_check` member and its use in `PoolConfig`;
only the source of the value changes. The `extern const ContentAddressedSettingsBool
skip_access_check;` declaration in that TU goes away with the settings entry.

After this change `cas_skip_access_check` is rejected as an unknown CAS setting, which is correct:
it is not one.

## Part 3 — the GCS conditional-PUT cap leaves CAS {#part-3-gcs-cap}

`gcs_max_conditional_put_bytes` is not a CAS setting. It is a property of the GCS conditional-write
dialect, in the same category as `gcs_issue_compose_request`.

The gate chain: `S3ObjectStorage::conditionalOpsUseGenerationTokens` returns
`client->supportsGcsNativeConditionalRequests()`; the CAS backend turns that into
`native_token_type = TokenType::Generation` in its constructor; and only inside
`if (native_token_type == TokenType::Generation)` does `conditionalWriteSettings` set
`s3_force_single_part_upload` together with the cap. On AWS S3 the predicate is false, forced
single-part never engages, a conditional write takes the ordinary multipart path, and the cap is
never applied. The
user-facing documentation already says as much: "Irrelevant on `ETag`-based backends such as
AWS S3."

What is genuinely CAS's here is the *policy* — "this write must not go multipart" — and that is
already expressed as a per-write flag, `WriteSettings::s3_force_single_part_upload`, of the same
kind as `object_storage_write_if_none_match`. The *number* belongs to the S3 layer.

### Where it goes and why {#gcs-cap-home}

Into `S3AuthSettings`'s `CLIENT_SETTINGS` block, immediately next to `gcs_issue_compose_request`,
**keeping the config spelling `gcs_max_conditional_put_bytes` unchanged**.

That is where every GCP-specific config key in the tree already lives — `http_client`
(`gcp_oauth` / `gcs_hmac`), `service_account`, `metadata_service`, `request_token_path`,
`google_adc_client_id`, `google_adc_client_secret`, `google_adc_refresh_token` in `AUTH_SETTINGS`,
and `gcs_issue_compose_request` in `CLIENT_SETTINGS`. There is no GCS object-storage type and no GCS
settings struct: GCS is a provider variant of the S3 object storage, detected from the endpoint.

The struct's name is a poor fit for the knob, but it is a poor fit for what it already holds —
`CLIENT_SETTINGS` carries `disable_checksum`, `use_adaptive_timeouts`, `uri_style`,
`expect_continue_min_bytes` and `gcs_issue_compose_request`, none of which is authentication. Client
and auth settings are read from a disk block **unprefixed**, so this placement costs zero config,
test and documentation edits for this key.

### Code changes {#gcs-cap-changes}

- `S3AuthSettings.cpp`: one `DECLARE(UInt64, gcs_max_conditional_put_bytes, …)` in `CLIENT_SETTINGS`.
- `S3Defines.h`: the default moves there as a named constant next to
  `DEFAULT_MAX_SINGLE_PART_UPLOAD_SIZE`, keeping the present value of 1 GiB.
- `S3ObjectStorage::writeObject`: where it currently consumes
  `write_settings.s3_single_part_upload_max_bytes_override`, it instead reads the setting from its
  own `s3_settings.get()->auth_settings` — **gated on `write_settings.s3_force_single_part_upload`**.
  The gate is load-bearing: applying the cap unconditionally would raise
  `max_single_part_upload_size` and `min_upload_part_size` from 32 MiB to 1 GiB for *every* write on
  every GCS disk, turning ordinary uploads into gigabyte RAM-buffered single PUTs.
- `WriteSettings`: `s3_single_part_upload_max_bytes_override` is **deleted**. Its only producer was
  the CAS backend, and with the number sourced at the point of use the field has no reason to exist.
  This also collapses the redundant `force`/`cap` pair into the single flag that carries the policy.
- `ObjectStorageBackend`: the `conditional_single_put_cap_` constructor parameter and the
  `conditional_single_put_cap` member are removed; `conditionalWriteSettings` sets only the flag.
- `ContentAddressedSettings`, `ContentAddressedMetadataStorage`: the setting, the member and the
  constructor plumbing that carries it to the backend are removed.
- `S3::ClientSettings` in `IO/S3/Client.h` is **not** touched: the value is consumed at write time,
  not at client construction time.
- The exception text in `WriteBufferFromS3::createMultipartUpload` names the setting; it stays
  correct in wording but stops describing it as a CAS disk setting.

### Upstream cost {#gcs-cap-cost}

This edits shared surfaces, which is why it is called out rather than folded into the sweep.

Adding a field to `S3AuthSettings` changes its binary serialization: `writeChangedBinary` writes
*changed* settings by name and `readBinary` calls `throwSettingNotFound` on a name it does not know.
A node running an older build therefore fails if a newer node actually sets this key and propagates
S3 settings to it — the standard cost of adding any S3 setting upstream, and a real difference from a
pre-release CAS setting, where there are no compatibility obligations at all.

Deleting a `WriteSettings` field is the safer direction: the struct is not serialized across
versions, and the field has exactly one producer and one consumer, both in this change.

## The full rename table {#rename-table}

Twenty-five keys are renamed. `skip_access_check` (Part 2) and `gcs_max_conditional_put_bytes`
(Part 3) are not, for the reasons above.

The `XML` column counts `<key>` elements outside `src/` and `docs/`; `disk()` counts assignment
lines inside files that build a CAS disk with the inline SQL form. Both are sweep-scoping figures,
not a substitute for running the sweep.

| Now | After | Type | Default | XML | `disk()` |
|-----|-------|------|---------|----:|------:|
| `server_root_id` | `cas_server_root_id` | String | — (required) | 65 | 36 |
| `gc_enabled` | `cas_gc_enabled` | Bool | `true` | 48 | 9 |
| `gc_interval_sec` | `cas_gc_interval_sec` | UInt64 | `60` | 41 | 9 |
| `gc_shards` | `cas_gc_shards` | UInt64 | `1` | 7 | 0 |
| `scratch_path` | `cas_scratch_path` | String | per-disk `cas_scratch/` | 4 | 0 |
| `manifest_decode_cache_bytes` | `cas_manifest_decode_cache_bytes` | UInt64 | 128 MiB | 3 | 0 |
| `staging_backend` | `cas_staging_backend` | String | `local` | 2 | 0 |
| `part_folder_validate` | `cas_part_folder_validate` | String | `always` | 1 | 0 |
| `gc_snapshot_generations_to_keep` | `cas_gc_snapshot_generations_to_keep` | UInt64 | `3` | 1 | 0 |
| `manifest_sweep_list_budget_keys` | `cas_manifest_sweep_list_budget_keys` | UInt64 | `1000` | 1 | 0 |
| `manifest_sweep_delete_budget_keys` | `cas_manifest_sweep_delete_budget_keys` | UInt64 | `100` | 1 | 0 |
| `blob_hash` | `cas_blob_hash` | String | `cityhash128` | 0 | 0 |
| `blob_hash_allow_new` | `cas_blob_hash_allow_new` | Bool | `false` | 0 | 0 |
| `part_folder_cache_bytes` | `cas_part_folder_cache_bytes` | UInt64 | 64 MiB | 0 | 0 |
| `part_folder_cache_max_entries` | `cas_part_folder_cache_max_entries` | UInt64 | `10000` | 0 | 0 |
| `part_folder_cache_max_entry_bytes` | `cas_part_folder_cache_max_entry_bytes` | UInt64 | 16 MiB | 0 | 0 |
| `gc_meta_pool_size` | `cas_gc_meta_pool_size` | UInt64 | `16` | 0 | 0 |
| `gc_round_graduation_budget` | `cas_gc_round_graduation_budget` | UInt64 | `5000` | 0 | 0 |
| `gc_round_redelete_budget` | `cas_gc_round_redelete_budget` | UInt64 | `5000` | 0 | 0 |
| `gc_round_sweep_namespace_budget` | `cas_gc_round_sweep_namespace_budget` | UInt64 | `20` | 0 | 0 |
| `gc_round_sweep_recovery_op_budget` | `cas_gc_round_sweep_recovery_op_budget` | UInt64 | `5000` | 0 | 0 |
| `gc_round_ref_cleanup_budget` | `cas_gc_round_ref_cleanup_budget` | UInt64 | `5000` | 0 | 0 |
| `gc_round_prefix_wholesale_budget` | `cas_gc_round_prefix_wholesale_budget` | UInt64 | `20000` | 0 | 0 |
| `gc_round_handoff_prefix_wholesale_budget` | `cas_gc_round_handoff_prefix_wholesale_budget` | UInt64 | `5000` | 0 | 0 |
| `gc_round_outcome_entry_budget` | `cas_gc_round_outcome_entry_budget` | UInt64 | `5000` | 0 | 0 |

## The sweep {#sweep}

### Classes of site {#sweep-classes}

Five, and a sweep that covers only the first two is incomplete:

1. **XML configs** — `<key>` in `tests/config/config.d/cas_*.xml`, the integration-test configs, and
   `utils/ca-soak/configs/`. 54 files carry a CAS disk block.
2. **Inline `disk(...)`** — multi-line `key = value` in the `04278`-`05015` stateless tests: 18
   `.sh`, 13 `.sql`, 1 `.py`.
3. **XML fragments inside Python strings** — integration tests that assemble config text, e.g. the
   GCS tests emitting `"<staging_backend>s3</staging_backend>"`.
4. **Key/value renderers** — `render_tuned_config` in the ca-soak framework takes a dict and emits
   `<key>value</key>`; the *dict keys* at its call sites and in its own tests are config keys.
5. **XML string literals in C++ tests** — `gtest_cas_settings.cpp`,
   `gtest_cas_part_folder_access.cpp`, `gtest_cas_retirement_sweep.cpp`, `gtest_cas_s3_staging.cpp`,
   plus the config examples in the subtree `README.md`.

Documentation is a sixth class: 18 files under `docs/en`, ~167 word occurrences.

### Traps {#sweep-traps}

Each of these was found while scoping the sweep; each would survive a naive
`sed s/<key>/<cas_key>/`.

- **Angle brackets in `src/` are usually not XML.** `<server_root_id>` appears in `Formats/CasLayout.h`
  and neighbours as a *path placeholder* (`pool/<server_root_id>/...`). Counting closing tags rather
  than opening ones separates the two: 15 real literals against 32 raw matches for that key.
- **Prose is not config.** Most textual `gc_shards=` occurrences under `tests/` and `utils/` are
  docstrings and comments (`# CA soak Phase-4 gc_shards=2 variant`), not keys.
- **Documentation anchors must not move.** `docs/en/antalya/cas/configuration.md` carries
  `### Choosing \`blob_hash\` {#choosing-blob-hash}`, and `docs/en/operations/storing-data.md` links
  to it. Rename the key text; leave the anchor slug alone unless every inbound link is updated in the
  same commit.
- **One documented claim is inverted by this change**, not merely reworded:
  `docs/en/operations/storing-data.md` states "Since the disk element already scopes every key to
  this disk, none of the keys below carry a redundant `cas_`/`ca_` prefix." That sentence is the
  rationale this design refutes — the block is shared, not CAS-scoped — and it must be rewritten to
  say why the prefix exists. The same rationale appears as a comment above the `DECLARE` list in
  `ContentAddressedSettings.cpp`.
- **`<path>` stays `<path>`.** It is the generic local-object-storage pool root, in the same class as
  `type`, `name`, `endpoint`. Only the 25 keys in the table change.

## Testing {#testing}

The existing suite in `gtest_cas_settings.cpp` inverts in an instructive way.

- `ObjectStorageKeysSkipped` today enumerates the foreign keys CAS happens to tolerate, and each of
  its regression pins records a startup outage. It is replaced by a test that asserts CAS ignores
  *whole classes* it can no longer know about: repeated `header` / `header[1]`, `access_header`, a
  `user_*` key, a `<proxy>` subtree, a `<server_side_encryption_kms_config>` subtree, an `s3_`-prefixed
  request setting, unlisted client keys (`connect_timeout_ms`, `session_token`), and the Azure
  spellings (`account_name`, `connection_string`, `container_name`). The point of the new test is
  that none of those names appears anywhere in CAS code.
- The field report gets a direct pin: a disk block carrying
  `<http_keep_alive_timeout>60</http_keep_alive_timeout>` loads.
- `RemovedCacheSettingsAreRejected`, `LegacyTokenProducingPutCapNameRejected` and `UnknownKeyRejected`
  keep their meaning under the `cas_` spelling, and are the reason Part 1 is one commit.
- New: the bare-name guard rejects `<gc_enabled>` with `UNKNOWN_SETTING` and a message naming
  `cas_gc_enabled`.
- New: `<skip_access_check>` lands on the CAS side while `<cas_skip_access_check>` is rejected.
- New: a duplicated `cas_gc_enabled` element is rejected.
- Part 3: `conditionalWriteSettings` sets `s3_force_single_part_upload` on a generation-token backend
  and nothing on an ETag one — the existing assertions in `gtest_cas_backend_generation.cpp` that
  read the deleted `WriteSettings` field are rewritten to the flag. On the S3 side, a test that
  `writeObject` raises the single-part limits only when the flag is set.

Gate: `unit_tests_dbms --gtest_filter='CAS*'` for the CAS suites, plus whatever S3 suite the new
`writeObject` test lands in. New suites must be named so they match `CAS*` — a suite that escapes
the filter escapes the gate.

## Rejected alternatives {#rejected}

- **Grow `non_cas_keys`.** The status quo; every new backend key breaks a config in the field rather
  than in CI.
- **Subtract builtin names via `hasBuiltin`.** The BACKLOG's recorded shape. Fails on
  `header[1]`/`user_*`/subtrees/per-backend spelling conventions; see [above](#unenumerable).
- **A nested `<cas>` block instead of a prefix.** Cleaner in XML, impossible in the inline
  `disk(...)` form, whose values must be a literal or an identifier; a dotted key
  (`cas.gc_enabled`) collides with Poco's path separator. Two spellings for one setting is worse
  than long names.
- **Edit-distance typo detection instead of a namespace.** Needs no config churn, but a false
  positive is a server that will not start — the exact failure being fixed.
- **Put the GCS cap in `S3RequestSettings`/`PART_UPLOAD_SETTINGS`.** Defensible by nature (it caps an
  upload size) but request settings are read from a disk block only under the `s3_` prefix, so the
  key would become `s3_gcs_max_conditional_put_bytes` — a rename after all, with two stacked scopes.
- **Split a real `GCSSettings` / `S3ClientSettings` struct out of `S3AuthSettings`.** The honest
  home, and a refactor of a shared serialized struct larger than this whole item, with no effect on
  how the key is spelled in a config.

## Out of scope {#out-of-scope}

- The unknown-key gate is evaluated only at disk creation, so a typo introduced by a config edit plus
  a reload is not diagnosed until the next restart. Unchanged by this design; tracked in
  `BACKLOG/operability-and-introspection.md`.
- CAS does not honour the server-level `global_skip_access_check` that the generic disk layer ORs in.
  Pre-existing, and a behaviour question rather than a config-parsing one.
