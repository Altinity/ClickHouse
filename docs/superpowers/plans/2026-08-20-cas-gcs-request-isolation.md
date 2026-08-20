---
description: 'Implementation plan for isolating GCS generation semantics to explicitly marked CAS requests while preserving every non-CAS GCS API and wire contract.'
sidebar_label: 'CAS GCS request isolation plan'
sidebar_position: 1
slug: /superpowers/plans/cas-gcs-request-isolation
title: 'CAS GCS Request Isolation Implementation Plan'
doc_type: 'reference'
---

# CAS GCS Request Isolation Implementation Plan {#cas-gcs-request-isolation-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve the exact public configuration and ordinary ETag behavior of every non-CAS GCS authentication path while giving explicitly marked CAS requests exact GCS generation semantics.

**Architecture:** Carry a backend-neutral `NativeConditional` bit from CAS through ClickHouse's existing AWS request wrapper into a typed `ExtendedHttpRequest`; derive it anew in `Client::BuildHttpRequest` on every SDK attempt. The OAuth and dedicated GOOG4 clients adapt generation preconditions, metadata, and response tokens only when that typed bit is set, while retry policy, authentication, and ordinary request behavior remain independent.

**Tech Stack:** C++23, ClickHouse object-storage abstractions, AWS SDK for C++, Poco HTTP, Google Cloud Storage XML API, Google OAuth and GOOG4 signing, GoogleTest, pytest/Praktika integration tests, Ninja.

**Spec:** `docs/superpowers/specs/2026-08-20-cas-gcs-request-isolation-design.md`

## Global Constraints {#global-constraints}

- Read the specification and `docs/superpowers/cas/AGENTS.md` before implementation; the specification defines semantics and the CAS guide wins over this plan if they diverge.
- Do not change any non-CAS user-facing selector or credential field. Ordinary S3 HMAC, `gcp_oauth`, and `http_client=gcs_hmac` retain their existing names and configuration.
- `ObjectStorageRequestMode::Default` must be structurally identical to the behavior before CAS dialect changes. New upstream operations default to it automatically.
- Request mode, authentication, and `ObjectStorageRetryProfile` are independent. Keep the existing base client and single-attempt clone; add no client, client cache, or bearer-token cache.
- Derive GCS generation capability only from the explicit `http_client` value (`gcp_oauth` or `gcs_hmac`), never from endpoint text, `ProviderType`, credentials, or a storage-wide mutable flag.
- The boolean on `RequestWithNativeConditionalMode` is the operation-level source of truth. `WriteSettings`, metadata APIs, and direct request setters may transport the decision only into that field.
- Preserve existing targeted `ApiMode::GCS` request mappings. Do not restore the blanket `x-amz-*` to `x-goog-*` rewrite.
- Unknown `x-amz-*` headers in the dedicated GOOG4 path fail before network I/O, but the explicit allowlist must include all headers produced by current ClickHouse operations and by the AWS SDK, including checksum and stream-framing headers.
- GCS CAS absence is a normal `std::nullopt`; no exception-driven 404 control flow and no follow-up `HEAD` after a successful token-producing write.
- Every GCS token-producing CAS write, including unconditional resurrection and CAS-owned copy, uses one PUT and fails before multipart creation above `gcs_max_token_producing_put_bytes`.
- A writable generation backend requires the full production capability battery, verified disabled bucket versioning, and `skip_access_check=false`. Read-only generation mounts may skip the mutating battery.
- Use Allman braces. Refer to functions without call parentheses in prose and comments. Say “exception” for `LOGICAL_ERROR` behavior.
- Follow TDD in every task. A new proof must first fail for the intended reason and must not be vacuous.
- Use `apply_patch` for edits. Preserve unrelated worktree changes and stage/commit only paths named by the current task. Never amend or rebase.
- Build with `ninja` without `-j`; redirect every build and test command to a unique file under `build/`. Per repository instructions, use the `clickhouse-build` and `clickhouse-local-tests` skills and delegate analysis of every build/test log to a subagent.
- Before every commit, verify `git diff --check` and inspect the exact staged diff. Commit messages must wrap literal classes, methods, settings, and log excerpts in backticks.

---

## File and dependency map {#file-and-dependency-map}

The change deliberately follows existing ownership boundaries:

- `src/IO/WriteSettings.h` owns the backend-neutral operation mode.
- `src/IO/S3/Requests.h` owns the AWS operation-wrapper bit; this remains the only operation-level source of truth.
- `src/IO/S3/PocoHTTPClientFactory.h` and `.cpp` own `ExtendedHttpRequest`, the typed per-attempt HTTP carrier.
- `src/IO/S3/Client.h` and `.cpp` own explicit-client capability and per-attempt transfer from operation wrapper to HTTP request.
- `src/IO/S3/GCSConditionalDialect.h` and `.cpp` own targeted generation and metadata translation plus the dedicated GOOG4 header policy.
- `src/IO/S3/PocoHTTPClient.h` and `.cpp` own authentication ordering, typed-mode consumption, response adaptation, and the existing `Expect: 100-continue` gate.
- `src/IO/S3/getObjectInfo.h` and `.cpp`, `src/IO/WriteBufferFromS3.h` and `.cpp`, and `src/IO/S3/copyS3File.h` and `.cpp` transport the mode onto concrete `HEAD`, PUT, and copy wrappers.
- `IObjectStorage`, `S3ObjectStorage`, and `CasObjectStorageBackend` select native-token operations without exposing the mode to users.
- CAS settings/pool files own cap naming and mount-time fail-closed policy.
- Existing S3 and CAS GoogleTests provide fast structural proofs; `test_storage_gcp_auth` protects ordinary OAuth behavior; a live-GCS release gate characterizes the dedicated GOOG4 path.

Dependency order is intentional: Task 1 creates typed carriers, Task 2 makes the HTTP adapter conditional, Tasks 3–5 route ordinary and CAS operations, Task 6 closes mount hazards, and Tasks 7–9 prove compatibility and integration behavior.

