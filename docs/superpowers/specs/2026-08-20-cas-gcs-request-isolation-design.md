---
description: 'Design for isolating GCS generation preconditions to content-addressed storage requests'
sidebar_label: 'CAS GCS request isolation'
sidebar_position: 1
slug: /superpowers/specs/cas-gcs-request-isolation-design
title: 'CAS GCS request isolation design'
doc_type: 'design'
---

# CAS GCS request isolation design {#cas-gcs-request-isolation-design}

**Status:** DRAFT for review (2026-08-20).  This document specifies behavior and architecture only;
it does not authorize or describe an already-landed implementation.

## Decision {#decision}

GCS authentication and CAS generation-token semantics will be independent concerns.  An ordinary
S3/GCS client will retain its established request headers and ETag behavior regardless of
`http_client`, endpoint, credentials, or the presence of conditional headers.  CAS will opt its own
operations into a backend-native conditional-token request mode.  `S3ObjectStorage` will serve that
mode through lazily-created client clones, enabling GCS generation preconditions only on those
clones.

No user-facing setting will enable the GCS conditional dialect for an entire object storage.  CAS
selects it through its operation API; ordinary operations cannot inherit it from shared client
state.  Unsupported combinations fail at CAS mount or before sending a mutation rather than falling
back to ordinary ETag semantics.

## Context {#context}

The current branch connects GCS generation handling to authentication.  `ClientFactory::create`
enables `gcs_conditional_dialect` whenever `http_client` is `gcp_oauth`, and
`PocoHTTPClientGCPOAuth::makeRequestInternal` consequently applies the dialect to every request made
by that client.  The common response path replaces `ETag` with `x-goog-generation` whenever the
latter header exists.

This coupling affects existing, non-CAS GCS users:

- ordinary ETag responses can become numeric generation tokens;
- valid ETag-based `If-Match` and `If-None-Match` requests can be rejected or reinterpreted;
- every remaining `x-amz-*` request header is renamed to an assumed `x-goog-*` equivalent even
  though no universal one-to-one mapping exists;
- LIST results still carry body ETags while HEAD and write responses can carry generations through
  the ETag field, creating two token dialects on the same client.

Authentication does not imply that a caller wants generation-token semantics.  Conversely, CAS
needs a stable, backend-native conditional token on HEAD and successful writes even when a request
has no conditional header.  Conditional-header detection is therefore not a sufficient boundary.

## Goals {#goals}

- Preserve the pre-CAS wire behavior and ETag behavior of GCS in ClickHouse when CAS is not used.
- Support CAS over GCS with generation-match preconditions and generation-valued incarnation
  tokens.
- Make CAS semantics explicit at the operation boundary so future upstream conditional operations
  remain on the ordinary client by default.
- Keep the fork diff small and localized, reuse the existing S3 client clone mechanism, and avoid
  duplicating the S3 object-storage implementation.
- Preserve the existing CAS token representation and on-disk formats: GCS tokens remain
  `TokenType::Generation`, while AWS-compatible stores continue to use `TokenType::ETag`.
- Fail closed when the requested native conditional-token semantics cannot be enforced or when a
  successful mutation does not yield the exact token of the incarnation it created.

## Non-goals {#non-goals}

- This design does not make ordinary ClickHouse operations use GCS generations.
- This design does not change the semantics of `http_client=gcp_oauth` authentication or token
  refresh.
- This design does not redesign AWS conditional operations, CAS retry resolution, or the CAS object
  format.
- This design does not expose GCS generation mode as a storage-wide user setting.
- This design does not infer CAS intent from an endpoint, credential type, retry profile, or
  conditional header.
- This design does not add fallback from generation tokens to ETags on GCS.

## Required invariants {#required-invariants}

### Ordinary-client isolation {#ordinary-client-isolation}

For an operation whose request mode is `Default`:

- `http_client=gcp_oauth` adds OAuth authentication and performs no CAS transformation;
- request `If-Match` and `If-None-Match` headers retain their values and names;
- request `x-amz-*` headers retain their values and names unless an independently documented
  authentication requirement transforms an allowlisted header;
- response `ETag` is never replaced with `x-goog-generation`;
- the result is independent of whether a CAS metadata storage shares the same
  `S3ObjectStorage` instance.

