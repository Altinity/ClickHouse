---
description: 'Narrowed proposal for provider-neutral conditional non-blob operations, native-token HEAD, and exact deletion for CAS'
sidebar_label: 'CAS conditional object storage'
sidebar_position: 2
slug: /superpowers/specs/cas-object-storage-conditional-operations-proposal
title: 'CAS object-storage conditional operations proposal'
doc_type: 'design'
---

# CAS object-storage conditional operations proposal {#cas-object-storage-conditional-operations-proposal}

Status: **narrowed draft for discussion (2026-08-23)**. This document proposes a follow-up refactor. It does not replace
the nearly implemented GCS request-isolation design in
`2026-08-20-cas-gcs-request-isolation-design.md`, and it must not interrupt that design's Task 9
verification or change its wire behavior.

The later
[unconditional blob-publication design](/superpowers/specs/cas-unconditional-blob-publication-design)
removed conditional blob PUT/copy and their response tokens from the production contract. This
proposal now applies to every conditional non-blob write, including create-if-absent
metadata/control artifacts and conditional replacements, plus native-version `HEAD` and
exact-version deletion. Historical API sketches below that mention conditional streaming blob
writes, write-once blob copy, or `copyObjectConditional` are retained as decision history and must
not be implemented as current requirements. Unconditional blob transport belongs to
`Backend::publishBlob`, outside this proposed conditional-operations extension.

The comparison base used while writing this proposal is
`git merge-base altinity/antalya-26.6 HEAD`, currently
`5e8eaeb4d7dd2484bd6dafc16f5d2f51b5e099da`.

## Executive summary {#executive-summary}

The implemented GCS isolation work fixes the urgent compatibility problem correctly: a typed,
per-request `native_conditional` bit activates `GCSConditionalDialect` only for CAS requests. Normal
`gcp_oauth` and `gcs_hmac` traffic keeps its pre-CAS behavior and response ETag semantics.

The remaining architectural problem is one level above the wire. CAS currently reaches conditional
storage semantics through a mixture of generic `IObjectStorage` virtuals, stringly typed ETag fields,
S3-specific `WriteSettings`, and S3 exception handling. That works for AWS and GCS, but it will make
Azure support another parallel special case.

The proposed follow-up is a provider-neutral conditional-operations extension:

- `IObjectStorageConditionalOperations` defines version-token `HEAD`, small-object conditional
  write, and exact-token remove.
- `S3ConditionalOperations` implements that semantic contract for the whole S3 family: ordinary AWS
  S3, other compatible S3 services, and GCS reached through the S3 XML API.
- `GCSConditionalDialect` remains a GCS-only HTTP wire adapter. It is not owned by, included by, or
  called directly from `S3ConditionalOperations`.
- A future `AzureConditionalOperations` implements the same narrowed semantic contract with Azure
  Blob SDK conditions and ETags.
- CAS depends only on the common extension and typed version tokens. Provider-specific retry,
  signing, response-token, and error-classification details remain in provider code. Blob multipart
  and native-copy transport remain on the ordinary object-storage path.

This is not expected to make the total fork diff materially smaller. Its value is to move behavior
out of frequently changed upstream files, remove provider details from CAS, prevent invalid
condition/token combinations, and make Azure an implementation of an existing contract instead of a
new CAS code path.

Recommendation: finish and validate the current GCS implementation first. Perform this refactor as a
separate, behavior-preserving series only if Azure CAS support is a near-term deliverable. If Azure is
not near term, keep the current implementation and limit immediate work to small naming and boundary
cleanups.

## Problem statement {#problem-statement}

### What the current implementation gets right {#what-the-current-implementation-gets-right}

The current design establishes the right compatibility boundary:

1. Authentication does not imply conditional semantics.
2. CAS explicitly marks only requests that need a native version-token dialect.
3. AWS SDK request objects carry typed in-memory state; no private marker header can leak onto the
   wire.
4. `Client::BuildHttpRequest` re-derives the state on every attempt, including a recreated request
   after redirect.
5. GCS request and response translation happens in the HTTP client, where signing order and response
   headers are available.
6. Ordinary GCS HEAD, GET, LIST, PUT, COPY, and DELETE requests are not opted into generation
   semantics, so their ETags and cache keys retain the non-CAS contract.
7. Conditional GCS writes fail closed when the operation cannot remain a token-producing single
   request.

These properties must survive any refactor proposed here.

### What is inconsistent today {#what-is-inconsistent-today}

The semantic API is currently distributed across several layers:

- `IObjectStorage::tryGetObjectMetadataWithNativeToken` returns a native token through
  `ObjectMetadata::etag`, even when the value is a GCS generation rather than an ETag.
- `IObjectStorage::removeObjectIfTokenMatches` is provider-neutral by name, but its parameter and
  comments still describe an ETag.
- `IObjectStorage::copyObjectConditional` returns `dest_etag`, which cannot honestly describe a GCS
  generation and is unsuitable as the future Azure-neutral result type.
- `IObjectStorage::conditionalOpsUseGenerationTokens` puts a GCS-specific capability on the generic
  storage interface.
- CAS constructs `WriteSettings` containing S3-private controls such as
  `s3_force_single_part_upload`, `s3_single_part_upload_max_bytes_override`,
  `s3_max_unexpected_write_error_retries_override`, and
  `s3_check_objects_after_upload_override`.
- CAS catches `S3Exception` to convert a precondition failure into a normal lost-race outcome.
- A successful conditional write obtains its token through the generic name
  `WriteBufferFromFileBase::getResultObjectETag`, although the result may be a GCS generation.
- HEAD, PUT, DELETE, and COPY transport conditional intent through different public surfaces.

The result is not merely a naming issue. Provider policy has leaked into CAS, while generic storage
interfaces expose operations whose real support is currently S3-family-specific. Adding Azure
directly to this shape would require either more provider tests inside CAS or more loosely specified
generic flags.

### Existing upstream conditional support is not the CAS abstraction {#existing-upstream-conditional-support-is-not-the-cas-abstraction}