---

### Task 1: Add typed request state and re-derive it on every SDK attempt {#task-1-add-typed-request-state}

**Files:**

- Modify: `src/IO/WriteSettings.h:14-80`
- Modify: `src/IO/S3/Requests.h:60-130`
- Modify: `src/IO/S3/PocoHTTPClientFactory.h:10-30`
- Modify: `src/IO/S3/PocoHTTPClientFactory.cpp:35-62`
- Modify: `src/IO/S3/Client.h:250-265`
- Modify: `src/IO/S3/Client.cpp:988-1002`
- Modify: `src/IO/S3/PocoHTTPClient.cpp:350-390`
- Test: `src/IO/S3/tests/gtest_aws_s3_client.cpp`

**Interfaces:**

- Consumes: existing `ExtendedRequest<BaseRequest>` and the two `PocoHTTPClientFactory::CreateHttpRequest` overloads.
- Produces: `ObjectStorageRequestMode`, `RequestWithNativeConditionalMode`, `ExtendedRequest::setNativeConditional`, `ExtendedHttpRequest`, `isNativeConditionalRequest`, and `Client::supportsGcsNativeConditionalRequests`.

- [ ] **Step 1: Add failing carrier and capability tests**

Add focused tests proving:

```cpp
TEST(IOTestAwsS3Client, RequestModeDefaultsToDefault);
TEST(IOTestAwsS3Client, FactoryAlwaysCreatesExtendedHttpRequest);
TEST(IOTestAwsS3Client, NativeConditionalModeRequiresExplicitGcsHttpClient);
TEST(IOTestAwsS3Client, ForeignHttpRequestReadsAsDefault);
TEST(IOTestAwsS3Client, RetryAndRedirectRederiveRequestMode);
```

Use a recording HTTP client/factory already patterned in this file. Exercise the sequence ordinary request → native request → ordinary request on the same `Client`. For retry, make the first attempt return a retryable status; for redirect, return a location once. Assert every created request is `ExtendedHttpRequest`, `Client::BuildHttpRequest` applies the bit on each attempt, and the final ordinary request remains false.

- [ ] **Step 2: Run the focused tests and confirm the expected compile/test failure**

Run:

```bash
ninja -C build unit_tests_dbms > build/task1_typed_request_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='IOTestAwsS3Client.*RequestMode*:IOTestAwsS3Client.FactoryAlwaysCreatesExtendedHttpRequest:IOTestAwsS3Client.ForeignHttpRequestReadsAsDefault:IOTestAwsS3Client.RetryAndRedirectRederiveRequestMode' > build/task1_typed_request_test.log 2>&1
```

The initial failure must be missing typed state or incorrect propagation, not unrelated environment failure.

- [ ] **Step 3: Add the backend-neutral and operation-wrapper state**

In `WriteSettings.h` add:

```cpp
enum class ObjectStorageRequestMode : uint8_t
{
    Default,
    NativeConditional,
};

ObjectStorageRequestMode object_storage_request_mode = ObjectStorageRequestMode::Default;
```

In `Requests.h` add the non-template interface and implement it in `ExtendedRequest`:

```cpp
class RequestWithNativeConditionalMode
{
public:
    virtual ~RequestWithNativeConditionalMode() = default;
    virtual bool isNativeConditional() const = 0;
};

void setNativeConditional(bool value = true) const { native_conditional = value; }
bool isNativeConditional() const override { return native_conditional; }
```

Store `mutable bool native_conditional = false`; do not add another enum or header carrier.

- [ ] **Step 4: Add the typed HTTP request**

Declare `ExtendedHttpRequest final` in `PocoHTTPClientFactory.h`, derive it from `Aws::Http::Standard::StandardHttpRequest`, inherit its constructors, and expose typed `setNativeConditional` / `isNativeConditional` methods. Add:

```cpp
bool isNativeConditionalRequest(const Aws::Http::HttpRequest & request) noexcept;
```

The helper dynamic-casts to `ExtendedHttpRequest`; it returns false for a foreign object. Make both factory overloads construct `ExtendedHttpRequest`.

- [ ] **Step 5: Transfer the bit in `Client::BuildHttpRequest`**

Add `Client::supportsGcsNativeConditionalRequests` as a pure predicate over lower-cased `client_configuration.http_client`. It returns true only for `gcp_oauth` and `gcs_hmac`.

After the base SDK `BuildHttpRequest`, dynamic-cast the operation wrapper and HTTP request. Set the HTTP bit to:

```cpp
wrapper && wrapper->isNativeConditional() && supportsGcsNativeConditionalRequests()
```

Always write the resulting boolean, including false, so a reused or reconstructed HTTP request cannot retain stale state. Keep the existing `ApiMode::GCS` removal of `x-amz-api-version` unchanged.

- [ ] **Step 6: Enable the construction assertion at the common HTTP boundary**

At the beginning of `PocoHTTPClient::makeRequestInternalImpl`, add a `chassert` that the incoming request is an `ExtendedHttpRequest`. Read the release path through `isNativeConditionalRequest`, whose foreign-request result remains false.

- [ ] **Step 7: Re-run focused tests and the ordinary S3 smoke lane**

```bash
ninja -C build unit_tests_dbms > build/task1_typed_request_rebuild.log 2>&1
build/src/unit_tests_dbms --gtest_filter='IOTestAwsS3Client.*' > build/task1_aws_client_tests.log 2>&1
python3 -m ci.praktika run "integration" --test test_storage_s3 > build/task1_storage_s3_integration.log 2>&1
```

Confirm normal retry and redirect each cause a new factory-created request in the vendored SDK and that `BuildHttpRequest` re-derives the field on every attempt.

- [ ] **Step 8: Commit Task 1**

```bash
git add src/IO/WriteSettings.h src/IO/S3/Requests.h src/IO/S3/PocoHTTPClientFactory.h src/IO/S3/PocoHTTPClientFactory.cpp src/IO/S3/Client.h src/IO/S3/Client.cpp src/IO/S3/tests/gtest_aws_s3_client.cpp src/IO/S3/PocoHTTPClient.cpp
git commit -m 'Add typed `NativeConditional` request state'
```