These requirements apply to GET, HEAD, LIST, PUT, COPY, multipart, and future upstream operations.
They are the compatibility boundary that reduces the non-CAS GCS regression risk to zero relative
to the pre-CAS implementation.

### Explicit CAS selection {#explicit-cas-selection}

Only an operation explicitly issued with the backend-native conditional-token request mode may:

- translate CAS preconditions into GCS generation-match headers;
- interpret `x-goog-generation` as the operation's incarnation token;
- reject a token of the wrong dialect;
- enforce the GCS single-PUT restriction for conditional writes.

The request mode and retry profile are orthogonal.  In particular,
`ObjectStorageRetryProfile::SingleAttempt` is not evidence that an operation belongs to CAS, and a
future upstream user of that retry profile must not acquire GCS generation semantics.

### One token dialect per CAS operation {#one-token-dialect-per-cas-operation}

A GCS CAS operation receives and consumes generation tokens only.  An AWS-compatible CAS operation
receives and consumes its established ETag tokens.  A token obtained from a normal LIST response is
not promoted to a GCS generation: GCS CAS listings omit tokens and the caller obtains an exact token
through a CAS-owned metadata request when required.

### Exact successful-write result {#exact-successful-write-result}

After a successful GCS CAS mutation, the returned token must identify the incarnation created by
that response.  If GCS omits `x-goog-generation` from the successful response, the operation throws
an exception.  It must not issue a racy follow-up HEAD and claim that the observed later generation
was created by the original operation.

The existing non-GCS behavior may keep its established fallback where this design does not alter
its correctness contract.  The GCS generation path has no such fallback.

## Architecture {#architecture}

### Request mode {#request-mode}

Add a small backend-neutral request mode to `WriteSettings`:

```cpp
enum class ObjectStorageRequestMode : uint8_t
{
    Default,
    NativeConditional,
};
```

`WriteSettings::object_storage_request_mode` defaults to `Default`.  `NativeConditional` means that
the caller requires the object store's native token dialect suitable for its conditional-update
API.  It does not itself select a retry policy or name GCS.

CAS sets `NativeConditional` on every write whose returned token enters CAS state, including:

- create-if-absent writes;
- compare-and-set overwrites;
- streaming conditional writes;
- unconditional resurrection writes;
- staging promotion and other CAS-owned conditional copies.

CAS continues to set `ObjectStorageRetryProfile::SingleAttempt` only on conditional mutations whose
retry decision must remain above the AWS SDK.  An unconditional CAS mutation may use
`NativeConditional` with the ordinary retry policy.

### CAS metadata operation {#cas-metadata-operation}

Ordinary `IObjectStorage::getObjectMetadata` and `tryGetObjectMetadata` remain unchanged.  Add one
narrow virtual operation for callers that require a token suitable for a later native conditional
update:

```cpp
virtual ObjectMetadata getObjectMetadataForConditionalUpdate(
    const std::string & path,
    bool with_tags) const
{
    return getObjectMetadata(path, with_tags);
}
```

The default implementation preserves source compatibility for every non-S3 object storage.
`S3ObjectStorage` overrides it to select its native-conditional client.  CAS uses the throwing form
and classifies not-found errors through its existing backend-specific error policy.  This design
does not add a second `try` virtual.

`CasObjectStorageBackend::nativeHead` and the raw CAS sentinel HEAD use this operation.  Ordinary
metadata reads in ClickHouse continue to use the existing methods and therefore the base client.

### S3 client roles {#s3-client-roles}

One `S3ObjectStorage` logically owns the following clients:

| Role | Selected by | GCS generation dialect | Retry policy |
|---|---|---:|---|
| Base | `Default` operations | Disabled | Configured default |
| Native-conditional | CAS metadata and `NativeConditional` writes | Enabled only for supported GCS authentication | Configured default |
| Native-conditional single-attempt | CAS conditional mutations | Enabled only for supported GCS authentication | One attempt |

The two non-base clients are lazy clones of the current base `S3::Client`.  Their caches retain a
shared pointer to the base client whose configuration and credentials they cloned.  A base-client
identity change invalidates and rebuilds the corresponding clone, following the existing
`getSingleAttemptClient` ABA-safe pattern.  No operation mutates an already-published client
configuration.

On AWS-compatible stores the same routing is allowed, but the clone does not enable the GCS
dialect.  Existing AWS ETag behavior remains unchanged.

