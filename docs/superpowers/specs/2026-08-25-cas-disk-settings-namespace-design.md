---
description: 'Design for letting a CAS disk carry any underlying object-storage setting, by giving the CAS settings their own config-key namespace instead of an enumerated skip-list'
sidebar_label: 'CAS disk settings namespace'
sidebar_position: 7
slug: /superpowers/specs/cas-disk-settings-namespace-design
title: 'CAS disk settings: which keys the CAS layer owns'
doc_type: 'design'
---

# CAS disk settings: which keys the CAS layer owns {#cas-disk-settings-namespace-design}

**Status:** DRAFT for review, rev.5 (2026-08-25).

rev.2 answered a review of rev.1; rev.3 reversed rev.2's no-migration-window decision on the owner's
instruction, because configurations already live in external CI/CD scripts. rev.4 answers a review of
rev.3 and one owner correction. From the review: the warning is separated from the temporary
application of legacy values and appears in the loader sketch; the landing order is fixed and made
bisectable; a duplicated legacy key is rejected rather than silently resolved; the state table's
pre-change row is corrected; the follow-up task is filed rather than described as filed; and the gate
is sized to the sweep. From the owner: a closed window **rejects** an unprefixed CAS setting name
rather than ignoring it, so no state of this design silently discards a value an operator wrote — at
the price of a reservation on twenty-five names, which [is now stated](#reservation) instead of
implied.

This specification covers item 4 of `docs/superpowers/cas/final-checks-todo.md`
(`{#fix-s3-key-whitelist}`) and the BACKLOG record `{#cas-disk-s3-key-whitelist-gap}`. The field
report behind it: adding `<http_keep_alive_timeout>60</http_keep_alive_timeout>` to a CAS disk block
— the mitigation suggested in Altinity/ClickHouse#2243 — fails server startup with
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
recognise, so **any legal disk key nobody thought to enumerate fails server startup with an
exception.**

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

**A boundary worth stating before it is misread.** This is a defect in *configuration parsing*, and
fixing it makes a backend's settings expressible, not a backend supported. Whether a store can host
a CAS pool is a separate, protocol-level question: any non-Local object storage opens in `Native`
mode, and Native requires enforced conditional operations — `removeObjectIfTokenMatches`, whose base
implementation throws precisely so the capability probe fails closed, and
`supportsRetryProfile(SingleAttempt)`, whose base implementation returns false. Azure implements
neither, so a CAS pool on Azure is refused at mount regardless of this change. The promise here is
"CAS never rejects a key belonging to its backend", not "CAS runs on any backend".

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

## How this sits with other disk types {#conventions}

There is no single convention to conform to here, and it is worth saying so rather than leaning on
the `s3_` precedent alone.

A plain disk type does use bare keys, because `type` already supplies the context: `cache` takes
`path`, `max_size`, `cache_policy`; `encrypted` takes `disk`, `key`, `algorithm`; S3 auth takes
`endpoint`, `access_key_id`, `connect_timeout_ms`; Azure takes `account_name`, `container_name`.

`object_storage` is not a plain disk type. Its block is read by several independent components at
once, and the tree already disambiguates them in more than one way: S3 request settings under the
`s3_` prefix, metadata-layer keys bare (`metadata_path`, `object_metadata_cache_size`), complex
components as subtrees (`proxy`, `locations`), and some historical auth keys bare. So `cas_*` is
mildly unusual measured against the *disk-type* habit and entirely ordinary measured against the
*composite-block* habit — and the composite block is what a CAS disk is.

The closest precedent to the code being replaced is `FileCacheSettings`, which scans its block and
skips a `non_cache_keys` set of six names (`type`, `disk`, `name`, `data_background_cleanup`,
`thread_pool_size`, `skip_access_check`). It works there because `cache` is a top-level disk type
whose foreign key space is small and closed. CAS sits on top of an arbitrary backend, so the same
pattern inherits an unbounded foreign key space. The pattern did not fail because it was badly
written; it failed because it was transplanted somewhere its precondition does not hold.

## Part 1 — CAS owns the `cas_` namespace {#part-1-prefix}

Two properties are wanted, and only one arrangement delivers both:

1. **Any underlying setting must work**, for every backend, now and for backends added later. This
   requires that CAS never *refuse* a configuration over a key it does not own, and never consume
   one. Reading a foreign key name to phrase a better error, on a path that is already failing, does
   not violate this; reserving that name does.
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

std::vector<std::string> legacy_names;

for (const std::string & key : config_keys)
{
    /// Poco renders repeated elements as `name`, `name[1]`, `name[2]` -- the convention
    /// `StorageURL.cpp` and `HTTPDictionarySource.cpp` both spell out. A duplicated key of OURS
    /// must therefore be recognised by its base name rather than passed over as foreign.
    const auto [base, repeated] = splitRepeatIndex(key);

    if (base.starts_with(CAS_KEY_PREFIX))
    {
        if (repeated)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "content_addressed disk: `{}` is set more than once", base);
        impl->set(base.substr(CAS_KEY_PREFIX.size()), config.getString(config_prefix + "." + key));
    }
    else if (ContentAddressedSettingsImpl::hasBuiltin(base))
    {
        if (repeated)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "content_addressed disk: `{}` is set more than once", base);
        legacy_names.emplace_back(base);
    }
    /// Anything else is foreign: not inspected, not consumed, never a reason to refuse.
}