---

### Task 2: Split GCS authentication preparation from native-conditional adaptation {#task-2-split-gcs-adaptation}

**Files:**

- Modify: `src/IO/S3/GCSConditionalDialect.h`
- Modify: `src/IO/S3/GCSConditionalDialect.cpp`
- Modify: `src/IO/S3/PocoHTTPClient.h:70-95,235-260`
- Modify: `src/IO/S3/PocoHTTPClient.cpp:620-790,890-1030`
- Modify: `src/IO/S3/Client.cpp:1265-1320`
- Modify: `src/IO/S3/Client.h:250-260`
- Test: `src/IO/S3/tests/gtest_gcs_conditional_dialect.cpp`
- Test: `src/IO/S3/tests/gtest_goog4_signer.cpp`
- Test: `src/IO/S3/tests/gtest_aws_s3_client.cpp`

**Interfaces:**

- Consumes: `isNativeConditionalRequest` and `Client::supportsGcsNativeConditionalRequests` from Task 1.
- Produces: `prepareGcsRequestForGoog4Authentication`, narrowed `applyGcsConditionalDialectToRequest`, and `applyGcsConditionalDialectToResponse`.

- [ ] **Step 1: Replace blanket-rewrite tests with failing responsibility tests**

Delete the expectation that every `x-amz-*` header is renamed. Add table-driven tests with these categories:

```text
authentication artifacts: delete before GOOG4 signing
targeted metadata: x-amz-meta-* -> x-goog-meta-*
targeted storage/copy headers: preserve existing ApiMode::GCS mappings
SDK checksums/framing: explicit pass/drop/map disposition
unknown x-amz extension: BAD_ARGUMENTS before network I/O
conditions: translate only in NativeConditional mode
response ETag/generation/metadata: adapt only in NativeConditional mode
```

Include `x-amz-sdk-checksum-algorithm`, representative `x-amz-checksum-*`, `x-amz-trailer`, and `x-amz-decoded-content-length`. Do not include `x-amz-acl` unless a current production request is found and documented.

- [ ] **Step 2: Run the dialect tests and verify they fail against blanket rewriting**

```bash
ninja -C build unit_tests_dbms > build/task2_gcs_adapter_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='GCSConditionalDialect*:GOOG4Signer*' > build/task2_gcs_adapter_red.log 2>&1
```

- [ ] **Step 3: Separate the three adapter responsibilities**

Keep the existing conditional translation in `applyGcsConditionalDialectToRequest`, but narrow it to:

- `If-None-Match: *` → `x-goog-if-generation-match: 0`;
- numeric `If-Match` → `x-goog-if-generation-match`;
- CAS request metadata `x-amz-meta-*` → `x-goog-meta-*`;
- fail-closed rejection of conditional `CompleteMultipartUpload`.

Implement `prepareGcsRequestForGoog4Authentication` as an explicit allowlist. It deletes AWS authentication artifacts, applies only proven compatibility mappings, and throws `BAD_ARGUMENTS` for an unknown remaining `x-amz-*` header. Keep the comment beside each SDK-generated header explaining whether GOOG4 accepts it, it must be renamed, or it must be removed before signing.

Implement `applyGcsConditionalDialectToResponse` so a native request:

- substitutes quoted `x-goog-generation` for the SDK-visible `ETag`;
- maps `x-goog-meta-*` to `x-amz-meta-*` before SDK parsing;
- throws on conflicting values present under both prefixes.

Default requests copy the original response headers byte-for-byte.

- [ ] **Step 4: Gate OAuth and GOOG4 behavior on typed request state**

In `PocoHTTPClientGCPOAuth::makeRequestInternal`, apply the native adapter only when `isNativeConditionalRequest(request)` is true, then add the Bearer token.

In `PocoHTTPClientGCSHMAC::makeRequestInternal`, first apply the native adapter only when marked, then always run `prepareGcsRequestForGoog4Authentication`, then sign the final headers. In the common response path call the response adapter only for a marked request.

Remove `gcs_conditional_dialect` from `PocoHTTPClientConfiguration`, `PocoHTTPClient`, and `ClientFactory::create`. Remove `Client::usesGcsConditionalDialect`; Task 1's explicit predicate replaces it.

- [ ] **Step 5: Preserve the existing Expect ordering**

Verify native translation still occurs before `PocoHTTPClient::makeRequestInternalImpl` checks for `x-goog-if-generation-match`. Add one test with a payload at the configured threshold and one `Default` ETag-conditional request; only the native generation request should use the GCS condition while the pre-existing threshold semantics remain unchanged.

- [ ] **Step 6: Run adapter, signer, and local HTTP tests**

```bash
ninja -C build unit_tests_dbms > build/task2_gcs_adapter_rebuild.log 2>&1
build/src/unit_tests_dbms --gtest_filter='GCSConditionalDialect*:GOOG4Signer*:IOTestAwsS3Client.*' > build/task2_gcs_adapter_green.log 2>&1
```

- [ ] **Step 7: Commit Task 2**

```bash
git add src/IO/S3/GCSConditionalDialect.h src/IO/S3/GCSConditionalDialect.cpp src/IO/S3/PocoHTTPClient.h src/IO/S3/PocoHTTPClient.cpp src/IO/S3/Client.h src/IO/S3/Client.cpp src/IO/S3/tests/gtest_gcs_conditional_dialect.cpp src/IO/S3/tests/gtest_goog4_signer.cpp src/IO/S3/tests/gtest_aws_s3_client.cpp
git commit -m 'Isolate GCS generation adaptation per request'
```

---

### Task 3: Restore and pin all ordinary GCS authentication contracts {#task-3-pin-ordinary-gcs-contracts}

**Files:**