The implementation may share clone construction and cache bookkeeping between the two non-base
roles.  It must not collapse their retry policies or use a single-attempt client for ordinary CAS
HEAD requests.

### Conditional operation routing {#conditional-operation-routing}

`S3ObjectStorage::writeObject` selects a client from both independent settings:

```text
request mode Default
    -> base client, regardless of retry profile

request mode NativeConditional + default retry profile
    -> native-conditional client

request mode NativeConditional + SingleAttempt retry profile
    -> native-conditional single-attempt client
```

The branch-owned token-exact APIs `removeObjectIfTokenMatches` and `copyObjectConditional` select
the native-conditional single-attempt client directly because their contracts already require an
exact conditional token.  If these APIs are generalized for non-CAS callers in a future upstream
merge, their signatures must gain an explicit request mode rather than changing the meaning of the
default path.

Ordinary `readObject` and LIST operations use the base client.  CAS obtains its token through the
CAS metadata operation; it does not need response-header generation substitution on a body GET.

### GCS capability selection {#gcs-capability-selection}

`S3ObjectStorage::conditionalOpsUseGenerationTokens` describes the native-conditional path, not the
base client.  It returns true only when the provider and authentication mode can construct a GCS
native-conditional client.  It must not be implemented by reading whether the base client happens
to have a dialect flag set.

For this design, supported GCS authentication modes are `gcp_oauth` and `gcs_hmac`.  Other GCS/S3
compatibility modes retain their current ETag semantics and fail the CAS capability probe if they
cannot enforce exact conditional updates.

`CasObjectStorageBackend` continues to derive `TokenType::Generation` from this capability.  Thus
existing GCS CAS persistent tokens keep the same type and value space after the refactoring.

## GCS transport decomposition {#gcs-transport-decomposition}

The current request adapter combines token translation, authentication cleanup, and broad header
renaming.  Split those responsibilities.

### Generation preconditions {#generation-preconditions}

The generation adapter handles only CAS token semantics:

```text
If-None-Match: *       -> x-goog-if-generation-match: 0
If-Match: <generation> -> x-goog-if-generation-match: <generation>
```

It rejects a non-numeric CAS `If-Match`, a CAS `If-None-Match` other than `*`, and a conditional
multipart completion before sending the request.  These checks run only on the native-conditional
GCS clients.  The same headers on the base client keep standard ETag semantics and are never passed
to this adapter.

### Authentication preparation {#authentication-preparation}

OAuth preparation removes any stale `Authorization` header and installs the Bearer token.  It does
not rename application or SDK headers and does not enable generation semantics.

GOOG4 signing remains an authentication concern.  If it requires GCS-named extension headers, its
preparation step uses an explicit allowlist of documented mappings.  It must not implement a
blanket `x-amz-*` to `x-goog-*` rename.  Authentication preparation runs before GOOG4 signing so
the signer covers the final header names.

### Response generation extraction {#response-generation-extraction}

The common HTTP response code substitutes `x-goog-generation` into the SDK ETag field only when the
request was sent through a native-conditional GCS client.  Base OAuth and HMAC clients preserve the
server's ETag even when the response also includes `x-goog-generation`.

This substitution remains transport-local so the existing CAS plumbing can carry the generation
without widening AWS SDK result types or changing persistent CAS formats.

## CAS data flow {#cas-data-flow}

### Read and compare {#read-and-compare}

1. CAS calls `getObjectMetadataForConditionalUpdate`.
2. `S3ObjectStorage` selects the native-conditional client.
3. On GCS the response adapter places `x-goog-generation` in the SDK ETag result field.
4. CAS wraps it as `TokenType::Generation`.
5. A later CAS mutation places the token value in the existing `If-Match` plumbing and marks the
   request `NativeConditional`.
6. The GCS generation adapter emits `x-goog-if-generation-match`.

### Create if absent {#create-if-absent}

1. CAS sets `If-None-Match: *`, `NativeConditional`, and `SingleAttempt`.
2. The native-conditional single-attempt client translates the condition to generation match `0`.
3. A precondition loss remains an ordinary CAS conflict outcome.
4. A successful response must provide the created generation; CAS returns that generation as the
   new token.

### Unconditional CAS write {#unconditional-cas-write}

1. CAS marks the write `NativeConditional` but does not invent a conditional header.
2. The native-conditional client leaves the request unconditional.
3. The response adapter extracts the newly-created generation.
4. Missing generation is an exception, not a follow-up-HEAD fallback.