Before the CAS-over-GCS work, upstream already had
`WriteSettings::object_storage_write_if_none_match` and
`WriteSettings::object_storage_write_if_match`. S3 writers consume them as HTTP preconditions; Azure
Blob and Azure Data Lake writers translate them to Azure access conditions; Iceberg uses the same
mechanism.

Those fields are useful low-level transport controls and must remain compatible. They are not a
complete CAS API because they do not specify all of the following as one contract:

- the kind of version token returned by HEAD and writes;
- an exact response token after a successful write;
- typed expected outcomes for a rejected condition;
- exact-token DELETE;
- write-once server-side COPY;
- provider retry and multipart restrictions required for correctness;
- mount-time validation of unsafe provider configuration.

The proposal therefore does not remove or reinterpret the existing upstream fields. It builds a
CAS-facing semantic layer above them and lets each provider implementation use the existing writer
plumbing where appropriate.

## Goals {#goals}

1. Preserve the user-facing and wire behavior of every non-CAS GCP configuration, including existing
   `gcp_oauth`, `gcs_hmac`, and ordinary S3 HMAC authentication selectors.
2. Preserve the already implemented CAS-over-GCS semantics for mutable operations, including
   generation tokens, per-request isolation, exact DELETE, single-attempt conditional writes, and
   fail-closed conditional-write multipart limits.
3. Give AWS, GCS, and Azure one provider-neutral CAS contract without pretending that their wire
   protocols are identical.
4. Remove S3 SDK types, S3 exceptions, S3 retry fields, and GCS token decisions from CAS.
5. Concentrate new fork-owned policy in new files with narrow ownership, reducing recurring merge
   conflicts in upstream hotspots.
6. Make invalid states hard to express: an empty match token, a generation passed to an ETag backend,
   or simultaneous match and absent conditions must be rejected before an I/O request.
7. Fail closed when the requested atomicity or exact response token cannot be guaranteed.
8. Keep CAS persistent formats and existing non-CAS configuration unchanged.

## Non-goals {#non-goals}

- Rewriting `GCSConditionalDialect` now that its request-isolation design is nearly complete.
- Adding an empty wire-adapter class for AWS or Azure merely to make the class diagram symmetric.
- Replacing ordinary `IObjectStorage::writeObject`, `copyObject`, or metadata APIs for non-CAS users.
- Generalizing all object-storage operations or all concurrency controls in one refactor.
- Owning the blob `HEAD`-then-unconditional-publication state machine or its transport.
- Claiming that every S3-compatible service supports the same conditional behavior as AWS S3.
- Changing user-visible names or defaults for non-CAS authentication and object-storage settings.

## Design principle: separate semantics from wire dialect {#design-principle-separate-semantics-from-wire-dialect}

There are two different adaptation problems, and they should remain two different layers.

### Semantic operation adapter {#semantic-operation-adapter}

`S3ConditionalOperations` and the future `AzureConditionalOperations` adapt a provider SDK and its
storage implementation to the operations CAS needs. They own questions such as:

- how to perform conditional create and replace;
- how to obtain the exact version token returned by a successful operation;
- how to classify a provider precondition failure as `ConditionalWriteConditionNotMet`;
- whether multipart is safe for a token-producing operation;
- which retry policy preserves the operation's meaning;
- how to perform exact-token DELETE and write-once COPY;
- which provider configuration makes writable CAS unsafe.

AWS does use this adapter. It does not need a separate HTTP dialect adapter because the AWS SDK and
S3 service already agree on `If-Match`, `If-None-Match`, and ETag semantics.

### HTTP wire-dialect adapter {#http-wire-dialect-adapter}

`GCSConditionalDialect` solves a narrower problem: the S3 SDK expresses an ETag dialect, while GCS
native conditional operations use generation headers and return a generation separately from ETag.
It translates only an explicitly marked S3-family request at the last point before GCS
authentication/signing and interprets the corresponding response.

It stays under `src/IO/S3` because that is the layer which has all of the required information:

- the concrete SDK request and HTTP headers;
- the selected `gcp_oauth` or `gcs_hmac` transport;
- signing/authentication order;
- retries and redirects;
- response headers such as `x-goog-generation`.

`S3ConditionalOperations` must never call `GCSConditionalDialect` directly. Its only GCS-specific
action is to mark the S3 request as `NativeConditional` through typed request state. The existing path
then performs the translation:

```text
S3ConditionalOperations
    -> S3 SDK request wrapper: native_conditional = true
    -> Client::BuildHttpRequest
    -> ExtendedHttpRequest: native_conditional = true
    -> GCS OAuth or GOOG4 HTTP client
    -> GCSConditionalDialect
    -> GCS XML API
```

An AWS request follows the same semantic adapter but the typed bit has no wire effect:

```text
S3ConditionalOperations
    -> S3 SDK If-Match / If-None-Match request
    -> ordinary AWS signing and HTTP path
    -> AWS S3
```

This asymmetry is intentional. The commonality is at the CAS operation contract, not at the wire
protocol.

## Proposed modules and class ownership {#proposed-modules-and-class-ownership}

### Common conditional contract {#common-conditional-contract}

Add new fork-owned files, tentatively:

- `src/Disks/DiskObjectStorage/ObjectStorages/ConditionalOperations.h`
- `src/Disks/DiskObjectStorage/ObjectStorages/ConditionalOperations.cpp`

They contain only provider-neutral types and interfaces:

- `ObjectStorageVersionKind`
- `ObjectStorageVersionToken`
- `ObjectStorageWriteCondition`
- result and capability types
- `IObjectStorageConditionalWrite`
- `IObjectStorageConditionalOperations`

`IObjectStorage` gains one optional extension factory, with a default result meaning unsupported.
This replaces several CAS-specific virtual methods rather than adding more of them.

### S3-family implementation {#s3-family-implementation}

Add:

- `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ConditionalOperations.h`
- `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ConditionalOperations.cpp`

`S3ConditionalOperations` covers AWS S3, S3-compatible services that pass the capability probe, and
GCS through either supported S3 HTTP client. It owns the S3-specific implementation currently spread
through `S3ObjectStorage`, CAS, `WriteSettings`, and helper call sites.