- Modify: `tests/integration/test_storage_gcp_auth/test.py`
- Modify: `tests/integration/test_storage_gcp_auth/gcs_mocks/echo.py`
- Modify: `tests/integration/test_storage_gcp_auth/configs/named_collections.xml`
- Test: `src/IO/S3/tests/gtest_aws_s3_client.cpp`
- Test: `src/IO/S3/tests/gtest_goog4_signer.cpp`

**Interfaces:**

- Consumes: Default-mode HTTP behavior and GOOG4 allowlist from Tasks 1–2.
- Produces: regression fixtures that expose request headers and response `ETag`/`x-goog-generation` independently.

- [ ] **Step 1: Add failing ordinary OAuth assertions**

Cover GET, HEAD, LIST, PUT with metadata, CopyObject, DELETE, and a multipart-sized write. Assert:

- Bearer authentication still works;
- `ETag` remains the server's ordinary ETag;
- non-numeric `If-Match` and non-star `If-None-Match` are not converted;
- arbitrary `x-amz-*` headers are not blanket-renamed;
- no `x-goog-if-generation-match` is emitted;
- the existing token-refresh count is unchanged.

- [ ] **Step 2: Run the integration test and confirm the mock lacks the required observations**

```bash
python3 -m ci.praktika run "integration" --test test_storage_gcp_auth > build/task3_gcp_oauth_red.log 2>&1
```

The new cases must fail because the mock cannot yet expose captured request headers or independent ETag/generation values; the existing authentication case must remain green.

- [ ] **Step 3: Extend the OAuth mock with request capture and mixed response tokens**

Make `echo.py` record method, path, and headers per request and return both a stable ordinary `ETag` and a different numeric `x-goog-generation` for HEAD/GET responses. Add reset and capture endpoints without changing the existing OAuth counter contract.

- [ ] **Step 4: Add ordinary S3-interoperability HMAC unit characterization**

Construct the standard S3 client without `http_client=gcs_hmac` and prove access key, secret key, AWS SigV4, error typing, `x-amz-meta-*`, CopyObject mappings, DELETE, batch `DeleteObjects`, checksum headers, and response ETags are unchanged and `native_conditional=false`.

- [ ] **Step 5: Run the ordinary GCS tests**

```bash
ninja -C build unit_tests_dbms > build/task3_gcs_compat_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='IOTestAwsS3Client.*Gcs*:IOTestAwsS3Client.*Hmac*:GOOG4Signer*' > build/task3_gcs_compat_unit.log 2>&1
python3 -m ci.praktika run "integration" --test test_storage_gcp_auth > build/task3_gcp_oauth_integration.log 2>&1
```

The old `test_gcp_auth` token request counts must remain green; new assertions must demonstrate that no generation leaks into ordinary traffic.

- [ ] **Step 6: Commit Task 3**

```bash
git add tests/integration/test_storage_gcp_auth src/IO/S3/tests/gtest_aws_s3_client.cpp src/IO/S3/tests/gtest_goog4_signer.cpp
git commit -m 'Pin non-CAS GCS authentication behavior'
```

---

### Task 4: Route CAS metadata, capability, and exact deletion through native tokens {#task-4-route-cas-metadata-and-delete}

**Files:**

- Modify: `src/IO/S3/getObjectInfo.h`
- Modify: `src/IO/S3/getObjectInfo.cpp`
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h:220-235`
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h:105-175`
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:489-535,619-645`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp:43-125`
- Test: `src/IO/tests/gtest_writebuffer_s3.cpp`
- Test: `src/Disks/tests/gtest_cas_backend_generation.cpp`
- Test: `src/Disks/tests/gtest_cas_probe.cpp`

**Interfaces:**

- Consumes: wrapper `setNativeConditional`, explicit `Client::supportsGcsNativeConditionalRequests`, and response adaptation.
- Produces: `IObjectStorage::tryGetObjectMetadataWithNativeToken`, S3 override, marked exact DELETE, and native CAS HEAD routing.

- [ ] **Step 1: Add failing metadata and exact-delete tests**

Add tests proving:

```cpp
TEST_F(S3ObjectStorageConditionalOpsTest, NativeTokenHeadIsMarkedAndMissingIsNullopt);
TEST_F(S3ObjectStorageConditionalOpsTest, GenerationDeleteUsesNativeConditionalMode);
TEST_F(S3ObjectStorageConditionalOpsTest, OrdinaryDeleteRemainsDefault);
TEST(CASBackendGeneration, NativeHeadUsesNativeTokenMetadataApi);
TEST(CASProbe, ExactDeleteBatteryDetectsMissingGenerationMode);
```

The last test must simulate both possible unsafe services: one rejects raw numeric `If-Match`, another ignores it. The battery must fail in either case because it verifies wrong-token preservation and correct-token deletion.

- [ ] **Step 2: Run the focused tests and confirm native routing is absent**

```bash
ninja -C build unit_tests_dbms > build/task4_cas_metadata_red_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='S3ObjectStorageConditionalOpsTest*.NativeToken*:S3ObjectStorageConditionalOpsTest*.GenerationDelete*:CASBackendGeneration.NativeHead*:CASProbe.ExactDeleteBattery*' > build/task4_cas_metadata_red.log 2>&1
```

The tests must fail on an unmarked HEAD/DELETE or the missing virtual, not on the existing error classifier.

- [ ] **Step 3: Add the defaulted native-token metadata virtual**

In `IObjectStorage` add:

```cpp
virtual std::optional<ObjectMetadata> tryGetObjectMetadataWithNativeToken(
    const std::string & path, bool with_tags) const
{
    return tryGetObjectMetadata(path, with_tags);
}
```

Do not modify unrelated object-storage implementations.

- [ ] **Step 4: Add S3 HEAD request-mode transport**

Extend `getObjectInfoIfExists` with an `ObjectStorageRequestMode` argument defaulting to `Default`. Set the concrete `S3::HeadObjectRequest` wrapper's native bit before submission. The ordinary `tryGetObjectMetadata` keeps the default call; the S3 override `tryGetObjectMetadataWithNativeToken` passes `NativeConditional` and preserves missing-object `std::nullopt` behavior.

