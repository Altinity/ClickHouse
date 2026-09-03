---
description: 'Implementation plan for the CAS backend request contract: a keyed string transport, an admitted operation handle whose every call names a retry policy, one deadline-bound engine, a five-alternative write result, and the migration of every production and test site onto it, batched so that rebuilds and test reruns happen at checkpoints rather than per site.'
sidebar_label: 'Backend request contract plan'
sidebar_position: 44
slug: /superpowers/plans/cas-backend-request-contract-plan
title: 'CAS backend request contract — implementation plan'
doc_type: 'guide'
---

# CAS backend request contract — implementation plan {#cas-backend-request-contract-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the CAS backend's caller-facing interface — raw conditional writes callable without a retry, string tokens anyone can construct — with a transport callable only through a key, an operation handle admitted under a fence, one deadline-bound retry engine for reads and writes, and a five-alternative write result; migrate every production and test site onto it; delete the old controller.

**Architecture:** `Backend` becomes a string-in/string-out transport whose every method takes a `TransportAccess` only `CasRequests` (and, for the migration window, `Backend` itself) can construct. `CasRequests` owns a backend and a `Fence`; `admit()`/`resume(generation)` return a `CasOperation` carrying the admitted generation and an optional liveness predicate; the operation's verbs (`read`, `head`, `list`, `forEachListedKey`, `remove`, `removeCurrent`, `probeSentinel`, `stream`, `publish`, `create`, `replace`, `readModifyWrite`, `readModifyWriteOnPresence`) each take a `Retry`. The engine re-checks admission before every attempt, before every sleep and after a proven commit, settles every conflict and ambiguity by one exact read, and reports `Committed | Declined | Conflict | Refused | GaveUp`. An upstream slice under `src/IO` and `ObjectStorages/S3` makes every control-plane request one physical attempt. The old controller and the new engine coexist during the migration; the lock deletes the old one.

**Tech Stack:** C++23 (ClickHouse tree, Allman braces), gtest (`unit_tests_dbms`, suites prefixed `CAS`), `InMemoryBackend` test doubles, the CAS gtest fake S3 client, `utils/ca-soak` for the soak, `ci.praktika` for stateless.

**Spec:** `docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md` (revision 12, commit `b9df0724639`). **Census:** `docs/superpowers/plans/2026-09-03-cas-backend-request-contract-census.md` (the per-file site checklists this plan cites). **Current declarations:** `tmp/unattended-2026-09-03/interfaces.md` (verbatim extracts the engine is rebuilt from; regenerate from the source if the file is gone).

## Global Constraints {#global-constraints}

- Branch `cas-gc-rebuild`, shared worktree: never rebase or amend; one commit per task (a task may commit twice only where the task text says so); never `git add -A`; add named files only. Commit trailers: `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01DAu8zEhBXrwRKTPhpXgZnS`.
- The gtest gate filter is exactly `CAS*`, never widened; every new suite name starts with `CAS`.
- Build command shape (always redirected, always with the marker): `ninja -C build_debug unit_tests_dbms > build_debug/build_<task>.log 2>&1; echo NINJA_EXIT=$? >> build_debug/build_<task>.log`. Test command shape: `build_debug/src/unit_tests_dbms --gtest_filter='<filter>' > build_debug/test_<task>.log 2>&1; echo GTEST_EXIT=$? >> build_debug/test_<task>.log`. Logs are read by a subagent, never into the implementer's context. No `-j`, no `nproc`.
- **Batching rule (the user's):** migration tasks (Tasks 8–17) do NOT build or run tests. They rewrite code and adjust tests. Checkpoint tasks (Tasks 7, 10, 13, 16, 19, 21) build once, run the gate, and fix what the compiler and the gate report. A migration task's deliverable is a commit that is *intended* to compile; the checkpoint proves it.
- Every new API has its own unit tests (Task 5's file). Existing tests are adjusted, never deleted, unless the spec retires the seam they test (`Range`, `ObjectMeta`, `GetStreamResult::token`, the old controller's own file), and then the retirement is named in the commit message.
- No fallback paths; a failure propagates. No `sleep` in C++ to fix a race. `LOGICAL_ERROR` is an "exception", not a "crash", in messages and docs.
- Upstream slice (Task 2): `src/IO`, `src/Disks/DiskObjectStorage/ObjectStorages/S3` and `IObjectStorage.h` only; no CAS identifier; additive.
- Test fault-injection overrides move to the new signatures **in the same commit as the production site they instrument**, and the commit shows the fault still fires (the test that depends on the override stays red-then-green across the change).
- The three planes construct their own `CasRequests`: mount plane over the mount fence (plus one over an open fence for the farewell), GC plane over an open fence, tools over an open fence.
- Retry policies: `standard()` unless the spec's "Where each verb goes" table says otherwise (`untilLeaseSafe` for renew and reads inside it; `once` for the `slotOccupy` callers, the heartbeat write, the maintenance-state catch-path write; `within(10 s)` for the farewell).

---

## File structure {#file-structure}

New files (all under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/` unless noted):

| file | responsibility |
|---|---|
| `CasIncarnation.h` | `Incarnation` (opaque, key- and backend-bound), `PersistedIncarnation`, the per-dialect grammar `isIncarnationValue(dialect, value)`, `Dialect` (renamed `TokenType` stays until the lock; `Dialect` is an alias until then) |
| `CasTransportAccess.h` | `TransportAccess` (private ctor, friends `CasRequests` and `Backend`; uncopyable) |
| `CasRetry.h` / `.cpp` | `Retry{deadline_ms, single_attempt}`, `within`/`standard`/`untilLeaseSafe`/`once`, `backoff(attempt)` with full jitter |
| `CasWriteResult.h` | `Observation`, `Committed`, `Declined`, `Conflict`, `Refused`, `GaveUp`, `WriteResult`, `orThrow`, `Removal`, `Object`, `Meta` |
| `CasFence.h` | `Fence` (`generation()`, `admit(generation, needed_ms)`, `checkOrThrow`) as a small value type over three callables, plus `Fence::open()` |
| `CasRequests.h` / `.cpp` | `CasRequests` (backend + fence; `admit`, `resume`), `CasOperation` (the verbs), the engine (admission schedule, classification, resolve, backoff), `isDefinitelyRefusedWrite` |
| `CasThrottlingBackend.h` | `ThrottlingBackend` decorator: first-per-key and every-n-th modes |
| `src/Disks/tests/gtest_cas_requests.cpp` | unit tests for everything above (suites `CASRequests*`, `CASRetry*`, `CASIncarnation*`, `CASFence*`) |
| `src/Disks/tests/gtest_cas_throttling_gate.cpp` | the coverage gate: pool-level scenarios under `ThrottlingBackend` first-per-key |
| `src/IO/ObjectStorageRequestMode.h` | the moved enum (upstream slice) |

Modified files, by task: `CasBackend.h` (new primitives beside the legacy ones; `migrationAccess`), `CasInMemoryBackend.{h,cpp}`, `CasObjectStorageBackend.{h,cpp}`, `CasInstrumentedBackend.h`, the 22 production files of census §1, the 85 test files of census §5a, the upstream slice's seven files, `ErrorCodes.cpp`, `ProfileEvents.cpp`, `ContentAddressedSettings` (two parsed settings), docs.

## Rebuild map {#rebuild-map}

| checkpoint | after task | what is built and run |
|---|---|---|
| CP1 | Task 1 | `unit_tests_dbms`; gate `CAS*` |
| CP2 | Task 2 | `unit_tests_dbms` + the IO gtests named in Task 2; gate `CAS*` and `S3*`/`ReadBufferFromS3*` |
| CP3 | Task 6 (new API complete) | gate `CAS*` — the new suites plus everything old, unchanged |
| CP4 | Tasks 8–9 (batch 1) | gate `CAS*` |
| CP5 | Tasks 11–12 (batch 2) | gate `CAS*` |
| CP6 | Tasks 14–15 (batch 3) | gate `CAS*` |
| CP7 | Tasks 17–18 (batch 4) | gate `CAS*` |
| CP8 | Task 20 (lock) | gate `CAS*` |
| CP9 | Task 22 (coverage gate + rename) | gate `CAS*` |
| soak, stateless | Tasks 23–24 | `utils/ca-soak` 30 minutes; the CA-s3 stateless lane |

---

### Task 1: Step 1 — safety inside the backend {#task-1}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp` (the legacy write entry points `putIfAbsent`/`putOverwrite`/`casPut`, `deleteExact`, `nativeHead`, `list`, `tokenFromWriteResult`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h` (add `isValidTokenValue`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.cpp` (`deleteExact`, `putOverwrite`, `casPut` reject an empty/`*`/comma token as `LOGICAL_ERROR`)
- Modify: `src/Common/ErrorCodes.cpp` (add `CAS_WRITE_UNATTRIBUTED`, `CAS_DELETE_MARKER`)
- Test: `src/Disks/tests/gtest_cas_backend.cpp` (adjust `CASBackend.NullBackendShapeAndDefaults`; add the grammar tests)

**Interfaces:**
- Consumes: today's `Backend` (interfaces.md §1), `ObjectStorageBackend::normalizeTokenValue`, `isValidGenerationTokenValue`, `tokenFromWriteResult`.
- Produces: `static bool ObjectStorageBackend::isValidTokenValue(TokenType, const String &)` — the grammar every later task reuses (Task 3 moves it to `CasIncarnation.h` as `isIncarnationValue`); error codes `CAS_WRITE_UNATTRIBUTED` (1012) and `CAS_DELETE_MARKER` (1013).

- [ ] **Step 1: Add the two error codes**

In `src/Common/ErrorCodes.cpp`, after the last `M(1011, PARTITION_EXPORT_FAILED) \` line:

```cpp
    M(1012, CAS_WRITE_UNATTRIBUTED) \
    M(1013, CAS_DELETE_MARKER) \
```

- [ ] **Step 2: Write the failing grammar tests**

Append to `src/Disks/tests/gtest_cas_backend.cpp` (the file already includes `CasObjectStorageBackend.h` and has an `ObjectStorageBackend` over `LocalObjectStorage` fixture; reuse its `makeLocalBackend()` helper or the nearest equivalent in the file):

```cpp
TEST(CASBackendGrammar, RejectsEmptyStarAndListTokensOnEveryMutation)
{
    auto backend = makeLocalBackend();   // Native-over-Local fixture already used in this file
    const DB::Cas::Token empty{"", DB::Cas::TokenType::ETag};
    const DB::Cas::Token star{"*", DB::Cas::TokenType::ETag};
    const DB::Cas::Token list{"\"a\", \"b\"", DB::Cas::TokenType::ETag};
    for (const auto & bad : {empty, star, list})
    {
        expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { backend->putOverwrite("k", "v", bad); });
        expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { backend->casPut("k", "v", bad); });
        expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { backend->deleteExact("k", bad); });
    }
}

TEST(CASBackendGrammar, GenerationDialectAcceptsOnlyCanonicalPositiveDecimal)
{
    using DB::Cas::ObjectStorageBackend;
    using DB::Cas::TokenType;
    EXPECT_TRUE(ObjectStorageBackend::isValidTokenValue(TokenType::Generation, "123"));
    EXPECT_FALSE(ObjectStorageBackend::isValidTokenValue(TokenType::Generation, "0"));
    EXPECT_FALSE(ObjectStorageBackend::isValidTokenValue(TokenType::Generation, "00123"));
    EXPECT_FALSE(ObjectStorageBackend::isValidTokenValue(TokenType::Generation, "\"123\""));
    EXPECT_FALSE(ObjectStorageBackend::isValidTokenValue(TokenType::Generation, "12a"));
    EXPECT_TRUE(ObjectStorageBackend::isValidTokenValue(TokenType::ETag, "\"abc\""));
    EXPECT_FALSE(ObjectStorageBackend::isValidTokenValue(TokenType::ETag, " * "));
    EXPECT_FALSE(ObjectStorageBackend::isValidTokenValue(TokenType::ETag, "a,b"));
    EXPECT_TRUE(ObjectStorageBackend::isValidTokenValue(TokenType::Emulated, "7"));
    EXPECT_FALSE(ObjectStorageBackend::isValidTokenValue(TokenType::Emulated, ""));
}

TEST(CASBackendGrammar, NamelessWriteResponseThrowsWriteUnattributed)
{
    /// The fake S3 client in this file's fixtures (or `FakeS3ClientReturningNoETag` in
    /// gtest_cas_object_storage_backend.cpp, whichever exists) answers PutObject with no ETag.
    auto backend = makeNativeBackendOverFakeS3ReturningNoETag();
    expectThrowsCode(DB::ErrorCodes::CAS_WRITE_UNATTRIBUTED, [&] { backend->putIfAbsent("k", "v"); });
}
```

If `makeNativeBackendOverFakeS3ReturningNoETag` does not exist, build it in the test file from the existing fake S3 client class the CAS gtests use for `ObjectStorageBackend` in Native mode (grep `FakeS3Client` / `MockS3Client` under `src/Disks/tests/gtest_cas_*`): subclass it, override `PutObject` to return a result whose ETag is empty, and construct an `ObjectStorageBackend(Mode::Native)` over an `S3ObjectStorage` built on it, exactly as the existing Native fixtures do.

- [ ] **Step 3: Run the new tests to verify they fail**

Run: `build_debug/src/unit_tests_dbms --gtest_filter='CASBackendGrammar.*' > build_debug/test_task1_red.log 2>&1; echo GTEST_EXIT=$? >> build_debug/test_task1_red.log` (after a build; the first build of this task is the one below — so run this step after Step 5's build with the implementation temporarily stubbed, or accept the compile error as the red state and record it in the report).
Expected: FAIL (`isValidTokenValue` undefined; empty token accepted).

- [ ] **Step 4: Implement the grammar and the guards**

In `CasObjectStorageBackend.h`, public section, replace `isValidGenerationTokenValue`'s private declaration with a public static grammar:

```cpp
    /// The per-dialect grammar a response value must meet to be an incarnation. Generation: canonical
    /// positive decimal AFTER the SDK ETag-field quote strip (no leading zero, not "0" — zero is the
    /// dialect's absence sentinel). ETag: non-empty, not "*" after trimming whitespace, no comma (a
    /// list matches any member). Emulated: non-empty.
    static bool isValidTokenValue(TokenType type, const String & value);
```

In `CasObjectStorageBackend.cpp`:

```cpp
bool ObjectStorageBackend::isValidTokenValue(TokenType type, const String & value)
{
    switch (type)
    {
        case TokenType::Generation:
        {
            if (value.empty() || value == "0")
                return false;
            if (value.size() > 1 && value.front() == '0')
                return false;
            return std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
        }
        case TokenType::ETag:
        {
            String trimmed = value;
            boost::algorithm::trim(trimmed);
            return !trimmed.empty() && trimmed != "*" && trimmed.find(',') == String::npos;
        }
        case TokenType::Emulated:
            return !value.empty();
    }
    return false;
}
```

At the top of `putOverwrite`, `casPut` (when `expected` is set) and `deleteExact`:

```cpp
    if (!mintingTypeMatches(expected.type) || !isValidTokenValue(expected.type, expected.value))
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS backend: refusing a conditional mutation of '{}' with a malformed token '{}' (dialect {}): "
            "an empty, wildcard or list token would turn the precondition into an unconditional write",
            key, expected.value, static_cast<int>(expected.type));
```

Where `nativeHead` and `list` mint tokens from a response (`tokenForHead`, `tokenForList`), reject a value that fails the grammar with `CORRUPTED_DATA` naming the key:

```cpp
    if (!isValidTokenValue(native_token_type, normalized))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS backend: the store answered for '{}' with a value '{}' that is not a valid {} incarnation",
            key, normalized, native_token_type == TokenType::Generation ? "generation" : "ETag");
```

Replace `tokenFromWriteResult`'s fallback `HEAD` (the branch taken when `etag` is empty) with:

```cpp
    throw Exception(ErrorCodes::CAS_WRITE_UNATTRIBUTED,
        "CAS backend: the store accepted a write of '{}' but returned no incarnation; the write may have "
        "committed and must be resolved by reading back", key);
```

In `CasInMemoryBackend.cpp`, add the same empty/`*`/comma guard (Emulated grammar: non-empty; plus `*` and comma refused) to `putOverwrite`, `casPut` (with `expected`) and `deleteExact`, throwing `LOGICAL_ERROR`.

- [ ] **Step 5: Build and run the gate (CP1)**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_task1.log 2>&1; echo NINJA_EXIT=$? >> build_debug/build_task1.log`
Then: `build_debug/src/unit_tests_dbms --gtest_filter='CAS*' > build_debug/test_task1_gate.log 2>&1; echo GTEST_EXIT=$? >> build_debug/test_task1_gate.log`
Expected: `NINJA_EXIT=0`, `GTEST_EXIT=0`. `CASBackend.NullBackendShapeAndDefaults` is the one existing test the spec expects to change (it asserts a default-constructed token round-trips through a mutation); adjust its expectation to the `LOGICAL_ERROR` refusal and say so in the commit.

- [ ] **Step 6: Commit**

```bash
git add src/Common/ErrorCodes.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.cpp src/Disks/tests/gtest_cas_backend.cpp
git commit -F - <<'MSG'
ca-backend: enforce the incarnation grammar at every mutation and mint, no fallback HEAD

Step 1 of the request contract: an empty, wildcard or list token is refused with LOGICAL_ERROR on
putOverwrite/casPut/deleteExact in all backends; a store value that fails the dialect grammar on HEAD
or list is CORRUPTED_DATA naming the key; a write response without an incarnation throws
CAS_WRITE_UNATTRIBUTED instead of issuing a HEAD. No caller changes.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01DAu8zEhBXrwRKTPhpXgZnS
MSG
```

---

### Task 2: Step 2 — the upstream slice {#task-2}