`S3ObjectStorage::createConditionalOperations` constructs the implementation. The implementation may
be declared a friend of `S3ObjectStorage` so it can use the current client, URI, request settings, and
logging without making new S3 internals public.

The returned implementation is a borrowed-lifetime extension: it may hold a reference to the
`S3ObjectStorage` which created it and must not outlive that storage. CAS already owns the
`ObjectStoragePtr`; its member order must destroy the conditional-operations object before the
storage. If this lifetime contract proves too subtle during implementation, use an owned
provider-state object rather than adding `enable_shared_from_this` to every `IObjectStorage`.

### GCS wire implementation {#gcs-wire-implementation}

Keep the current files and responsibilities:

- `src/IO/S3/GCSConditionalDialect.h`
- `src/IO/S3/GCSConditionalDialect.cpp`
- typed state in `src/IO/S3/Requests.h` and `PocoHTTPClientFactory.h`
- derivation in `Client::BuildHttpRequest`
- consumption in the GCS OAuth and GOOG4 HTTP paths

The request bit remains an S3-I/O implementation detail. Once CAS calls
`IObjectStorageConditionalOperations`, `ObjectStorageRequestMode` should no longer be part of the
generic CAS-facing `WriteSettings` contract. Whether the enum itself can move under `src/IO/S3` is a
mechanical follow-up decided by remaining non-CAS users.

### Azure implementation {#azure-implementation}

When Azure CAS is implemented, add:

- `src/Disks/DiskObjectStorage/ObjectStorages/AzureBlobStorage/AzureConditionalOperations.h`
- `src/Disks/DiskObjectStorage/ObjectStorages/AzureBlobStorage/AzureConditionalOperations.cpp`

`AzureConditionalOperations` maps common conditions to `BlobAccessConditions`, captures the ETag
from upload or `CommitBlockList` responses, performs exact-token deletion and write-once copy where
the Azure SDK can guarantee them, and maps only documented precondition failures to typed outcomes.

Azure does not use `GCSConditionalDialect`, the S3 request carrier, or S3 retry profiles.

### Cached and decorated storage {#cached-and-decorated-storage}

`CachedObjectStorage` cannot simply return the underlying implementation without review. Conditional
HEAD must not return a stale cached token, and successful conditional writes, copies, and deletes
must invalidate the same cache entries as their ordinary equivalents.

The preferred solution is `CachedConditionalOperations`, a small decorator which:

- delegates version-token HEAD directly to the underlying provider, bypassing cached metadata;
- invalidates the destination cache before or after a successful write according to existing cache
  safety rules;
- invalidates a removed key only after `Removed`, not after `TokenMismatch` or `NotFound` unless the
  existing cache contract already requires it;
- invalidates a copy destination after `ConditionalCopyCreated`;
- preserves exact response tokens and typed outcomes unchanged.

If production construction proves that CAS always receives an unwrapped provider storage, that
invariant may replace the decorator. It must be demonstrated in code and tested; it must not be
assumed from the current happy path.

## Proposed API {#proposed-api}

Names below are concrete enough to review but remain draft names. The important choice is the type
boundary, not the exact spelling.

### Version token {#version-token}

```cpp
enum class ObjectStorageVersionKind : uint8_t
{
    ETag,
    Generation,
};

struct ObjectStorageVersionToken
{
    ObjectStorageVersionKind kind;
    String value;

    static ObjectStorageVersionToken etag(String value);
    static ObjectStorageVersionToken generation(String value);
};
```

Construction validates that `value` is non-empty. A generation constructor additionally validates
the canonical numeric form expected by GCS. Provider implementations reject a token whose kind does
not match their pinned capability before issuing I/O.

This prevents the current situation in which a field called ETag sometimes contains a generation.
It also gives one explicit mapping point from storage tokens to the existing CAS `TokenType`.

### Write condition {#write-condition}

```cpp
class ObjectStorageWriteCondition
{
public:
    enum class Kind : uint8_t
    {
        None,
        IfAbsent,
        IfVersionMatches,
    };

    static ObjectStorageWriteCondition none();
    static ObjectStorageWriteCondition ifAbsent();
    static ObjectStorageWriteCondition ifVersionMatches(ObjectStorageVersionToken expected);

    Kind kind() const;
    const ObjectStorageVersionToken & expectedVersion() const;
};
```

Use constructors or a private tagged variant so it is impossible to represent both `IfAbsent` and
`IfVersionMatches`, or `IfVersionMatches` without a token. Do not expose two independent strings like
the low-level `WriteSettings` fields as the CAS API.

### Metadata and outcomes {#metadata-and-outcomes}

```cpp
struct ObjectMetadataWithVersion
{
    ObjectMetadata metadata;
    ObjectStorageVersionToken version;
};

struct ConditionalWriteSucceeded
{
    ObjectStorageVersionToken version;
};

struct ConditionalWriteConditionNotMet {};

using ConditionalWriteResult = std::variant<
    ConditionalWriteSucceeded,
    ConditionalWriteConditionNotMet>;

enum class ConditionalRemoveOutcome : uint8_t
{
    Removed,
    VersionMismatch,
    NotFound,
};

struct ConditionalRemoveResult
{
    ConditionalRemoveOutcome outcome;
};

struct ConditionalCopyCreated
{
    ObjectStorageVersionToken destination_version;
};

struct ConditionalCopyDestinationExists {};

using ConditionalCopyResult = std::variant<
    ConditionalCopyCreated,
    ConditionalCopyDestinationExists>;
```

The invariants are:

- `ConditionalWriteSucceeded` and `ConditionalCopyCreated` carry the exact token returned by that
  successful operation by construction.
- `ConditionalWriteConditionNotMet` and `ConditionalCopyDestinationExists` carry no token.
- A missing or empty successful response token is an exception, not permission to perform a HEAD
  fallback.
- Only the provider's documented precondition failure becomes a normal outcome. Authentication,
  transport, timeout, parsing, and unknown service failures propagate.

The no-HEAD-fallback rule is important. A HEAD after the write can observe a later writer and return
the wrong incarnation token.

### Conditional streaming write {#conditional-streaming-write}