- [ ] **Step 5: Route CAS HEAD and exact DELETE**

Change only `ObjectStorageBackend::nativeHead` to use `tryGetObjectMetadataWithNativeToken`; emulated metadata reads remain ordinary.

In `S3ObjectStorage::removeObjectIfTokenMatches`, call `request.setNativeConditional` before `DeleteObject`. Preserve existing 412/404/error mapping. Replace the `conditionalOpsUseGenerationTokens` implementation with `Client::supportsGcsNativeConditionalRequests` on the selected client.

- [ ] **Step 6: Verify metadata attributes and token type**

Return `x-goog-meta-*` from the fake native HEAD and assert exact `HeadResult::attributes`. Return a quoted generation through the adapter and assert CAS stamps `TokenType::Generation`. Add conflicting `x-amz-meta-*`/`x-goog-meta-*` response coverage that fails before ambiguous attributes reach CAS.

- [ ] **Step 7: Run focused S3 and CAS tests**

```bash
ninja -C build unit_tests_dbms > build/task4_cas_metadata_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='S3ObjectStorageConditionalOpsTest*:CASBackendGeneration*:CASProbe*' > build/task4_cas_metadata_tests.log 2>&1
```

- [ ] **Step 8: Commit Task 4**

```bash
git add src/IO/S3/getObjectInfo.h src/IO/S3/getObjectInfo.cpp src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp src/IO/tests/gtest_writebuffer_s3.cpp src/Disks/tests/gtest_cas_backend_generation.cpp src/Disks/tests/gtest_cas_probe.cpp
git commit -m 'Route CAS metadata and delete through GCS generations'
```

---

### Task 5: Mark every CAS token-producing write and enforce one-PUT attribution {#task-5-route-token-producing-writes}

**Files:**

- Modify: `src/IO/WriteBufferFromS3.h`
- Modify: `src/IO/WriteBufferFromS3.cpp:410-770`
- Modify: `src/IO/S3/copyS3File.h:20-70`
- Modify: `src/IO/S3/copyS3File.cpp:80-180,620-940`
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:680-835`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h:60-220`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp:180-230,820-860,1000-1060`
- Test: `src/IO/tests/gtest_writebuffer_s3.cpp`
- Test: `src/Disks/tests/gtest_cas_backend_generation.cpp`
- Test: `src/Disks/tests/gtest_cas_s3_staging.cpp`

**Interfaces:**

- Consumes: `WriteSettings::object_storage_request_mode` and wrapper `setNativeConditional`.
- Produces: native mode on actual PUT/copy requests, `tokenProducingWriteSettings`, and fail-closed cap handling for conditional and unconditional writes.

- [ ] **Step 1: Add failing PUT and copy routing tests**

For create-if-absent, compare-and-set overwrite, streaming conditional writes, unconditional resurrection, and conditional staging promotion, assert the actual `PutObjectRequest` or `CopyObjectRequest` has native mode. Add ordinary write and ordinary copy controls that remain false.

- [ ] **Step 2: Add failing cap and missing-generation tests**

For every generation-token write kind assert:

- body at the cap uses one PUT and returns that response's generation;
- body one byte above cap raises `NOT_IMPLEMENTED` before `CreateMultipartUpload` or copy-multipart initiation;
- successful PUT/copy without generation raises an exception;
- no `HEAD` occurs after successful upload;
- conditional `CompleteMultipartUpload` still fails as defense in depth.

Replace `ResurrectIsNotBoundByTheSinglePutCap` with the opposite required contract.

- [ ] **Step 3: Run the focused write tests and confirm they expose the old routing**

```bash
ninja -C build unit_tests_dbms > build/task5_token_writes_red_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='WBS3Test*NativeConditional*:S3ObjectStorageConditionalOpsTest*NativeConditional*:CASBackendGeneration*SinglePut*:CASS3Staging*NativeConditional*' > build/task5_token_writes_red.log 2>&1
```

At least the unconditional resurrection-above-cap case and request-mode capture must fail before implementation.

- [ ] **Step 4: Propagate mode through `WriteBufferFromS3`**

In `getPutRequest`, set native mode from:

```cpp
write_settings.object_storage_request_mode == ObjectStorageRequestMode::NativeConditional
```

Set the same bit on multipart request wrappers for diagnostic consistency, but retain the earlier `s3_force_single_part_upload` exception before multipart creation. Do not infer mode from `If-Match` or `If-None-Match`.

- [ ] **Step 5: Propagate mode through native copy**

Add an `ObjectStorageRequestMode request_mode = ObjectStorageRequestMode::Default` argument to `copyS3File` and its helper state. Mark `CopyObjectRequest` and any multipart-copy request wrappers from it. Pass the caller's `WriteSettings::object_storage_request_mode` from `S3ObjectStorage::copyObjectConditional`; ordinary `copyObject` passes Default.

When native conditional copy cannot stay single-operation under the cap, fail before multipart or read-write fallback. A CAS-owned conditional copy must never fall back to an unconditional write.

- [ ] **Step 6: Centralize generation token-producing settings**

Rename the private cap member to `token_producing_single_put_cap`. Add:

```cpp
WriteSettings ObjectStorageBackend::tokenProducingWriteSettings() const;
WriteSettings ObjectStorageBackend::conditionalWriteSettings() const;
```

`tokenProducingWriteSettings` sets `object_storage_request_mode=NativeConditional` and, for generation backends, forces one PUT with the configured cap. `conditionalWriteSettings` starts from it and additionally sets `SingleAttempt`, one unexpected-write attempt, and disables the racy upload recheck. Use the first helper for unconditional resurrection and other unconditional token-producing writes; use the second for compare/create operations.

- [ ] **Step 7: Require exact response token attribution**

For generation mode, accept only the successful response token carried by the write/copy result. Reject an empty result or a non-numeric generation. Keep ETag-mode behavior unchanged and do not add a follow-up metadata request.

- [ ] **Step 8: Run write, staging, and generation tests**

```bash
ninja -C build unit_tests_dbms > build/task5_token_writes_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='WBS3Test*:SyncAsync*:S3ObjectStorageConditionalOpsTest*:CASBackendGeneration*:CASS3Staging*' > build/task5_token_writes_tests.log 2>&1
```

- [ ] **Step 9: Commit Task 5**

```bash
git add src/IO/WriteBufferFromS3.h src/IO/WriteBufferFromS3.cpp src/IO/S3/copyS3File.h src/IO/S3/copyS3File.cpp src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp src/IO/tests/gtest_writebuffer_s3.cpp src/Disks/tests/gtest_cas_backend_generation.cpp src/Disks/tests/gtest_cas_s3_staging.cpp
git commit -m 'Bind GCS CAS writes to exact response generations'
```

---

### Task 6: Tighten writable GCS CAS mount preconditions and rename the cap {#task-6-tighten-mount-preconditions}

**Files:**

- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp:60-90`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h:580-620`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:65-85,285-310,680-760`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h:275-295`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h:115-140`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp:53-90`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.h:85-100`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:450-485`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h:185-200`
- Modify: `docs/en/operations/storing-data.md:520-580`
- Modify: `docs/en/antalya/cas/configuration.md:85-105`
- Modify: `docs/en/antalya/cas/bucket-requirements.md:20-45`
- Modify: `docs/en/antalya/cas/architecture/blob-protocol.md:220-235`
- Modify: `docs/en/antalya/cas/architecture/backend.md:45-95`
- Test: `src/Disks/tests/gtest_cas_settings.cpp`
- Test: `src/Disks/tests/gtest_cas_backend_generation.cpp`
- Test: `src/Disks/tests/gtest_cas_pool.cpp`
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

