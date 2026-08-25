---
description: 'Implementation plan for giving the CAS disk settings their own `cas_` config-key namespace, so a CAS disk can carry any underlying object-storage setting'
sidebar_label: 'CAS disk settings namespace'
sidebar_position: 8
slug: /superpowers/plans/cas-disk-settings-namespace
title: 'CAS disk settings namespace implementation plan'
doc_type: 'guide'
---

# CAS disk settings namespace implementation plan {#cas-disk-settings-namespace-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** A CAS disk accepts any setting of its underlying object storage — S3, GCS, Azure, or a
backend that does not exist yet — because the CAS settings move into their own `cas_` config-key
namespace and CAS stops inspecting keys it does not own.

**Architecture:** `ContentAddressedSettings::loadFromConfig` keeps scanning the disk block but
consumes only `cas_`-prefixed keys, and the enumerated `non_cas_keys` skip-list is deleted. Two keys
leave the CAS settings rather than gaining the prefix: `skip_access_check`, which is deliberately
shared with the generic disk layer, and `gcs_max_conditional_put_bytes`, which is a property of the
GCS conditional-write dialect and moves into `S3AuthSettings`. Unprefixed CAS names are accepted for
a bounded migration window because configurations already live in external CI/CD scripts.

**Tech Stack:** C++ (`BaseSettings`, Poco configuration), gtest (`unit_tests_dbms`), praktika for
stateless and integration lanes.

**Spec:** `docs/superpowers/specs/2026-08-25-cas-disk-settings-namespace-design.md`

## Global Constraints {#global-constraints}

Every task's requirements implicitly include this section.

- **Branch:** `cas-gc-rebuild`. Add new commits; never rebase or amend.
- **Landing order is load-bearing and bisectable.** Task 1 before Task 2. Reversing them makes the
  migration warning tell operators to write `cas_gcs_max_conditional_put_bytes`, a spelling Task 1
  then turns into an unknown CAS setting.
- **Gate filter is exactly `CAS*`.** Every new CAS suite must be named so it matches. Never widen the
  filter; a suite that does not match gets renamed instead.
- **Allman braces** (opening brace on its own line). Enforced by the CI style check.
- **Comments carry the reason, never the provenance.** No references to this plan, the spec, a
  BACKLOG anchor, a task number, or a review. Those documents are deleted from the branch; the code
  is not.
- **Build output always goes to a log file in the build directory**, and a subagent analyses that log
  and returns a summary. Never paste a build log into the transcript.
- **No `-j` argument to ninja, and no `nproc`.** Let ninja decide.
- **Documentation headers under `docs/` need an explicit `{#kebab-anchor}`**; new documentation files
  need the frontmatter block.
- **The migration window is temporary.** Its removal is filed as
  `docs/superpowers/cas/BACKLOG/operability-and-introspection.md` `{#cas-config-prefix-window}`, and
  Task 10 of this plan is the placement that executes it.

## Conventions {#conventions}

**BUILD** — from the repository root:

```bash
ninja -C build unit_tests_dbms > build/build_<task>.log 2>&1
echo "NINJA_EXIT=$?"
```

**ANALYZE** — dispatch a subagent with this instruction, never read the log inline:

> Read `build/build_<task>.log`. Report only: did the build succeed, and if not, the first error with
> its file and line. Do not paste the log.

**TEST (unit)**:

```bash
build/src/unit_tests_dbms --gtest_filter='<pattern>' > build/test_<name>.log 2>&1
echo "GTEST_EXIT=$?"
```

Then dispatch a subagent to read `build/test_<name>.log` and report the pass/fail tally and the first
failure. A test log is analysed by a subagent exactly as a build log is.

**TEST (stateless)** — praktika takes **one** `--test` flag with space-separated names; repeating the
flag silently keeps only the last:

```bash
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
ninja -C build clickhouse > build/build_server.log 2>&1
python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test 04278 04279 04285 \
    > build/test_stateless_<name>.log 2>&1
```

**TEST (integration)** — praktika **exits 0 even when tests fail** for integration jobs, so the exit
code is not a verdict; read the pytest summary line out of the log body:

```bash
python3 -m ci.praktika run "Integration tests (amd_binary, 1/5)" --test test_cas_s3 test_cas_gcs \
    > build/test_integration_<name>.log 2>&1
```

The integration lanes run `ci/tmp/clickhouse`, which symlinks `build/programs/clickhouse`. Building
`unit_tests_dbms` does **not** relink it, so build the `clickhouse` target first and verify
positively, e.g. `strings build/programs/clickhouse | grep -c cas_server_root_id`, rather than
trusting a timestamp.

**COMMIT** — stage explicit paths; never `git add -A` in this tree (it has large untracked artifact
directories):

```bash
git add <exact paths>
git commit -F - <<'MSG'
<type>: <subject>

<body>

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
git log --oneline -1
```

The tree is shared with other sessions. Two things follow, and both have already gone wrong here:

- **Confirm the branch, not only the commit.** After every commit run
  `git rev-parse --abbrev-ref HEAD` as well as `git log --oneline -1`. Another session checking out
  its own branch moves this worktree under you, and a commit that looks correct in
  `git log -1` can land on a branch you never chose.
- **Record a baseline before the first edit**, because `git status` in this tree is never clean:

  ```bash
  mkdir -p tmp/sweep
  git status --porcelain > tmp/sweep/baseline_status.txt
  ```

  Every later cleanliness check is a diff against that file, not against an empty status.

## File Structure {#file-structure}

| File | Responsibility after this plan |
|---|---|
| `src/IO/S3AuthSettings.cpp` | Declares `gcs_max_conditional_put_bytes` in `CLIENT_SETTINGS`, next to `gcs_issue_compose_request` |
| `src/IO/S3Defines.h` | Holds the cap's default constant next to `DEFAULT_MAX_SINGLE_PART_UPLOAD_SIZE` |
| `src/IO/WriteSettings.h` | Keeps `s3_force_single_part_upload`; loses `s3_single_part_upload_max_bytes_override` |
| `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp` | Applies the cap from its own settings, gated on the flag |
| `.../ContentAddressed/ContentAddressedSettings.cpp` | Owns `cas_*` only; no foreign-key knowledge; the migration window lives here |
| `.../ContentAddressed/ContentAddressedSettings.h` | Gains `skipAccessCheck`; its `loadFromConfig` contract comment is rewritten |
| `.../ContentAddressed/ContentAddressedMetadataStorage.cpp` | Loses the cap member and its plumbing; reads `skipAccessCheck` |
| `.../ContentAddressed/Backend/CasObjectStorageBackend.{h,cpp}` | Loses `conditional_single_put_cap`; sets only the policy flag |
| `src/Disks/tests/gtest_cas_settings.cpp` | Pins the namespace, the window, duplicates, and foreign-key classes |
| `src/IO/tests/gtest_s3_auth_settings.cpp` | New: pins that the cap parses from a disk block and defaults correctly |
| `tests/integration/test_cas_azure/` | New: the smoke that proves a non-S3 backend now works |

`unit_tests_dbms` globs `gtest*.cpp` recursively under `src` with `CONFIGURE_DEPENDS`, so new test
files need no CMake edit.

---

## Task 1: Move the GCS conditional-PUT cap out of CAS {#task-1}

This lands **first**. While it lands, `ContentAddressedSettings` still scans the whole block and
rejects unknown keys, so the same commit must keep the key acceptable to that scanner.

**Files:**
- Modify: `src/IO/S3Defines.h`
- Modify: `src/IO/S3AuthSettings.cpp`
- Modify: `src/IO/WriteSettings.h`
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.{h,cpp}`
- Modify: `src/IO/WriteBufferFromS3.cpp` (exception text only)
- Create: `src/IO/tests/gtest_s3_auth_settings.cpp`
- Modify: `src/Disks/tests/gtest_cas_backend_generation.cpp`
- Modify: `src/Disks/tests/gtest_cas_settings.cpp`

**Interfaces:**
- Produces: `S3AuthSetting::gcs_max_conditional_put_bytes` (a `S3AuthSettingsUInt64`), and
  `S3::DEFAULT_GCS_MAX_CONDITIONAL_PUT_BYTES`.
- Produces: `ObjectStorageBackend(ObjectStoragePtr, Mode)` — the two-argument constructor; the third
  parameter is gone.
- Consumes: nothing from later tasks.

- [ ] **Step 1: Write the failing settings test**

Create `src/IO/tests/gtest_s3_auth_settings.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Core/Settings.h>
#include <IO/S3AuthSettings.h>
#include <IO/S3Defines.h>
#include <Poco/Util/XMLConfiguration.h>
#include <Poco/AutoPtr.h>
#include <sstream>