```cpp
struct ConditionalWriteRequest
{
    StoredObject object;
    WriteMode mode = WriteMode::Rewrite;
    std::optional<ObjectAttributes> attributes;
    size_t buffer_size = DBMS_DEFAULT_BUFFER_SIZE;
    ObjectStorageWriteCondition condition = ObjectStorageWriteCondition::none();
};

class IObjectStorageConditionalWrite
{
public:
    virtual WriteBufferFromFileBase & buffer() = 0;
    virtual ConditionalWriteResult commit() = 0;
    virtual void cancel() noexcept = 0;
    virtual ~IObjectStorageConditionalWrite() = default;
};
```

`commit` finalizes the provider writer, classifies an expected precondition loss, and returns the
exact response token. It is the only success boundary CAS uses. Destruction without `commit` must
cancel or abandon the write according to existing writer semantics; it must never implicitly commit.

The session exists because returning a bare `WriteBufferFromFileBase` would leave the caller
responsible for provider exception mapping and token extraction again. It also lets the provider own
retry and multipart policy for the entire write lifetime.

### Operations extension {#operations-extension}

```cpp
struct ConditionalOperationsSettings
{
    uint64_t max_token_producing_write_bytes = 0;
};

struct ConditionalOperationsCapabilities
{
    ObjectStorageVersionKind version_kind;
    bool supports_conditional_create = false;
    bool supports_conditional_replace = false;
    bool supports_exact_remove = false;
    bool supports_copy_if_absent = false;
    bool list_returns_version_tokens = false;
};

class IObjectStorageConditionalOperations
{
public:
    virtual const ConditionalOperationsCapabilities & capabilities() const = 0;

    virtual std::optional<ObjectMetadataWithVersion> tryGetMetadata(
        const String & path,
        bool with_tags) const = 0;

    virtual std::unique_ptr<IObjectStorageConditionalWrite> openWrite(
        ConditionalWriteRequest request) = 0;

    virtual ConditionalRemoveResult removeIfVersionMatches(
        const StoredObject & object,
        const ObjectStorageVersionToken & expected) = 0;

    virtual ConditionalCopyResult copyIfAbsent(
        const StoredObject & source,
        const StoredObject & destination,
        const ReadSettings & read_settings,
        std::optional<ObjectAttributes> destination_attributes = {}) = 0;

    virtual void validateWritableConfiguration() = 0;

    virtual ~IObjectStorageConditionalOperations() = default;
};
```

The single optional hook on `IObjectStorage` is:

```cpp
virtual std::unique_ptr<IObjectStorageConditionalOperations> createConditionalOperations(
    const ConditionalOperationsSettings & settings)
{
    return {};
}
```

A null result means that the storage does not implement native conditional operations. CAS Native
mode fails closed; it does not infer support from the storage type or fall back to ordinary methods.

`ConditionalOperationsSettings` contains CAS policy which a provider needs to implement the
contract, not raw SDK knobs. In particular, CAS supplies a maximum token-producing write size; only
`S3ConditionalOperations` decides that GCS must convert that policy into a single-PUT or
single-COPY limit.

### Capability stability {#capability-stability}

The implementation pins `version_kind` when the extension is created. A client reload which would
change an active mount from ETag semantics to generation semantics, or the reverse, must be rejected
or cause subsequent operations to fail closed before I/O. It must not silently change the meaning of
persisted CAS tokens.

For S3, capability selection is derived from the explicit `http_client` declaration, not endpoint URL
inspection:

- `gcp_oauth` and `gcs_hmac` select `Generation`;
- ordinary S3 clients select `ETag`;
- an unknown client mode does not acquire generation semantics from a hostname heuristic.

This preserves private/proxied GCS endpoints and prevents future upstream endpoint changes from
silently opting ordinary requests into a different token dialect.

## Provider responsibilities {#provider-responsibilities}

### S3 and AWS {#s3-and-aws}

`S3ConditionalOperations` is responsible for:

- placing `If-None-Match: *` or `If-Match: <etag>` on the correct SDK request;
- selecting the single-attempt client for conditional writes whose replay is not proven safe;
- extracting the exact ETag from PUT, multipart completion, or COPY response;
- classifying only S3 precondition failure as an expected outcome;
- implementing `DeleteObject` with `If-Match`;
- refusing native conditional COPY when the implementation would fall back to an unconditional
  read/write copy;
- reporting bucket-versioning state where it matters to the selected dialect.

AWS requests do not set the GCS transport mode. There is still an adapter class because the adapter
is the provider-family implementation of the CAS contract; there is simply no AWS wire translation.

S3-compatible services are supported only after the existing mutating capability battery proves the
required behavior. A syntactically accepted header is not evidence that the service enforces it.

### GCS over the S3 XML API {#gcs-over-the-s3-xml-api}

For a generation-token implementation, `S3ConditionalOperations` additionally:

- marks the relevant HEAD, PUT, DELETE, COPY, and defensive multipart-completion request wrappers as
  native conditional;
- constrains every token-producing write or conditional copy to an operation shape for which GCS
  enforces the precondition and returns `x-goog-generation`;
- rejects requests above the configured cap before issuing I/O;
- requires the generation returned on the same response and never substitutes a later HEAD;
- fails writable mount validation when bucket versioning is enabled or cannot be verified;
- requires the mutating capability battery, so writable generation mounts cannot use
  `skip_access_check=true`.

The HTTP layer remains responsible for translating conditional and metadata headers, removing only
the authentication artifacts required by the marked OAuth path, signing GOOG4 requests after
translation, and exposing the response generation through the SDK result plumbing.

Unmarked `gcp_oauth` and `gcs_hmac` requests remain byte-for-byte or semantically identical to their
documented pre-CAS behavior, subject only to pre-existing targeted GCS COPY rewrites. Their response
ETag is never overwritten with a generation.

### Azure Blob Storage {#azure-blob-storage}

The Azure implementation should:

- use Azure ETags as `ObjectStorageVersionToken::ETag`;
- apply `IfNoneMatch` or `IfMatch` through Azure access-condition types;
- retrieve the exact ETag from the successful upload or block-list commit response;
- map Azure's documented condition-not-met response to `ConditionalWriteConditionNotMet`;
- perform delete with `IfMatch` and distinguish mismatch from absence;
- implement copy-if-absent only if the service-side copy operation enforces the destination
  precondition for every supported size and mode;
- keep Azure retry and block-upload decisions out of CAS.

Azure Blob and Azure Data Lake paths must be evaluated separately. Sharing a configuration family
does not prove they return or enforce tokens in the same places.

If Azure cannot return the exact token for a successful multipart/block commit, that operation shape
is unsupported until it can. A post-write HEAD is not an acceptable compatibility fallback.

## CAS responsibilities after the refactor {#cas-responsibilities-after-the-refactor}

CAS becomes responsible only for protocol intent:

- request Native conditional operations at mount;
- require the capability subset used by the selected CAS mode;
- map `ObjectStorageVersionKind` to its existing persisted `TokenType` once;
- choose `None`, `IfAbsent`, or `IfVersionMatches`;
- interpret typed written/lost-race/remove/copy outcomes;
- supply the operator-configured maximum size for a token-producing write;
- run the provider-neutral mutating capability battery.

CAS no longer:

- includes or catches `S3Exception`;
- sets S3 retry, multipart, check-after-upload, or request-mode fields;
- asks whether generic storage "uses generation tokens";
- reads a generation from a field named ETag;
- knows that GCS uses `x-goog-if-generation-match`;
- needs a new conditional branch when Azure is added.

The existing CAS on-disk formats do not change. The boundary conversion is conceptually:

```cpp
Token toCasToken(const ObjectStorageVersionToken & token)
{
    return {
        .type = token.kind == ObjectStorageVersionKind::Generation
            ? TokenType::Generation
            : TokenType::ETag,
        .value = token.value,
    };
}
```

## End-to-end class relationships {#end-to-end-class-relationships}

```text
Cas::ObjectStorageBackend
    owns ObjectStoragePtr
    owns IObjectStorageConditionalOperations (created by the storage)
                    |
          +---------+----------+
          |                    |
S3ConditionalOperations   AzureConditionalOperations
          |                    |
    S3ObjectStorage          AzureObjectStorage
          |
    AWS SDK request wrappers
          |
    Client::BuildHttpRequest
          |
   +------+------------------+
   |                         |
ordinary S3 HTTP       marked GCS HTTP request
                             |
                    GCSConditionalDialect
                             |
                    OAuth or GOOG4 signing
```

The diagram contains one provider-neutral semantic interface, one implementation per provider SDK
family, and a GCS-only wire translator where the protocol mismatch actually exists.

## Representative call flows {#representative-call-flows}

### Conditional HEAD {#conditional-head}

1. CAS calls `IObjectStorageConditionalOperations::tryGetMetadata`.
2. `S3ConditionalOperations` issues HEAD with the pinned request mode.
3. AWS returns an ETag normally; marked GCS response processing supplies the generation as the native
   token; Azure returns its ETag through the Azure SDK.
4. The provider constructs `ObjectMetadataWithVersion` with an honest token kind.
5. Absence returns `std::nullopt`; it is not exception-driven.

### Conditional streaming PUT {#conditional-streaming-put}

1. CAS calls `openWrite` with a typed condition.
2. The provider validates token kind, retry policy, and supported operation shape before I/O.
3. CAS writes bytes through `IObjectStorageConditionalWrite::buffer`.
4. CAS calls `commit`.
5. The provider maps only a rejected precondition to `ConditionalWriteConditionNotMet`; all other
   failures propagate.
6. On success, the result contains the exact response ETag or generation.

### Exact DELETE {#exact-delete}

1. CAS calls `removeIfVersionMatches` with a typed token.
2. The provider validates the token kind and sends its native exact-delete condition.
3. The result is `Removed`, `VersionMismatch`, or `NotFound`.
4. The GCS capability battery still exercises this same production method with both correct and
   incorrect tokens. Losing the request marker therefore prevents a writable mount rather than
   silently degrading GC correctness.

### Write-once COPY {#write-once-copy}

1. CAS calls `copyIfAbsent`.
2. The provider verifies that the selected native copy path cannot fall back to an unconditional
   read/write copy.
3. The provider keeps the request within any dialect-specific single-operation cap.
4. Success returns `ConditionalCopyCreated` with the destination's exact version token; an existing
   destination returns `ConditionalCopyDestinationExists`; all other failures propagate.

## Relationship to the nearly completed GCS work {#relationship-to-the-nearly-completed-gcs-work}

The current implementation is not throwaway work. Most of it becomes the lower half of the proposed
architecture unchanged:

| Current mechanism | Proposed relationship |
|---|---|
| Typed `native_conditional` state on S3 request wrappers | Retained as the private S3-to-HTTP carrier |
| `ExtendedHttpRequest` | Retained |
| `Client::BuildHttpRequest` stamping | Retained |
| `GCSConditionalDialect` request/response translation | Retained at the HTTP boundary |
| OAuth and GOOG4 authentication order | Retained |
| GCS live test groups | Retained as release gates |
| `tryGetObjectMetadataWithNativeToken` | Moved behind `S3ConditionalOperations::tryGetMetadata` |
| `removeObjectIfTokenMatches` | Moved behind typed `removeIfVersionMatches` |
| `copyObjectConditional` | Moved behind typed `copyIfAbsent` |
| CAS construction of S3 `WriteSettings` | Moved into `S3ConditionalOperations` |
| CAS `S3Exception` classification | Moved into `S3ConditionalOperations` |
| `conditionalOpsUseGenerationTokens` | Replaced by `version_kind` returned from `capabilities` |
| `getResultObjectETag` for CAS | Consumed privately by provider write session; CAS sees a version token |

The safe ordering is therefore to establish a green current baseline first and then move ownership
without changing the already tested wire path.

## User-facing impact {#user-facing-impact}

For users without CAS, there is no intended change:

- no configuration name, default, authentication mode, header policy, retry policy, or ETag behavior
  changes;