**Files:**
- Create: `src/IO/ObjectStorageRequestMode.h`
- Modify: `src/IO/WriteSettings.h` (remove the enum definition, include the new header; add `object_storage_attempt_timeout_ms`)
- Modify: `src/IO/ReadSettings.h` (add `object_storage_request_mode`, `object_storage_retry_profile`, `object_storage_attempt_timeout_ms`)
- Modify: `src/IO/ReadBufferFromS3.h`, `src/IO/ReadBufferFromS3.cpp` (`sendRequest` marks under the mode; `initialize` records the first ETag and sets `response_identity_changed`; `responseIdentityChanged()` accessor)
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h` (three retry-profile overloads with default bodies)
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h`, `.cpp` (`readObject` selects the client, pins `max_single_read_retries`, wraps the refresh callback; `readSmallObjectAndGetObjectMetadata` throws on drift; `getSingleAttemptClient(request_timeout_ms)`; the three overloads; `refreshAndRetryOnExpiredCredentials`)
- Test: `src/IO/tests/gtest_read_buffer_from_s3_drift.cpp` (new; suite `S3ReadDrift`) — or, if the IO tests have no fake S3 client, the CAS fake in `src/Disks/tests/gtest_cas_object_storage_backend.cpp` (suite name then `CASUpstreamSlice`); the plan accepts either, the report must say which.

**Interfaces:**
- Consumes: interfaces.md §7–8 (`readObject`, `writeObject`, `getSingleAttemptClient`, `readSmallObjectAndGetObjectMetadata`, `tryGetObjectMetadataImpl`, `iterate`, `removeObjectIfTokenMatches`, `getObjectMetadata`, `tryRefreshCredentialsViaCallback`, `nextImpl`, `processException`, `initialize`, `sendRequest`).
- Produces (used by Task 4's `ObjectStorageBackend` and by nothing else in CAS):
  - `ReadSettings::object_storage_request_mode`, `ReadSettings::object_storage_retry_profile`, `ReadSettings::object_storage_attempt_timeout_ms` (0 = the disk's default), `WriteSettings::object_storage_attempt_timeout_ms`.
  - `std::optional<ObjectMetadata> IObjectStorage::tryGetObjectMetadataWithNativeToken(const std::string & path, bool with_tags, ObjectStorageRetryProfile profile) const`
  - `ObjectStorageIteratorPtr IObjectStorage::iterate(const std::string & path_prefix, size_t max_keys, bool with_tags, const std::optional<std::string> & start_after, ObjectStorageRetryProfile profile) const`
  - `ConditionalRemoveResult IObjectStorage::removeObjectIfTokenMatches(const StoredObject & object, const std::string & etag, ObjectStorageRetryProfile profile)`
  - `S3ObjectStorage::readSmallObjectAndGetObjectMetadata` throws `CANNOT_READ_ALL_DATA`-class `Exception` with message containing `"response identity changed"` when a reissue answered with a different ETag.

- [ ] **Step 1: Move the enum**

Create `src/IO/ObjectStorageRequestMode.h`:

```cpp
#pragma once
#include <cstdint>

namespace DB
{

/// How a request is issued to an object storage. `NativeConditional` marks a request as carrying (or
/// eligible to carry) a storage-native conditional header, which lets a provider-specific client
/// translate `If-Match`/`ETag` into its own vocabulary (e.g. a GCS generation) on the request and the
/// response. Reads use it so that a plain GET answers with the same incarnation identity a HEAD does.
enum class ObjectStorageRequestMode : uint8_t
{
    Default,
    NativeConditional,
};

}
```

In `src/IO/WriteSettings.h`: delete the `enum class ObjectStorageRequestMode` definition, add `#include <IO/ObjectStorageRequestMode.h>`, and beside `object_storage_retry_profile` add:

```cpp
    /// Request timeout (send/receive inactivity bound) for the single-attempt client selected by
    /// `object_storage_retry_profile == SingleAttempt`. 0 = the storage's configured timeout.
    uint64_t object_storage_attempt_timeout_ms = 0;
```

In `src/IO/ReadSettings.h`, include both `IO/ObjectStorageRequestMode.h` and the header that declares `ObjectStorageRetryProfile` (leave the enum in `WriteSettings.h`; include it, or move `ObjectStorageRetryProfile` into the new header too — the implementer picks the one that compiles without a cycle and says which), and add to `ReadSettings`:

```cpp
    ObjectStorageRequestMode object_storage_request_mode = ObjectStorageRequestMode::Default;
    ObjectStorageRetryProfile object_storage_retry_profile = ObjectStorageRetryProfile::Default;
    uint64_t object_storage_attempt_timeout_ms = 0;
```

- [ ] **Step 2: Write the failing drift and single-attempt tests**

Create the test file (IO or CAS location per the note above). The fake client: a class deriving from the fake S3 client the CAS Native tests already use (find it with `grep -rn "class .*S3.*Client.*: public" src/Disks/tests/gtest_cas_*.cpp src/IO/tests/`), scripted so that successive `GetObject` calls return a chosen sequence of (status, ETag, body):

```cpp
TEST(CASUpstreamSlice, ReadSmallObjectThrowsWhenAReissueAnswersWithADifferentETag)
{
    auto client = std::make_shared<ScriptedGetObjectClient>();
    client->script({ {200, "\"e1\"", "AAAA", /*fail_mid_body=*/true}, {200, "\"e2\"", "BBBB", false} });
    auto storage = makeS3ObjectStorageOver(client);           // the fixture the CAS Native tests use
    DB::ReadSettings rs;
    rs.object_storage_request_mode = DB::ObjectStorageRequestMode::NativeConditional;
    /// Default profile here: the buffer's own 4-attempt loop is what straddles the replacement.
    expectThrowsMessageContains("response identity changed",
        [&] { storage->readSmallObjectAndGetObjectMetadata(DB::StoredObject("k"), rs, 1 << 20); });
}

TEST(CASUpstreamSlice, SingleAttemptProfileIssuesExactlyOneGetOnThrottle)
{
    auto client = std::make_shared<ScriptedGetObjectClient>();
    client->script({ {429, "", "", false}, {200, "\"e1\"", "AAAA", false} });
    auto storage = makeS3ObjectStorageOver(client);
    DB::ReadSettings rs;
    rs.object_storage_retry_profile = DB::ObjectStorageRetryProfile::SingleAttempt;
    EXPECT_ANY_THROW(storage->readSmallObjectAndGetObjectMetadata(DB::StoredObject("k"), rs, 1 << 20));
    EXPECT_EQ(client->getObjectCalls(), 1u);
}

TEST(CASUpstreamSlice, SingleAttemptClientCarriesTheRequestedTimeout)
{
    auto client = std::make_shared<ScriptedGetObjectClient>();
    auto storage = makeS3ObjectStorageOver(client);
    auto c = storage->getSingleAttemptClientForTest(/*request_timeout_ms=*/1234);
    EXPECT_EQ(c->getClientConfiguration().requestTimeoutMs, 1234);
    EXPECT_EQ(c->getClientConfiguration().retry_strategy.max_retries, 0);
}

TEST(CASUpstreamSlice, ExpiredTokenOnSingleAttemptReadInstallsTheRefreshedClientIntoTheStorage)
{
    auto client = std::make_shared<ScriptedGetObjectClient>();
    client->script({ {403, "", "ExpiredToken", false} });
    auto refreshed = std::make_shared<ScriptedGetObjectClient>();
    auto storage = makeS3ObjectStorageOver(client, /*credentials_refresh_callback=*/[&] { return refreshed; });
    DB::ReadSettings rs;
    rs.object_storage_retry_profile = DB::ObjectStorageRetryProfile::SingleAttempt;
    EXPECT_ANY_THROW(storage->readSmallObjectAndGetObjectMetadata(DB::StoredObject("k"), rs, 1 << 20));
    EXPECT_EQ(storage->currentClientForTest().get(), refreshed.get());
}

TEST(CASUpstreamSlice, HeadListRemoveOverloadsRefuseSingleAttemptOnTheBaseStorage)
{
    DB::LocalObjectStorage local(/*...the fixture's local storage args...*/);
    expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED,
        [&] { local.tryGetObjectMetadataWithNativeToken("k", false, DB::ObjectStorageRetryProfile::SingleAttempt); });
    EXPECT_NO_THROW(local.tryGetObjectMetadataWithNativeToken("k", false, DB::ObjectStorageRetryProfile::Default));
}
```

- [ ] **Step 3: Implement the slice**

`ReadBufferFromS3.h`: add members `std::optional<String> first_response_etag;` and `bool response_identity_changed = false;` and `bool responseIdentityChanged() const { return response_identity_changed; }`.

`ReadBufferFromS3::sendRequest`: after `S3::setClickhouseAttemptNumber(req, attempt);` add

```cpp
    if (read_settings.object_storage_request_mode == ObjectStorageRequestMode::NativeConditional)
        req.setNativeConditional();
```

`ReadBufferFromS3::initialize`: after `auto read_result = sendRequest(...)`:

```cpp
    const String etag = read_result.GetETag();
    if (!first_response_etag)
        first_response_etag = etag;
    else if (*first_response_etag != etag)
        response_identity_changed = true;   /// per reissue: A→B→A′ is caught at B
```

`S3ObjectStorage::readObject`: when `read_settings.object_storage_retry_profile == SingleAttempt`, select `getSingleAttemptClient(read_settings.object_storage_attempt_timeout_ms)` instead of `client.get()`, set `request_settings[S3RequestSetting::max_single_read_retries] = 1`, and hand the buffer a wrapping callback:

```cpp
    S3CredentialsRefreshCallback refresh = credentials_refresh_callback;
    if (single_attempt && credentials_refresh_callback)
        refresh = [this]() -> std::shared_ptr<const S3::Client>
        {
            auto new_client = credentials_refresh_callback();
            if (new_client)
                client.set(new_client);      /// install into the storage, so the next single-attempt request sees it
            return new_client;
        };
```

`S3ObjectStorage::readSmallObjectAndGetObjectMetadata`: after `copyDataMaxBytes`, before reading metadata:

```cpp
    auto * s3_buffer = dynamic_cast<ReadBufferFromS3 *>(buffer.get());
    if (s3_buffer->responseIdentityChanged())
        throw Exception(ErrorCodes::CANNOT_READ_ALL_DATA,
            "Object '{}' response identity changed between reissued GET requests; the bytes read are not from one incarnation",
            object.remote_path);
```

`S3ObjectStorage::getSingleAttemptClient(uint64_t request_timeout_ms)`: add the parameter (default 0 = keep the base timeout); key the cache on `(base, request_timeout_ms)`; when non-zero set `cfg.requestTimeoutMs = request_timeout_ms;` before cloning. Keep the zero-argument behaviour for `writeObject`, which now passes `write_settings.object_storage_attempt_timeout_ms`.

`IObjectStorage.h`: add the three overloads with default bodies:

```cpp
    virtual std::optional<ObjectMetadata> tryGetObjectMetadataWithNativeToken(
        const std::string & path, bool with_tags, ObjectStorageRetryProfile profile) const
    {
        if (profile == ObjectStorageRetryProfile::SingleAttempt)
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "{} does not support single-attempt metadata requests", getName());
        return tryGetObjectMetadataWithNativeToken(path, with_tags);
    }
```

and the same shape for `iterate(..., ObjectStorageRetryProfile)` and `removeObjectIfTokenMatches(..., ObjectStorageRetryProfile)`.

`S3ObjectStorage`: implement the three overloads by selecting `getSingleAttemptClient()` vs `client.get()` exactly as `writeObject` does, and wrap the `HEAD` and `DELETE` bodies (the two that issue inline) in:

```cpp
template <typename Fn>
auto S3ObjectStorage::refreshAndRetryOnExpiredCredentials(Fn && fn) const
{
    try
    {
        return fn();
    }
    catch (const S3Exception & e)
    {
        if (!e.isAccessTokenExpiredError() || !credentials_refresh_callback)
            throw;
        auto new_client = credentials_refresh_callback();
        if (!new_client)
            throw;
        client.set(std::move(new_client));
        return fn();
    }
}
```

(`iterate` constructs an iterator and issues nothing; it gets the client selection only.) Add `getSingleAttemptClientForTest(uint64_t)` and `currentClientForTest()` accessors for the tests.

- [ ] **Step 4: Build and run (CP2)**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_task2.log 2>&1; echo NINJA_EXIT=$? >> build_debug/build_task2.log`
Then: `build_debug/src/unit_tests_dbms --gtest_filter='CAS*:S3*:ReadBufferFromS3*' > build_debug/test_task2_gate.log 2>&1; echo GTEST_EXIT=$? >> build_debug/test_task2_gate.log`
Expected: `NINJA_EXIT=0`, `GTEST_EXIT=0`, the five new tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/IO/ObjectStorageRequestMode.h src/IO/WriteSettings.h src/IO/ReadSettings.h src/IO/ReadBufferFromS3.h src/IO/ReadBufferFromS3.cpp src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp <the test file>
git commit -F - <<'MSG'
IO: single-attempt reads that mark GET, detect body drift and keep credential refresh

`ObjectStorageRequestMode` moves to its own header and `ReadSettings` carries it, so a marked GET
answers with the same incarnation identity a HEAD does; `ReadBufferFromS3` records whether a reissue
answered with a different ETag and `readSmallObjectAndGetObjectMetadata` refuses mixed bodies;
`readObject` under `SingleAttempt` uses the single-attempt client with `max_single_read_retries`
pinned to one, a request timeout from the settings, and a refresh callback that installs the
refreshed client into the storage; `tryGetObjectMetadataWithNativeToken`, `iterate` and
`removeObjectIfTokenMatches` gain retry-profile overloads whose S3 bodies select the client and
whose base bodies refuse `SingleAttempt`.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01DAu8zEhBXrwRKTPhpXgZnS
MSG
```

---

### Task 3: The new types — `Incarnation`, `TransportAccess`, `Retry`, `WriteResult`, `Fence` {#task-3}

**Amendments (second review, binding — supersede the conflicting lines below):**
- `Retry` holds a *window*, not an absolute deadline: `struct Retry { uint64_t window_ms; std::optional<uint64_t> lease_deadline_ms /* absolute boot ms, already minus the margin */; bool single_attempt; static uint64_t backoff(uint32_t); static Retry within(uint64_t ms); static Retry standard(); static Retry untilLeaseSafe(uint64_t lease_deadline_ms, uint64_t margin_ms); static Retry once(); struct Bound { uint64_t deadline_ms; bool lease_bound; }; Bound bind(uint64_t now_ms) const; }` — `bind` (saturating) is called once at call entry with the engine's injected clock; `GaveUp::deadline_source` comes from `Bound::lease_bound`. No boot-clock dependency in `CasRetry`.
- No `CasIncarnationTestAccess`, no test friend on `Incarnation` — tests obtain values through admitted writes (Task 6). `CASIncarnation.RenderAndPersistedCompare` and the Committed/Declined arms of the `orThrow` test move to Task 6.
- `Incarnation::render()` ends its switch with `UNREACHABLE()`; no `"unknown:"`.
- `PersistedIncarnation` gains forward-only `static PersistedIncarnation capture(const Incarnation &)`; add `static_assert(!std::is_constructible_v<Incarnation, PersistedIncarnation>)`.
- All deadline arithmetic saturates (`now >= deadline → 0 remaining`; compare `needed <= remaining`, never `now + needed > deadline`).

**Files:**
- Create: `.../Backend/CasIncarnation.h`, `.../Backend/CasTransportAccess.h`, `.../Backend/CasRetry.h`, `.../Backend/CasRetry.cpp`, `.../Backend/CasWriteResult.h`, `.../Backend/CasFence.h`
- Test: `src/Disks/tests/gtest_cas_requests.cpp` (new; this task writes the type-level tests; Task 5 adds the engine tests to the same file)

**Interfaces:**
- Consumes: `TokenType` from `Primitives/CasTypes.h` (the dialect enum; `Dialect` is `using Dialect = TokenType;` until the lock renames it), `ObjectStorageBackend::isValidTokenValue` from Task 1 (moved here as `isIncarnationValue`; Task 1's static becomes a one-line forwarder).
- Produces (exact):

```cpp
namespace DB::Cas
{
using Dialect = TokenType;
bool isIncarnationValue(Dialect dialect, const String & value);

class Incarnation
{
public:
    Incarnation() = delete;
    bool operator==(const Incarnation &) const = default;
    String render() const;                       // "etag:<value>" | "generation:<value>" | "emulated:<value>"
    const String & key() const;
    Dialect dialect() const;
    uint64_t backendId() const;
private:
    friend class CasRequests;
    friend class CasIncarnationTestAccess;         // tests only: gtest_cas_requests.cpp
    Incarnation(uint64_t backend_id, String key, Dialect dialect, String value);
    const String & value() const;                  // the transport's own text; CasRequests reads it
    uint64_t backend_id_; String key_; Dialect dialect_; String value_;
};

struct PersistedIncarnation
{
    String dialect;    // "etag" | "generation" | "emulated"
    String value;
    bool matches(const Incarnation & live) const;   // live.render() == dialect + ":" + value
};

class TransportAccess
{
    friend class CasRequests;
    friend class Backend;                    // migration only; deleted at the lock (Task 20)
    TransportAccess() = default;
public:
    TransportAccess(const TransportAccess &) = delete;
    TransportAccess & operator=(const TransportAccess &) = delete;
};

struct Retry
{
    uint64_t deadline_ms;      // absolute, CasMountRuntime::bootMs() clock
    bool single_attempt;
    static uint64_t backoff(uint32_t attempt);            // full jitter: uniform(0, min(5000, 200 << (attempt-1)))
    static Retry within(uint64_t ms);
    static Retry standard();                              // within(90'000)
    static Retry untilLeaseSafe(uint64_t lease_deadline_ms, uint64_t safety_margin_ms);
    static Retry once();
};

struct Object { String bytes; Incarnation incarnation; };
struct Meta   { uint64_t size; Incarnation incarnation; };
enum class Removal : uint8_t { Removed, Gone, Mismatch };

struct NotObserved {};
struct ProvenAbsent {};
using Observation = std::variant<NotObserved, ProvenAbsent, Meta, Object>;

struct Committed { Incarnation incarnation; uint32_t attempts_sent; bool resolved_by_read; };
struct Declined  { Observation seen; };
struct Conflict  { Observation seen; };
struct Refused   { int store_error; String message; };
struct GaveUp
{
    enum class Why : uint8_t { Deadline, FenceLost, Unresolved };
    enum class Source : uint8_t { Policy, Lease };
    Why why; Source deadline_source; bool sent_any; Observation last_seen;
};
using WriteResult = std::variant<Committed, Declined, Conflict, Refused, GaveUp>;
std::optional<Incarnation> orThrow(WriteResult && result, std::string_view what);

struct Fence
{
    enum class Admit : uint8_t { Ok, LostOrRearmed, NoBudget };
    std::function<uint64_t()> generation;
    std::function<Admit(uint64_t admitted_generation, uint64_t needed_ms)> admit;
    std::function<void(uint64_t admitted_generation)> check_or_throw;
    static Fence open();                                  // generation 0 forever; admit always Ok
};
}
```

- [ ] **Step 1: Write the failing type tests**

Create `src/Disks/tests/gtest_cas_requests.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasIncarnation.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasTransportAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include "cas_test_helpers.h"

namespace DB::Cas
{
/// Test-only minter: the one door for a test to hold an Incarnation without a backend.
class CasIncarnationTestAccess
{
public:
    static Incarnation mint(uint64_t backend_id, String key, Dialect d, String value)
    {
        return Incarnation(backend_id, std::move(key), d, std::move(value));
    }
};
}

using namespace DB::Cas;

static_assert(!std::is_default_constructible_v<Incarnation>);
static_assert(!std::is_constructible_v<Incarnation, String>);
static_assert(!std::is_default_constructible_v<TransportAccess>);
static_assert(!std::is_copy_constructible_v<TransportAccess>);

TEST(CASIncarnation, GrammarRefusesTheNineWays)
{
    EXPECT_FALSE(isIncarnationValue(Dialect::ETag, ""));
    EXPECT_FALSE(isIncarnationValue(Dialect::ETag, "*"));
    EXPECT_FALSE(isIncarnationValue(Dialect::ETag, " * "));
    EXPECT_FALSE(isIncarnationValue(Dialect::ETag, "\"a\",\"b\""));
    EXPECT_TRUE(isIncarnationValue(Dialect::ETag, "\"abc\""));
    EXPECT_FALSE(isIncarnationValue(Dialect::Generation, "0"));
    EXPECT_FALSE(isIncarnationValue(Dialect::Generation, "00123"));
    EXPECT_FALSE(isIncarnationValue(Dialect::Generation, "\"123\""));
    EXPECT_TRUE(isIncarnationValue(Dialect::Generation, "123"));
    EXPECT_FALSE(isIncarnationValue(Dialect::Emulated, ""));
}

TEST(CASIncarnation, RenderAndPersistedCompare)
{
    auto inc = CasIncarnationTestAccess::mint(7, "k", Dialect::Generation, "42");
    EXPECT_EQ(inc.render(), "generation:42");
    EXPECT_TRUE((PersistedIncarnation{"generation", "42"}).matches(inc));
    EXPECT_FALSE((PersistedIncarnation{"etag", "42"}).matches(inc));
    EXPECT_FALSE((PersistedIncarnation{"generation", "43"}).matches(inc));
}

TEST(CASRetry, BackoffIsFullJitterUnderTheCap)
{
    for (uint32_t attempt = 1; attempt <= 12; ++attempt)
    {
        const uint64_t ceiling = std::min<uint64_t>(5000, 200ull << (attempt - 1));
        uint64_t sum = 0;
        for (int i = 0; i < 1000; ++i)
        {
            const uint64_t s = Retry::backoff(attempt);
            ASSERT_LE(s, ceiling);
            sum += s;
        }
        const double mean = static_cast<double>(sum) / 1000.0;
        EXPECT_GT(mean, ceiling * 0.35) << "attempt " << attempt;   /// a mean near half the ceiling: jitter is real
        EXPECT_LT(mean, ceiling * 0.65) << "attempt " << attempt;
    }
}

TEST(CASRetry, PoliciesAreShapedAsSpecified)
{
    const uint64_t now = DB::Cas::CasMountRuntime::bootMs();
    EXPECT_GE(Retry::standard().deadline_ms, now + 90'000 - 5);
    EXPECT_FALSE(Retry::standard().single_attempt);
    EXPECT_TRUE(Retry::once().single_attempt);
    const Retry lease = Retry::untilLeaseSafe(now + 10'000, 2'000);
    EXPECT_LE(lease.deadline_ms, now + 8'000);
    EXPECT_GE(Retry::within(1'000).deadline_ms, now + 1'000 - 5);
}

TEST(CASWriteResult, OrThrowMapsEveryAlternative)
{
    auto inc = CasIncarnationTestAccess::mint(1, "k", Dialect::Emulated, "1");
    EXPECT_EQ(*orThrow(WriteResult{Committed{inc, 1, false}}, "t"), inc);
    EXPECT_FALSE(orThrow(WriteResult{Declined{NotObserved{}}}, "t").has_value());
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { orThrow(WriteResult{Conflict{ProvenAbsent{}}}, "t"); });
    expectThrowsCode(DB::ErrorCodes::S3_ERROR, [&] { orThrow(WriteResult{Refused{DB::ErrorCodes::S3_ERROR, "denied"}}, "t"); });
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { orThrow(WriteResult{GaveUp{GaveUp::Why::Deadline, GaveUp::Source::Policy, true, NotObserved{}}}, "t"); });
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { orThrow(WriteResult{GaveUp{GaveUp::Why::Unresolved, GaveUp::Source::Policy, true, ProvenAbsent{}}}, "t"); });
    EXPECT_ANY_THROW(orThrow(WriteResult{GaveUp{GaveUp::Why::FenceLost, GaveUp::Source::Lease, false, NotObserved{}}}, "t"));   /// throwCasTransientUnavailable's code
}

TEST(CASFence, OpenFenceAdmitsEverythingAndNeverMoves)
{
    Fence f = Fence::open();
    EXPECT_EQ(f.generation(), 0u);
    EXPECT_EQ(f.admit(0, 1'000'000), Fence::Admit::Ok);
    EXPECT_NO_THROW(f.check_or_throw(0));
}
```

- [ ] **Step 2: Implement the headers**

`CasIncarnation.h` — the class exactly as in Interfaces above; `isIncarnationValue` is Task 1's grammar moved (Task 1's `ObjectStorageBackend::isValidTokenValue` becomes `return isIncarnationValue(type, value);`). `render()`:

```cpp
inline String Incarnation::render() const
{
    switch (dialect_)
    {
        case Dialect::ETag: return "etag:" + value_;
        case Dialect::Generation: return "generation:" + value_;
        case Dialect::Emulated: return "emulated:" + value_;
    }
    return "unknown:" + value_;
}
inline bool PersistedIncarnation::matches(const Incarnation & live) const { return live.render() == dialect + ":" + value; }
```

`CasRetry.cpp`:

```cpp
uint64_t Retry::backoff(uint32_t attempt)
{
    if (attempt == 0)
        return 0;
    const uint32_t doublings = std::min<uint32_t>(attempt - 1, 20);
    const uint64_t ceiling = std::min<uint64_t>(5000, 200ull << doublings);
    return thread_local_rng() % (ceiling + 1);     /// full jitter: uniform(0, ceiling)
}
Retry Retry::within(uint64_t ms) { return Retry{CasMountRuntime::bootMs() + ms, false}; }
Retry Retry::standard() { return within(90'000); }
Retry Retry::untilLeaseSafe(uint64_t lease_deadline_ms, uint64_t safety_margin_ms)
{
    const uint64_t lease_bound = lease_deadline_ms > safety_margin_ms ? lease_deadline_ms - safety_margin_ms : 0;
    return Retry{std::min(standard().deadline_ms, lease_bound), false};
}
Retry Retry::once() { return Retry{standard().deadline_ms, true}; }
```

`CasWriteResult.h` — the structs as in Interfaces; `orThrow`:

```cpp
inline std::optional<Incarnation> orThrow(WriteResult && result, std::string_view what)
{
    return std::visit(overloaded{
        [](Committed & c) -> std::optional<Incarnation> { return std::move(c.incarnation); },
        [](Declined &) -> std::optional<Incarnation> { return std::nullopt; },
        [&](Conflict & c) -> std::optional<Incarnation>
        {
            throw Exception(ErrorCodes::ABORTED, "{}: conflict, observed {}", what, renderObservation(c.seen));
        },
        [&](Refused & r) -> std::optional<Incarnation>
        {
            throw Exception(r.store_error, "{}: the store refused the write: {}", what, r.message);
        },
        [&](GaveUp & g) -> std::optional<Incarnation>
        {
            switch (g.why)
            {
                case GaveUp::Why::FenceLost:
                    throwCasTransientUnavailable(String(what), "mount fence tripped: the durable write is refused because this node no longer holds the mount incarnation it was admitted under");
                case GaveUp::Why::Deadline:
                    throwCasWriteRetryLater(fmt::format("{}: gave up at the {} deadline after {} attempt(s)", what, g.deadline_source == GaveUp::Source::Lease ? "lease" : "policy", g.sent_any ? "one or more" : "zero"));
                case GaveUp::Why::Unresolved:
                    throwCasWriteRetryLater(fmt::format("{}: the write is unresolved (sent, resolve read found {})", what, renderObservation(g.last_seen)));
            }
            UNREACHABLE();
        }}, result);
}
```

with `renderObservation` returning `"nothing observed"`, `"absent"`, `"present (meta)"`, or `"present (<render>)"`. `throwCasTransientUnavailable` and `throwCasWriteRetryLater` are declared in `CasRequestControl.h` today; include it (the lock moves them into `CasRequests.h`).

`CasFence.h` — the struct as in Interfaces; `Fence::open()` returns `{[]{ return 0; }, [](uint64_t, uint64_t){ return Fence::Admit::Ok; }, [](uint64_t){}}`.

- [ ] **Step 3: Do not build yet**

This task's tests compile at CP3 (Task 7). Record in the report that the tests are written red-by-construction (the headers did not exist).

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasIncarnation.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasTransportAccess.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp src/Disks/tests/gtest_cas_requests.cpp
git commit -F - <<'MSG'
ca-requests: the contract's types — Incarnation, TransportAccess, Retry, WriteResult, Fence

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01DAu8zEhBXrwRKTPhpXgZnS
MSG
```

---

### Task 4: `Backend` gains the keyed string primitives; the three backends and the decorator implement them; `ThrottlingBackend` {#task-4}

**Amendments (second review, binding — supersede the conflicting lines below):**
- The legacy methods stay **virtual** in `Backend` with forwarding default bodies (`get`, `head(key)`, `putIfAbsent`, `putOverwrite`, `casPut`, `deleteExact`, `list(prefix,cursor,limit)`, `publishBlob`, `probeSentinelRaw(key)`); the three production backends delete their legacy overrides; test doubles' legacy overrides are left alone here and move to the primitive in the batch that migrates the production site they instrument (Task 7 converts nothing). The virtual forwarders die at the lock.
- `getStream` is **not** forwarded and no `Token{}` is synthesized: the three backends keep their legacy `getStream` implementations (they may share a private helper with `stream`) until Tasks 8 and 9 migrate its two callers; Task 9 deletes it.
- The S3 overloads from Task 2 take `(profile, request_timeout_ms)`: `ObjectStorageBackend` passes its `attempt_timeout_ms`. `ObjectStorageBackend`'s constructor gains `bool single_attempt_control_plane` and `uint64_t attempt_timeout_ms`, set in `ContentAddressedMetadataStorage::openPoolView` (`!read_only && mode == Native`, and the budget's `attempt_timeout_ms`) — not by `Pool::open`, which receives the backend already built.
- `Backend` gains a non-transport capability method `virtual bool refreshCredentials()` (`ObjectStorageBackend` → `object_storage->tryRefreshCredentialsViaCallback()`; `InMemoryBackend` → the value of a new knob `setRefreshCredentialsResult(bool)`, default false). The façade uses it (Task 5).
- `Backend::write` returns the store's raw value unvalidated; validation is the façade's (Task 5): an invalid value on a successful write is treated as an ambiguous attempt (the write may have landed), never `CORRUPTED_DATA`.
- `CasBackend.h`'s new list types are `RawListedKey`/`RawListPage`; `CasRequests.h`'s are `KeyEntry`/`KeyPage` (renamed to `ListedKey`/`ListPage` at the lock).

**Files:**
- Modify: `.../Backend/CasBackend.h` (new pure virtuals beside the legacy ones; `migrationAccess`; legacy non-virtual conveniences forward)
- Modify: `.../Backend/CasInMemoryBackend.h`, `.cpp` (implement the primitives; legacy overrides forward to them through the virtual)
- Modify: `.../Backend/CasObjectStorageBackend.h`, `.cpp` (same; `read` uses `readSmallObjectAndGetObjectMetadata` under `NativeConditional` + `SingleAttempt`; `head`/`list`/`remove` use Task 2's overloads with `SingleAttempt` in Native mode and `Default` for a read-only pool)
- Modify: `.../Backend/CasInstrumentedBackend.h` (forward the new primitives, counting)
- Create: `.../Backend/CasThrottlingBackend.h`
- Test: `src/Disks/tests/gtest_cas_requests.cpp` (append `CASBackendPrimitives.*`, `CASThrottlingBackend.*`)

**Interfaces:**
- Consumes: Task 3's types; Task 2's overloads.
- Produces (exact, added to `class Backend`):

```cpp
    struct Raw     { String bytes; String value; };
    struct RawMeta { uint64_t size; String value; };
    struct RawListedKey { String key; uint64_t size; std::optional<String> value; };
    struct RawListPage  { std::vector<RawListedKey> keys; String next_cursor; };
    struct RawConflict {};
    enum class RawRemoval : uint8_t { Removed, Gone, Mismatch, DeleteMarker };
    virtual std::optional<Raw>     read  (const String & key, TransportAccess &) = 0;
    virtual std::optional<RawMeta> head  (const String & key, TransportAccess &) = 0;
    virtual RawListPage            list  (const String & prefix, const String & cursor, size_t limit, TransportAccess &) = 0;
    virtual RawRemoval             remove(const String & key, const String & expected_value, TransportAccess &) = 0;
    virtual std::expected<String, RawConflict> write(const String & key, const String & bytes, const std::optional<String> & expected_value, TransportAccess &) = 0;
    virtual SentinelProbeResult    probeSentinelRaw(const String & key, TransportAccess &);   // default body: head → read, as today
    virtual std::unique_ptr<ReadBuffer> stream(const String & key, TransportAccess &) = 0;
    virtual void                   publish(const BlobPublishRequest & request, TransportAccess &) = 0;
    virtual Dialect                dialect() const = 0;            // the minting dialect (ObjectStorageBackend: native or Emulated; InMemory: Emulated)
    uint64_t backendId() const { return backend_id; }             // per-instance counter, assigned in the ctor
protected:
    static TransportAccess migrationAccess() { return TransportAccess{}; }   // migration window only; Task 20 deletes it
```

  and `ThrottlingBackend`:

```cpp
class ThrottlingBackend final : public Backend
{
public:
    enum class Mode : uint8_t { FirstPerKey, EveryNth };
    ThrottlingBackend(BackendPtr inner, Mode mode, size_t n, int status /* 429 | 503 */);
    size_t refusals(const String & key) const;    // how many times this key was refused
    // every transport method: if the seam says refuse → throw S3Exception with the chosen status; else forward with the same TransportAccess
};
```

- [ ] **Step 1: Write the failing tests**

Append to `gtest_cas_requests.cpp`:

```cpp
/// Test door for the raw primitives: goes through CasRequests once Task 5 lands; until then the
/// migration key is the only way, and it is what these tests use (Backend is a friend of TransportAccess).
struct RawDoor : DB::Cas::Backend
{
    static DB::Cas::TransportAccess key() { return migrationAccess(); }
};

TEST(CASBackendPrimitives, InMemoryWriteReadRemoveRoundTripInStrings)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto key = RawDoor::key();
    auto w1 = b->write("k", "v1", std::nullopt, key);
    ASSERT_TRUE(w1.has_value());
    auto r = b->read("k", key);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->bytes, "v1");
    EXPECT_EQ(r->value, *w1);
    auto w2 = b->write("k", "v2", std::nullopt, key);     /// must be absent → refused
    EXPECT_FALSE(w2.has_value());
    auto w3 = b->write("k", "v2", *w1, key);
    ASSERT_TRUE(w3.has_value());
    EXPECT_EQ(b->remove("k", *w1, key), Backend::RawRemoval::Mismatch);
    EXPECT_EQ(b->remove("k", *w3, key), Backend::RawRemoval::Removed);
    EXPECT_EQ(b->remove("k", *w3, key), Backend::RawRemoval::Gone);
}