using namespace DB;

namespace DB::S3AuthSetting
{
    extern const S3AuthSettingsUInt64 gcs_max_conditional_put_bytes;
}

namespace
{
Poco::AutoPtr<Poco::Util::XMLConfiguration> makeDiskConfig(const std::string & inner)
{
    std::istringstream iss("<clickhouse><disk>" + inner + "</disk></clickhouse>");
    return new Poco::Util::XMLConfiguration(iss);
}
}

/// The cap is a property of the GCS conditional-write dialect, so it is read from the disk block
/// unprefixed, exactly like `gcs_issue_compose_request` beside it.
TEST(S3AuthSettingsConfig, GcsConditionalPutCapParsesFromDiskBlock)
{
    Settings query_settings;

    auto with_override = makeDiskConfig(
        "<gcs_max_conditional_put_bytes>4096</gcs_max_conditional_put_bytes>");
    S3::S3AuthSettings overridden(*with_override, query_settings, "disk");
    EXPECT_EQ(overridden[S3AuthSetting::gcs_max_conditional_put_bytes].value, 4096u);

    auto without = makeDiskConfig("<endpoint>http://x/y</endpoint>");
    S3::S3AuthSettings defaulted(*without, query_settings, "disk");
    EXPECT_EQ(defaulted[S3AuthSetting::gcs_max_conditional_put_bytes].value,
              S3::DEFAULT_GCS_MAX_CONDITIONAL_PUT_BYTES);
}
```

- [ ] **Step 2: Build and confirm it fails to compile**

BUILD with `build_task1_red.log`, then ANALYZE. Expected: a compile error naming
`gcs_max_conditional_put_bytes` as undeclared in `S3AuthSetting`. A compile error is the correct red
here — the symbol genuinely does not exist yet.

- [ ] **Step 3: Declare the setting and its default**

In `src/IO/S3Defines.h`, beside `DEFAULT_MAX_SINGLE_PART_UPLOAD_SIZE`:

```cpp
inline static constexpr uint64_t DEFAULT_GCS_MAX_CONDITIONAL_PUT_BYTES = 1ULL << 30;
```

In `src/IO/S3AuthSettings.cpp`, in the `CLIENT_SETTINGS` list, immediately after
`gcs_issue_compose_request`:

```cpp
    DECLARE(UInt64, gcs_max_conditional_put_bytes, S3::DEFAULT_GCS_MAX_CONDITIONAL_PUT_BYTES, "", 0) \
```

- [ ] **Step 4: Build and run the settings test to green**

BUILD with `build_task1_green.log`, ANALYZE, then TEST with
`--gtest_filter='S3AuthSettingsConfig.*'` into `build/test_s3_auth_settings.log` and dispatch the
analysis subagent. Expected: 1 test, passing.

- [ ] **Step 5: Move the cap out of the backend constructor in every test that passes it**

`src/Disks/tests/gtest_cas_backend_generation.cpp` has **four** sites handing a cap to the
constructor, not one. Removing the parameter without converting all four fails to compile.

1. `CASBackendGeneration.ConditionalWriteSettingsForceSinglePutOnGenerationStores` — drop the
   `/*conditional_single_put_cap=*/123` argument and both
   `s3_single_part_upload_max_bytes_override` assertions. The surviving assertions —
   `s3_force_single_part_upload` true for `Generation`, false for `ETag` — are what it pins now.
2. The `CASBackendGenerationS3` fixture helper `makeBackend(uint64_t cap, TokenType)` — the cap
   belongs to the object storage now, so the signature becomes
   `makeBackend(TokenType token_type = TokenType::Generation)` and its seven call sites drop
   `/*cap=*/1024`, a don't-care in all seven.
3. `CASBackendGeneration.PublishBlobAboveFormerGenerationCapUsesOrdinaryMultipart` — here the cap is
   the point (publication must ignore it), so it moves onto the storage rather than disappearing.
4. `CASBackendGeneration.PublishBlobSucceedsWithoutResponseGeneration` — same, with cap 1.

Give the storage helper a cap parameter:

```cpp
std::shared_ptr<DB::S3ObjectStorage> makeGenerationS3ObjectStorageForTest(
    FakeGenerationS3Client *& out_client,
    bool force_multipart = false,
    std::optional<UInt64> conditional_put_cap = {})
{
    ...
    if (conditional_put_cap)
        settings->auth_settings[S3AuthSetting::gcs_max_conditional_put_bytes] = *conditional_put_cap;
    ...
}
```

The `...` lines are the helper's existing body, unchanged. Pass `/*conditional_put_cap=*/16` and
`/*conditional_put_cap=*/1` at sites 3 and 4, and add the
`extern const S3AuthSettingsUInt64 gcs_max_conditional_put_bytes;` declaration to the file's
namespace block.

- [ ] **Step 5b: Add the test the move actually needs**

No test today asserts that the cap is *enforced* on a conditional write. The fixture comment claims
"single-PUT cap enforcement ... exercised for real", but every test that passes a cap asserts
something else and passes a don't-care value. The move is the moment to add it, and it is cheap: the
fixture already builds a real `S3ObjectStorage` over `FakeGenerationS3Client`, so the whole path is
exercisable — CAS sets the flag, `writeObject` reads the cap from the storage's own settings,
`WriteBufferFromS3` enforces it.

```cpp
/// The moved cap, end to end: a conditional write on a generation store stays in ONE PUT up to the
/// cap the OBJECT STORAGE carries, and refuses rather than silently taking the multipart path above
/// it -- GCS enforces no precondition on CompleteMultipartUpload.
TEST(CASBackendGeneration, ConditionalWriteHonoursTheObjectStorageConditionalPutCap)
{
    (void)getContext();
    FakeGenerationS3Client * client = nullptr;
    auto storage = makeGenerationS3ObjectStorageForTest(
        client, /*force_multipart=*/false, /*conditional_put_cap=*/64);
    ObjectStorageBackend backend(storage, ObjectStorageBackend::Mode::Native);
    backend.setNativeTokenTypeForTest(TokenType::Generation);

    const String small(32, 'a');
    EXPECT_NO_THROW(backend.casPut("p/gen/under-cap", small, std::nullopt, ObjectMeta{}));
    EXPECT_EQ(client->put_object_calls, 1u);
    EXPECT_EQ(client->create_multipart_calls, 0u);

    const String large(4096, 'b');
    try
    {
        backend.casPut("p/gen/over-cap", large, std::nullopt, ObjectMeta{});
        FAIL() << "a conditional write above the cap must refuse, not go multipart";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NOT_IMPLEMENTED);
    }
    EXPECT_EQ(client->create_multipart_calls, 0u);
}
```

Read `casPut`'s exact signature in `CasObjectStorageBackend.h` before writing the assertions and use
the real one — the shape above is the conditional-write entry point as of writing, and the test uses
what is there, not this sketch.

- [ ] **Step 6: Remove the field and its producer**

In `src/IO/WriteSettings.h`, delete `s3_single_part_upload_max_bytes_override` and its comment; keep
`s3_force_single_part_upload` and extend its comment to say that the size ceiling for such a write
comes from the object storage's own `gcs_max_conditional_put_bytes`.

In `CasObjectStorageBackend.h`, drop the third constructor parameter and the
`conditional_single_put_cap` member. In `CasObjectStorageBackend.cpp`, drop the member initialiser,
and in `conditionalWriteSettings` set only:

```cpp
    if (native_token_type == TokenType::Generation)
        ws.s3_force_single_part_upload = true;
```

In `S3ObjectStorage.cpp::writeObject`, replace the block that read the removed field:

```cpp
    if (write_settings.s3_force_single_part_upload)
    {
        /// A conditional write on a generation-token store must stay in ONE buffered part, so the
        /// single-PUT path remains available up to the configured ceiling.
        const UInt64 cap = s3_settings.get()->auth_settings[S3AuthSetting::gcs_max_conditional_put_bytes];
        request_settings[S3RequestSetting::max_single_part_upload_size] = cap;
        request_settings[S3RequestSetting::min_upload_part_size] = cap;
    }
```

Add `extern const S3AuthSettingsUInt64 gcs_max_conditional_put_bytes;` to that file's
`namespace S3AuthSetting` block, in the same shape `diskSettings.cpp` uses.

- [ ] **Step 7: Remove the CAS-side setting and keep the old scanner happy**

In `ContentAddressedSettings.cpp`: delete the `gcs_max_conditional_put_bytes` `DECLARE` line, and add
the key to `non_cas_keys`:

```cpp
    "gcs_max_conditional_put_bytes",