### Listing {#listing}

GCS LIST remains on the base client and continues to expose the XML body's ETag.  Because it is not
a generation, `supportsListTokens` remains false for a generation-token backend.  Callers that need
an exact token issue the CAS metadata operation.  No attempt is made to correlate LIST ETags with
generations.

## Error handling {#error-handling}

- Failure to construct or refresh a native-conditional client propagates as an exception.
- A CAS mount over a GCS mode that cannot enforce generation preconditions fails its capability
  checks; it does not mount using ordinary ETags.
- A token whose `TokenType` differs from the backend's native token type is rejected before a remote
  mutation.
- An invalid generation value is rejected before the request is sent.
- Conditional multipart completion is rejected before the request is sent.
- A successful GCS CAS mutation without a response generation is treated as an unverifiable result
  and raises an exception.
- Authentication, transport, and service errors continue through the existing typed error paths.
  Only a verified precondition failure maps to a normal CAS conflict outcome.

## Compatibility and upgrade behavior {#compatibility-and-upgrade-behavior}

### GCS without CAS {#gcs-without-cas}

Removing the authentication-based dialect selection restores the pre-CAS behavior.  No config
change is required.  Existing OAuth reads, writes, standard ETag conditions, custom metadata,
copies, and multipart operations use the base client.

The compatibility claim is structural rather than convention-based: new upstream code constructs
`WriteSettings` with request mode `Default` and calls ordinary metadata APIs, so it cannot acquire
CAS behavior unless it explicitly selects the new mode or API.

### Existing CAS over GCS {#existing-cas-over-gcs}

No object-format or token migration is required.  CAS continues to store and compare numeric GCS
generations as `TokenType::Generation`.  Existing CAS configuration using `gcp_oauth` or
`gcs_hmac` does not need a new storage-wide switch.

### Existing CAS over AWS-compatible storage {#existing-cas-over-aws-compatible-storage}

The new request mode selects the established conditional operation path but does not enable GCS
translation.  ETag token values, conditional headers, and persistent CAS formats remain unchanged.

## Testing strategy {#testing-strategy}

### Characterization tests before refactoring {#characterization-tests-before-refactoring}

Add tests that encode the required ordinary-client behavior independently of CAS:

- an OAuth base request preserves a non-numeric `If-Match`;
- an OAuth base request preserves `If-None-Match` values;
- arbitrary and metadata `x-amz-*` headers are not renamed by the OAuth client;
- a response containing both `ETag` and `x-goog-generation` returns the original ETag;
- GET, HEAD, LIST, PUT, COPY, and multipart requests do not acquire generation headers.

These tests must fail against the current unconditional `gcp_oauth` wiring and pass after the base
client is isolated.

### Adapter unit tests {#adapter-unit-tests}

Retain focused tests for:

- create-if-absent translation to generation `0`;
- quoted and unquoted numeric generation translation;
- wrong token dialect rejection;
- conditional multipart rejection;
- successful response generation extraction;
- no response override when generation is absent;
- authentication cleanup and any GOOG4 header allowlist independently of generation translation.

Remove tests whose expected behavior is blanket `x-amz-*` renaming.

### Client-selection isolation test {#client-selection-isolation-test}

Using one `S3ObjectStorage` and one captured HTTP endpoint, execute this sequence:

```text
ordinary HEAD
CAS metadata HEAD
ordinary conditional PUT with an ETag
CAS conditional PUT with a generation
ordinary HEAD
```

Assert that only the two CAS operations use generation semantics, that the first and final ordinary
HEAD return the server ETag, and that call order does not mutate the base client.  Repeat after a
base-client credentials/configuration rotation and assert that both lazy CAS clones are rebuilt from
the new base identity.

### CAS backend tests {#cas-backend-tests}

Verify both token dialects:

- AWS CAS metadata and successful writes return ETag tokens;
- GCS CAS metadata and successful writes return generation tokens;
- GCS LIST does not surface its body ETag as a CAS token;
- CAS sets `NativeConditional` on conditional and unconditional token-producing writes;
- conditional operations combine `NativeConditional` with `SingleAttempt`;
- an unconditional CAS write may combine `NativeConditional` with the default retry profile;
- a missing GCS response generation raises an exception without a follow-up HEAD;
- unsupported GCS authentication fails the CAS capability check.