/// MIGRATION WINDOW. Detection above is permanent; everything below is the window. Closing it
/// replaces this whole block with the throw sketched underneath -- one edit, one place.
if (!legacy_names.empty())
    LOG_WARNING(log, "content_addressed disk `{}`: {} use the pre-rename spelling and are applied "
        "for now; write them with the `cas_` prefix. Support for the unprefixed spelling will be "
        "removed.", disk_name, fmt::join(legacy_names, ", "));

for (const std::string & key : legacy_names)
{
    if (impl->isChanged(key))
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "content_addressed disk: both `{}` and `cas_{}` are set; remove the unprefixed one",
            key, key);
    impl->set(key, config.getString(config_prefix + "." + key));
}

/// ... and what the block above becomes once the window closes:
///
///     if (!legacy_names.empty())
///         throw Exception(ErrorCodes::UNKNOWN_SETTING,
///             "content_addressed disk: {} use the pre-rename spelling; write them with the "
///             "`cas_` prefix", fmt::join(legacy_names, ", "));
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

### The migration window {#migration-window}

Configurations using the unprefixed spelling already exist outside this repository — the field report
came from one — so the unprefixed spelling is **accepted for a bounded period** rather than dropped
at once. The loader therefore has three states, and only the middle one is temporary:

| | Unprefixed CAS setting name | Foreign key | Unknown `cas_*` |
|---|---|---|---|
| Before this change | consumed | **startup fails** (`UNKNOWN_SETTING`) | `UNKNOWN_SETTING` |
| Window open | consumed, one `WARNING` per disk | ignored | `UNKNOWN_SETTING` |
| Window closed | `UNKNOWN_SETTING`, naming the `cas_` spelling | ignored | `UNKNOWN_SETTING` |

**A closed window rejects; it does not ignore.** Silently dropping a value an operator wrote is worse
than refusing to start, and refusing is what the rest of this subtree does with a setting it cannot
honour. The consequence is worth stating plainly rather than burying: after the window closes there
is **no state in which a CAS setting value is silently discarded** — during the window legacy values
are applied, and afterwards they are refused.

Closing the window is therefore one edit in one place: the loop that applies legacy values, together
with its warning, becomes a throw that names the `cas_` spelling for every legacy key it found.
Aggregation stays — one message per disk, listing all of them, not one per key.

Three rules hold in every state:

- **A foreign key is never inspected for acceptance.** `header[1]`, `<proxy>`, `s3_retry_attempts`,
  `account_name` and everything a future backend invents pass through untouched. This is the fix.
- **Ambiguity is never resolved silently.** A block carrying both `gc_interval_sec` and
  `cas_gc_interval_sec` is rejected with `BAD_ARGUMENTS` rather than one of them quietly winning —
  and so is a block carrying the same key twice in either spelling. The duplicate case needs the
  explicit repeat-index split above: without it `gc_enabled` would pass the builtin check while
  `gc_enabled[1]` failed it, the first value would silently win, and a configuration that fails
  loudly today would start succeeding quietly.