TEST(CASBackendPrimitives, LegacyOverridesStillFireWhenTheNewPrimitiveIsCalled)
{
    /// The migration rule: new methods are the primitives; a fault-injection override written against
    /// the NEW signature intercepts a legacy caller too, because legacy forwards through the virtual.
    struct Counting : InMemoryBackend
    {
        size_t writes = 0;
        std::expected<String, RawConflict> write(const String & k, const String & v, const std::optional<String> & e, TransportAccess & a) override
        {
            ++writes;
            return InMemoryBackend::write(k, v, e, a);
        }
    };
    auto b = std::make_shared<Counting>();
    b->putIfAbsent("k", "v");                 /// legacy call
    EXPECT_EQ(b->writes, 1u);
}

TEST(CASThrottlingBackend, FirstPerKeyRefusesOnceThenForwards)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto t = std::make_shared<ThrottlingBackend>(inner, ThrottlingBackend::Mode::FirstPerKey, 0, 429);
    auto key = RawDoor::key();
    EXPECT_THROW(t->read("k", key), DB::S3Exception);
    EXPECT_NO_THROW(t->read("k", key));
    EXPECT_EQ(t->refusals("k"), 1u);
    EXPECT_THROW(t->write("k2", "v", std::nullopt, key), DB::S3Exception);
    EXPECT_TRUE(t->write("k2", "v", std::nullopt, key).has_value());
}
```

- [ ] **Step 2: Implement `Backend`'s primitives and the forwarders**

In `CasBackend.h`, add the declarations from Interfaces. Turn every legacy **virtual** into a non-virtual forwarder in the base (so subclasses' legacy overrides become plain overrides of nothing — the compiler flags them, which is the point: each backend's legacy overrides are deleted in this task and the three backends implement only the new primitives):

```cpp
    std::optional<GetResult> get(const String & key, Range range)
    {
        if (!range.whole())
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ranged get is retired");
        auto access = migrationAccess();
        auto raw = read(key, access);
        if (!raw)
            return std::nullopt;
        return GetResult{std::move(raw->bytes), Token{std::move(raw->value), dialect()}, {}};
    }
    HeadResult head(const String & key)
    {
        auto access = migrationAccess();
        auto raw = head(key, access);
        if (!raw)
            return {};
        return HeadResult{true, raw->size, Token{std::move(raw->value), dialect()}, {}};
    }
    PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta &)
    {
        auto access = migrationAccess();
        auto r = write(key, bytes, std::nullopt, access);
        if (!r)
            return PutResult{PutOutcome::PreconditionFailed, {}};
        return PutResult{PutOutcome::Done, Token{std::move(*r), dialect()}};
    }
    PutResult putOverwrite(const String & key, const String & bytes, const Token & expected, const ObjectMeta &)
    {
        auto access = migrationAccess();
        auto r = write(key, bytes, expected.value, access);
        if (!r)
            return PutResult{PutOutcome::PreconditionFailed, {}};
        return PutResult{PutOutcome::Done, Token{std::move(*r), dialect()}};
    }
    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected, const ObjectMeta &)
    {
        auto access = migrationAccess();
        auto r = write(key, bytes, expected ? std::optional<String>(expected->value) : std::nullopt, access);
        if (!r)
            return CasResult{CasOutcome::Conflict, {}};
        return CasResult{CasOutcome::Committed, Token{std::move(*r), dialect()}};
    }
    DeleteOutcome deleteExact(const String & key, const Token & token)
    {
        auto access = migrationAccess();
        switch (remove(key, token.value, access))
        {
            case RawRemoval::Removed: return {DeleteOutcome::Kind::Deleted, false};
            case RawRemoval::Gone: return {DeleteOutcome::Kind::NotFound, false};
            case RawRemoval::Mismatch: return {DeleteOutcome::Kind::TokenMismatch, false};
            case RawRemoval::DeleteMarker: return {DeleteOutcome::Kind::Deleted, true};
        }
        UNREACHABLE();
    }
    ListPage list(const String & prefix, const String & cursor, size_t limit)
    {
        auto access = migrationAccess();
        auto raw = list(prefix, cursor, limit, access);
        ListPage page;
        page.next_cursor = std::move(raw.next_cursor);
        for (auto & k : raw.keys)
            page.keys.push_back(ListedKey{std::move(k.key), k.size, k.value ? std::optional<Token>(Token{std::move(*k.value), dialect()}) : std::nullopt});
        return page;
    }
    std::optional<GetStreamResult> getStream(const String & key, Range range)
    {
        if (!range.whole())
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ranged getStream is retired");
        auto access = migrationAccess();
        auto s = stream(key, access);
        if (!s)
            return std::nullopt;
        return GetStreamResult{std::move(s), Token{}};   /// the token nobody consumed is gone; callers that read it are migrated in batch tasks
    }
    void publishBlob(const BlobPublishRequest & request) { auto access = migrationAccess(); publish(request, access); }