### GCP integration coverage {#gcp-integration-coverage}

Extend the existing GCP OAuth integration test with captured request and response behavior for
ordinary HEAD, LIST, PUT, COPY, multipart, and ETag preconditions.  Add a CAS-over-GCP integration
case that covers metadata HEAD, create-if-absent, compare-and-set overwrite, exact delete, and a lost
precondition race.  The ordinary and CAS cases must share the same authentication mode so the test
proves that operation selection, not authentication, controls the dialect.

## Expected source footprint {#expected-source-footprint}

The implementation should remain within these responsibilities:

- `src/IO/WriteSettings.h`: the request-mode enum and defaulted field;
- `src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h`: one defaulted conditional-update
  metadata API;
- `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h` and
  `S3ObjectStorage.cpp`: native-conditional client selection, caching, and routing;
- `src/IO/S3/Client.h` and `Client.cpp`: clone capability inspection and removal of authentication-
  based dialect activation;
- `src/IO/S3/PocoHTTPClient.h` and `PocoHTTPClient.cpp`: separate authentication and optional
  generation handling;
- `src/IO/S3/GCSConditionalDialect.h` and `GCSConditionalDialect.cpp`: generation-only request and
  response adaptation;
- `CasObjectStorageBackend.h` and `CasObjectStorageBackend.cpp`: select the request mode and the
  conditional-update metadata API;
- focused S3, OAuth, and CAS tests.

Adding a second full object-storage implementation, copying S3 request methods into CAS, or adding
a storage-wide user setting is outside the accepted design.

## Merge and maintenance properties {#merge-and-maintenance-properties}

- New upstream operations default to the base client because the request-mode field defaults to
  `Default` and ordinary metadata APIs are unchanged.
- Authentication changes can evolve independently of CAS token semantics.
- CAS-specific fork conflicts concentrate in explicit client-selection sites instead of every S3
  request and response.
- The implementation reuses `cloneWithConfigurationOverride` and the existing ABA-safe clone cache
  pattern rather than changing shared client objects.
- The default virtual metadata implementation prevents mechanical edits to unrelated object-storage
  backends.
- There is no configuration or persistent-data compatibility layer to carry across future merges.

## Rejected alternatives {#rejected-alternatives}

### Storage-wide opt-in flag {#rejected-storage-wide-opt-in-flag}

A default-off `gcs_generation_preconditions` setting would protect non-CAS installations, but every
ordinary operation on an enabled CAS disk would still share changed ETag semantics.  Future upstream
conditional uses on that disk could regress.  The request-mode boundary is only slightly larger and
removes that class of problem structurally.

### Conditional-header detection {#rejected-conditional-header-detection}

Header detection cannot identify CAS-owned HEAD or unconditional token-producing writes.  It also
captures future non-CAS users of ordinary ETag preconditions.

### CAS-owned duplicate S3 client implementation {#rejected-cas-owned-duplicate-s3-client-implementation}

Issuing raw AWS SDK requests directly from `CasObjectStorageBackend` would isolate behavior, but it
would duplicate endpoint handling, credential rotation, logging, throttling, and error mapping.  It
would create a large and fragile fork surface.

### Global ETag-to-generation replacement with call-site discipline {#rejected-global-etag-to-generation-replacement}

Relying on current call sites not to consume ETags is not future-proof.  It also cannot prevent
user-visible `_etag`, cache, or metadata behavior from changing.  Isolation must occur before the
request reaches the shared HTTP client.

## Review acceptance criteria {#review-acceptance-criteria}

The design is acceptable only if reviewers can confirm all of the following:

- an ordinary `gcp_oauth` operation follows exactly the base path even when CAS shares the same
  object storage;
- no authentication mode implicitly enables generation semantics;
- every CAS operation that mints or consumes a token has an explicit native-conditional route;
- request mode and retry profile remain independent;
- the GCS adapter no longer performs blanket `x-amz-*` renaming;
- GCS CAS never accepts an ETag as a generation or returns a LIST ETag as a CAS token;
- successful GCS CAS mutations cannot report a token obtained from a racy follow-up HEAD;
- client rotation cannot leave a CAS clone using retired credentials or configuration;
- existing CAS persistent formats and configuration remain valid;
- tests prove isolation by alternating ordinary and CAS operations through one object storage.