**Interfaces:**

- Consumes: generation capability and token-producing settings from Tasks 4–5.
- Produces: `gcs_max_token_producing_put_bytes`, `Backend::checkSkipAccessCheckSupport`, and non-bypassable writable-generation mount checks.

- [ ] **Step 1: Add failing setting and mount-policy tests**

Assert:

- `gcs_max_token_producing_put_bytes` parses and reaches `ObjectStorageBackend`;
- old `gcs_max_conditional_put_bytes` is rejected as unknown, with no alias;
- verified disabled versioning passes;
- enabled and unknown versioning both reject writable generation mode;
- writable generation plus `skip_access_check=true` rejects before pool mutation;
- read-only generation mode may skip the mutating battery;
- ETag-mode and non-CAS disks retain existing `skip_access_check` behavior.

- [ ] **Step 2: Run the focused tests and confirm the unsafe policies are still accepted**

```bash
ninja -C build unit_tests_dbms > build/task6_mount_safety_red_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CASContentAddressedSettings*:CASBackendGeneration*Versioning*:CASPool*SkipAccess*:CASMount*SkipAccess*' > build/task6_mount_safety_red.log 2>&1
```

The unknown-versioning and writable-generation `skip_access_check=true` cases must fail because current code accepts them; the renamed-setting test may fail at compile or parsing time.

- [ ] **Step 3: Rename the pre-release cap end-to-end**

Rename settings declaration, members, constructor parameters, docs, and tests to `gcs_max_token_producing_put_bytes`. Do not add an alias or migration shim.

- [ ] **Step 4: Fail closed on unverifiable versioning**

Change `ObjectStorageBackend::checkPoolPreconditions` so `std::nullopt` throws `NOT_IMPLEMENTED` with an actionable message, just like enabled versioning. Keep the no-op for non-generation and non-native modes.

- [ ] **Step 5: Make the capability battery mandatory for writable generation mounts**

Add this default no-op capability hook to `Backend` and forward it through `InstrumentedBackend`:

```cpp
virtual void checkSkipAccessCheckSupport() {}
```

Override it in `ObjectStorageBackend`: throw `NOT_IMPLEMENTED` only for Native generation-token mode. In the writable branch of `Pool::open`, call it before honoring `skip_access_check=true`, then retain the existing `checkConditionalWriteSingleAttemptSupport` call. This expresses the policy without RTTI on concrete storage and without performing the skipped mutating battery. Read-only mounts never enter this writable branch.

Keep read-only open non-mutating. Keep the existing single-attempt gate for other writable native backends.

- [ ] **Step 6: Update operator documentation**

Document:

- the renamed cap applies to conditional and unconditional token-producing writes;
- versioning must be verifiably disabled;
- writable generation mounts cannot use `skip_access_check=true`;
- GCS soft delete cannot be uniformly inspected through this XML API path, so disabling it is an explicit operator precondition for prompt physical reclamation;
- no non-CAS GCS API or authentication configuration changes.

Preserve all existing explicit heading anchors.

- [ ] **Step 7: Run CAS settings and mount tests**

```bash
ninja -C build unit_tests_dbms > build/task6_mount_safety_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CASContentAddressedSettings*:CASBackendGeneration*:CASPool*:CASMount*' > build/task6_mount_safety_tests.log 2>&1
```