```

`ObjectMeta` and `Range` are retired here with their contract tests (`gtest_cas_backend.cpp`: delete the ranged-window tests and the `cas_owner` round-trip assertions; name them in the commit). The grammar guards from Task 1 move into the new `write`/`remove` bodies of each backend (the legacy forwarders reach them through the virtual).

`InMemoryBackend`: implement `read/head/list/remove/write/stream/publish/dialect` over the existing `store_`; keep the fault-injection knobs (`failNextCasPut` → fires on `write` with an expected value; `injectAmbiguousPutIfAbsent` → fires on `write` with `nullopt`); `remove` returns `DeleteMarker` when `simulate_delete_markers_`; delete the legacy overrides.

`ObjectStorageBackend`: `read` = `readSmallObjectAndGetObjectMetadata` under `ReadSettings{object_storage_request_mode = NativeConditional, object_storage_retry_profile = SingleAttempt (Native writable) | Default (read-only), object_storage_attempt_timeout_ms = attempt_timeout}` with `max_size_bytes = ZSTD_compressBound(largest control cap)` computed once beside the caps table in `Formats/CasFormat.h`; `head` = `tryGetObjectMetadataWithNativeToken(path, false, profile)`; `list` via `iterate(..., profile)`; `remove` = `removeObjectIfTokenMatches(..., profile)` mapping `Removed`/`TokenMismatch`/`NotFound` and `delete_marker` → `DeleteMarker`; `write` = today's `nativeConditionalPut` body returning the ETag text (or `RawConflict{}` on `PreconditionFailed`) and throwing `CAS_WRITE_UNATTRIBUTED` when the response has no ETag; the emulated mode maps onto the `emu*` helpers. The retry profile is `SingleAttempt` when the pool is writable Native (the backend learns it from a constructor flag `single_attempt_control_plane`, set by `Pool::open` for `!read_only && Mode::Native`), else `Default`. `probeSentinelRaw`'s Native path becomes one `read` whose 404 body distinguishes `NoSuchKey` from `NoSuchBucket` (keep the emulated container stat).

`InstrumentedBackend`: forward the new primitives, counting with `CasOp::Get` → rename to `CasOp::Read` (retire `GetStream` counting; the `CAS*Get`/`CAS*GetStream` ProfileEvents go at the lock).

`CasThrottlingBackend.h`: as in Interfaces; `FirstPerKey` keeps a `std::set<String>` of keys already refused; `EveryNth` keeps a counter; refusal = `throw S3Exception(Aws::S3::S3Errors::SLOW_DOWN, "throttled by ThrottlingBackend")` for 429 and `SERVICE_UNAVAILABLE` for 503 (`S3Exception::isRetryableError()` must be true for both; confirm against `S3Common.cpp` and pick the error enum accordingly).

- [ ] **Step 3: Commit (no build)**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasThrottlingBackend.h src/Disks/tests/gtest_cas_requests.cpp src/Disks/tests/gtest_cas_backend.cpp
git commit -F - <<'MSG'
ca-backend: keyed string primitives are the transport; legacy methods forward through them

`read`/`head`/`list`/`remove`/`write`/`stream`/`publish` take a TransportAccess and deal in strings;
the three backends implement only them; the legacy Token-typed methods become base forwarders that
obtain the migration key and call the virtual, so a fault-injection override on the new signature
intercepts a legacy caller too. `Range`, `ObjectMeta` and `GetStreamResult::token` are retired with
their contract tests. `ThrottlingBackend` (first-per-key and every-n-th) is the coverage seam.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01DAu8zEhBXrwRKTPhpXgZnS
MSG
```

---

### Task 5: `CasRequests`, `CasOperation` and the engine {#task-5}

**Amendments (second review, binding — supersede the conflicting code below where they differ):**
- Only `CasRequests` constructs `TransportAccess`: a private `template <typename Fn> auto CasRequests::withTransportAccess(Fn && fn) { TransportAccess a; return fn(a); }` that `CasOperation` calls (it is a friend of `CasRequests`, or the method is befriended); `CasOperation` never names `TransportAccess` itself.
- Every verb binds its policy once at entry: `const Retry::Bound bound = policy.bind(owner.now_ms());` and carries `bound.lease_bound` into every `GaveUp`.
- One call-scoped `WriteState { uint32_t attempts_sent = 0; bool sent_any = false; bool any_ambiguous = false; Observation last_seen = NotObserved{}; uint32_t reissues = 0; }` is threaded through `writeLoop` and, for `readModifyWrite`, through every inner write of the same call; `GaveUp` is built from it (never from a fresh default).
- `writeLoop` terminal structure after the attempt (the resolve read is made for EVERY `RawConflict` and EVERY ambiguous attempt):
```cpp
state.last_seen = observe(key, bound, state);            // exact read; NotObserved when the read itself gave up
if (auto * obj = std::get_if<Object>(&state.last_seen))
{
    if (state.any_ambiguous && obj->bytes == bytes)
        return postCommit(obj->incarnation, /*resolved_by_read=*/true, state, bound);
    return Conflict{state.last_seen};                        // someone else's object: the answer, ambiguous or not
}
if (!state.any_ambiguous)
    return Conflict{state.last_seen};                        // a plain refusal with absent/unobserved: the answer
if (policy.single_attempt)
    return gaveUp(GaveUp::Why::Unresolved, state, bound);
return pauseAndReissue(state, bound);                        // gate(sleep + 2*reservation) then sleep; GaveUp carries state.last_seen
```
  `postCommit(inc, resolved, state, bound)` runs the post-commit admission: `Ok → Committed{inc, state.attempts_sent, resolved}`, `LostOrRearmed → GaveUp{FenceLost, sent_any = true}`, `NoBudget → GaveUp{Deadline, Lease, sent_any = true}`. A successful raw write whose value fails the grammar is treated as ambiguous (`state.any_ambiguous = true`, proceed to `observe`), never thrown.
- Credential refresh is typed: on an `isAccessTokenExpiredError` (write or read) call `backend->refreshCredentials()`; if it returns true, the attempt is ambiguous and reissued under the deadline; if false, a read propagates the error and a write is `Refused` iff `isDefinitelyRefusedWrite(e)` (which, by construction, excludes the refresh class — so it becomes ambiguous and reissues; state it). `Refused` is reachable only with `!state.any_ambiguous` and no refresh performed.
- `readModifyWrite`: the initial read goes through `observe` (a `GaveUp` is returned, not thrown); `decide` is called outside every classification `try` so its exception propagates unchanged; on `Conflict` adopt its observation as the next `current`, return the `Conflict` immediately under `once`, else `gate(Retry::backoff(++state.reissues) + 2*reservation)` then sleep, then re-decide; a fresh read after the sleep only when the observation is `NotObserved`; every gate failure maps to `GaveUp{FenceLost | Deadline(source from bound)}` with `state.last_seen`.
- `readLoop` for reads/head/list/remove/probe/stream/publish: reservation 1×; the same typed refresh rule; `remove` maps `DeleteMarker` to the `CAS_DELETE_MARKER` exception.
- Saturating arithmetic everywhere: `remaining = now >= deadline ? 0 : deadline - now; if (needed > remaining) → give up`.

**Files:**
- Create: `.../Backend/CasRequests.h`, `.../Backend/CasRequests.cpp`
- Modify: `src/Common/ProfileEvents.cpp` (add `CASRequestAttempt`, `CASRequestReissue`, `CASRequestResolveRead`, `CASRequestGaveUp`, `CASRequestRefused`, `CASRequestFenceLostPostWrite`)

**Interfaces:**
- Consumes: Task 3's types, Task 4's `Backend` primitives, `isDeterministicLocalFailure` (moved here from `CasRequestControl.cpp` as a public function), `S3Exception` predicates from `IO/S3Common.h` (`isMalformedRequestError`, `isEntityTooLargeError`, `isAccessDeniedError`, `S3Exception::isAccessTokenExpiredError`, `isRetryableError`).
- Produces (exact):

```cpp
namespace DB::Cas
{
bool isDefinitelyRefusedWrite(const std::exception & e);      // malformed | too large | (access denied && !token expired)
bool isDeterministicLocalFailure(int code);                    // moved from CasRequestControl.cpp

using Liveness       = std::function<bool()>;
using DecideOnObject = std::function<std::optional<String>(const std::optional<Object> &)>;
using DecideOnMeta   = std::function<std::optional<String>(const std::optional<Meta> &)>;
using ListedKeyFn    = std::function<bool(const ListedKey &)>;   // false stops the walk
struct ListedKey { String key; uint64_t size; std::optional<Incarnation> incarnation; };
struct ListPage  { std::vector<ListedKey> keys; String next_cursor; };

class CasOperation;
class CasRequests
{
public:
    CasRequests(BackendPtr backend, Fence fence, std::function<uint64_t()> now_ms = {}, std::function<void(uint64_t)> sleep_ms = {});
    CasOperation admit(Liveness liveness = {});
    CasOperation resume(uint64_t admitted_generation, Liveness liveness = {});
    Backend & backendForCapabilityPredicates();   // the four predicates and dialect(); no transport method is reachable through it without a key
    void setSleepFnForTest(std::function<void(uint64_t)>);
    void setNowFnForTest(std::function<uint64_t()>);
private:
    friend class CasOperation;
    // minting, key/backend binding checks, the engine
    BackendPtr backend; Fence fence; std::function<uint64_t()> now_ms; std::function<void(uint64_t)> sleep_ms; uint64_t attempt_reservation_ms;
};

class CasOperation
{
public:
    CasOperation(CasOperation &&) = default;
    CasOperation(const CasOperation &) = delete;
    uint64_t generation() const;
    bool admitted() const;                               // fence.admit(generation, 0) == Ok && liveness()
    std::optional<Object>       read  (const String & key, const Retry &);
    std::optional<Meta>         head  (const String & key, const Retry &);
    ListPage                    list  (const String & prefix, const String & cursor, size_t limit, const Retry &);
    void                        forEachListedKey(const String & prefix, const ListedKeyFn &, const Retry & per_page, size_t page_limit = 1000, const std::function<void()> & on_page_fetched = {});
    Removal                     remove(const String & key, const Incarnation & seen, const Retry &);
    Removal                     removeCurrent(const String & key, const Retry &);
    SentinelProbeResult         probeSentinel(const String & key, const Retry &);
    std::unique_ptr<ReadBuffer> stream(const String & key, const Retry &);
    void                        publish(const BlobPublishRequest &, const Retry &);
    WriteResult create (const String & key, const String & bytes, const Retry &);
    WriteResult replace(const String & key, const String & bytes, const Incarnation & seen, const Retry &);
    WriteResult readModifyWrite          (const String & key, const DecideOnObject &, const Retry &);
    WriteResult readModifyWriteOnPresence(const String & key, const DecideOnMeta &, const Retry &);
};
}
```

- [ ] **Step 1: Write `CasRequests.h`** — the declarations above, plus the private engine surface:

```cpp
private:
    CasRequests & owner; uint64_t admitted_generation; Liveness liveness;
    enum class Gate : uint8_t { Ok, FenceLost, NoBudget };
    Gate gate(uint64_t needed_ms) const;                                  // fence.admit(generation, needed) then liveness()
    /// One attempt of a read-class request under the policy; classifies, sleeps, reissues; returns the value or throws
    template <typename Fn> auto readLoop(std::string_view what, const Retry &, Fn && once);
    /// The write engine: one call, any policy; settles every conflict/ambiguity by resolve read
    WriteResult writeLoop(const String & key, const String & bytes, const std::optional<Incarnation> & expected, const Retry &);
    Observation resolveRead(const String & key, const Retry &);          // exact read; NotObserved if it failed
    Incarnation mint(const String & key, String value) const;            // grammar check; CORRUPTED_DATA on failure
    const String & valueFor(const String & key, const Incarnation &) const;   // key/backend binding; LOGICAL_ERROR on mismatch
```