- `gcp_oauth`, `gcs_hmac`, and ordinary S3 HMAC keep their current meanings;
- existing uses of `object_storage_write_if_none_match` and `object_storage_write_if_match` keep their
  upstream behavior;
- LIST and ordinary HEAD continue to expose the service ETag, not a GCS generation;
- existing cache keys and the `_etag` virtual column retain non-CAS semantics.

For CAS users, behavior also remains the same after the refactor. The observable improvement is that
AWS, GCS, and eventually Azure are selected through one checked capability contract. Unsupported or
ambiguous cases fail at mount or before I/O with a provider-specific explanation.

There are not two new HMAC user modes in this proposal. Ordinary S3 HMAC is the AWS SigV4 path.
`gcs_hmac` is the existing selector for the GOOG4 HMAC signing path needed by GCS interoperability.
They remain separate because the signature algorithms and canonicalization rules are different, not
because CAS exposes two versions of the same operation API.

## Compatibility and future-upstream safety {#compatibility-and-future-upstream-safety}

The compatibility invariant is stronger than "upstream does not currently use conditional ETags":

> A request which does not explicitly enter the CAS conditional-operations extension must behave as
> it would have behaved without this fork feature, even if future upstream code starts using
> `If-Match`, `If-None-Match`, response ETags, or new `x-amz-*` headers.

This is achieved by keeping GCS generation translation behind typed per-request state. Detection must
never be based on the presence of conditional headers, an endpoint hostname, an authentication mode
alone, or a storage-wide mutable flag.

Future upstream code can therefore add ordinary conditional operations without accidentally
receiving GCS generation semantics. It opts in only by calling the explicit fork extension or by
setting the private typed S3 request state itself.

New fork code should be concentrated in new files. Existing upstream hotspots should contain only
narrow hooks and transport plumbing:

- one optional factory on `IObjectStorage`;
- one override on each supporting object storage;
- existing request-carrier and HTTP-consumption hooks under `src/IO/S3`;
- minimal provider writer support for returning exact response tokens where upstream lacks it.

As of the comparison used for this draft, the relevant current production slice touches 29 files and
adds roughly 1,447 lines, including GCS authentication and wire support rather than only conditional
operations. Since the merge base, `IObjectStorage.h` has 18 commits on `upstream/master`,
`S3ObjectStorage.cpp` 22, and `Client.cpp` 11. New fork-owned implementation files avoid some of this
churn, although the total line count is unlikely to decrease.

## Work estimate and diff impact {#work-estimate-and-diff-impact}

These are review estimates, not commitments:

| Work item | Estimated changed lines | Main risk |
|---|---:|---|
| Common types and interfaces | 150–220 | Over-designing capabilities or lifetime |
| S3 operations and write session | 250–400 | Behavior drift while moving working code |
| CAS conversion | 150–250 | Lost outcome or token mapping |
| Remove old fork-only generic fields and virtuals | 100–200 deletions | Hidden callers and tests |
| Cache/decorator integration | 80–150 | Stale tokens or missed invalidation |
| Contract and regression tests | 250–500 | Mock coverage mistaken for live proof |
| Future Azure implementation | 300–600 | Block commit tokens, copy, delete semantics |

The behavior-preserving pre-Azure refactor is likely 700–1,200 changed lines. Net production code may
grow by roughly 50–250 lines because typed results and a write session replace compact but leaky
strings and flags.

The more important diff improvement is location:

- approximately 150–300 lines of CAS-specific logic can move out of `IObjectStorage.h`,
  `S3ObjectStorage.cpp`, `WriteSettings.h`, and `CasObjectStorageBackend.cpp`;
- most new policy lives in new fork-owned files with no upstream history;
- future Azure work adds an implementation file instead of reopening CAS and S3 code;
- reviews can distinguish semantic CAS changes from GCS wire/authentication changes.

This refactor improves mergeability, not necessarily `git diff --stat`.

## Quality improvements {#quality-improvements}

### Stronger types {#stronger-types}

The API distinguishes ETags from generations, absence from mismatch, and an expected race loss from a
transport failure. Invalid combinations are rejected before I/O rather than encoded as empty strings
or pairs of flags.

### Clearer ownership {#clearer-ownership}

CAS owns protocol intent. Provider adapters own provider semantics. The GCS HTTP adapter owns only
wire translation. Authentication owns only authentication. Each layer can be reviewed without
understanding every other layer's private switches.

### Better extension path {#better-extension-path}

Azure implements a stable interface and contract test suite. It does not cause another
`#if USE_AZURE_BLOB_STORAGE` exception branch in CAS or require GCS concepts to become more generic.

### Better failure behavior {#better-failure-behavior}

The provider rejects unsupported multipart, missing response tokens, unsafe retries, token-kind drift,
or unverified bucket policy at the boundary which understands them. There is no consequential
fallback.

### Better test architecture {#better-test-architecture}

A provider-neutral contract suite can exercise the same abstract outcomes for S3, GCS, and Azure,
while wire tests remain provider-specific. This avoids both duplicated CAS tests and false confidence
from a mock that cannot prove live service behavior.

## Risks and concerns {#risks-and-concerns}

### Refactoring a working GCS path {#refactoring-a-working-gcs-path}

The largest immediate risk is changing ownership after the current design has reached Task 9. A
mechanically attractive refactor can omit one marker, retry override, response token, or multipart
guard. The refactor must preserve behavior commit by commit and rerun the entire current GCS gate.

### Abstraction cost {#abstraction-cost}

The interface, typed results, and write session add concepts and lines. If Azure is speculative, this
may be architecture in search of a second implementation. That is why the recommendation is
conditional on near-term Azure work.

### Lifetime of the extension {#lifetime-of-the-extension}

A factory-created provider facade which holds a reference to its storage has a lifetime contract.
CAS member order can enforce it, but reviewers may prefer a storage-owned view or a separate owned
provider-state object. Do not add shared ownership to all `IObjectStorage` instances merely to hide
this question.

### Cached and other decorator storages {#cached-and-other-decorator-storages}

Delegation can bypass cache invalidation or return stale metadata. Every wrapper which can reach CAS
must either decorate the conditional extension correctly or explicitly reject it. This includes any
future encryption, throttling, or observability wrapper, not only `CachedObjectStorage`.