```

with a comment saying it is an S3 client setting read by the object storage. **This one line is what
makes the commit bisectable:** without it, the still-scanning loader hands the key to
`BaseSettings::set` and every configuration carrying it fails to start at this commit. Task 2 deletes
`non_cas_keys` entirely, this line with it.

In `ContentAddressedMetadataStorage.h/.cpp`: delete the `gcs_max_conditional_put_bytes` member, its
`extern` declaration, its constructor initialiser, and pass two arguments at the
`std::make_shared<Cas::ObjectStorageBackend>` call site.

In `gtest_cas_settings.cpp`: delete `CASContentAddressedSettings.ConditionalPutCapParsesAndDefaults`
(replaced by the new `S3AuthSettingsConfig` test) and
`CASContentAddressedSettings.LegacyTokenProducingPutCapNameRejected` (it pinned the absence of an
alias for a CAS setting that no longer exists; Task 2 adds the check that belongs now — that
`cas_gcs_max_conditional_put_bytes` is not a CAS setting).

In `WriteBufferFromS3.cpp`, in the `createMultipartUpload` exception text, change "the disk setting
gcs_max_conditional_put_bytes" to "the disk's `gcs_max_conditional_put_bytes` S3 setting".

- [ ] **Step 8: Build**

BUILD with `build_task1_final.log`, then ANALYZE.

- [ ] **Step 9: Run the CAS gate and the new suite**

```bash
build/src/unit_tests_dbms --gtest_filter='CAS*' > build/test_task1_cas.log 2>&1
echo "GTEST_EXIT=$?"
build/src/unit_tests_dbms --gtest_filter='S3AuthSettingsConfig*:WriteBufferFromS3*:WBS3*' \
    > build/test_task1_s3.log 2>&1
echo "GTEST_EXIT=$?"
```

Dispatch one analysis subagent per log. Both must be green.

- [ ] **Step 10: Commit**

```bash
git add src/IO/S3Defines.h src/IO/S3AuthSettings.cpp src/IO/WriteSettings.h \
    src/IO/WriteBufferFromS3.cpp src/IO/tests/gtest_s3_auth_settings.cpp \
    src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp \
    src/Disks/tests/gtest_cas_backend_generation.cpp src/Disks/tests/gtest_cas_settings.cpp
git commit -F - <<'MSG'
fix: move the GCS conditional-PUT cap from CAS to the S3 settings

`gcs_max_conditional_put_bytes` is a property of the GCS conditional-write
dialect, not a CAS policy: it takes effect only on a generation-token store,
and what CAS owns is the policy that such a write must not go multipart. That
policy is already a per-write flag. Only the number was in the wrong place.

The setting moves to `S3AuthSettings`, beside `gcs_issue_compose_request`,
keeping its config spelling, and `S3ObjectStorage` applies it itself when the
write carries the flag. `WriteSettings::s3_single_part_upload_max_bytes_override`
had exactly one producer and disappears with it.

The key is added to the CAS `non_cas_keys` skip-list in this commit so that the
still-scanning CAS loader keeps accepting it; the whole list is removed next.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
git log --oneline -1
```

---

## Task 2: The `cas_` namespace, the migration window, and `skip_access_check` {#task-2}

Parts 1 and 2 of the specification, in one commit. Part 2 cannot go separately: `skip_access_check`
is deliberately absent from `non_cas_keys` today because it *is* a CAS setting, so demoting it while
the scanner still runs would make the bare key unknown — the same hazard Task 1 handled with a
temporary list entry.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
- Modify: `src/Disks/tests/gtest_cas_settings.cpp`

**Interfaces:**
- Consumes: Task 1's removal of `gcs_max_conditional_put_bytes` from the CAS settings.
- Produces: `ContentAddressedSettings::skipAccessCheck() const -> bool`.
- Produces: the config-key contract every later task sweeps to — `cas_<setting name>` for the
  twenty-five names in the specification's rename table.

- [ ] **Step 1: Write the failing tests**

Replace `CASContentAddressedSettings.ObjectStorageKeysSkipped` — which enumerated the foreign keys
CAS happened to tolerate — with a test that asserts CAS ignores whole classes it can no longer know
about, and add the window, duplicate and demotion tests. In `src/Disks/tests/gtest_cas_settings.cpp`:

```cpp
/// The point of this test is that none of these names appears anywhere in CAS code. It is not an
/// enumeration to be extended when a backend adds a setting; it samples the classes that a
/// name-based skip-list provably cannot cover.
TEST(CASContentAddressedSettings, ForeignKeysAreNeverInspected)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        /// Generic disk and object-storage layer.
        "<type>object_storage</type><object_storage_type>s3</object_storage_type>"
        "<metadata_type>cas</metadata_type><endpoint>http://x/y</endpoint>"
        "<path>cas_pool/</path><name>cas_test_disk</name>"
        "<use_fake_transaction>1</use_fake_transaction>"
        /// The field report: a legal S3 client setting that was missing from the old skip-list.
        "<http_keep_alive_timeout>60</http_keep_alive_timeout>"
        "<http_keep_alive_max_requests>100</http_keep_alive_max_requests>"
        "<connect_timeout_ms>1000</connect_timeout_ms><session_token>t</session_token>"
        /// S3 request settings, which on a disk carry the `s3_` prefix -- not one of them was in
        /// the old skip-list, so none of them could be set on a CAS disk.
        "<s3_retry_attempts>7</s3_retry_attempts><s3_max_put_rps>100</s3_max_put_rps>"
        /// Open-ended forms a name list cannot express: repeated elements and an arbitrary suffix.
        "<header>X-A: 1</header><header>X-B: 2</header>"
        "<access_header>X-C: 3</access_header><user_alice>alice</user_alice>"
        /// Subtrees.
        "<proxy><uri>http://proxy:8080</uri></proxy>"
        "<server_side_encryption_kms_config><key_id>k</key_id></server_side_encryption_kms_config>"
        /// Azure spellings -- a backend a CAS disk could not previously be configured over at all.
        "<account_name>acct</account_name><container_name>c</container_name>"
        "<connection_string>DefaultEndpointsProtocol=http;</connection_string>");
    ContentAddressedSettings s;
    EXPECT_NO_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros));
}

/// The migration window: an unprefixed configuration -- the shape that exists in external CI/CD
/// scripts today -- still loads, and its values still land.
TEST(CASContentAddressedSettings, LegacySpellingStillLoadsDuringMigrationWindow)
{
    auto cfg = makeConfig(
        "<server_root_id>srv1</server_root_id><gc_shards>4</gc_shards>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
    EXPECT_EQ(s[ContentAddressedSetting::gc_shards].value, 4u);
}

/// A partially migrated block loads, and the warning is the ONLY signal that a key was written in
/// the superseded spelling -- so the warning is asserted, not assumed. `ScopedCasGcLogCapture` in
/// `gtest_cas_ref_catalog.cpp` is the pattern this copies: swap the logger's channel for a
/// `Poco::StreamChannel` over a string stream, restore it in the destructor.
namespace
{
class ScopedCasSettingsLogCapture
{
public:
    ScopedCasSettingsLogCapture()
        : logger(getLogger("ContentAddressedSettings"))
        , channel(new Poco::StreamChannel(stream))
        , old_channel(logger->getChannel())
        , old_level(logger->getLevel())
    {
        logger->setChannel(channel.get());
        logger->setLevel("warning");
    }

    ~ScopedCasSettingsLogCapture()
    {
        logger->setChannel(old_channel);
        logger->setLevel(old_level);
    }

    String captured() const { return stream.str(); }

private:
    LoggerPtr logger;
    std::ostringstream stream;
    Poco::AutoPtr<Poco::StreamChannel> channel;
    Poco::AutoPtr<Poco::Channel> old_channel;
    int old_level;
};

size_t countOccurrences(const String & haystack, const String & needle)
{
    size_t n = 0;
    for (size_t at = haystack.find(needle); at != String::npos; at = haystack.find(needle, at + 1))
        ++n;
    return n;
}
}

TEST(CASContentAddressedSettings, PartialMigrationLoadsAndReportsEveryLegacyKey)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<gc_shards>4</gc_shards><gc_interval_sec>7</gc_interval_sec>");
    ContentAddressedSettings s;
    String captured;
    {
        ScopedCasSettingsLogCapture capture;
        s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
        captured = capture.captured();
    }
    EXPECT_EQ(s[ContentAddressedSetting::gc_shards].value, 4u);
    EXPECT_EQ(s[ContentAddressedSetting::gc_interval_sec].value, 7u);

    /// ONE aggregated report per disk, naming EVERY superseded key -- not one line per key, and not
    /// a line that names only the first one found.
    EXPECT_EQ(countOccurrences(captured, "superseded unprefixed spelling"), 1u);
    EXPECT_NE(captured.find("gc_shards"), String::npos);
    EXPECT_NE(captured.find("gc_interval_sec"), String::npos);
}