- [ ] **Step 2: Write `CasRequests.cpp`** — the engine. The write loop, verbatim (this is the design's center; every later verb is a thin wrapper):

```cpp
WriteResult CasOperation::writeLoop(const String & key, const String & bytes, const std::optional<Incarnation> & expected, const Retry & policy)
{
    const uint64_t reservation = owner.attempt_reservation_ms;
    std::optional<String> expected_value;
    if (expected)
        expected_value = valueFor(key, *expected);
    uint32_t attempts_sent = 0;
    bool any_ambiguous = false;
    auto gave_up = [&](GaveUp::Why why, Observation seen) -> WriteResult
    {
        ProfileEvents::increment(ProfileEvents::CASRequestGaveUp);
        return GaveUp{why, policy.deadline_ms < Retry::standard().deadline_ms ? GaveUp::Source::Lease : GaveUp::Source::Policy, attempts_sent > 0, std::move(seen)};
    };
    for (uint32_t attempt = 1;; ++attempt)
    {
        /// Admission, before every attempt: the write plus the resolve read that may settle it.
        switch (gate(2 * reservation))
        {
            case Gate::FenceLost: return gave_up(GaveUp::Why::FenceLost, NotObserved{});
            case Gate::NoBudget:  return gave_up(GaveUp::Why::Deadline, NotObserved{});
            case Gate::Ok: break;
        }
        if (owner.now_ms() + 2 * reservation > policy.deadline_ms)
            return gave_up(GaveUp::Why::Deadline, NotObserved{});

        ProfileEvents::increment(ProfileEvents::CASRequestAttempt);
        ++attempts_sent;
        std::expected<String, Backend::RawConflict> outcome;
        bool ambiguous = false;
        try
        {
            TransportAccess access;
            outcome = owner.backend->write(key, bytes, expected_value, access);
        }
        catch (const Exception & e)
        {
            if (isDeterministicLocalFailure(e.code()))
                throw;
            if (isDefinitelyRefusedWrite(e) && !any_ambiguous)
            {
                ProfileEvents::increment(ProfileEvents::CASRequestRefused);
                return Refused{e.code(), e.message()};
            }
            /// Everything else — throttling, timeouts, an unmodeled store error, a definite refusal AFTER an
            /// earlier ambiguous attempt, an expired credential — is ambiguous: this attempt may have landed.
            ambiguous = true;
            any_ambiguous = true;
            if (const auto * s3 = dynamic_cast<const S3Exception *>(&e); s3 && s3->isAccessTokenExpiredError())
                owner.refreshCredentialsOnce();    /// tryRefreshCredentialsViaCallback on the storage; the reissue sees the new client
        }
        catch (const std::exception &)
        {
            ambiguous = true;
            any_ambiguous = true;
        }

        if (!ambiguous && outcome.has_value())
        {
            /// Proven commit. Admission once more: a fence lost here means the write may be durable but this
            /// call must never claim it (today's FenceLostPostWrite, on which the ref lane wedges).
            Incarnation inc = mint(key, std::move(*outcome));
            if (gate(0) != Gate::Ok)
            {
                ProfileEvents::increment(ProfileEvents::CASRequestFenceLostPostWrite);
                return gave_up(GaveUp::Why::FenceLost, NotObserved{});
            }
            return Committed{std::move(inc), attempts_sent, false};
        }

        /// Refused precondition or ambiguous attempt: settle by ONE exact read, always, under any policy.
        ProfileEvents::increment(ProfileEvents::CASRequestResolveRead);
        Observation seen = resolveRead(key, policy);
        if (const auto * obj = std::get_if<Object>(&seen))
        {
            if (obj->bytes == bytes)
                return Committed{obj->incarnation, attempts_sent, /*resolved_by_read=*/true};
            if (!ambiguous || expected_value)
                return Conflict{std::move(seen)};              /// someone else's object: the answer
            return Conflict{std::move(seen)};                  /// ambiguous create that lost the race: same answer
        }
        if (std::holds_alternative<ProvenAbsent>(seen))
        {
            if (!ambiguous && expected_value)
                return Conflict{std::move(seen)};              /// If-Match refused and the key is gone: vanished
            /// ambiguous and nothing there: this attempt did not land; reissue (or, under once, report Unresolved)
        }
        else
        {
            /// NotObserved: the resolve read itself failed. Under once this is Unresolved; otherwise reissue.
        }
        if (policy.single_attempt)
            return gave_up(GaveUp::Why::Unresolved, std::move(seen));
        if (!ambiguous && !expected_value)
            return Conflict{std::move(seen)};                  /// create refused, occupant read: conflict (create never reissues on a plain refusal)

        /// Sleep with jitter; admission before the sleep.
        const uint64_t sleep = Retry::backoff(attempt);
        switch (gate(sleep + 2 * reservation))
        {
            case Gate::FenceLost: return gave_up(GaveUp::Why::FenceLost, std::move(seen));
            case Gate::NoBudget:  return gave_up(GaveUp::Why::Deadline, std::move(seen));
            case Gate::Ok: break;
        }
        if (owner.now_ms() + sleep + 2 * reservation > policy.deadline_ms)
            return gave_up(GaveUp::Why::Deadline, std::move(seen));
        ProfileEvents::increment(ProfileEvents::CASRequestReissue);
        owner.sleep_ms(sleep);
    }
}
```

`resolveRead` is `read` under the same policy, returning `Object` / `ProvenAbsent` / `NotObserved` (a read that fails classifies exactly like a `read` verb — reissued under the policy — and is `NotObserved` only when the policy is exhausted). `create(key, bytes, r)` = `writeLoop(key, bytes, nullopt, r)`; `replace(key, bytes, seen, r)` = `writeLoop(key, bytes, seen, r)`.

`readLoop` (reads, `head`, `list` pages, `remove`, `probeSentinel`, `stream` open, `publish` initiation):

```cpp
template <typename Fn>
auto CasOperation::readLoop(std::string_view what, const Retry & policy, Fn && once)
{
    const uint64_t reservation = owner.attempt_reservation_ms;
    for (uint32_t attempt = 1;; ++attempt)
    {
        switch (gate(reservation))
        {
            case Gate::FenceLost: throwCasTransientUnavailable(String(what), "mount fence tripped before the request");
            case Gate::NoBudget:  throwCasWriteRetryLater(fmt::format("{}: no lease budget for one more request", what));
            case Gate::Ok: break;
        }
        if (owner.now_ms() + reservation > policy.deadline_ms)
            throwCasWriteRetryLater(fmt::format("{}: gave up at the deadline after {} attempt(s)", what, attempt - 1));
        ProfileEvents::increment(ProfileEvents::CASRequestAttempt);
        try
        {
            TransportAccess access;
            return once(access);
        }
        catch (const Exception & e)
        {
            if (isDeterministicLocalFailure(e.code()))
                throw;
            if (const auto * s3 = dynamic_cast<const S3Exception *>(&e))
            {
                if (s3->isAccessTokenExpiredError())
                    owner.refreshCredentialsOnce();
                else if (!s3->isRetryableError())
                    throw;                                  /// 404 is handled inside once(); a definite refusal propagates
            }
            if (policy.single_attempt)
                throw;
        }
        const uint64_t sleep = Retry::backoff(attempt);
        switch (gate(sleep + reservation))
        {
            case Gate::FenceLost: throwCasTransientUnavailable(String(what), "mount fence tripped before the reissue");
            case Gate::NoBudget:  throwCasWriteRetryLater(fmt::format("{}: no lease budget for the reissue", what));
            case Gate::Ok: break;
        }
        if (owner.now_ms() + sleep + reservation > policy.deadline_ms)
            throwCasWriteRetryLater(fmt::format("{}: gave up at the deadline after {} attempt(s)", what, attempt));
        ProfileEvents::increment(ProfileEvents::CASRequestReissue);
        owner.sleep_ms(sleep);
    }
}
```

`readModifyWrite`:

```cpp
WriteResult CasOperation::readModifyWrite(const String & key, const DecideOnObject & decide, const Retry & policy)
{
    std::optional<Object> current = read(key, policy);
    for (;;)
    {
        std::optional<String> next = decide(current);        /// may throw: propagates unchanged, never classified
        if (!next)
            return Declined{current ? Observation{*current} : Observation{ProvenAbsent{}}};
        WriteResult r = current ? writeLoop(key, *next, current->incarnation, policy) : writeLoop(key, *next, std::nullopt, policy);
        auto * conflict = std::get_if<Conflict>(&r);
        if (!conflict)
            return r;
        /// The engine's resolve read IS the next iteration's read: no second GET per conflict.
        if (auto * obj = std::get_if<Object>(&conflict->seen))
            current = std::move(*obj);
        else if (std::holds_alternative<ProvenAbsent>(conflict->seen))
            current.reset();
        else
            current = read(key, policy);                       /// the resolve read failed: one fresh read
        const uint64_t sleep = Retry::backoff(1);
        if (gate(sleep + 2 * owner.attempt_reservation_ms) != Gate::Ok || owner.now_ms() + sleep + 2 * owner.attempt_reservation_ms > policy.deadline_ms)
            return GaveUp{GaveUp::Why::Deadline, GaveUp::Source::Policy, true, current ? Observation{*current} : Observation{ProvenAbsent{}}};
        owner.sleep_ms(sleep);
    }
}
```

`readModifyWriteOnPresence` is the same loop over `head` with `Meta` observations and a decide over `std::optional<Meta>`; `removeCurrent` is `head` → `remove(seen)`; on `Mismatch` re-`head` and repeat under the policy with the engine's sleep. `forEachListedKey` walks pages, each page a `readLoop` under `per_page`, stopping when the callback returns `false`. `remove` maps `RawRemoval::DeleteMarker` to `throw Exception(ErrorCodes::CAS_DELETE_MARKER, ...)`.

`mint` applies `isIncarnationValue(backend->dialect(), value)` and throws `CORRUPTED_DATA` naming the key on failure; `valueFor` throws `LOGICAL_ERROR` when `inc.key() != key || inc.backendId() != backend->backendId()`.

`attempt_reservation_ms` is the value the backend's storage was built with (`ObjectStorageBackend::attemptTimeoutMs()`, from the disk setting Task 20 adds; 5000 until then); `InMemoryBackend` reports 0.

- [ ] **Step 3: Commit (no build)**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp src/Common/ProfileEvents.cpp
git commit -F - <<'MSG'
ca-requests: CasRequests, CasOperation and the engine

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01DAu8zEhBXrwRKTPhpXgZnS
MSG
```

---

### Task 6: Engine tests {#task-6}

**Amendments (second review, binding):**
- Add the tests moved from Task 3: `CASIncarnation.RenderAndPersistedCompare` (an `Incarnation` from `op.create`, `render()`, `PersistedIncarnation::capture` round trip, `matches`) and the Committed/Declined arms of `orThrow`.
- `ReadModifyWriteLosesNoIncrementUnderContentionAndBoundsAHotKey`: two `std::thread`s each running 50 increments through their own `CasOperation` over the same backend (a real `CasRequests` with real clock/sleep, no fake clock), assert the final value is 100; the hot-key half uses a backend hook that overwrites the key immediately before EVERY replace (`onBeforeWrite` knob added to `InMemoryBackend`), asserts `GaveUp{Deadline}` on a fake clock and that `clock.sleeps` is non-empty.
- `OnceSendsOneWriteAndAtMostOneResolveRead`: assert `CountingBackend` totals: writes == 1, gets == 1 (the resolve), and no sleep.
- `AdmissionIsCheckedAtThreePoints` gains the before-sleep gate: flip the liveness predicate from inside the backend's resolve-read hook so the first attempt conflicts, the resolve read runs, and the gate before the sleep refuses — assert `GaveUp{FenceLost, sent_any = true}`, zero sleeps, and no second write.
- `ExpiredToken` has two tests: refresh available (`setRefreshCredentialsResult(true)`) → `Committed` with `attempts_sent == 2`; refresh unavailable → the write reissues under the deadline (the error is in the refresh class, excluded from `Refused`) and a read propagates.
- `Refused` test: an `AccessDenied`-class error that is NOT in the refresh class does not exist in S3's vocabulary (`isAccessDeniedError` ⊂ refresh class) — use `MalformedRequest`/`EntityTooLarge` (`isMalformedRequestError`) for the `Refused` test.

**Files:**
- Test: `src/Disks/tests/gtest_cas_requests.cpp` (append)

**Interfaces:** consumes Tasks 3–5; produces the fake-clock harness `FakeClock { uint64_t now; std::vector<uint64_t> sleeps; }` reused by Task 22.

- [ ] **Step 1: Write the tests** (all red until CP3):

```cpp
struct FakeClock
{
    uint64_t now = 1'000'000;
    std::vector<uint64_t> sleeps;
    std::function<uint64_t()> nowFn() { return [this] { return now; }; }
    std::function<void(uint64_t)> sleepFn() { return [this](uint64_t ms) { sleeps.push_back(ms); now += ms; }; }
};

static CasRequests makeRequests(std::shared_ptr<Backend> b, FakeClock & clock, Fence fence = Fence::open())
{
    return CasRequests(std::move(b), std::move(fence), clock.nowFn(), clock.sleepFn());
}

TEST(CASRequests, CreateThenReplaceThenRemove)
{
    FakeClock clock; auto b = std::make_shared<InMemoryBackend>(); auto rq = makeRequests(b, clock);
    auto op = rq.admit();
    auto c1 = orThrow(op.create("k", "v1", Retry::standard()), "t");
    ASSERT_TRUE(c1);
    auto r = op.read("k", Retry::standard());
    ASSERT_TRUE(r); EXPECT_EQ(r->bytes, "v1"); EXPECT_EQ(r->incarnation, *c1);
    auto c2 = orThrow(op.replace("k", "v2", *c1, Retry::standard()), "t");
    EXPECT_EQ(op.remove("k", *c1, Retry::standard()), Removal::Mismatch);
    EXPECT_EQ(op.remove("k", *c2, Retry::standard()), Removal::Removed);
    EXPECT_EQ(op.remove("k", *c2, Retry::standard()), Removal::Gone);
}

TEST(CASRequests, KeyBindingThrowsBeforeAnyRequest)
{
    FakeClock clock; auto b = std::make_shared<CountingBackend>(); auto rq = makeRequests(b, clock);
    auto op = rq.admit();
    auto a = *orThrow(op.create("a", "v", Retry::standard()), "t");
    b->resetCounts();
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { op.replace("b", "w", a, Retry::standard()); });
    EXPECT_EQ(b->putTotal(), 0u);
}

TEST(CASRequests, EveryConflictIsSettledByOneReadAndCarriesTheOccupant)
{
    FakeClock clock; auto b = std::make_shared<CountingBackend>(); auto rq = makeRequests(b, clock);
    auto op = rq.admit();
    orThrow(op.create("k", "theirs", Retry::standard()), "t");
    b->resetCounts();
    auto r = op.create("k", "mine", Retry::once());
    auto * c = std::get_if<Conflict>(&r);
    ASSERT_TRUE(c);
    auto * obj = std::get_if<Object>(&c->seen);
    ASSERT_TRUE(obj); EXPECT_EQ(obj->bytes, "theirs");
    EXPECT_EQ(b->putTotal(), 1u); EXPECT_EQ(b->getTotal(), 1u);        /// slotOccupy's contract: Occupied costs two
}

TEST(CASRequests, AmbiguousCreateThatLandedIsCommittedByTheResolveRead)
{
    FakeClock clock; auto b = std::make_shared<InMemoryBackend>(); auto rq = makeRequests(b, clock);
    b->injectAmbiguousPutIfAbsent("k");        /// the write lands, the response is lost
    auto op = rq.admit();
    auto r = op.create("k", "v", Retry::standard());
    auto * c = std::get_if<Committed>(&r);
    ASSERT_TRUE(c); EXPECT_TRUE(c->resolved_by_read); EXPECT_EQ(c->attempts_sent, 1u);
}