### Configuration reload and token-kind drift {#configuration-reload-and-token-kind-drift}

Changing `http_client` on a live mount can change the token dialect. A CAS token recorded as an ETag
must never later be interpreted as a generation. The extension must pin the kind and fail closed if
the current client becomes incompatible.

### Exact response token availability {#exact-response-token-availability}

Single PUT, multipart completion, server-side COPY, and Azure block-list commit do not necessarily
expose version tokens through the same SDK result. Each supported operation shape requires proof.
Missing token plumbing is a hard unsupported case, not a reason to issue HEAD.

### Retry ambiguity {#retry-ambiguity}

An unconditional token-producing write retried after an ambiguous failure can create a second
generation and orphan the first. A conditional write replay may change a timeout into a
precondition failure. Provider implementations must own and document these distinctions; a single
generic retry boolean is insufficient.

### Multipart and copy semantics {#multipart-and-copy-semantics}

GCS does not enforce the required condition on every multipart completion shape. Azure block upload
and service-side copy need an independent analysis. Size caps must be applied before any request that
could leave an uncommitted or incorrectly committed object.

### Delete semantics, versioning, and soft delete {#delete-semantics-versioning-and-soft-delete}

Exact-token DELETE can be correct as a compare operation while still failing CAS reclaim goals when
bucket versioning archives the old incarnation. GCS versioning is verifiable through the XML API and
must fail closed when enabled or unverifiable. GCS soft-delete policy is only inspectable through the
JSON API, so the XML-backed mount can give an operator recommendation but cannot make the same
verified guarantee without adding another API dependency.

Azure retention, snapshots, versions, and soft-delete policy require the same liveness analysis
before writable CAS is declared supported.

### S3-compatible services {#s3-compatible-services}

An SDK method or accepted header is not a capability guarantee. Exact DELETE and conditional COPY in
particular vary across services and versions. Keep the mutating mount-time probe and avoid capability
claims based only on client type.

### User-visible exception and metrics changes {#user-visible-exception-and-metrics-changes}

Moving error classification can change exception messages, error codes, blob-storage log entries, or
ProfileEvents even when storage behavior is correct. These are observable operational contracts and
must be included in the behavior-preserving test inventory.

## Alternatives considered {#alternatives-considered}

### Alternative A: keep the current design and add Azure directly {#alternative-a-keep-current-design-and-add-azure-directly}

This has the lowest immediate risk and is the right answer if Azure is not scheduled. It avoids
refactoring a newly validated GCS path.

Its cost is accumulating parallel provider branches: CAS will need Azure exception mapping, response
ETag extraction, retry/block-upload policy, conditional DELETE, and copy behavior in addition to the
existing S3-specific fields. The next provider will make the same problem larger.

### Alternative B: smaller cleanup directly on IObjectStorage {#alternative-b-smaller-cleanup-directly-on-iobjectstorage}

Add typed token and result types but keep conditional HEAD, write, delete, and copy as virtual methods
directly on `IObjectStorage`. This is approximately half the work and removes the most misleading ETag
names.

It also expands a heavily changed upstream interface with several rarely implemented methods and
leaves provider policy mixed into large object-storage classes. It is a reasonable compromise if
extension lifetime and decorator handling outweigh merge concerns.

### Alternative C: optional conditional-operations extension {#alternative-c-optional-conditional-operations-extension}

This is the proposal in this document. It adds one hook to `IObjectStorage`, keeps the provider
surface cohesive, and makes unsupported storage explicit.

It has the best long-term Azure and fork story, but the highest short-term refactor risk. Choose it
only after the current GCS baseline is green and when the second implementation is real.

### Alternative D: universal HTTP conditional-dialect adapter {#alternative-d-universal-http-conditional-dialect-adapter}

Rejected. AWS and Azure do not need GCS header translation, and Azure does not use the S3 HTTP stack.
A universal wire adapter would either be an empty abstraction for most providers or would mix SDK,
HTTP, and storage semantics into one layer.

### Alternative E: one client per conditional mode {#alternative-e-one-client-per-conditional-mode}

Rejected. It multiplies GCP OAuth token caches and refresh timelines, requires client cache
invalidation on rotation, and models a per-request property as a client matrix. Retry strategy may
remain client-level because the AWS SDK makes it client-level; the GCS token dialect must remain
per-request.

### Alternative F: storage-wide mode or URL detection {#alternative-f-storage-wide-mode-or-url-detection}

Rejected. A storage-wide mode changes unrelated requests and recreates the original regression.
Endpoint detection fails for private and proxied GCS URLs and may silently change when upstream adds
new endpoint forms. The explicit `http_client` declaration is the user's authentication and protocol
intent; the request wrapper is the operation intent.

### Alternative G: infer CAS intent from conditional headers {#alternative-g-infer-cas-intent-from-conditional-headers}

Rejected. Future upstream code may legitimately use `If-Match` or `If-None-Match` without requesting
GCS generation semantics. Header detection would turn an unrelated upstream feature into a silent
wire-format and ETag-semantic change.

### Alternative H: thread-local request mode {#alternative-h-thread-local-request-mode}

Rejected. Parallel upload work does not reliably inherit a caller's thread-local scope. The request
wrapper is the only source of truth which follows the operation through asynchronous execution,
retries, and redirect reconstruction.

## Recommended migration {#recommended-migration}

### Phase 0: finish the current GCS gate {#phase-0-finish-the-current-gcs-gate}

Complete Task 9 of the existing plan, including ordinary GCS compatibility, native conditional OAuth
and GOOG4 live groups, exact DELETE, token-producing writes, cache ETag consistency, and zero-test
guards. Record a green baseline before architectural movement.

### Phase 1: introduce the common contract without changing behavior {#phase-1-introduce-the-common-contract-without-changing-behavior}

Add typed common types, the optional `IObjectStorage` factory, and
`S3ConditionalOperations`. Initially delegate to the existing working S3/GCS methods and writer
plumbing. Add provider-neutral contract tests, but keep the old CAS entry points active.