/// The negative half. Without it, a warning emitted unconditionally would still pass the test above.
TEST(CASContentAddressedSettings, FullyMigratedBlockWarnsAboutNothing)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id><cas_gc_shards>4</cas_gc_shards>");
    ContentAddressedSettings s;
    ScopedCasSettingsLogCapture capture;
    s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
    EXPECT_EQ(capture.captured().find("superseded"), String::npos);
}

TEST(CASContentAddressedSettings, BothSpellingsOfOneSettingRejected)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_gc_shards>4</cas_gc_shards><gc_shards>8</gc_shards>");
    ContentAddressedSettings s;
    try
    {
        s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
        FAIL() << "expected the ambiguous pair to be rejected";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
    }
}

/// Poco renders a repeated element as `name`, `name[1]`. Without an explicit repeat-index split the
/// second copy looks foreign, the first value wins, and a configuration that fails loudly today
/// starts succeeding quietly.
TEST(CASContentAddressedSettings, RepeatedKeyRejectedInEitherSpelling)
{
    for (const std::string & spelling : {std::string("gc_shards"), std::string("cas_gc_shards")})
    {
        SCOPED_TRACE(spelling);
        auto cfg = makeConfig(
            "<cas_server_root_id>srv1</cas_server_root_id>"
            "<" + spelling + ">4</" + spelling + ">"
            "<" + spelling + ">8</" + spelling + ">");
        ContentAddressedSettings s;
        try
        {
            s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
            FAIL() << "expected a repeated key to be rejected";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
        }
    }
}

/// `skip_access_check` is shared with the generic disk layer, so it keeps its bare spelling and is
/// not a CAS setting. Both halves of that need pinning.
TEST(CASContentAddressedSettings, SkipAccessCheckKeepsItsBareSpelling)
{
    auto with = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<skip_access_check>1</skip_access_check>");
    ContentAddressedSettings s;
    s.loadFromConfig(*with, "disk", "/scratch", "/scratch", identity_macros);
    EXPECT_TRUE(s.skipAccessCheck());

    auto prefixed = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_skip_access_check>1</cas_skip_access_check>");
    ContentAddressedSettings rejected;
    try
    {
        rejected.loadFromConfig(*prefixed, "disk", "/scratch", "/scratch", identity_macros);
        FAIL() << "expected `cas_skip_access_check` to be unknown";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::UNKNOWN_SETTING);
    }
}

/// The GCS cap is an S3 setting now, so its prefixed spelling is not a CAS setting either.
TEST(CASContentAddressedSettings, PrefixedGcsCapIsNotACasSetting)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_gcs_max_conditional_put_bytes>4096</cas_gcs_max_conditional_put_bytes>");
    ContentAddressedSettings s;
    try
    {
        s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
        FAIL() << "expected the prefixed cap name to be unknown";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::UNKNOWN_SETTING);
    }
}
```

Also convert every remaining test in the file to the `cas_` spelling — `DefaultsAndOverridesLand`,
`ValidateFailsClosed`, `RelativeScratchPathAnchored`, `AbsentScratchPathUsesDefaultVerbatim` — and
change the two rejection tests to the prefixed spelling, because an unprefixed removed name is now
foreign and therefore ignored rather than rejected:

- `RemovedCacheSettingsAreRejected`: `cas_deduplication_cache_bytes`, `cas_deduplication_head_first_min_bytes`.
- `UnknownKeyRejected`: `cas_gc_shardz`.

- [ ] **Step 2: Build and run the tests to confirm they fail**

BUILD with `build_task2_red.log`, ANALYZE, then TEST with `--gtest_filter='CAS*'` into
`build/test_task2_red.log` and dispatch the analysis subagent.

Expected reds, and this list is the check that the tests are testing something: the two
`ForeignKeysAreNeverInspected` / prefixed-spelling groups fail with `UNKNOWN_SETTING` because the
scanner still rejects unknown keys; `SkipAccessCheckKeepsItsBareSpelling` fails to compile until
`skipAccessCheck` exists. Compile failure blocks the runtime reds, so expect to see it first and the
rest after Step 3.

- [ ] **Step 3: Add the accessor so the rest can fail at runtime rather than at compile time**

In `ContentAddressedSettings.h`, beside `blobHashAlgo`:

```cpp
    /// Shared with the generic disk layer, which reads the same unprefixed key and ORs it with the
    /// server-level flag before the generic access check. This one governs the CAS capability probe
    /// only; the two scopes are deliberately distinct.
    bool skipAccessCheck() const;
```

In `ContentAddressedSettings.cpp`, add `bool skip_access_check_cached = false;` to
`ContentAddressedSettingsImpl` beside `blob_hash_algo_cached`, and the accessor beside `blobHashAlgo`.
Leave the `DECLARE` line in place for this step so the rest of the reds are runtime reds.

Rebuild into `build_task2_red2.log`, ANALYZE, re-run the gate into `build/test_task2_red2.log`, and
confirm the remaining reds are the expected `UNKNOWN_SETTING` / missing-value failures.

- [ ] **Step 4: Implement the namespace and the window**

In `ContentAddressedSettings.cpp`, delete `non_cas_keys` and its comment block in full. Fix the
includes in the same edit: the new code needs `<Common/logger_useful.h>`, `<fmt/ranges.h>` for
`fmt::join`, `<vector>` and `<string_view>`, and `<set>` becomes unused when `non_cas_keys` goes.
Add `UNKNOWN_SETTING` to the file's `ErrorCodes` block, and add to the anonymous namespace:

```cpp
/// Poco renders repeated XML elements as `name`, `name[1]`, `name[2]` -- the convention
/// `StorageURL` and `HTTPDictionarySource` both handle when reading repeated `<header>` elements.
/// A key of ours that appears twice must be recognised by its base name rather than passed over as
/// foreign, or the first value would silently win where a duplicate used to be an error.
struct ConfigKeyName
{
    std::string_view base;
    bool repeated = false;
};

ConfigKeyName splitRepeatIndex(const std::string & key)
{
    const auto bracket = key.find('[');
    if (bracket == std::string::npos)
        return {key, false};
    return {std::string_view(key).substr(0, bracket), true};
}

constexpr std::string_view CAS_KEY_PREFIX = "cas_";
}
```

Replace the scan loop in `loadFromConfig`:

```cpp
    Poco::Util::AbstractConfiguration::Keys config_keys;
    config.keys(config_prefix, config_keys);

    std::vector<std::string> legacy_names;

    for (const std::string & key : config_keys)
    {
        const auto [base, repeated] = splitRepeatIndex(key);

        if (base.starts_with(CAS_KEY_PREFIX))
        {
            if (repeated)
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "content_addressed disk `{}`: `{}` is set more than once", config_prefix, base);
            impl->set(base.substr(CAS_KEY_PREFIX.size()), config.getString(config_prefix + "." + key));
        }
        else if (ContentAddressedSettingsImpl::hasBuiltin(base))
        {
            if (repeated)
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "content_addressed disk `{}`: `{}` is set more than once", config_prefix, base);
            legacy_names.emplace_back(base);
        }
        /// Anything else belongs to another consumer of this disk block -- the object storage, the
        /// generic disk layer, the proxy resolver -- and is neither read nor judged here.
    }

    /// The unprefixed spelling is accepted for a bounded period, because configurations using it
    /// already exist outside this repository. Deleting this block is what closes that period: an
    /// unprefixed CAS setting name then throws instead, naming the spelling to use.
    if (!legacy_names.empty())
        LOG_WARNING(getLogger("ContentAddressedSettings"),
            "content_addressed disk `{}`: {} use the superseded unprefixed spelling and are applied "
            "for now; write them with the `cas_` prefix. Support for the unprefixed spelling will be "
            "removed.", config_prefix, fmt::join(legacy_names, ", "));

    for (const std::string & key : legacy_names)
    {
        if (impl->isChanged(key))
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "content_addressed disk `{}`: both `{}` and `cas_{}` are set; remove the unprefixed "
                "one", config_prefix, key, key);
        impl->set(key, config.getString(config_prefix + "." + key));
    }