TEST(CASRequests, DeadlineIsTheOnlyBoundUnderZeroLatencyThrottling)
{
    FakeClock clock; auto inner = std::make_shared<InMemoryBackend>();
    auto t = std::make_shared<ThrottlingBackend>(inner, ThrottlingBackend::Mode::EveryNth, 1, 429);   /// refuse everything
    auto rq = makeRequests(t, clock);
    auto op = rq.admit();
    const uint64_t start = clock.now;
    auto r = op.create("k", "v", Retry::standard());
    auto * g = std::get_if<GaveUp>(&r);
    ASSERT_TRUE(g); EXPECT_EQ(g->why, GaveUp::Why::Deadline); EXPECT_TRUE(g->sent_any);
    EXPECT_GE(clock.now - start, 85'000u);                    /// it kept going until the deadline
    EXPECT_GT(clock.sleeps.size(), 16u);                      /// no sixteen-attempt ceiling
}

TEST(CASRequests, OnceSendsOneWriteAndAtMostOneResolveRead)
{
    FakeClock clock; auto inner = std::make_shared<CountingBackend>();
    auto t = std::make_shared<ThrottlingBackend>(inner, ThrottlingBackend::Mode::EveryNth, 1, 503);
    auto rq = makeRequests(t, clock);
    auto op = rq.admit();
    auto r = op.create("k", "v", Retry::once());
    auto * g = std::get_if<GaveUp>(&r);
    ASSERT_TRUE(g); EXPECT_EQ(g->why, GaveUp::Why::Unresolved);
    EXPECT_TRUE(std::holds_alternative<NotObserved>(g->last_seen));   /// the resolve read was throttled too
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, RefusedIsReturnedNotThrownAndDoesNotSetSentAny)
{
    FakeClock clock; auto b = std::make_shared<InMemoryBackend>();
    b->failNextWriteWith("k", DB::S3Exception(Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));   /// add this knob to InMemoryBackend in this task
    auto rq = makeRequests(b, clock);
    auto op = rq.admit();
    auto r = op.create("k", "v", Retry::standard());
    ASSERT_TRUE(std::holds_alternative<Refused>(r));
}

TEST(CASRequests, ExpiredTokenIsAmbiguousAndReissuedOnce)
{
    FakeClock clock; auto b = std::make_shared<InMemoryBackend>();
    b->failNextWriteWith("k", DB::S3Exception(Aws::S3::S3Errors::INVALID_CLIENT_TOKEN_ID, "ExpiredToken"));
    auto rq = makeRequests(b, clock);
    auto op = rq.admit();
    auto r = op.create("k", "v", Retry::standard());
    ASSERT_TRUE(std::holds_alternative<Committed>(r));
    EXPECT_EQ(std::get<Committed>(r).attempts_sent, 2u);
}

TEST(CASRequests, AdmissionIsCheckedAtThreePoints)
{
    FakeClock clock; auto b = std::make_shared<InMemoryBackend>();
    std::atomic<uint64_t> gen{1}; std::atomic<bool> lost{false};
    Fence fence{[&] { return gen.load(); },
                [&](uint64_t admitted, uint64_t) { return (lost || admitted != gen) ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok; },
                [&](uint64_t) {}};
    auto rq = makeRequests(b, clock, fence);
    /// (1) before the first attempt, on a resumed handle from an older generation
    {
        auto op = rq.resume(0);
        auto r = op.create("k", "v", Retry::standard());
        auto * g = std::get_if<GaveUp>(&r);
        ASSERT_TRUE(g); EXPECT_EQ(g->why, GaveUp::Why::FenceLost); EXPECT_FALSE(g->sent_any);
    }
    /// (2) before the next verb of the same handle, after a re-arm between two verbs (the ensureBlobPresent shape)
    {
        auto op = rq.admit();
        EXPECT_TRUE(op.head("k", Retry::standard()) == std::nullopt);
        gen = 2;   /// re-armed, fence open
        auto r = op.create("k", "v", Retry::standard());
        ASSERT_TRUE(std::holds_alternative<GaveUp>(r));
        EXPECT_FALSE(std::get<GaveUp>(r).sent_any);
    }
    /// (3) after a proven commit: the write landed, then the fence tripped before the call returned
    {
        auto op = rq.admit();
        b->onWriteCommitted("k2", [&] { lost = true; });      /// add this hook to InMemoryBackend in this task
        auto r = op.create("k2", "v", Retry::standard());
        auto * g = std::get_if<GaveUp>(&r);
        ASSERT_TRUE(g); EXPECT_EQ(g->why, GaveUp::Why::FenceLost); EXPECT_TRUE(g->sent_any);
        EXPECT_TRUE(b->read("k2", RawDoor::key()));           /// and the write IS durable
    }
}

TEST(CASRequests, LivenessPredicateEndsTheOperationLikeAFenceLoss)
{
    FakeClock clock; auto b = std::make_shared<InMemoryBackend>(); auto rq = makeRequests(b, clock);
    bool alive = true;
    auto op = rq.admit([&] { return alive; });
    EXPECT_TRUE(op.admitted());
    alive = false;
    EXPECT_FALSE(op.admitted());
    auto r = op.create("k", "v", Retry::standard());
    ASSERT_TRUE(std::holds_alternative<GaveUp>(r));
    EXPECT_EQ(std::get<GaveUp>(r).why, GaveUp::Why::FenceLost);
}

TEST(CASRequests, LeaseBoundPolicyIssuesNothingPastTheBoundary)
{
    FakeClock clock; auto inner = std::make_shared<CountingBackend>();
    auto t = std::make_shared<ThrottlingBackend>(inner, ThrottlingBackend::Mode::EveryNth, 1, 429);
    auto rq = makeRequests(t, clock);
    rq.setAttemptReservationForTest(1'000);
    const uint64_t lease_deadline = clock.now + 10'000;
    auto op = rq.admit();
    auto r = op.replace("k", "v", CasIncarnationTestAccess::mint(t->backendId(), "k", Dialect::Emulated, "1"), Retry::untilLeaseSafe(lease_deadline, 2'000));
    ASSERT_TRUE(std::holds_alternative<GaveUp>(r));
    EXPECT_EQ(std::get<GaveUp>(r).deadline_source, GaveUp::Source::Lease);
    EXPECT_LE(clock.now, lease_deadline - 2'000);             /// zero requests after lease − margin
}

TEST(CASRequests, ReadModifyWriteLosesNoIncrementUnderContentionAndBoundsAHotKey)
{
    FakeClock clock; auto b = std::make_shared<InMemoryBackend>(); auto rq = makeRequests(b, clock);
    auto op = rq.admit();
    orThrow(op.create("ctr", "0", Retry::standard()), "t");
    auto increment = [&](CasOperation & o)
    {
        return o.readModifyWrite("ctr", [](const std::optional<Object> & cur) -> std::optional<String>
        {
            return std::to_string(std::stoi(cur ? cur->bytes : "0") + 1);
        }, Retry::standard());
    };
    for (int i = 0; i < 50; ++i) orThrow(increment(op), "t");
    EXPECT_EQ(op.read("ctr", Retry::standard())->bytes, "50");
    /// perpetual conflict: a backend that rewrites the key under us on every write
    b->onWriteCommitted("ctr", [&] { auto k = RawDoor::key(); (void)b->write("ctr", "x", std::nullopt, k); });
    /// (the hook fires after commit, so the next read sees "x" and the next replace conflicts forever)
}

TEST(CASRequests, DecideMayThrowAndTheExceptionPropagatesUnchanged)
{
    FakeClock clock; auto b = std::make_shared<CountingBackend>(); auto rq = makeRequests(b, clock);
    auto op = rq.admit();
    struct Marker {};
    EXPECT_THROW(op.readModifyWrite("k", [](const std::optional<Object> &) -> std::optional<String> { throw Marker{}; }, Retry::standard()), Marker);
    EXPECT_EQ(b->putTotal(), 0u);
}

TEST(CASRequests, OnPresenceIssuesHeadsAndNoGet)
{
    FakeClock clock; auto b = std::make_shared<CountingBackend>(); auto rq = makeRequests(b, clock);
    auto op = rq.admit();
    auto r = op.readModifyWriteOnPresence("k", [](const std::optional<Meta> & m) -> std::optional<String> { return m ? std::nullopt : std::optional<String>("v"); }, Retry::standard());
    ASSERT_TRUE(std::holds_alternative<Committed>(r));
    EXPECT_EQ(b->getTotal(), 0u); EXPECT_GE(b->headTotal(), 1u);
}

TEST(CASRequests, ForEachListedKeyStopsEarlyAndBudgetsPerPage)
{
    FakeClock clock; auto b = std::make_shared<CountingBackend>(); auto rq = makeRequests(b, clock);
    auto op = rq.admit();
    for (int i = 0; i < 25; ++i) orThrow(op.create("p/" + std::to_string(i), "v", Retry::standard()), "t");
    size_t seen = 0;
    op.forEachListedKey("p/", [&](const ListedKey &) { return ++seen < 3; }, Retry::standard(), /*page_limit=*/10);
    EXPECT_EQ(seen, 3u);
    EXPECT_EQ(b->listTotal(), 1u);
}

TEST(CASRequests, DeleteMarkerIsANamedException)
{
    FakeClock clock; auto b = std::make_shared<InMemoryBackend>(); b->setSimulateDeleteMarkers(true);
    auto rq = makeRequests(b, clock); auto op = rq.admit();
    auto inc = *orThrow(op.create("k", "v", Retry::standard()), "t");
    expectThrowsCode(DB::ErrorCodes::CAS_DELETE_MARKER, [&] { op.remove("k", inc, Retry::standard()); });
}
```

Add to `InMemoryBackend` in this task: `failNextWriteWith(key, S3Exception)` (one-shot: the next `write` on the key throws that exception without applying) and `onWriteCommitted(key, std::function<void()>)` (fires after the write is applied, before the value is returned). `CountingBackend` gains `resetCounts`, `putTotal`, `getTotal`, `headTotal`, `listTotal` if it lacks them (it has the per-op counters; add the accessors).

- [ ] **Step 2: Commit (no build)**

```bash
git add src/Disks/tests/gtest_cas_requests.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.cpp src/Disks/tests/cas_test_helpers.h
git commit -F - <<'MSG'
ca-tests: the engine's contract, on fake clocks

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01DAu8zEhBXrwRKTPhpXgZnS
MSG
```

---

### Task 7: Checkpoint CP3 — build the new API, run everything {#task-7}

**Amendments (second review, binding):** Task 7 converts NO test overrides (the legacy virtuals stay, so nothing breaks at CP3); it is build + gate + the compile fixes of Tasks 3-6 only. Test fault-injection overrides move with their production sites in Tasks 8-18.

**Files:** whatever the compiler and the gate name; no new files.

- [ ] **Step 1: Build**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_task7.log 2>&1; echo NINJA_EXIT=$? >> build_debug/build_task7.log`
Expected: errors — this is the first compile of Tasks 3–6 and of the legacy forwarders over 73 test subclasses whose legacy overrides no longer override anything. Fix in this order: (a) header/typing errors in the new files; (b) every test subclass that overrides a legacy method: convert the override to the new primitive signature (mechanical: `std::optional<GetResult> get(const String & key, Range) override` → `std::optional<Raw> read(const String & key, TransportAccess & a) override` forwarding to `InMemoryBackend::read(key, a)`; `putIfAbsent/putOverwrite/casPut` overrides → one `write` override switching on `expected_value`; `deleteExact` → `remove`; `head` → `head(key, a)`; `list` → `list(prefix, cursor, limit, a)`; `getStream` → `stream`; `publishBlob` → `publish`). Census §5b lists every subclass and the methods it overrides. The overrides' *behaviour* (counting, scripting, failing) is preserved exactly.
Repeat until `NINJA_EXIT=0`.

- [ ] **Step 2: Run the gate**

Run: `build_debug/src/unit_tests_dbms --gtest_filter='CAS*' > build_debug/test_task7_gate.log 2>&1; echo GTEST_EXIT=$? >> build_debug/test_task7_gate.log`
Expected: `GTEST_EXIT=0`. The new suites (`CASIncarnation`, `CASRetry`, `CASWriteResult`, `CASFence`, `CASBackendPrimitives`, `CASThrottlingBackend`, `CASRequests`) all pass; every old suite still passes through the legacy forwarders. A red old test here means a forwarder changed behaviour — fix the forwarder, not the test, unless the test asserted a retired seam (`Range`, `ObjectMeta`, `GetStreamResult::token`), which Task 4 already deleted.

- [ ] **Step 3: Commit**

```bash
git add <the files the fixes touched — name them>
git commit -F - <<'MSG'
ca-tests: test doubles override the transport primitives; the new API builds and the gate is green

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01DAu8zEhBXrwRKTPhpXgZnS
MSG
```

---

## Migration batches {#migration-batches}

Four batches, each two migration tasks (no build) and one checkpoint (build + gate + fix). Every batch follows the same recipe; the recipe is written once here and each task names its files, its census rows and its verb mapping.

**The recipe for a migration task.**

1. Read the spec's "Where each verb goes" table rows for the task's functions, then the census rows the task names (line numbers of every legacy call).
2. For each function declared over `Backend &` in the task (census §2): change the parameter to `CasOperation & op` (or, for a function that admits its own operation, `CasRequests & requests` and an `op = requests.admit(...)` at the top). Every caller in other files gets the matching one-line call-site edit in the same task.
3. Replace each legacy call by the verb and policy the spec's table names. The patterns:

```cpp
// read
auto got = backend.get(key);                    →  auto got = op.read(key, Retry::standard());
// got->token → got->incarnation; got->bytes unchanged
// head
HeadResult hr = backend.head(key);              →  std::optional<Meta> hr = op.head(key, Retry::standard());
// hr.exists → hr.has_value(); hr.token → hr->incarnation; hr.size → hr->size
// create
auto pr = backend.putIfAbsent(key, bytes);      →  WriteResult r = op.create(key, bytes, Retry::standard());
// pr.outcome == Done → std::holds_alternative<Committed>(r); pr.token → std::get<Committed>(r).incarnation
// PreconditionFailed → std::get_if<Conflict>(&r) (its seen carries the occupant)
// replace
auto pr = backend.putOverwrite(key, bytes, tok); →  WriteResult r = op.replace(key, bytes, incarnation, Retry::standard());
auto cr = backend.casPut(key, bytes, expected);  →  expected ? op.replace(key, bytes, *expected, policy) : op.create(key, bytes, policy)
// remove
auto d = backend.deleteExact(key, tok);          →  Removal d = op.remove(key, incarnation, Retry::standard());
// Deleted → Removed; NotFound → Gone; TokenMismatch → Mismatch; created_delete_marker → the CAS_DELETE_MARKER exception
// list
ListPage p = backend.list(prefix, cursor, n);    →  ListPage p = op.list(prefix, cursor, n, Retry::standard());
forEachListedKey(backend, prefix, cb, ...)       →  op.forEachListedKey(prefix, [&](const ListedKey & k) { cb(k); return true; }, Retry::standard(), ...)
// stream / publish
auto s = backend.getStream(key);                 →  auto s = op.stream(key, Retry::standard());   // s is the ReadBuffer; no token
backend.publishBlob(req);                        →  op.publish(req, Retry::standard());
// a hand-rolled read → decide → conditional write loop
for (attempt...) { get; mutate; casPut; }        →  op.readModifyWrite(key, [&](const std::optional<Object> & cur) -> std::optional<String> { ... }, Retry::standard())
```

4. A caller that switched on `CasWriteOutcome`/`PutOutcome`/`CasOutcome` switches on the `WriteResult` alternatives; a caller that threw on exhaustion uses `orThrow(std::move(r), "<what>")`; a caller that needs the exception shape `checkFenceOrThrow` threw uses `orThrow` too (it maps `FenceLost` back). Never convert an `Incarnation` to a `Token` or back — if a neighbour still speaks `Token`, that neighbour is in this task or a later batch; a struct field that crosses the boundary is converted in the batch that owns the struct (census §3).
5. Tests: the test files census §5c maps to the task's production files are updated in the same commit — assertions on `token` fields become assertions on `incarnation`; test doubles' overrides were already moved at CP3; a test that instrumented a site through a legacy override keeps working because the override is on the primitive. Add one new test per row of the spec's table that names a behaviour change (bounded loop, terminal conflict, `once`, the open-fence farewell) — the checkpoint task runs them.
6. Commit with the message shape `ca-<area>: <files> on CasOperation — <verbs>`. Do not build.

The migration implementer is a narrow-case worker (Sonnet): one dispatch per task, the census rows and the recipe as its brief, no builds, no test runs.

---

### Task 8: Batch 1a — pool wiring, sentinel probe, capability probe, plain objects, pool meta, manifest reader, pool reads {#task-8}

**Amendments (second review, binding):**
- `CasMountRuntime::admit` uses saturating arithmetic:
```cpp
if (lost || generation != admitted) return LostOrRearmed;
if (now >= deadline) return NoBudget;
const uint64_t remaining = deadline - now;
if (needed_ms >= remaining || lease_safety_margin_ms >= remaining - needed_ms) return NoBudget;
return Ok;
```
- `ObjectStorageBackend`'s `single_attempt_control_plane`/`attempt_timeout_ms` are set in `ContentAddressedMetadataStorage::openPoolView`, which constructs it; `Pool` only wires the three `CasRequests`.
- Test overrides in the files census §5c maps to this task's production files move to the primitives here, in this commit, and the report names each and the test that shows the fault still fires.

**Files:**
- Modify: `Pool/CasPool.h`, `Pool/CasPool.cpp` — add the three `CasRequests` members and accessors: `CasRequests & mountRequests()` (over the mount fence: `Fence{[&]{ return mount_runtime.fenceGeneration(); }, [&](uint64_t g, uint64_t needed){ return mount_runtime.admit(g, needed); }, [&](uint64_t g){ mount_runtime.checkFenceOrThrow(g); }}`), `CasRequests & farewellRequests()` (open fence), `CasRequests & gcRequests()` (open fence); `CasMountRuntime` gains `Fence::Admit admit(uint64_t admitted_generation, uint64_t needed_ms) const` = `lost || generation != admitted → LostOrRearmed; needed_ms + lease_safety_margin_ms >= deadline − now → NoBudget; else Ok`. Migrate the `get`/`list` sites census §1 lists for `CasPool.cpp` (`get`: 256,847,1640,1687; `list`: 1767).
- Modify: `Backend/CasSentinelProbe.h`, `.cpp` (`probeSentinel`, `probePoolBootstrapResidual`: census `get`: 91; `list`: 50; `probeSentinelRaw`: 11) → `op.probeSentinel`, `op.forEachListedKey` with an early stop, `op.read`.
- Modify: `Backend/CasProbe.h`, `.cpp` (`runCapabilityProbe`: census `casPut`: 133,142,151,166; `deleteExact`: 38,180,213,246; `get`: 65,78,106,123,159,171,185,226; `head`: 36,244; `list`: 194,231; `putIfAbsent`: 58,73; `putOverwrite`: 101,115) — every call `standard`; **reorder** so every wrong value is the same key's previous incarnation: overwrite with `t1` first, then the refusal check with `t1`; on the CAS key create and commit with `ct1`, then the stale conflict; the wrong-token delete with `t1`. The delete-marker step catches `CAS_DELETE_MARKER` and rethrows its `NOT_IMPLEMENTED` operator message unchanged.
- Modify: `Pool/CasPlainObjects.h`, `.cpp` (`casPutObject` → `readModifyWriteOnPresence`, `standard`; `casRemoveObject` → `removeCurrent`, `standard`; `casGetObject` → `read`; `listNamespaceFiles` → `forEachListedKey`; `mountpointObjectExists` → `head`; census `deleteExact`: 83; `get`: 61; `head`: 42,79,146; `list`: 108; `putIfAbsent`: 46; `putOverwrite`: 51). The `check_fence_or_throw_fn(admitted_generation)` calls go: the class takes a `CasRequests &` and admits an operation per call (`resume(admitted_generation)` where a caller passes one).
- Modify: `Pool/CasPoolMeta.cpp`, `Formats/CasPoolMetaFormat.h` (`admitOrValidate` → `readModifyWrite`, `standard`; `createOrValidate` → `read` first, `create` on absence, then `admitOrValidate`; census `casPut`: 92,155; `get`: 96,124,160).
- Modify: `Pool/CasManifestReader.h`, `.cpp` (`readManifestShared`: `get`: 55 → `read`, `standard`).
- Modify: `Pool/CasRefProtocol.h`, `.cpp` (reads only: `get`: 885,974,1012,1035,1080 → `read`, `standard`; the three functions over `Backend &` take `CasOperation &`).
- Modify: `ContentAddressedTransaction.cpp` (`getStream`: 292 → `op.stream` under `standard` on the mount plane's operation).
- Tests (census §5c): the files it maps to these production files; plus new: `CASPlainObjects.CasPutObjectIssuesHeadsOnly`, `CASPlainObjects.CasRemoveObjectReheadsOnMismatch`, `CASPoolMeta.AdmitOrValidateEndsAtTheDeadlineUnderPerpetualConflict`, `CASProbe.ReorderedProbePassesOnAllThreeDialects`.

**Interfaces:**
- Consumes: Tasks 3–5.
- Produces: `Pool::mountRequests()`, `Pool::farewellRequests()`, `Pool::gcRequests()`; `CasMountRuntime::admit`; `runCapabilityProbe(CasOperation &, ...)`; `probeSentinel(CasOperation &, ...)`; `probePoolBootstrapResidual(CasOperation &, ...)`; `CasPlainObjects(CasRequests &, ...)`; `admitOrValidate(CasOperation &, ...)`, `createOrValidate(CasOperation &, ...)`; `CasManifestReader(CasRequests &, ...)`; the three `CasRefProtocol` readers over `CasOperation &`.

- [ ] **Step 1: Wire the planes into `Pool`** (code above).
- [ ] **Step 2: Migrate the files in the order listed**, per the recipe.
- [ ] **Step 3: Update the mapped tests; add the four new tests.**
- [ ] **Step 4: Commit (no build).**

```bash
git add <the files above>
git commit -F - <<'MSG'
ca-pool: the three planes' CasRequests; sentinel probe, capability probe, plain objects, pool meta, manifest reader and pool reads on CasOperation

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01DAu8zEhBXrwRKTPhpXgZnS
MSG
```

---

### Task 9: Batch 1b — GC leaves and the tools {#task-9}

**Amendments (second review, binding):** this task keeps only the sites with no persisted-token flow — `Gc/CasGcMaintenanceState`, `Gc/CasNamespaceJanitor`, `Tools/CasDecommission` (and the `getStream` caller in `Gc/CasBlobInDegree.cpp:openSourceEdgeRun` → `stream`, after which the legacy `getStream` is deleted from the three backends). `Gc/CasBlobInDegree`'s token-bearing structs (`RetiredEntry`, `CondemnedRow`, `SourceEdgeRecord`), `Gc/CasOrphanManifestSweep` (its nomination feeds a persisted compare), `Tools/CasFsck` and `Tools/CasInspect` (they render/compare persisted rows) move to Task 18 — the persisted-token component. Test overrides move with their sites.

**Files:**
- Modify: `Gc/CasBlobInDegree.h`, `.cpp` (`openSourceEdgeRun` → `stream`; `putDeterministicArtifact` → `create` with compare-adopt on `Conflict`'s `Object`; `foldDeltasIntoGeneration`, `zeroInDegree`, `PriorEdgeCursor` over `CasOperation &`; census `get`: 339; `getStream`: 281; `putIfAbsent`: 337; token fields at `.h` 29,77,202 → `Incarnation`/`PersistedIncarnation` as the struct dictates — the condemned-row byte layout keeps `token_type` as the persisted dialect byte and becomes `PersistedIncarnation` in Task 18).
- Modify: `Gc/CasGcMaintenanceState.h`, `.cpp` and `Gc/CasNamespaceJanitor.h`, `.cpp` (`casGcMaintenanceState` → `create` on absence else `replace`, `standard`; the write inside `catch (...)` in `runOnePage` → `once`; `readGcMaintenanceState` → `read`; census `casPut`: 34; `get`: 14; janitor `deleteExact`: 111; `head`: 91; `list`: 25).
- Modify: `Gc/CasOrphanManifestSweep.h`, `.cpp` (`forEachListedKey`: 557; `get`: 58,97,101,255,304,642; `head`: 583; `deleteExact`: 586; `list`: 616 — reads, one list walk, one `remove`; all `standard`).
- Modify: `Tools/CasFsck.cpp` (`forEachListedKey`: 60,528; `get`: 378,643,817,833,922; `head`: 654,754,958,1056 — over `pool.gcRequests().admit()` (open fence); all `standard`).
- Modify: `Tools/CasDecommission.h`, `.cpp` (`deleteExact`: 68,92; `forEachListedKey`: 50,222; `get`: 296,318,380,390,408; `head`: 59,179; `putOverwriteControlled`: 422 → `replace`, `standard`, on an open-fence `CasRequests` the tool constructs itself — replacing its private `CasRequestController` with `fence_ok = [] { return true; }`; the `CasRequestBudget` at 421 goes).
- Modify: `Tools/CasInspect.cpp` (`Token` at 290 → render an `Incarnation`; reads → `read`).
- Tests: census §5c files for these; new: `CASGcMaintenanceState.CatchPathWriteIsOnce`, `CASDecommission.RunsOnAnOpenFence`.

**Interfaces:** consumes Task 8's `gcRequests()`; produces the listed functions over `CasOperation &`.

- [ ] **Step 1: Migrate per the recipe.**
- [ ] **Step 2: Tests.**
- [ ] **Step 3: Commit (no build):** `ca-gc: blob in-degree, maintenance state, janitor, orphan sweep, fsck, decommission and inspect on CasOperation`.

---

### Task 10: Checkpoint CP4 {#task-10}

- [ ] **Step 1:** `ninja -C build_debug unit_tests_dbms > build_debug/build_task10.log 2>&1; echo NINJA_EXIT=$? >> build_debug/build_task10.log` — fix until `NINJA_EXIT=0` (signature mismatches at the batch boundary are expected: a caller in a later batch's file still passes `Backend &` — give it a one-line `pool.gcRequests().admit()` at the call, never a `Token`/`Incarnation` conversion).
- [ ] **Step 2:** `build_debug/src/unit_tests_dbms --gtest_filter='CAS*' > build_debug/test_task10_gate.log 2>&1; echo GTEST_EXIT=$? >> build_debug/test_task10_gate.log` — `GTEST_EXIT=0`.
- [ ] **Step 3:** Commit the fixes: `ca-tests: batch 1 builds and the gate is green`.

---

### Task 11: Batch 2a — the ref catalog and the checkpoint publisher {#task-11}

**Files:**
- Modify: `Pool/CasRefCatalog.h`, `.cpp` — `Snapshot::token` (h:29) → `std::optional<Incarnation> incarnation`; `casUpdateImpl` (cpp:115; `casPut`: 127; `get`: 63) → one `readModifyWrite` under `standard`, the `mutate` lambda inside `decide` (its four typed markers propagate unchanged); `read` (41) → `read`; `readOptionalForBootstrap` (25) → `read`; `initializeEmptyForNewPool` (52; `putIfAbsent`: 56) → `create`; `deleteCompletedRemovingAtSnapshot` (362; `casPut`: 414) stays hand-written over `read` + `replace` + post-write `read` under ONE `Retry::standard()` captured before the loop, `Retry::backoff(attempt)` between iterations, `LeaderFenceStatus` parameter deleted and the two fence probes (404, 429) → `op.admitted()`; the `mutate` lambdas' `check_fence_or_throw` try/catch (301, 470, 521, 621) → `if (!op.admitted()) throw CatalogFenceMovedMarker{}`. Every function over `Backend &` (census §2, 16 functions) takes `CasOperation &`.
- Modify: `Pool/CasRefCkpt.h`, `.cpp` — `publishCkpt` (197; `casPut`: 306; `get`: 189) → `readModifyWrite` under `standard` on the operation the caller passes (the caller resumed it under the persisted generation); the four `check_fence_or_throw` points (250, 273, 287, 331) → `op.admitted()` at the two decline-time verdicts (`IdenticalSkip`, the epoch-decrease arm) and deleted at the two pre-request points (the engine checks); the `check_admitted` callback parameter deleted; `readCkpt` (187) → `read`; token fields (cpp 297,298,362; h 133,164) → `Incarnation`.
- Modify (call sites only): `Pool/CasRefLedger.cpp` calls of `publishCkpt`/`publishCkptContribution` and of the catalog (they pass `pool.mountRequests().resume(admitted_generation, liveness)` where `liveness` is the composite predicate minus its generation term — see Task 14 for the predicate bodies; in this task the call sites pass `Liveness{}` and Task 14 fills them in); `Gc/CasGc.cpp` reads of `catalog_snapshot.token` (1595 and the other §3 sites) → `.incarnation`; `Gc/CatalogLifecycleReconciler.cpp` (fence probe at 80 → `op.admitted()`).
- Tests: `gtest_cas_ref_catalog*.cpp`, `gtest_cas_ref_ckpt*.cpp` (census §5c); new: `CASRefCatalog.CasUpdateEndsAtTheDeadlineNotAfterAHundredUnsleptIterations`, `CASRefCkpt.DeclineTimeVerdictsReadAdmitted`.

- [ ] Steps: migrate per the recipe; tests; commit (no build): `ca-ref: catalog and checkpoint publisher on CasOperation — readModifyWrite, admitted() at the verdict points`.

---

### Task 12: Batch 2b — the GC core {#task-12}

**Amendments (second review, binding):** this task migrates the GC core's LIVE paths only — `acquireOrRenewLease`, `pulseHeartbeat`, the round and rebuild commits, the folds' `forEachListedKey` walks and reads. The retirement/redelete/outcomes paths (`RetiredEntry`, `ReplacedEntry`, `OutcomeEntry::token`, the delete loops at 671/1061/1111/3330/3434/3438, `deleteConfirmedMeta`, `CasGcMetaWriter`'s condemn markers) stay on the legacy methods here and migrate in Task 18 with the formats, so no batch boundary needs a Token↔Incarnation conversion. Test overrides move with their sites.

**Files:**
- Modify: `Gc/CasGc.h`, `.cpp` — over `pool.gcRequests().admit()` (open fence); per the spec's table: `acquireOrRenewLease` (4373, 4397, 4418, 4472; reads 4363, 4382, 4430, 4481) → one `readModifyWrite` under `standard` whose decide is the steal machine (renew when ours; steal only when the lease tuple is unchanged across two observations AND the heartbeat pair is unchanged AND `allow_steal`; `nullopt` otherwise; its `gc/hb` read under `standard`; `CORRUPTED_DATA` on vanish-after-observe from inside decide); `pulseHeartbeat` (4365, 4376) → `read` under `standard` then `replace`/`create` under `once`, conflict ignored; the round commit (944) and the rebuild commit (4246) → `replace`, `standard`, conflict → today's `ABORTED`; every `deleteExact` (671, 1061, 1111, 3330, 3434, 3438) → `remove` with the persisted compare (`PersistedIncarnation::matches(meta->incarnation)` after a `head`, `standard`) or the live incarnation; every `forEachListedKey` (1347, 1470, 3654, 3918, 3955, 4151) → `op.forEachListedKey`; every `get`/`head` → `read`/`head`, `standard`; the `check_fence` callback body (4522, 4523) deleted (the catalog reads `op.admitted()` now). `Token` fields in `CasGc.h` (456, 515, 540, 700, 789) → `Incarnation` / `PersistedIncarnation` as each is live or persisted.
- Modify: `Gc/CasGcMetaWriter.h`, `.cpp` (token fields 36, 57–59, 72–74; cpp 137, 190–218 → `Incarnation`; `deleteConfirmedMeta` (76) → `remove` after the confirmation registry's render compare).
- Modify: `Gc/CasGcShardPlan.h`, `.cpp` (`reduce` over `CasOperation &`).
- Tests: `gtest_cas_gc*.cpp` (census §5c); new: `CASGc.HeartbeatPulseIsOnceAndAConflictIsIgnored`, `CASGc.LeaseDecideStealsOnlyWithAllThreeConjuncts`, `CASGc.RoundCommitConflictDropsTheRound`.

- [ ] Steps: migrate; tests; commit (no build): `ca-gc: lease, heartbeat, commits, deletes and folds on CasOperation`.

---

### Task 13: Checkpoint CP5 {#task-13}

As Task 10, logs `build_task13.log` / `test_task13_gate.log`; commit `ca-tests: batch 2 builds and the gate is green`.

---

### Task 14: Batch 3a — the ref ledger {#task-14}

**Files:**
- Modify: `Pool/CasRefLedger.h`, `.cpp` — the ref lane: `commitRefChunk` (`putIfAbsentControlled`: 3748) → `create` under `standard` on `pool.mountRequests().resume(admitted_fence_generation, liveness)` where `liveness = [&] { return !rt->catalog_life_invalidated.load() && !rt->superseded_by_remount.load(); }` (today's `check_commit_admitted` minus its generation term); the switch's arms become `Committed` (install, publish the frontier on the same handle), `Refused` (return the attempt to `Ready`, `CASRefAppendDefiniteFailure`), `Conflict{Object}` (a different occupant at the content-addressed key: compare, `CORRUPTED_DATA`), `GaveUp{sent_any=false}` (`Ready`, `CASRefAppendPreAttemptRefused`), `GaveUp{sent_any=true}` (`Wedged`, `CASRefAppendWedged`). `resolveWedgeOnce` (`slotOccupy`: 2354) → `create` under `once` on `resume(wedge.admitted_fence_generation, [&]{ ... same_wedge_under_lock() ... })` (today's `check_wedge_admitted` minus the generation term); the recovery walk (`slotOccupy`: 1195; `putIfAbsentControlled`: 266, 4550; `putIfAbsentControlledMutable`: 278; `putOverwriteControlled`: 273) → `create`/`replace` on resumed handles with `admitted_fence_ok`'s remaining terms (`!token->stopping() && !catalog_life_invalidated && !superseded_by_remount`) as liveness; the `staging*` entry points (`stagingPutIfAbsent`, `stagingPutIfAbsentMutable`, `stagingConditionalOverwrite`) → `create`/`replace` returning `WriteResult`; every `unresolvedProvesNothingWasSent` (1263, 2520, 4066, 4085) → `!gave_up.sent_any`; the seven `get`s → `read`; the fence-trio sites census §4 lists (652/654, 4954/4956, 5021/5023, 5078) → the pre-request check deleted, the post-read check `op.admitted()`; the pre-sampled cancellation flag rule does not apply here (the ledger has no cancellation verdict).
- Modify: `Pool/CasPool.h`, `.cpp` — `stagingPutIfAbsent(key, bytes, Token *)` (722, 724) → returns `WriteResult`; its delegation to the ledger.
- Tests: the six files including `CasRequestControl.h` that are ref-lane tests (`gtest_cas_slot_occupy.cpp`, `gtest_cas_ref_wedge_every_attempt.cpp`, `gtest_cas_ref_install_safety.cpp`, `gtest_cas_confirm_exact_ref.cpp`, `gtest_cas_ref_contiguous_alloc.cpp`) re-pointed at `WriteResult`; `gtest_cas_request_control.cpp` is NOT touched (it dies with the old controller at the lock); new: `CASRefLane.RefusedReturnsTheAttemptToReadyAndDoesNotWedge`, `CASRefLane.PostCommitFenceLossWedges`, `CASRefLane.LivenessPredicateWithoutGenerationTermStillRefusesARetiredRuntime`.

- [ ] Steps: migrate; tests; commit (no build): `ca-ref: the ref lane on resumed operations — four arms, sent_any, liveness without a generation term`.

---

### Task 15: Batch 3b — blob meta, part write, mount runtime {#task-15}

**Amendments (second review, binding):** the five surviving budget fields (`attempt_timeout_ms`, `lease_safety_margin_ms`, the three `recovery_retry_*`) and `validateCasRequestBudget` (the surviving clause) move here into a new `Backend/CasRequestBudget.h`/`.cpp` (no controller include), so the lock can delete `CasRequestControl.h` without orphaning them; every includer of `CasRequestControl.h` that only needed the budget switches to the new header.

**Files:**
- Modify: `Pool/CasBlobMeta.h`, `.cpp` (`loadMeta` (16) → `read`; `deleteMetaExact` (39) → `remove`; token fields 23, 57, 63 → `Incarnation`; `casMeta` → `replace`).
- Modify: `Pool/CasPartWriteTxn.cpp` — `ensureBlobPresent` (331 `head`, 417 `publishBlob`): the outer publication loop stays under ONE `Retry::standard()` with `Retry::backoff` between iterations; `head`/`publish`/`create` on the operation admitted at txn start (`pool.mountRequests().admit()` captured once, replacing the `fenceGeneration()` capture at 265); the seven fence checks: 392 and 445 (pre-request) deleted, 362/371/383/447/464 (verdict points) → `op.admitted()`; `reconcileMetaClean` → `readModifyWrite`, `standard`; `isDeterministicBlobPublicationFailure` (73) → over `isDefinitelyRefusedWrite` and moved out of the old controller's file if it lived there; `promote` (737 `get`) → `read`; `cleanupStagedManifestDebrisBestEffort` (1137 `head`, 1139 `deleteExact`) → `head` + `remove`; `stageManifest` compares the `Conflict{Object}` and throws its definite-failure message on `Refused`.
- Modify: `Pool/CasMountRuntime.h`, `.cpp` — `refAppendFenceOk` becomes `admit(...) == Ok` with `needed_ms = attempt_timeout_ms`; keep `checkFenceOrThrow` for `orThrow`'s mapping; the `CasRequestBudget` member keeps `attempt_timeout_ms`, `lease_safety_margin_ms` and the recovery-walk three.
- Modify: `ContentAddressedTransaction.cpp` — the staging write buffer's `checkFenceOrThrow` callback (926) stays (outside this API by design) but takes the generation from the operation handle (`op.generation()`), not a separate capture (912).
- Tests: `gtest_cas_part_write*.cpp`, `gtest_cas_blob_meta*.cpp`, `gtest_cas_mount_runtime*.cpp` (census §5c); new: `CASPartWrite.EnsureBlobPresentSharesOneRetryAcrossItsLoop`, `CASPartWrite.DependencyProofIsRefusedAfterARearm`.

- [ ] Steps: migrate; tests; commit (no build): `ca-write: blob meta, part write and the mount runtime's admit on CasOperation`.

---

### Task 16: Checkpoint CP6 {#task-16}

As Task 10, logs `build_task16.log` / `test_task16_gate.log`; commit `ca-tests: batch 3 builds and the gate is green`.

---

### Task 17: Batch 4a — the server root {#task-17}

**Amendments (second review, binding):** add `Pool/CasMountRuntime.cpp`'s `consumeRenewResult` (it reads the renewal diagnostics — rewrite over the new result fields: `attempts_sent`, `resolved_by_read`, `deadline_source`, `sent_any`, the site's pre-sampled cancellation flag) and `src/Storages/System/StorageSystemContentAddressedMounts.cpp` (`read` consumes `listMounts`' result and mount diagnostics; its signature follows `listMounts`) to this task's files. Test overrides move with their sites.

**Files:**
- Modify: `Pool/CasServerRoot.h`, `.cpp` — `MountLeaseKeeper::renew` (1735 `putOverwriteControlled`) → `replace` under `Retry::untilLeaseSafe(confirmed_deadline_boot_ms, lease_safety_margin_ms)` on `pool.mountRequests().admit()`; its verdicts: `Committed` → as today; `Conflict{Object}` → `throwRenewConflict` over the observation; `Conflict{ProvenAbsent}` → `Vanished` (`FILE_DOESNT_EXIST`); `Conflict{NotObserved}` / `GaveUp{Deadline|Unresolved}` → retry-later; `GaveUp{FenceLost, sent_any=false}` with the pre-sampled cancellation flag true → `NotAttempted`, else terminal; `CASMountRenewalRecovered` from `Committed::attempts_sent > 1 || resolved_by_read`; `CASMountRenewalDeadlineExceeded` from `GaveUp{Deadline, Source::Lease}`. `MountLeaseKeeper::terminate` (1835 `putOverwrite`, 1838 `get`) → `replace` under `Retry::within(10'000)` on `pool.farewellRequests().admit()`; on `Conflict` the observation is the re-read (silent on `gc_fenced`, else `CASMountExclusivityViolation` + `ABORTED`). `MountLeaseKeeper::claim` (1462 `head`, 1465 `putIfAbsent`, 1476 `get`, 1513 `putOverwrite`, 1516 `get`) → `read` then `create`/`replace`, `standard`, conflict terminal — two requests on both paths. `claimMount` (909 `get`, 915 `putIfAbsent`, 950/982 `putOverwrite`), `claimMountAwaitingExpiry` (1083 `get`), `claimOwnerOrThrow` (682 `putIfAbsent`, 722 `get`) → the same shape. `allocateWriterEpoch` (803 `casPut`; 722 `get`; 745 `probeSentinelRaw`; `serverRootSubtreeEmpty` 63 `list`) → `readModifyWrite`, `standard`, the decide issuing the subtree walk and the sentinel probe under `standard` and carrying "my previous attempt was an absent-create that lost" in captured state. `computeHeartbeatFloor` (1141 `list`, 1164 `get`, 1215 `putOverwrite`) → `readModifyWrite`, `standard`, decide = the stability classification. `listMounts`, `probeNonTerminalMountSlots`, `isCreatorFenceTerminal`, `readOwnerObject`, `readOwnerUuid`, `prefixHasAnyKey` → reads/lists under `standard`. Token fields (h 238, 260, 314, 508, 532; cpp 725, 905, 1064, 1080, 1460, 1548, 1695) → `Incarnation`; `proven_dead_token` stays an `Incarnation` observed live. `MountRenewResult::diagnostics` is replaced by the fields the two counters need (`attempts_sent`, `resolved_by_read`, `deadline_source`, `sent_any`).
- Modify: `Pool/CasPool.cpp`, `ContentAddressedMetadataStorage.cpp` (call sites; the budget struct's construction).
- Tests: `gtest_cas_server_root*.cpp`, `gtest_cas_mount_lease*.cpp`, `gtest_cas_pool.cpp`'s renewal tests (census §5c); the two renewer tests re-pointed (`Vanished` on `ProvenAbsent`, retry-later on `NotObserved`); new: `CASMountLease.FarewellRunsOnAnOpenFenceAfterATerminalRenewal`, `CASMountLease.ClaimAdoptIsTwoRequests`, `CASServerRoot.AllocateWriterEpochKeepsThePostConflictCorruptionCheck`.

- [ ] Steps: migrate; tests; commit (no build): `ca-mount: renew, farewell, claim, epoch allocation and the heartbeat floor on CasOperation`.

---

### Task 18: Batch 4b — the persisted pair and the formats {#task-18}

**Amendments (second review, binding):** this task is the **persisted-token component**, one commit: the three wire formats; `Gc/CasBlobInDegree.{h,cpp}` (`RetiredEntry`, `CondemnedRow`, `SourceEdgeRecord` → `PersistedIncarnation` where persisted, `Incarnation` where live; the condemned-row byte layout); `Gc/CasOrphanManifestSweep` (nomination → persisted compare); `Gc/CasGc.cpp`'s retirement/redelete/outcomes paths (deferred from Task 12: the delete loops at 671/1061/1111/3330/3434/3438 → `head` + `PersistedIncarnation::matches` + `remove`; `OutcomeEntry::token` → `PersistedIncarnation`); `Gc/CasGcMetaWriter.{h,cpp}` (its confirmation registry compares a `PersistedIncarnation` or rendered text, never a live `Incarnation`; `deleteConfirmedMeta` → `head` + compare + `remove`); `Tools/CasFsck.cpp` (retirement check → `matches`); `Tools/CasInspect.cpp` (render persisted rows); `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol.cpp` (`makeSourceEdgeRecords` builds `PersistedIncarnation`). `PersistedIncarnation::capture` (Task 3) is the only way a live value enters a persisted field. Test overrides move with their sites.

**Files:**
- Modify: `Formats/CasWireVocab.h`, `.cpp` (`TokenFields::build` → returns `PersistedIncarnation`; `writeTokenFields(out, first, const PersistedIncarnation &)`; `tokenTypeFromWord` → `dialectWordFromString` producing the `"etag"|"generation"|"emulated"` word), `Formats/CasRecordStreamFormat.h`, `.cpp` (88; 201, 280), `Formats/CasGcOutcomesFormat.h`, `.cpp` (43; 58, 104), `Gc/CasBlobInDegree.h`, `.cpp` (the condemned-row byte layout: `token_type` byte encodes the dialect, `[token_len][token bytes]` the value; decode to `PersistedIncarnation`; 72; 207, 219), `Tools/CasInspect.cpp` (render persisted rows).
- Modify: every site that compared a persisted token with a live one (GC's redelete, `CasFsck`'s retirement check, the fold's supersede compare, the meta writer's confirmation registry) → `persisted.matches(live)`.
- Modify: `Primitives/CasTypes.h` — `Token` stays until the lock; add `using Dialect = TokenType;` if Task 3 did not.
- Tests: `gtest_cas_wire_vocab*.cpp`, `gtest_cas_record_stream*.cpp`, `gtest_cas_gc_outcomes*.cpp`, `gtest_cas_blob_in_degree*.cpp` (census §5c); new: `CASPersistedIncarnation.RoundTripsThroughEveryFormatAndNeverBecomesAnIncarnation` (a compile-time `static_assert(!std::is_constructible_v<Incarnation, PersistedIncarnation>)` plus the three format round trips).

- [ ] Steps: migrate; tests; commit (no build): `ca-formats: PersistedIncarnation in the wire vocabulary, the record stream, the outcomes and the condemned rows`.

---

### Task 19: Checkpoint CP7 {#task-19}

As Task 10, logs `build_task19.log` / `test_task19_gate.log`; commit `ca-tests: batch 4 builds and the gate is green`. After this checkpoint `grep -rn "backend()\.\|backend\.\(get\|head\|putIfAbsent\|putOverwrite\|casPut\|deleteExact\|list\|getStream\|publishBlob\)(" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed --include=*.cpp` must print nothing outside `Backend/`; the report lists any survivor and the checkpoint migrates it.

---

### Task 20: The lock (Step 5) {#task-20}

**Amendments (second review, binding):** prerequisites checklist before deleting anything — (1) `Backend/CasRequestBudget.h` exists (Task 15) and no includer of `CasRequestControl.h` needs the controller; (2) `PersistedIncarnation::capture` exists (Task 3) and the four formats use it (Task 18); (3) `consumeRenewResult` and `StorageSystemContentAddressedMounts` are on the new fields (Task 17); (4) the benchmark builds (Task 18); (5) `grep -rn "getStream(" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed` outside `Backend/` is empty (Task 9). Then delete the legacy virtual forwarders, `migrationAccess`, the `friend class Backend`, `Token`, the controller and its test file, `Pool::backend()`, and rename `KeyEntry`/`KeyPage` → `ListedKey`/`ListPage`, `TokenType` → `Dialect`.

**Files:**
- Modify: `Backend/CasBackend.h` — delete the legacy forwarders (`get`, `getStream`, `head(key)`, `putIfAbsent`, `publishBlob`, `putOverwrite`, `casPut`, `deleteExact`, `list(prefix,cursor,limit)`), `GetResult`, `GetStreamResult`, `HeadResult`, `PutOutcome`, `CasOutcome`, `WriteResultT`, `PutResult`, `CasResult`, `DeleteOutcome`, `ListedKey`(old), `ListPage`(old), the free `forEachListedKey`, `DeleteClass`/`classifyDeleteOutcome`/`deleteClassName` (their consumers switched on `Removal`), `migrationAccess`; `Backend/CasTransportAccess.h` — delete `friend class Backend;`.
- Delete: `Backend/CasRequestControl.h`, `.cpp`; `src/Disks/tests/gtest_cas_request_control.cpp`. Move `throwCasWriteRetryLater`, `makeCasWriteRetryLaterExceptionPtr`, `throwCasTransientUnavailable`, `isDeterministicLocalFailure` into `CasRequests.h/.cpp` (Task 5 may have done it already).
- Modify: `Primitives/CasTypes.h` — delete `Token`; rename `TokenType` → `Dialect` (`using` alias removed).
- Modify: `Pool/CasPool.h`, `.cpp` — delete `Pool::backend()` and `poolBackendPtr()` (the four capability predicates go through `CasRequests::backendForCapabilityPredicates()`); `CasRequestBudget` → loses `operation_deadline_ms`, `max_attempts`, `retry_initial_backoff_ms`, `retry_max_backoff_ms`; `validateCasRequestBudget` keeps `attempt_timeout + margin < TTL` only.
- Modify: `ContentAddressedSettings.h`, `.cpp`, `ContentAddressedMetadataStorage.cpp` — two parsed disk settings `cas_attempt_timeout_ms` (default 5000) and `cas_lease_safety_margin_ms` (default 2000) feeding `PoolConfig::cas_request_budget` and the backend's `attemptTimeoutMs()`; `docs/en/antalya/cas/` reference page for the two settings.
- Modify: `Backend/CasObjectStorageBackend.h`, `.cpp` — delete `mintingTypeMatches`, `tokenForHead`, `tokenForList`, `tokenMatches`, `tokenFromWriteResult`, `Token` uses; `Backend/CasInstrumentedBackend.h` — delete `CasOp::Get`/`GetStream`; `src/Common/ProfileEvents.cpp` — retire `CAS*Get`/`CAS*GetStream` events.
- Modify: `src/Disks/tests/gtest_cas_requests.cpp` — the four `static_assert`s are already there; add `static_assert(!std::is_constructible_v<Incarnation, PersistedIncarnation>)` if Task 18 did not; delete `RawDoor` (tests go through `CasRequests` over `Fence::open()` and `Retry::once()`; every remaining `RawDoor::key()` use is rewritten).
- Modify: `src/Disks/tests/cas_test_helpers.h` — `openPoolForTest` unchanged; add `openRequestsForTest(std::shared_ptr<Backend>) → CasRequests` over `Fence::open()`.

- [ ] **Step 1: Delete and rename; let the compiler list the stragglers.** `ninja -C build_debug unit_tests_dbms > build_debug/build_task20.log 2>&1; echo NINJA_EXIT=$? >> build_debug/build_task20.log` — every error is a site the batches missed; migrate it per the recipe (no conversions). Repeat until `NINJA_EXIT=0`.
- [ ] **Step 2: Gate (CP8):** `--gtest_filter='CAS*'` → `GTEST_EXIT=0`.
- [ ] **Step 3: Acceptance greps** (all must print nothing): `rg -n "\bToken\b" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed src/Disks/tests/gtest_cas_*.cpp src/Disks/tests/cas_test_helpers.h`; `rg -n "CasRequestController|CasRequestBudget::max_attempts|operation_deadline_ms|mintingTypeMatches|migrationAccess|Pool::backend\(" src`; `rg -n "check_fence_or_throw\(|checkFenceOrThrow\(" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed --include=*.cpp` (only `ContentAddressedTransaction.cpp`'s staging-buffer callback and `CasMountRuntime.cpp`'s definition may remain).
- [ ] **Step 4: Commit:** `ca-lock: the transport is private, Token and the old controller are gone, the budget is two parsed settings`.

---

### Task 21: The coverage gate and the fake GCS throttling mode {#task-21}

**Files:**
- Create: `src/Disks/tests/gtest_cas_throttling_gate.cpp` — suites `CASThrottlingGate.*`: open a pool over `ThrottlingBackend(InMemoryBackend, FirstPerKey, 0, 429)` through `openPoolForTest`'s shape (the helper gains an overload taking a `BackendPtr`), and run: `CREATE`-shaped namespace creation, an `INSERT`-shaped part write (the `PartWriteTxn` fixture the pool tests use), `DROP`, `RENAME`, a writable mount at open (the probe included), one GC round. Assert every statement succeeds and, for every key the throttling backend recorded, `refusals(key) == 1` and the key was requested at least twice (`InstrumentedBackend`-style counters on the inner `CountingBackend`). The one stated exclusion — the recovery walk's epoch seal at `T+1` — is not reached by these scenarios; the test states it in a comment.
- Modify: `tests/integration/helpers/` fake GCS service (find it by `grep -rln "fake_gcs\|FakeGcs" tests/integration/helpers`): add a `first_per_key_throttle` control endpoint that refuses the first request on every key once with 429; one integration test in `tests/integration/test_cas_gcs_*` (the existing CAS GCS suite) that turns it on and runs `CREATE TABLE`, `INSERT`, `SELECT`, `DROP` against a CAS disk.
- [ ] Steps: write the gtest (red-by-construction until a pool runs under throttling — it runs after Task 20, so this task builds: `build_task21.log`, then `--gtest_filter='CASThrottlingGate.*:CAS*'` → green); write the integration test; run it with `python -m ci.praktika run "integration" --test <the test>` redirected to `build_debug/test_task21_integration.log`; commit `ca-tests: the throttling coverage gate, unit and integration`.

---

### Task 22: Companion rename — `MountLeaseKeeper` → `MountLeaseRenewer` {#task-22}

- [ ] **Step 1:** `rg -l "MountLeaseKeeper|installKeeper|startKeeper|admitKeeperCall|MountLeaseKeeperState|keeper_state|mount_keeper" src docs | xargs sed -i 's/MountLeaseKeeperState/MountLeaseRenewerState/g; s/MountLeaseKeeper/MountLeaseRenewer/g; s/installKeeper/installRenewer/g; s/startKeeper/startRenewer/g; s/admitKeeperCall/admitRenewerCall/g; s/keeper_state/renewer_state/g; s/mount_keeper/mount_renewer/g'` — then review the diff for any hit that names ClickHouse Keeper (the `Coordination::Exception` comment in the old `CasRequestControl.h` is gone with the file; `docs/en` may mention the real Keeper — revert those lines by hand).
- [ ] **Step 2:** build + gate (CP9): `build_task22.log`, `test_task22_gate.log`.
- [ ] **Step 3:** commit `ca-mount: MountLeaseKeeper is MountLeaseRenewer — it renews a lease and is not ClickHouse Keeper`.

---

### Task 23: Thirty-minute soak {#task-23}

- [ ] **Step 1:** Build the release binary the soak uses: `ninja -C build clickhouse > build/build_task23.log 2>&1; echo NINJA_EXIT=$? >> build/build_task23.log`.
- [ ] **Step 2:** Confirm no foreign compose stack holds ports 8123/9000 (`docker ps`); never stop a foreign stack — report BLOCKED instead.
- [ ] **Step 3:** Run phase 3 for thirty minutes, under `nohup`, marker at the end: `cd utils/ca-soak && nohup bash -c 'python3 scenarios/run.py --phase 3 --duration 30m --scale dev > ../../build/soak_task23.log 2>&1; echo SOAK_EXIT=$? >> ../../build/soak_task23.log' > /dev/null 2>&1 &` (the exact driver invocation is in `utils/ca-soak/scenarios/RUN_HISTORY.md`'s last phase-3 entry — copy it and change only the duration). Poll for `SOAK_EXIT=` with bounded `until` loops.
- [ ] **Step 4:** A subagent reads the run directory's `report.md`: every verdict passes; `ca_event_bad_total` empty; no `GaveUp` storms in `system.cas_log` (`SELECT count() FROM system.cas_log WHERE event_type LIKE '%GaveUp%'` on ch1 is reported as a number); commit nothing unless a card changes; append the run to `RUN_HISTORY.md` in the table's format and commit that.

---

### Task 24: Stateless tests, CA-s3 lane {#task-24}

- [ ] **Step 1:** `python3 -m ci.praktika run "stateless" --test <the CA-s3 lane selector recorded in tmp/... or docs/superpowers/cas/AGENTS.md> > build/test_task24_stateless.log 2>&1; echo PRAKTIKA_EXIT=$? >> build/test_task24_stateless.log` under `nohup` with a marker; the lane's known ignore list (S2/dynamic FP, mysql/ipv6, tpc_ds web-disk) is not a CAS failure.
- [ ] **Step 2:** A subagent reads `ci/tmp/test_result.txt` live and the log at the end: every CAS-attributable red gets a root cause and a fix or a tracked return item; no "pre-existing" verdicts.
- [ ] **Step 3:** Commit any fix as its own commit.

---

### Task 25: Documents {#task-25}

- [ ] **Step 1:** `docs/superpowers/cas/BACKLOG.md`: close `[write-token-provenance-not-in-the-api]`, `[resolved-by-get-unbounds-clone-overlap]` if the engine's resolve closes it (say why), and every item the retry-coverage audit filed (Gaps 1–12 by name) with the commit that closes each; add the two backlog items the spec defers (`PoolMeta::admitOrValidate` is closed by this design — say so; `readSmallObjectAndGetObjectMetadata`'s drift contract is in the slice — say so).
- [ ] **Step 2:** `docs/superpowers/specs/2026-09-01-cas-self-authored-mount-reclaim-design.md`: rewrite its prerequisites section against the landed contract (the incarnation the reclaim recognises is a rendered `Incarnation`; the driver-ownership capability stays open).
- [ ] **Step 3:** `docs/en/antalya/cas/`: the two new settings; the retry policy in one paragraph for operators (ninety seconds, then the statement fails; lease-bound renewals; the farewell).
- [ ] **Step 4:** Commit `ca-docs: backlog, reclaim prerequisites and operator notes after the request contract`.

---

## Self-review {#self-review}

**Spec coverage.** The contract's six bullets → Tasks 3–5 (types, transport, admission, policy, engine, result). One type → Task 3. Two layers → Tasks 4–5 and the wiring in Task 8. One write result → Tasks 3, 5, 14, 17. Retry → Tasks 3, 5, 20 (settings). `readModifyWrite` → Tasks 5, 11, 12, 15, 17. Where each verb goes → Tasks 8–18 (every row named in a task). The upstream slice → Task 2 (read-only regime: Task 4's profile choice). Persisted incarnations → Task 18. The write anomaly → Tasks 1, 4. Request promises → Tasks 2, 4, 21. Seams → Tasks 4, 6, 8 (probe reorder), 20 (test door). Landing order → the batch structure. Verification → Tasks 6, 21 and the per-task new tests. Acceptance items 1–8 → Tasks 1, 2, 20 (greps), 21, 6/17, 14/17, 2/4, every checkpoint. Companion rename → Task 22. Out of scope: untouched.

**Placeholders.** None: every migration task names its files, census rows, verbs and policies; the recipe carries the code patterns; the engine and the new tests are written out.

**Type consistency.** `Retry::untilLeaseSafe(lease_deadline_ms, safety_margin_ms)` takes the margin explicitly (the spec's prose form takes it from mount config; the parameter is where it comes from). `GaveUp::Why` has three members everywhere. `Observation` has four alternatives everywhere. `ListedKey`/`ListPage` are redefined in `CasRequests.h` over `Incarnation` and the old ones die at the lock — until then the two coexist under different namespaces of use (`Backend::RawListedKey` vs `Cas::ListedKey`); Task 4 names the raw ones `RawListedKey`/`RawListPage` to avoid the clash.

**Batching.** Rebuilds: CP1, CP2, CP3, CP4–CP7, CP8, CP9 — nine, plus the soak's release build. Migration tasks never build.