- **A CAS setting is never silently dropped.** Applied, or refused with its correct spelling named.

### The reservation, stated {#reservation}

A closed window means CAS permanently reserves the twenty-five names in the
[rename table](#rename-table) inside a disk block it does not own. That is a real cost and the
specification states it rather than implying the namespace is fully open: **the promise is "any
underlying setting works, except the twenty-five listed names, which CAS answers for."**

The reason this is acceptable, and the original defect was not, is the difference between a bounded
known list and an unbounded unknown one:

- The defect being fixed rejects keys **nobody enumerated anywhere** — every present and future
  setting of every backend, plus open-ended forms like `header[1]` and `user_*`. There is no list to
  consult and no fix short of re-enumerating a space that cannot be enumerated.
- The reservation rejects twenty-five names that are **written down in this document, in the user
  documentation, and in one `DECLARE` list in the source**. If a backend ever adds a colliding
  setting, the failure is loud at startup and names the key, and the remedy is a one-line exception
  in a list that is right there. The names are also unusually CAS-shaped — `gc_round_redelete_budget`,
  `manifest_sweep_list_budget_keys`, `blob_hash_allow_new` — with `scratch_path` the only plausible
  collision candidate in the set.

The alternative — ignore-with-warning after the window — was considered and rejected: it trades a
loud, diagnosable, one-line-fixable collision risk for a silent one. A dropped `blob_hash` is
unrecoverable, because the algorithm is fixed at pool creation; a dropped `gc_shards` is
creation-time-only in the same way. A warning in a log is not a control for that.

### What stays loud {#stays-loud}

Typo detection inside the namespace is unaffected: `cas_gc_shardz` is unknown, and
`BaseSettings::set` throws `UNKNOWN_SETTING` with hints exactly as today. Three existing regression
tests pin that posture — `RemovedCacheSettingsAreRejected`, `LegacyTokenProducingPutCapNameRejected`
and `UnknownKeyRejected` in `gtest_cas_settings.cpp` — and they are also why Part 1 ships as one
commit rather than "stop scanning now, add the prefix later": the intermediate state would require
deleting those three tests and restoring them afterwards.

## Part 2 — `skip_access_check` leaves the settings list {#part-2-skip-access-check}

`skip_access_check` keeps its bare spelling, because prefixing it would split one config key into two
keys with two meanings: `registerDiskObjectStorage` reads the bare key from the same block for the
generic access check, and CAS reads it to decide whether to run its boot-time capability probe.

**One key, two policies — not one value.** The generic layer computes
`global_skip_access_check || <the disk key>` and hands the result to `IDisk::startup`, which runs
`startupImpl` — and therefore `metadata_storage->startup`, and therefore the CAS probe — *before*
`checkAccess`. CAS reads only the disk-local key. So the server-level override reaches the generic
access check and never reaches the CAS probe, and the probe runs first.

**Decision: keep the scopes distinct.** Propagating the server-global flag into CAS would not merely
widen a skip. `ObjectStorageBackend::checkSkipAccessCheckSupport` throws `NOT_IMPLEMENTED` for
`skip_access_check=true` on a writable generation-token mount, because the capability battery is what
proves a token-exact DELETE honours the generation precondition. A server started with the global
flag would therefore refuse **every** writable CAS-on-GCS mount. The user documentation states the
split in one sentence: the server-level flag skips the generic disk access check; the CAS capability
probe is governed by the disk's own key.

Mechanically, the setting is removed from `LIST_OF_CONTENT_ADDRESSED_SETTINGS` and becomes a plain
member of `ContentAddressedSettingsImpl` alongside `blob_hash_algo_cached`, populated in
`loadFromConfig` from the unprefixed key and exposed through an accessor shaped like `blobHashAlgo`.
`ContentAddressedMetadataStorage` keeps its member and its use in `PoolConfig`; only the source of
the value changes, and the `extern const ContentAddressedSettingsBool skip_access_check;`
declaration in that TU goes away with the settings entry. After this, `cas_skip_access_check` is
rejected as an unknown CAS setting — correct, because it is not one.

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

### The trade this makes {#gcs-cap-tradeoff}

This placement is a considered compromise, not a convention. By nature the setting is an
upload-sizing knob: it directly overrides `max_single_part_upload_size` and `min_upload_part_size`,
both of which live in `S3RequestSettings`'s `PART_UPLOAD_SETTINGS`. Putting it in `S3AuthSettings`
adds to a class whose name already fits its contents poorly.

What buys the compromise is the spelling. Client and auth settings are read from a disk block
unprefixed, so the key keeps the name it already carries in deployed configurations and in published
documentation, whereas the request-settings home would force `s3_gcs_max_conditional_put_bytes`. A
change whose entire purpose is to stop breaking existing object-storage configuration should not
introduce a second, unrelated rename of an object-storage key. If the semantic debt is later paid off
by splitting a real client-settings struct out of `S3AuthSettings`, this setting moves with its
neighbours and the config spelling is unaffected either way.

Part 3 is a separate commit inside the same pull request — it changes a serialized shared S3 surface
and is not part of removing `non_cas_keys` — and it must land **first**; see
[landing order](#landing-order).

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

## Landing order {#landing-order}

The parts are separate commits, and only one order is bisectable — at every commit the tree must
build, pass, and start on a configuration that was valid at the previous one.

1. **Part 3.** The GCS cap moves to `S3AuthSettings`, and the *same* commit adds
   `gcs_max_conditional_put_bytes` to `non_cas_keys`. Without that one line the still-scanning loader
   hands the key to `BaseSettings::set` at this commit and every configuration carrying it fails to
   start. The line is deleted along with the rest of the set in step 2.
2. **Parts 1 and 2 together.** The prefix, the migration window, `skip_access_check` demoted to a
   plain member, and `non_cas_keys` deleted. Part 2 cannot precede Part 1 for exactly the reason
   Part 3 could not: `skip_access_check` is deliberately *absent* from `non_cas_keys` today — the
   set's own comment says so, because it is a CAS setting — so removing it from the settings list
   while the scanner still runs would make the bare key unknown.
3. **The sweep.** In-tree configurations, tests, fixtures and documentation move to `cas_*`. The
   migration window is what makes this an independently revertable commit, and it can be split
   further by sweep class: while the window is open both spellings work, so a partial sweep cannot
   break a lane.
4. **Later: close the window.** See [the follow-up task](#upgrade).

Reversing 1 and 2 produces actively wrong advice rather than a mere ordering wart: with Part 1 first,
the bare `gcs_max_conditional_put_bytes` is still a CAS builtin, so the migration warning would tell
an operator to write `cas_gcs_max_conditional_put_bytes` — a spelling Part 3 then turns into an
unknown CAS setting.

## Upgrade and mixed-version rollout {#upgrade}

The window makes the direction that matters work: **a new binary reads an existing unprefixed
configuration**, so a CI/CD pipeline that pins configuration files separately from binaries keeps
running across the upgrade, with a warning telling it what to change.

The other direction still does not work, and cannot without patching the older binary: an **old
binary reading a `cas_`-prefixed configuration** hands `cas_server_root_id` to `BaseSettings::set`
and dies with `UNKNOWN_SETTING`, because the pre-change loader rejects every key it does not know.
That fixes the rollout order:

1. Upgrade binaries. Existing configurations keep working; each CAS disk logs one warning.
2. Migrate configurations to `cas_*`, at whatever pace the pipelines allow.
3. Close the window in a later change. From then on **any** block still carrying an unprefixed CAS
   setting name fails to start with `UNKNOWN_SETTING`, whether it is fully unmigrated or missed a
   single key — there is no "partially migrated but running" state to reason about, and no key whose
   value is quietly discarded. Step 2 is therefore not "migrate `server_root_id`", it is "migrate
   every key"; the warning emitted in step 1 names them all, which is what makes step 2 mechanical.

Do not reverse steps 1 and 2, and roll a configuration back together with any binary rollback.

**Closing the window is a task, not a note.** It is one deleted loop in `loadFromConfig`, one deleted
test, one replaced test, and a warning that becomes a throw.
The trigger is external and checkable: the CAS configurations in the `clickhouse-regression` suite
are on the `cas_` spelling.

It is recorded in `BACKLOG/operability-and-introspection.md` under `{#cas-config-prefix-window}`, and
the implementation plan for this specification **must** carry it as an explicit follow-up task naming
that trigger. Neither placement is optional: an item that exists only in a ledger is one context loss
away from never happening, and a deprecation window nobody closes is worse than no window at all.

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
- New: an entirely unprefixed block loads while the window is open and the values land — the
  external-CI/CD case. This is the test the closing task rewrites.
- New: a partially migrated block — `cas_server_root_id` plus a bare `gc_enabled` — loads with
  `gc_enabled` taking the bare value, and emits exactly **one** warning naming exactly the bare keys.
  Assert the count and the content: a warning that is the only signal for a stale key is part of the
  contract, not decoration.
- New: a block carrying both `gc_interval_sec` and `cas_gc_interval_sec` is rejected with
  `BAD_ARGUMENTS`; so is a block carrying `<gc_enabled>` twice, and `<cas_gc_enabled>` twice.
- The closed-window behaviour cannot be pinned until the window closes. The closing task rewrites the
  first two tests above into one asserting `UNKNOWN_SETTING` on an unprefixed block, with every
  legacy key and its `cas_` spelling named in the message.
- New: `<skip_access_check>` lands on the CAS side while `<cas_skip_access_check>` is rejected.
- New: a duplicated `cas_gc_enabled` element is rejected.
- Part 3: `conditionalWriteSettings` sets `s3_force_single_part_upload` on a generation-token backend
  and nothing on an ETag one — the existing assertions in `gtest_cas_backend_generation.cpp` that
  read the deleted `WriteSettings` field are rewritten to the flag. On the S3 side, a test that
  `writeObject` raises the single-part limits only when the flag is set.

### The gate {#gate}

Unit tests alone do not cover this change: its most likely defect is a missed sweep site, and no
unit test can see one. The gate is therefore:

- `unit_tests_dbms --gtest_filter='CAS*'` — the CAS suites. New suites must be named so they match
  `CAS*`; a suite that escapes the filter escapes the gate.
- `unit_tests_dbms --gtest_filter='WriteBufferFromS3*'` — the home of the Part 3 `writeObject`
  assertions, in `src/IO/tests/gtest_writebuffer_s3.cpp`. Named here rather than left as "whatever
  S3 suite", so the gate is runnable as written.
- Every stateless CAS test that builds a disk with the inline `disk(...)` form — the `04278`-`05015`
  range, 32 files. These are the sweep sites that a unit test cannot reach.
- The server-configuration lane: a server started with `tests/config/config.d/cas_*.xml` must come
  up. Those two files are the only in-tree CAS disks defined by server configuration rather than by
  a test, and the `path` key in one of them is already the source of one past startup outage.
- CAS integration tests over S3 and GCS — `test_cas_s3`, `test_cas_gc_s3`, `test_cas_gcs`,
  `test_cas_gc_sharded`, `test_cas_shared_pool` — which carry the Python-assembled XML fragments and
  the `render_tuned_config` dict keys, sweep classes 3 and 4.
- **The field report's own case, end to end.** The CAS integration disk carries
  `http_keep_alive_timeout` and five more keys of the same class — client settings, `s3_`-prefixed
  request settings, a repeated `<header>` — each of which used to fail server startup. The lane then
  fails if the disk block ever stops accepting them.

  This replaces an earlier proposal for a CAS-over-Azure smoke. Azure cannot host a CAS pool for the
  protocol reason given above, so such a test could not pass; the Azure *spellings* are covered in
  the unit test, which asserts they are never inspected. Conflating the two claims is exactly what
  the boundary note above exists to prevent.

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
- Splitting a real client-settings struct out of `S3AuthSettings`, which would give the GCS cap a
  home that matches its name. Recorded in [the trade this makes](#gcs-cap-tradeoff); it does not
  change how any key is spelled in a configuration.