```

Then demote `skip_access_check`: delete its `DECLARE` line, and read it directly, immediately after
the loops above:

```cpp
    /// Not a CAS setting: the generic disk layer reads this same unprefixed key for its own access
    /// check, so one spelling must serve both.
    impl->skip_access_check_cached = config.getBool(config_prefix + ".skip_access_check", false);
```

In `ContentAddressedMetadataStorage.cpp`, replace the member initialiser
`settings_[ContentAddressedSetting::skip_access_check].value` with `settings_.skipAccessCheck()`, and
delete the now-unused `extern` declaration.

Finally rewrite the `loadFromConfig` contract comment in `ContentAddressedSettings.h`: it currently
describes the skip-set and cites `FileCacheSettings::non_cache_keys` as the model. It must now say
that only `cas_`-prefixed keys are consumed, that every other key belongs to another consumer of the
shared disk block and is untouched, and that the unprefixed spelling is temporarily accepted.

- [ ] **Step 5: Build and run the gate to green**

BUILD with `build_task2_green.log`, ANALYZE, TEST with `--gtest_filter='CAS*'` into
`build/test_task2_green.log`, and dispatch the analysis subagent. Every CAS suite must pass.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.h \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
    src/Disks/tests/gtest_cas_settings.cpp
git commit -F - <<'MSG'
fix: give the CAS disk settings their own config-key namespace

A CAS disk block is shared with the object storage, the generic disk layer and
the proxy resolver, and the CAS settings were the only consumer in it that
scanned every key and rejected whatever was not in a hand-written skip-list. Any
legal key nobody had enumerated failed server startup: all `s3_*` request
settings, most S3 client settings, repeated `<header>` elements, `<proxy>`, and
every Azure key, so a CAS disk over Azure could not be configured at all.

The CAS settings now live under a `cas_` config-key prefix and nothing else in
the block is read, so the skip-list is gone with nothing in its place. A
mis-spelled CAS setting is still rejected, because that check is now made over a
namespace CAS actually owns.

The unprefixed spelling is accepted for a bounded period, since configurations
using it already exist in external CI/CD scripts; each disk reports its
superseded keys once. A key written in both spellings, or twice in one spelling,
is rejected rather than resolved silently.

`skip_access_check` keeps its bare spelling and stops being a CAS setting: the
generic disk layer reads the same key, and one key must not become two.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
git log --oneline -1
```

---

## Task 3: Sweep the XML configurations {#task-3}

Sweep class 1. From here on the migration window means each sweep commit is independently
revertable: both spellings work, so a partial sweep cannot break a lane.

**Files:** every `*.xml` carrying a CAS disk block — 54 files, ~175 `<key>` elements. Enumerate them
rather than trusting this count:

```bash
git grep -l "server_root_id" -- '*.xml'
```

**Interfaces:** consumes Task 2's key contract; produces nothing later tasks read.

- [ ] **Step 1: Save the file list, then rename the twenty-five keys**

The tree is shared with other sessions and carries untracked artifacts, so every sweep task records
the files it is going to touch **before** touching them, and stages only that list:

```bash
mkdir -p tmp/sweep
git grep -l "server_root_id" -- '*.xml' | sort > tmp/sweep/task3_files.txt
wc -l tmp/sweep/task3_files.txt
```

For each of the twenty-five names in the specification's rename table, rewrite `<name>` and
`</name>` to `<cas_name>` and `</cas_name>` in the files on that list only. Do **not** touch
`skip_access_check`, `gcs_max_conditional_put_bytes`, or any generic key (`path`, `name`, `type`,
`endpoint`, `object_storage_type`, `metadata_type`).

- [ ] **Step 2: Verify no CAS key is left unprefixed and none was double-prefixed**

```bash
git grep -nE "<(server_root_id|gc_enabled|gc_interval_sec|gc_shards|scratch_path|blob_hash|blob_hash_allow_new|staging_backend|part_folder_validate|part_folder_cache_bytes|part_folder_cache_max_entries|part_folder_cache_max_entry_bytes|manifest_decode_cache_bytes|manifest_sweep_list_budget_keys|manifest_sweep_delete_budget_keys|gc_snapshot_generations_to_keep|gc_meta_pool_size|gc_round_graduation_budget|gc_round_redelete_budget|gc_round_sweep_namespace_budget|gc_round_sweep_recovery_op_budget|gc_round_ref_cleanup_budget|gc_round_prefix_wholesale_budget|gc_round_handoff_prefix_wholesale_budget|gc_round_outcome_entry_budget)>" -- '*.xml'
git grep -n "cas_cas_" -- '*.xml'
```

Both must print nothing.

- [ ] **Step 3: Start a server on the swept stateless configuration**

`tests/config/config.d/cas_storage_policy_for_merge_tree_by_default.xml` and
`cas_s3_storage_policy_for_merge_tree_by_default.xml` are the only in-tree CAS disks defined by
server configuration rather than by a test, and one of them is the source of a past startup outage.
Run the local-backend stateless lane, which installs them:

```bash
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
ninja -C build clickhouse > build/build_server_task3.log 2>&1
echo "NINJA_EXIT=$?"
python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test 04278 04279 \
    > build/test_task3_server.log 2>&1
```

ANALYZE `build/build_server_task3.log`, then dispatch a subagent to read
`build/test_task3_server.log` and report whether the server started and the per-test verdicts. A
server that fails to start shows up as "Server died" / connection refused, not as a diff.

- [ ] **Step 4: Commit**

```bash
git status --porcelain -- $(cat tmp/sweep/task3_files.txt)
git add --pathspec-from-file=tmp/sweep/task3_files.txt
git status --porcelain --cached
git commit -F - <<'MSG'
test: move the XML CAS disk configurations to the `cas_` prefix

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
git log --oneline -1
```

---

## Task 4: Sweep the inline `disk(...)` stateless tests {#task-4}

Sweep class 2 — 32 files (18 `.sh`, 13 `.sql`, 1 `.py`), ~54 assignment lines. The form is
multi-line, so the sites are `key = value` lines inside a `disk(` argument list, not a single-line
pattern.

**Files:** save the list first, as in Task 3:

```bash
git grep -lE "metadata_type[[:space:]]*=[[:space:]]*'?cas'?" -- 'tests/*' ':!*.md' ':!*.xml' \
    | sort > tmp/sweep/task4_files.txt
wc -l tmp/sweep/task4_files.txt
```

- [ ] **Step 1: Rename the twenty-five keys in those files**

Only lines of the form `^\s*<name>\s*=` inside a `disk(` argument list. Leave `name =`, `path =`,
`type =`, `object_storage_type =`, `metadata_type =` and `use_fake_transaction =` alone — they are
generic disk keys, and `05015_cas_reject_fake_transaction` depends on the last one reaching the
generic check.

- [ ] **Step 2: Verify**

```bash
git grep -nE "^[[:space:]]*(server_root_id|gc_enabled|gc_interval_sec|gc_shards|scratch_path|blob_hash|staging_backend|part_folder_validate)[[:space:]]*=" \
    -- $(git grep -lE "metadata_type[[:space:]]*=[[:space:]]*'?cas'?" -- 'tests/*' ':!*.md' ':!*.xml')
```

Must print nothing.

- [ ] **Step 3: Run every stateless test this task touched — derived, not hand-picked**

A hand-written selector list silently under-covers a sweep. Derive it from the files that actually
changed:

```bash
SEL=$(grep '^tests/queries/0_stateless/' tmp/sweep/task4_files.txt \
      | sed -E 's|.*/([0-9]{5})_.*|\1|' | sort -u | tr '\n' ' ')
echo "selectors: $SEL"
python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test $SEL \
    > build/test_task4_stateless.log 2>&1
```

One `--test` flag, space separated — repeating the flag silently keeps only the last value. If the
list is long enough to be unwieldy, split it into batches of roughly ten and write each batch to its
own log file; never drop a selector to shorten the run.

Dispatch a subagent per log to report the `Failed: N, Passed: M` tally and every non-OK test. The run
is finished only at `Run script finished`; per-worker "N tests passed" lines appear mid-run and are
not the verdict. Confirm the count of tests actually run matches the number of selectors — a selector
that matches nothing is a silent gap, not a pass.

- [ ] **Step 4: Commit**

```bash
git status --porcelain -- $(cat tmp/sweep/task4_files.txt)
git add --pathspec-from-file=tmp/sweep/task4_files.txt
git status --porcelain --cached
git commit -F - <<'MSG'
test: move the inline CAS disk() stateless tests to the `cas_` prefix

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
git log --oneline -1
```