- [ ] **Step 8: Commit Task 6**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h docs/en/operations/storing-data.md docs/en/antalya/cas/configuration.md docs/en/antalya/cas/bucket-requirements.md docs/en/antalya/cas/architecture/blob-protocol.md docs/en/antalya/cas/architecture/backend.md src/Disks/tests/gtest_cas_settings.cpp src/Disks/tests/gtest_cas_backend_generation.cpp src/Disks/tests/gtest_cas_pool.cpp src/Disks/tests/gtest_cas_mount.cpp
git commit -m 'Fail closed for unsafe GCS CAS mounts'
```

---

### Task 7: Prove user-visible ETag and cache-key isolation {#task-7-prove-etag-and-cache-isolation}

**Files:**

- Modify: `src/Storages/ObjectStorage/StorageObjectStorageSource.cpp:1125-1240` only if a narrow test seam is required
- Test: `src/IO/S3/tests/gtest_aws_s3_client.cpp`
- Create: `src/Storages/ObjectStorage/tests/gtest_storage_object_storage_source.cpp`
- Modify: `tests/integration/test_storage_gcp_auth/test.py`

**Interfaces:**

- Consumes: ordinary response ETag preservation from Tasks 2–3.
- Produces: direct regression coverage for `_etag`, filesystem cache, page cache, and Parquet metadata-cache inputs.

- [ ] **Step 1: Identify the narrowest existing cache-key seam**

Trace the current ETag flow at `StorageObjectStorageSource.cpp:1135-1217`. Prefer testing an existing helper. If the code is inline and inaccessible, extract only a pure internal helper that returns the ETag-derived cache-key inputs; do not redesign cache ownership or public APIs.

- [ ] **Step 2: Write the failing LIST-versus-HEAD regression test**

For the same object, feed an XML LIST ETag and an ordinary OAuth HEAD response containing that ETag plus a different generation. Assert:

- `_etag` receives the ordinary ETag in both cases;
- filesystem-cache and page-cache inputs match;
- Parquet metadata cache receives the same `(path, etag)` pair;
- the generation appears in none of those non-CAS values.

- [ ] **Step 3: Run the focused test and confirm it catches the old blanket override**

```bash
ninja -C build unit_tests_dbms > build/task7_etag_cache_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='IOTestAwsS3Client.*ETag*:StorageObjectStorageSource*' > build/task7_etag_cache_red.log 2>&1
```

When run against the pre-isolation behavior, the HEAD and LIST keys must differ; otherwise the test is not an effective regression proof.

- [ ] **Step 4: Add only the test seam needed and make the test pass**

Do not special-case caches for GCS. The production fix is already the Default response behavior from Task 2; this task should normally add tests only, plus at most a pure helper extraction.

- [ ] **Step 5: Run unit and OAuth integration coverage**

```bash
ninja -C build unit_tests_dbms > build/task7_etag_cache_rebuild.log 2>&1
build/src/unit_tests_dbms --gtest_filter='IOTestAwsS3Client.*ETag*:StorageObjectStorageSource*' > build/task7_etag_cache_green.log 2>&1
python3 -m ci.praktika run "integration" --test test_storage_gcp_auth > build/task7_gcp_oauth_cache_integration.log 2>&1
```

- [ ] **Step 6: Commit Task 7**

Stage only the files actually needed by the chosen seam, then commit:

```bash
git commit -m 'Test ordinary GCS ETag cache consistency'
```

---

### Task 8: Add end-to-end CAS-over-GCS and dedicated GOOG4 characterization {#task-8-add-gcs-integration-coverage}

**Files:**

- Create: `tests/integration/test_cas_gcs/__init__.py`
- Create: `tests/integration/test_cas_gcs/test.py`
- Create: `tests/integration/test_cas_gcs/configs/config.xml`
- Create: `tests/integration/test_cas_gcs/gcs_mocks/server.py`
- Create: `tests/integration/test_cas_gcs/gcs_mocks/auth.py`
- Modify: `tests/integration/test_storage_gcp_auth/test.py` if shared OAuth assertions belong there
- Create: `tests/integration/test_gcs_hmac_live/__init__.py`
- Create: `tests/integration/test_gcs_hmac_live/test.py`

**Interfaces:**

- Consumes: all production behavior from Tasks 1–7.
- Produces: deterministic CAS protocol integration coverage plus an opt-in live-GCS release gate for dedicated GOOG4 compatibility.

- [ ] **Step 1: Write failing CAS-over-GCS integration scenarios**

Cover both `gcp_oauth` and dedicated `gcs_hmac` configuration on a proxy endpoint whose hostname does not contain `storage.googleapis.com`. Assert:

- mount selects `TokenType::Generation` from explicit `http_client`;
- create-if-absent and overwrite use generation preconditions;
- CAS metadata attributes round-trip;
- stale exact DELETE preserves the object and matching delete removes it;
- LIST remains unmarked and its ETag never becomes a CAS token;
- a token-producing body above the cap fails before multipart;
- missing successful-write generation fails without HEAD;
- ordinary traffic sharing the client remains ETag-based.

- [ ] **Step 2: Run the new suite and confirm the missing protocol fixture is the failure**

```bash
python3 -m ci.praktika run "integration" --test test_cas_gcs > build/task8_cas_gcs_red.log 2>&1
```

The suite must fail because the generation-aware service/configuration has not been implemented, not because it accidentally targets an unrelated S3 service.

- [ ] **Step 3: Add deterministic GCS XML protocol fixtures**

The fake service must model generations independently from ETags and record every request. It must support conditional PUT, HEAD, exact DELETE, LIST, metadata, CopyObject, and the mount battery. Add explicit service modes that reject raw numeric `If-Match` and that ignore it, so the battery proof is not coupled to one guessed GCS behavior.

- [ ] **Step 4: Add OAuth client-count instrumentation**

Count HTTP client construction and token fetches while alternating ordinary and CAS operations. Assert only the existing base and single-attempt clients exist; request mode creates no third client or token cache. Accept one token cache per existing client as specified.

- [ ] **Step 5: Add the opt-in live GOOG4 characterization**

Gate the suite on explicit environment credentials/bucket variables and skip when they are absent. Against live GCS, run Default HEAD, GET, LIST, PUT with metadata, CopyObject, DELETE, batch `DeleteObjects`, multipart, checksum-required requests, and chunked upload. Assert preserved ETags, no generation precondition, accepted existing `http_client=gcs_hmac` spelling, and exact error typing.

This live suite is the release gate for the GOOG4 allowlist. A mock may test syntax and routing but must not be cited as proof that GCS accepts a header disposition.

- [ ] **Step 6: Run deterministic integration tests**

```bash
python3 -m ci.praktika run "integration" --test "test_cas_gcs test_storage_gcp_auth" > build/task8_cas_gcs_integration.log 2>&1
```

- [ ] **Step 7: Run the live gate when credentials are available**

```bash
python3 -m ci.praktika run "integration" --test test_gcs_hmac_live > build/task8_gcs_hmac_live.log 2>&1
```

If credentials are unavailable, record the suite as not executed; do not replace this evidence with a mock claim. The change is not release-ready until the live gate has passed in an authorized environment.

- [ ] **Step 8: Commit Task 8**

```bash
git add tests/integration/test_cas_gcs tests/integration/test_gcs_hmac_live tests/integration/test_storage_gcp_auth
git commit -m 'Test CAS GCS request isolation end to end'
```

---

### Task 9: Run the full verification and audit the upstream fork surface {#task-9-final-verification}

**Files:**

- Verify: every file committed in Tasks 1–8
- Modify: `docs/superpowers/plans/2026-08-20-cas-gcs-request-isolation.md` only to check completed boxes and append exact evidence if the execution workflow records progress in-plan

**Interfaces:**

- Consumes: completed implementation and all tests.
- Produces: merge-ready evidence that ordinary GCS behavior is isolated and CAS generation behavior is complete.

- [ ] **Step 1: Audit forbidden coupling and stale names**

Run untruncated searches:

```bash
rg -n 'gcs_conditional_dialect|usesGcsConditionalDialect|gcs_max_conditional_put_bytes' src tests docs/en
rg -n 'storage\.googleapis\.com' src/Disks/DiskObjectStorage src/IO/S3
rg -n 'x-amz-' src/IO/S3/GCSConditionalDialect.cpp
```

Expected: no old dialect flag/accessor or old cap; no endpoint-based CAS capability; every remaining `x-amz-*` in the GOOG4 adapter has an explicit tested disposition.

- [ ] **Step 2: Audit every CAS token-producing operation**

Enumerate all callers of `nativeConditionalPut`, `putIfAbsentStream`, resurrection, and `copyObjectConditional`. For each, record which settings helper or direct setter places `NativeConditional` on the wrapper and where the response generation is consumed. Do not rely on a truncated grep.

- [ ] **Step 3: Build ClickHouse and the unit-test binary**

```bash
ninja -C build clickhouse > build/task9_clickhouse_build.log 2>&1
ninja -C build unit_tests_dbms > build/task9_unit_tests_build.log 2>&1
```

Confirm the test binary was rebuilt after the final source change.

- [ ] **Step 4: Run focused S3/GCS unit coverage**

```bash
build/src/unit_tests_dbms --gtest_filter='GCSConditionalDialect*:GOOG4Signer*:IOTestAwsS3Client*:WBS3Test*:SyncAsync*:S3ObjectStorageConditionalOpsTest*:StorageObjectStorageSource*' > build/task9_s3_gcs_unit.log 2>&1
```

- [ ] **Step 5: Run the complete CAS unit gate**

```bash
build/src/unit_tests_dbms --gtest_filter='CAS*' > build/task9_cas_unit.log 2>&1
```

Every CAS suite must begin with `CAS`; fix a misnamed suite rather than widening the filter.

- [ ] **Step 6: Run the relevant integration lanes**

```bash
python3 -m ci.praktika run "integration" --test "test_storage_gcp_auth test_storage_s3 test_cas_s3 test_cas_gc_s3 test_cas_gcs" > build/task9_integration.log 2>&1
```

Run `test_gcs_hmac_live` separately in the authorized live environment and retain its log as release evidence.

- [ ] **Step 7: Review the fork diff against the merge base**

```bash
git diff --stat "$(git merge-base altinity/antalya-26.6 HEAD)..HEAD"
git diff "$(git merge-base altinity/antalya-26.6 HEAD)..HEAD" -- src/IO/S3 src/IO/WriteSettings.h src/IO/WriteBufferFromS3.cpp src/Disks/DiskObjectStorage/ObjectStorages src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
```

Verify the final architecture adds one operation bit and one HTTP bit, no client matrix, no user setting for request mode, no endpoint heuristic, and no duplicate S3 implementation. Confirm all non-CAS configuration names are unchanged.

- [ ] **Step 8: Check formatting, staged scope, and worktree integrity**

```bash
git diff --check
git status --short
git log --oneline --decorate -12
```

Do not stage unrelated pre-existing files. Confirm each implementation commit contains only its declared task files.

- [ ] **Step 9: Commit only final verification-driven corrections, if any**

If verification required changes, repeat the affected focused tests and add a new commit without amending:

```bash
git commit -m 'Complete `CAS` GCS isolation verification'
```

If no source or plan changes were needed, do not create an empty commit.

---

## Completion checklist {#completion-checklist}

- [ ] Ordinary `gcp_oauth` is byte-for-byte pre-CAS in Default mode, including response ETag semantics.
- [ ] Ordinary S3-interoperability HMAC keeps AWS SigV4 and existing configuration.
- [ ] Dedicated `http_client=gcs_hmac` keeps its user API and passes the live Default-operation allowlist gate.
- [ ] Only explicitly marked CAS requests receive GCS generation and metadata adaptation.
- [ ] `Client::BuildHttpRequest` re-derives mode on every retry and post-redirect attempt.
- [ ] CAS capability uses explicit `http_client`, including proxy/private endpoints.
- [ ] CAS HEAD absence returns `std::nullopt`; attributes round-trip; LIST is never a generation-token source.
- [ ] Exact DELETE uses generation match and the production battery cannot be bypassed by a writable generation mount.
- [ ] Every generation token-producing write is a single PUT within the cap and returns the exact response generation.
- [ ] Unknown/enabled versioning and writable `skip_access_check=true` fail closed; soft-delete limitations are documented.
- [ ] `_etag`, filesystem cache, page cache, and Parquet metadata cache remain in one ordinary ETag domain outside CAS.
- [ ] No third GCS client or OAuth token cache exists.
- [ ] Full build, focused S3/GCS tests, `CAS*` gate, deterministic integrations, and live GOOG4 characterization are green.