Every commit must build and preserve the current wire tests. Do not remove an old hook until its last
caller has moved in the same or an earlier commit.

### Phase 2: switch CAS atomically {#phase-2-switch-cas-atomically}

Construct the extension at mount, pin the token kind, validate capabilities, and route HEAD, PUT,
DELETE, and COPY through it. Move expected-condition classification out of CAS. Do not leave an
intermediate commit in which the old GCS adapter has been disabled before all new call sites mark
their requests.

There is no runtime fallback to the old path. A missing extension is a mount failure for Native mode.

### Phase 3: move provider policy and remove old fork-only API {#phase-3-move-provider-policy-and-remove-old-fork-only-api}

Move S3 retry selection, single-operation caps, response-token extraction, versioning queries, and
provider exception mapping into `S3ConditionalOperations`. Remove the superseded CAS-only virtuals
and generic fields after repository-wide call-site checks.

Retain upstream non-CAS `WriteSettings` conditional fields and their existing Azure/Iceberg users.

### Phase 4: handle decorators and rerun all gates {#phase-4-handle-decorators-and-rerun-all-gates}

Implement or prove the cache/decorator boundary. Rerun all current unit, integration, adversarial, and
live GCS gates. Compare request logs, ProfileEvents, cache behavior, exception classification, and
exact response tokens against the Phase 0 baseline.

### Phase 5: add Azure {#phase-5-add-azure}

Implement `AzureConditionalOperations` against the frozen contract. Add live Azure tests for HEAD,
conditional single and block writes, exact DELETE, write-once COPY, attributes, token equality, and
retention/versioning prerequisites. CAS code should require no provider branch.

## Required tests and release gates {#required-tests-and-release-gates}

### Provider-neutral contract tests {#provider-neutral-contract-tests}

- absent and existing version-token HEAD;
- unconditional write returns the exact response token;
- create-if-absent winner and loser;
- replace-if-version-matches winner and stale-token loser;
- token-kind mismatch fails before I/O;
- exact remove: removed, mismatch, and absent;
- copy-if-absent: created and destination exists;
- unknown provider error propagates;
- missing successful response token fails closed;
- extension absence fails Native mount;
- token-kind change after reload fails closed.

### S3 and GCS tests {#s3-and-gcs-tests}

- all existing `GCSConditionalDialect`, `GOOG4Signer`, `IOTestAwsS3Client`, `WBS3`,
  `S3ObjectStorageConditionalOpsTest`, and CAS probe tests;
- request-marker propagation on every attempt and redirect;
- ordinary GCS traffic never activates generation translation;
- exact DELETE uses the marked generation path;
- a token-producing write or copy above the GCS cap fails before multipart;
- response attributes round-trip across GCS prefix translation;
- cache key and `_etag` consistency between LIST and ordinary HEAD;
- live default `gcs_hmac`, native `gcp_oauth`, and native `gcs_hmac` groups.

A mock proves routing and classification, not that GCS accepts the resulting signed wire request. Live
provider gates remain required before release.

### Azure tests {#azure-tests}

- live single-upload and block-upload response ETags;
- documented precondition status mapping;
- exact DELETE with correct and stale ETag;
- destination-conditional service-side copy;
- metadata/attribute round-trip;
- cache invalidation through `CachedConditionalOperations`;
- retention, soft-delete, snapshots, or versioning settings which prevent actual reclaim.

## Acceptance criteria {#acceptance-criteria}

1. All pre-refactor GCS request-isolation and live tests pass unchanged or with only ownership-focused
   test rewrites.
2. No non-CAS user-facing configuration or ordinary GCP/S3/Azure behavior changes.
3. CAS includes no provider SDK exception type and sets no provider-specific `WriteSettings` field.
4. CAS HEAD, write, delete, and copy use only `IObjectStorageConditionalOperations`.
5. Successful token-producing operations return the exact token from their own response; no HEAD
   fallback exists.
6. Token kind is explicit, validated, and stable for a mounted backend.
7. GCS generation translation is activated only by typed per-request state.
8. Unsupported multipart, copy, retry, versioning, or response-token shapes fail before a
   consequential fallback.
9. `CachedObjectStorage` and every applicable decorator preserve token freshness and cache
   invalidation, or explicitly reject the extension.
10. Adding Azure requires no provider branch in CAS and no change to `GCSConditionalDialect`.
11. Each migration commit builds and leaves the tree functionally coherent.

## Open decisions before implementation {#open-decisions-before-implementation}

1. Is Azure CAS support scheduled soon enough to justify a 700–1,200-line behavior-preserving
   refactor immediately after Task 9?
2. Should the extension be factory-owned as proposed, or a storage-owned non-owning view? The answer
   must make lifetime and configuration reload explicit.
3. Can production guarantee that CAS never receives `CachedObjectStorage`, or is
   `CachedConditionalOperations` required from day one?
4. Do Azure Blob and Azure Data Lake both need Native CAS support, and do their block-commit and copy
   responses provide exact ETags for every supported path?
5. Should `max_token_producing_write_bytes` remain a CAS setting passed to the extension, or become a
   provider capability with an operator-configured upper bound? It must not become a generic S3 knob
   in CAS again.
6. Which S3-compatible providers, beyond AWS and live-tested GCS, are supported versus merely allowed
   to pass the mutating capability probe?
7. Which observable exception codes, log events, and ProfileEvents must remain stable during the
   ownership move?

## Recommendation {#recommendation}

Do not redesign the nearly completed request-isolation work in place. Finish it and retain
`GCSConditionalDialect` as the GCS-only HTTP adapter.

If Azure is the next concrete backend, adopt Alternative C in a new spec and plan: introduce a typed
`IObjectStorageConditionalOperations` extension, implement `S3ConditionalOperations` for both AWS and
GCS, switch CAS without behavior change, then implement `AzureConditionalOperations`.

If Azure is not imminent, stop after Task 9 and choose Alternative A plus small local cleanup. The
current solution is correct enough to ship; the larger refactor earns its cost primarily by providing
a second provider implementation and preventing the third one from adding another provider-specific
CAS path.