---

## Task 5: Sweep the Python-assembled configurations {#task-5}

Sweep classes 3 and 4 — XML fragments inside Python strings, and the dictionary keys that a generic
renderer turns into `<key>value</key>`. These are the classes a `*.xml` grep does not see, and the
most likely place for a missed site.

**Files:**
- `tests/integration/test_cas_gcs/test.py`, `tests/integration/test_gcs_live/test.py` — XML fragments
  in string literals, e.g. `"<staging_backend>s3</staging_backend>"`.
- `utils/ca-soak/scenarios/framework/cluster_boot.py` and its `render_tuned_config` call sites —
  dictionary keys.
- `utils/ca-soak/scenarios/tests/test_render_tuned_config.py` — asserts rendered XML text.
- `utils/ca-soak/docker-compose*.yml` and `utils/ca-soak/configs/*.xml` if Task 3 did not already
  cover them.

- [ ] **Step 1: Save the file list, then rename in string literals and dictionary keys**

Record the files the two greps below name, into `tmp/sweep/task5_files.txt`, before editing:

```bash
git grep -nE "<(server_root_id|gc_enabled|gc_interval_sec|gc_shards|scratch_path|blob_hash|staging_backend|part_folder_validate|manifest_decode_cache_bytes)>" \
    -- 'tests/*' 'utils/*' ':!*.xml' ':!*.md'
git grep -nE "\"(server_root_id|gc_enabled|gc_interval_sec|gc_shards|scratch_path|blob_hash|staging_backend|part_folder_validate|manifest_decode_cache_bytes)\"[[:space:]]*:" \
    -- 'tests/*' 'utils/*' ':!*.md'
```

Rewrite every hit. A hit inside a comment or a docstring is prose, not a config key — most textual
`gc_shards=` occurrences under `utils/ca-soak` are exactly that. Rewrite prose only where it
describes the config key an operator writes.

- [ ] **Step 2: Run the ca-soak renderer's own tests**

```bash
python3 -m pytest utils/ca-soak/scenarios/tests/test_render_tuned_config.py -q \
    > build/test_task5_renderer.log 2>&1
echo "PYTEST_EXIT=$?"
```

Dispatch a subagent to summarise the log.

- [ ] **Step 3: Run the CAS integration lanes that carry these fixtures**

```bash
ninja -C build clickhouse > build/build_server_task5.log 2>&1
echo "NINJA_EXIT=$?"
strings build/programs/clickhouse | grep -c cas_server_root_id
python3 -m ci.praktika run "Integration tests (amd_binary, 1/5)" \
    --test test_cas_s3 test_cas_gc_s3 test_cas_gcs test_cas_gc_sharded test_cas_shared_pool \
    > build/test_task5_integration.log 2>&1
```

The `strings` count must be non-zero — that is the positive check that the binary the lane runs
carries this change; ninja on `unit_tests_dbms` does not relink it. Praktika **exits 0 even when
integration tests fail**, so dispatch a subagent to read the pytest summary and the per-test
verdicts out of the log body rather than trusting the exit code.

- [ ] **Step 4: Commit**

`utils/ca-soak` currently holds another session's edit to `RUN_HISTORY.md` and a pile of untracked
`.db` and `.yml` run artifacts. Staging the directory would sweep all of it into this commit, so
stage the recorded list and nothing else:

```bash
git status --porcelain -- $(cat tmp/sweep/task5_files.txt)
git add --pathspec-from-file=tmp/sweep/task5_files.txt
git status --porcelain --cached
git commit -F - <<'MSG'
test: move the Python-assembled CAS configurations to the `cas_` prefix

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
git log --oneline -1
```

---

## Task 6: Sweep the C++ test literals {#task-6}

Sweep class 5 — XML in C++ string literals, plus the configuration examples in the subtree README.

**Files:** `src/Disks/tests/gtest_cas_part_folder_access.cpp`,
`src/Disks/tests/gtest_cas_retirement_sweep.cpp`, `src/Disks/tests/gtest_cas_s3_staging.cpp`,
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/README.md`.
`gtest_cas_settings.cpp` was already converted in Task 2.

- [ ] **Step 1: Rename, counting closing tags rather than opening ones**

```bash
git grep -nE "</(server_root_id|gc_enabled|gc_interval_sec|gc_shards|scratch_path|blob_hash|blob_hash_allow_new|staging_backend|part_folder_validate)>" -- 'src/**'
```

**Trap:** in `src/`, angle brackets are usually *not* XML. `Formats/CasLayout.h`,
`CasServerRootFormats.h`, `Pool/CasPool.cpp` and `StorageSystemContentAddressedMounts.h` use
`<server_root_id>` as a path placeholder — `pool/<server_root_id>/...` — and must not be touched.
Counting closing tags separates the two: 15 real literals against 32 raw matches for that key. Use
the closing-tag grep above as the work list, not an opening-tag grep.

- [ ] **Step 2: Build and run the gate**

BUILD with `build_task6.log`, ANALYZE, TEST with `--gtest_filter='CAS*'` into
`build/test_task6.log`, dispatch the analysis subagent.

- [ ] **Step 3: Commit**

```bash
git add src/Disks/tests/gtest_cas_part_folder_access.cpp \
    src/Disks/tests/gtest_cas_retirement_sweep.cpp \
    src/Disks/tests/gtest_cas_s3_staging.cpp \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/README.md
git status --porcelain --cached
git commit -F - <<'MSG'
test: move the CAS config literals in C++ tests to the `cas_` prefix

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
git log --oneline -1
```

---

## Task 7: Documentation {#task-7}

18 files under `docs/en`, ~167 occurrences of the twenty-five names.

**Files:** record the list first, as in Task 3:

```bash
git grep -lE "server_root_id|gc_interval_sec|part_folder_validate" -- 'docs/en/**' \
    | sort > tmp/sweep/task7_files.txt
```

It covers `docs/en/antalya/cas/configuration.md`, `docs/en/operations/storing-data.md`,
`docs/en/antalya/cas/quick-start.md`, and the architecture and operations pages.

- [ ] **Step 1: Rename the keys in prose, tables and examples**

- [ ] **Step 2: Rewrite the claim this change refutes**

`docs/en/operations/storing-data.md` currently states: "Since the disk element already scopes every
key to this disk, none of the keys below carry a redundant `cas_`/`ca_` prefix." That sentence is the
rationale this design disproves — the block is shared, not CAS-scoped. Replace it with one or two
sentences saying that the disk element is read by several components at once, so the CAS settings
carry a `cas_` prefix and every other key belongs to the object storage or the generic disk layer.
The same rationale appears as a comment above the `DECLARE` list in `ContentAddressedSettings.cpp`
and must be rewritten there too, in this commit.

- [ ] **Step 3: Document the migration window and the two unprefixed keys**

In `docs/en/antalya/cas/configuration.md`, add a short section stating that the unprefixed spelling
is accepted for now and reported at startup, that it will stop being accepted, and that two keys are
deliberately not prefixed: `skip_access_check`, shared with the generic disk layer, and
`gcs_max_conditional_put_bytes`, which is an S3 client setting. Add the sentence the specification
asks for on scope: the server-level `skip_access_check` flag skips the generic disk access check,
while the CAS capability probe is governed by the disk's own key.

- [ ] **Step 4: Do not move anchors**

`docs/en/antalya/cas/configuration.md` carries `### Choosing \`blob_hash\` {#choosing-blob-hash}` and
`docs/en/operations/storing-data.md` links to it. Rename the key text inside the header; leave the
anchor slug alone. Then verify no link broke:

```bash
git grep -n "choosing-blob-hash" -- 'docs/**'
```

- [ ] **Step 5: Commit**

```bash
git status --porcelain -- $(cat tmp/sweep/task7_files.txt)
git add --pathspec-from-file=tmp/sweep/task7_files.txt \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp
git status --porcelain --cached
git commit -F - <<'MSG'
docs: document the `cas_` config-key prefix and its migration window

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
git log --oneline -1
```

---

## Task 8: End-to-end proof on the keys that used to fail {#task-8}

The reachable claim is "CAS no longer rejects a backend's settings", and the field report is its
concrete case: `<http_keep_alive_timeout>` in a CAS disk block failed server startup. Prove exactly
that, in a lane that already exists.

**Not Azure.** A CAS pool cannot be hosted on Azure regardless of config parsing: any non-Local
object storage opens in `Native` mode (`ContentAddressedMetadataStorage::openPoolView`), and Native
requires enforced conditional operations — `removeObjectIfTokenMatches`, whose base implementation
throws by design so the capability probe fails closed, and `supportsRetryProfile(SingleAttempt)`,
whose base implementation returns false. Azure implements neither. "The CAS parser accepts Azure
keys" and "Azure can host a CAS pool" are different claims, and only the first is true after this
change. The Azure spellings are covered where they belong — in the unit test of Task 2, which asserts
they are not inspected.

**Files:**
- Modify: `tests/integration/test_cas_s3/configs/storage_conf.xml`
- Modify: `tests/integration/test_cas_s3/test.py`

- [ ] **Step 1: Add the previously rejected keys to the CAS disk block**

Into the existing CAS disk element in `tests/integration/test_cas_s3/configs/storage_conf.xml`, and
nothing else about the disk changes:

```xml
                <!-- Settings of the underlying object storage, in the same element as the CAS
                     settings. Each of these used to fail server startup on a CAS disk. -->
                <http_keep_alive_timeout>60</http_keep_alive_timeout>
                <http_keep_alive_max_requests>100</http_keep_alive_max_requests>
                <connect_timeout_ms>5000</connect_timeout_ms>
                <s3_retry_attempts>7</s3_retry_attempts>
                <s3_max_single_read_retries>3</s3_max_single_read_retries>
                <header>X-Cas-Test: 1</header>
```

Deliberately **not** included: `<proxy>`, which would route requests through a host that does not
exist, and any credential key, which would change what the lane authenticates as. The point is keys
that are parsed and inert, not keys that change behaviour.

- [ ] **Step 2: Add the test that names what is being proven**

In `tests/integration/test_cas_s3/test.py`:

```python
def test_disk_accepts_backend_settings_that_used_to_be_rejected(started_cluster):
    """The CAS disk block carries settings of its underlying object storage.

    Before the `cas_` namespace, the CAS settings scanned the whole disk element and rejected every
    key they did not recognise, so `http_keep_alive_timeout` -- the mitigation suggested in #2243 --
    failed server startup. The server having started with the config this module installs is most of
    the proof; this test states it, and checks the disk is actually usable rather than merely
    present.
    """
    node = started_cluster.instances["node"]
    assert node.query(
        "SELECT count() FROM system.disks WHERE name = 'cas_s3'"
    ).strip() == "1"
    node.query("DROP TABLE IF EXISTS t_foreign_settings SYNC")
    node.query(
        "CREATE TABLE t_foreign_settings (a UInt64) ENGINE = MergeTree ORDER BY a "
        "SETTINGS storage_policy = 'cas_s3'"
    )
    node.query("INSERT INTO t_foreign_settings SELECT number FROM numbers(100)")
    assert node.query("SELECT sum(a) FROM t_foreign_settings").strip() == "4950"
    node.query("DROP TABLE t_foreign_settings SYNC")
```

Check the instance name, disk name and policy name against the module's own fixture before writing
them — the names above follow the module's convention but the fixture is the authority.

- [ ] **Step 3: Establish what the test proves, mechanically**

A failing-first run is not available: there is no prior binary to fail against. Establish the claim
from history instead, which is checkable rather than asserted:

```bash
git show <commit-before-task-2>:src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp \
    | sed -n '/non_cas_keys = {/,/};/p'
```

`http_keep_alive_timeout`, `http_keep_alive_max_requests`, `connect_timeout_ms`, `header` and every
`s3_`-prefixed name must be absent from the set it prints. That absence is what made each of them
fail server startup, and it is why these particular keys are the ones in the config.

- [ ] **Step 4: Run the lane**

```bash
ninja -C build clickhouse > build/build_server_task8.log 2>&1
echo "NINJA_EXIT=$?"
strings build/programs/clickhouse | grep -c cas_server_root_id
python3 -m ci.praktika run "Integration tests (amd_binary, 1/5)" --test test_cas_s3 \
    > build/test_task8.log 2>&1
```

Praktika exits 0 even when integration tests fail; dispatch a subagent to read the pytest summary out
of the log body.

- [ ] **Step 5: Commit**

```bash
git add tests/integration/test_cas_s3/configs/storage_conf.xml tests/integration/test_cas_s3/test.py
git status --porcelain --cached
git commit -F - <<'MSG'
test: pin that a CAS disk carries its backend's own settings

`http_keep_alive_timeout` in a CAS disk block used to fail server startup, which
is what blocked the mitigation suggested in Altinity/ClickHouse#2243. The CAS
integration disk now carries that key and five more of the same class, so the
lane fails if the disk block ever stops accepting them.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
git log --oneline -1
git rev-parse --abbrev-ref HEAD
```

## Task 9: The full gate {#task-9}

Nothing new is written here. This task exists because the most likely defect in this change is a
missed sweep site, and no unit test can see one.

- [ ] **Step 1: Unit lanes**

```bash
ninja -C build unit_tests_dbms > build/build_gate.log 2>&1
echo "NINJA_EXIT=$?"
build/src/unit_tests_dbms --gtest_filter='CAS*' > build/gate_cas.log 2>&1
echo "GTEST_EXIT=$?"
build/src/unit_tests_dbms --gtest_filter='S3AuthSettingsConfig*:WriteBufferFromS3*:WBS3*' \
    > build/gate_s3.log 2>&1
echo "GTEST_EXIT=$?"
```

ANALYZE the build log and each test log with its own subagent.

- [ ] **Step 2: Stateless lanes, both backends, with derived selectors**

```bash
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
ninja -C build clickhouse > build/build_gate_server.log 2>&1
echo "NINJA_EXIT=$?"
SEL=$(grep '^tests/queries/0_stateless/' tmp/sweep/task4_files.txt \
      | sed -E 's|.*/([0-9]{5})_.*|\1|' | sort -u | tr '\n' ' ')
echo "selectors: $SEL"
python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test $SEL \
    > build/gate_stateless_local.log 2>&1
python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed s3 storage, parallel)" \
    --test $SEL > build/gate_stateless_cas_s3.log 2>&1
```

The selector list is derived from the recorded sweep list, not hand-picked, and both lanes get the
same list. Split into batches of about ten if a single invocation is unwieldy, and give each batch
its own log; never shorten the list.

The CA-s3 lane needs `ci/tmp/rustfs`; without it the server-start step fails with "rustfs binary not
found". Restore it rather than skipping the lane, and never delete it during `ci/tmp` cleanup. Only
one praktika job runs per worktree at a time — a second returns "Docker container 'praktika_...' is
already running", so these two invocations are sequential, not concurrent.

Some reference diffs on the CA-s3 lane are known false positives unrelated to CAS; treat a diff as
real only after confirming the test touches a CAS disk.

- [ ] **Step 3: Integration lanes**

```bash
python3 -m ci.praktika run "Integration tests (amd_binary, 1/5)" \
    --test test_cas_s3 test_cas_gc_s3 test_cas_gcs test_cas_gc_sharded test_cas_shared_pool \
    > build/gate_integration.log 2>&1
```

Read the pytest summary from the log body; the exit code is 0 even on failures.

- [ ] **Step 4: Confirm the sweep is complete — all five classes, not just XML**

An XML-only check passes while an inline `disk(...)`, a Python dictionary key or a string-literal
fragment is still unprefixed. Run one probe per class. Define the name list once:

```bash
KEYS='server_root_id|gc_enabled|gc_interval_sec|gc_shards|scratch_path|blob_hash|blob_hash_allow_new|staging_backend|part_folder_validate|part_folder_cache_bytes|part_folder_cache_max_entries|part_folder_cache_max_entry_bytes|manifest_decode_cache_bytes|manifest_sweep_list_budget_keys|manifest_sweep_delete_budget_keys|gc_snapshot_generations_to_keep|gc_meta_pool_size|gc_round_graduation_budget|gc_round_redelete_budget|gc_round_sweep_namespace_budget|gc_round_sweep_recovery_op_budget|gc_round_ref_cleanup_budget|gc_round_prefix_wholesale_budget|gc_round_handoff_prefix_wholesale_budget|gc_round_outcome_entry_budget'
```

Class 1, XML elements:

```bash
git grep -nE "<($KEYS)>" -- '*.xml'
```

Class 2, inline `disk(...)` assignments, inside the files that build one:

```bash
git grep -nE "^[[:space:]]*($KEYS)[[:space:]]*=" -- $(cat tmp/sweep/task4_files.txt)
```

Class 3, XML fragments in string literals, and class 4, dictionary keys:

```bash
git grep -nE "<($KEYS)>" -- 'tests/*' 'utils/*' ':!*.xml' ':!*.md'
git grep -nE "\"($KEYS)\"[[:space:]]*:" -- 'tests/*' 'utils/*' ':!*.md'
```

Class 5, C++ string literals, by closing tag so the path placeholders in `Formats/`, `Pool/` and
`src/Storages/System` do not register:

```bash
git grep -nE "</($KEYS)>" -- 'src/**'
```

Documentation:

```bash
git grep -nE "\`($KEYS)\`" -- 'docs/en/**'
```

And the double-prefix probe, which catches a sed applied twice:

```bash
git grep -n "cas_cas_" -- ':!docs/superpowers'
```

Every one of these must print nothing except the known path-placeholder sites, which the class-5
probe already excludes by shape. Anything else is a missed sweep site.

- [ ] **Step 5: Confirm the tree is otherwise unchanged, against the recorded baseline**

`git status` in this tree is never clean, so compare with the baseline recorded before the first
edit rather than expecting emptiness:

```bash
git status --porcelain > tmp/sweep/final_status.txt
diff tmp/sweep/baseline_status.txt tmp/sweep/final_status.txt
```

The only differences should be files this plan named. Anything else — especially under
`utils/ca-soak`, which carries another session's edits and untracked run artifacts — is something a
sweep picked up that it should not have.

- [ ] **Step 6: Confirm every commit landed on the intended branch**

```bash
git rev-parse --abbrev-ref HEAD
git log --oneline cas-gc-rebuild -12
```

All commits of this plan must appear on `cas-gc-rebuild`. This worktree is shared: another session
checking out its own branch moves it, and commits made afterwards land on that branch instead, which
`git log --oneline -1` alone does not reveal.

## Task 10: File the window-closing follow-up {#task-10}

The BACKLOG entry `{#cas-config-prefix-window}` exists. This task is the second placement, because an
item that lives only in a ledger is one context loss away from never happening.

- [ ] **Step 1: Add the follow-up to the pull request description**

State the trigger — the CAS configurations in the `clickhouse-regression` suite are on the `cas_`
spelling — and what closing the window is: delete the warning and the loop that applies legacy values
in `ContentAddressedSettings::loadFromConfig`, replace them with a throw naming every unprefixed CAS
setting name found and its `cas_` spelling, rewrite
`CASContentAddressedSettings.LegacySpellingStillLoadsDuringMigrationWindow` and
`PartialMigrationLoadsAndReportsEveryLegacyKey` into one test asserting `UNKNOWN_SETTING`, and drop
the deprecation section from `docs/en/antalya/cas/configuration.md` and
`docs/en/operations/storing-data.md`.

- [ ] **Step 2: Verify the BACKLOG entry still matches what shipped**

```bash
grep -A 20 "cas-config-prefix-window" docs/superpowers/cas/BACKLOG/operability-and-introspection.md
```

If Task 2 named things differently from the entry, fix the entry, not the memory of it.

---

## Self-Review {#self-review}

**Specification coverage.** Part 1 is Task 2; Part 2 is Task 2 (it cannot be separate — see the task
preamble); Part 3 is Task 1. One specification claim is narrowed by Task 8 and must be narrowed in
the specification too: a CAS disk accepts any backend's *settings*, which is not the same as CAS
running on any backend. The landing order the specification requires is Task 1 → Task 2, stated
in the global constraints and in both task preambles. The five sweep classes map to Tasks 3, 4, 5, 6
and the documentation class to Task 7. Every gate lane the specification names has a step: unit
(Tasks 1, 2, 6, 9), `WriteBufferFromS3` (Tasks 1, 9), stateless inline `disk(...)` (Tasks 4, 9),
server configuration (Task 3), integration S3/GCS (Tasks 5, 9), the new Azure smoke (Task 8). The
window-closing task is Task 10.

**Symbols used here that were read in the tree first**, with where:

| Symbol | Read at |
|---|---|
| `BaseSettings::hasBuiltin(std::string_view)` | `src/Core/BaseSettings.h:221` |
| `BaseSettings::isChanged(std::string_view)` | `src/Core/BaseSettings.h:200` |
| `BaseSettings::set(std::string_view, const Field &)` | `src/Core/BaseSettings.h:191` |
| `S3::DEFAULT_MAX_SINGLE_PART_UPLOAD_SIZE` | `src/IO/S3Defines.h:23` |
| `S3AuthSetting::gcs_issue_compose_request` + the `CLIENT_SETTINGS` list | `src/IO/S3AuthSettings.cpp` |
| the `extern const S3AuthSettingsBool …` declaration shape | `src/Disks/DiskObjectStorage/ObjectStorages/S3/diskSettings.cpp:56` |
| `WriteSettings::s3_single_part_upload_max_bytes_override` | `src/IO/WriteSettings.h:82` |
| its consumer in `writeObject` | `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:343-350` |
| `S3ObjectStorage::s3_settings` is `MultiVersion<S3Settings>` | `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h:198` |
| `ObjectStorageBackend` ctor, third parameter defaulted | `.../Backend/CasObjectStorageBackend.h:66` |
| its single three-argument call site | `.../ContentAddressedMetadataStorage.cpp:714` |
| `conditionalWriteSettingsForTest` | used at `src/Disks/tests/gtest_cas_backend_generation.cpp:269` |
| `loadFromConfig` has **no** disk-name parameter | `.../ContentAddressedSettings.h:64-69` |
| `blobHashAlgo` / `stagingBackend` / `partFolderValidate` accessor shape | `.../ContentAddressedSettings.h:79-81` |
| `makeConfig`, `identity_macros` test helpers | `src/Disks/tests/gtest_cas_settings.cpp:31-37` |
| Poco renders repeats as `name`, `name[1]`, `name[2]` | stated in `src/Storages/StorageURL.cpp:1683` and `src/Dictionaries/HTTPDictionarySource.cpp:254` |
| `unit_tests_dbms` globs `gtest*.cpp` under `src` with `CONFIGURE_DEPENDS` | `src/CMakeLists.txt:901-908` |
| Azure disk keys as an operator writes them | `tests/config/config.d/azure_storage_conf.xml` |
| `openPoolView` picks `Native` for any non-Local storage | `.../ContentAddressedMetadataStorage.cpp` |
| `removeObjectIfTokenMatches` and `supportsRetryProfile` base implementations refuse by design | `src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h` |
| `makeGenerationS3ObjectStorageForTest`, `FakeGenerationS3Client`, `makeBackend` | `src/Disks/tests/gtest_cas_backend_generation.cpp:464-511` |
| the four sites passing a cap to the backend constructor | same file, lines 269, 508, 519, 547 |
| `ScopedCasGcLogCapture`, the log-capture pattern | `src/Disks/tests/gtest_cas_ref_catalog.cpp:178-195` |

Three symbols I had used in the design sketch turned out not to exist and are corrected here:
`loadFromConfig` has no disk name, so the warning identifies the disk by `config_prefix`;
`ContentAddressedSettingsImpl::hasBuiltin` is the static inherited from `BaseSettings`, not a member
of the public class; and `splitRepeatIndex` is new code introduced in Task 2, not something to call.

**Corrections carried in from review, so they are not re-derived.**

- An earlier draft claimed the `writeObject` line had no unit seam and would need an upstream API
  widened to test. That was wrong: `makeGenerationS3ObjectStorageForTest` in
  `gtest_cas_backend_generation.cpp` already builds a real `S3ObjectStorage` over
  `FakeGenerationS3Client`, so the whole path is testable today. Step 5b adds that test, and the same
  fixture turns out to have **four** sites passing the cap into the constructor rather than one, all
  of which Task 1 must convert or the file stops compiling.
- An earlier draft proved the change with a CAS-over-Azure round trip. Azure cannot host a CAS pool
  at all: a non-Local object storage opens in `Native` mode, which requires enforced conditional
  operations Azure does not implement, and the base implementations refuse by design so the
  capability probe fails closed. The parser accepting Azure keys and Azure hosting a pool are
  different claims; only the first is true, and it is proven where it belongs, in a unit test.
  Task 8 proves the reachable claim end to end instead, on the keys from the field report.
- The plan's own "stage exact paths" rule was violated by four of its own commit steps
  (`git add tests/integration utils/ca-soak` and similar). `utils/ca-soak` holds another session's
  edits and untracked run artifacts, so those steps would have committed foreign changes. Every
  sweep task now records its file list first and stages from that list.

**Placeholders.** None. The nine code blocks are complete; the sweep tasks give an enumeration
command instead of a file list because the list is long and must be re-derived at execution time
rather than trusted from this document.
