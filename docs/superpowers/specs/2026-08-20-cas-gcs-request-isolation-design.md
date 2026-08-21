---
description: 'Design for isolating GCS generation preconditions to content-addressed storage requests'
sidebar_label: 'CAS GCS request isolation'
sidebar_position: 1
slug: /superpowers/specs/cas-gcs-request-isolation-design
title: 'CAS GCS request isolation design'
doc_type: 'design'
---

# CAS GCS request isolation design {#cas-gcs-request-isolation-design}

**Status:** DRAFT for review, rev.6 (2026-08-20). This specification defines the target state. The
current branch already contains the GCS conditional adapter and response generation override
(`41a247e3310`), plus their authentication-wide wiring and the `gcs_hmac` client
(`9604d6a5be9`); their current behavior and the required target-state changes are described below.

## Decision {#decision}

GCS authentication and CAS generation-token semantics are independent concerns. An ordinary
S3/GCS request retains the behavior established before the CAS changes. CAS sets only its own
token-producing and token-consuming requests to `NativeConditional`. The mode travels as a typed
C++ field on ClickHouse's AWS request wrapper and HTTP request object. The GCS HTTP clients apply
generation semantics only when that per-request field is true.

The typed field cannot become a header, enter a signature, or appear on the network. This keeps the
existing base client and single-attempt retry clone without adding a generation-specific client or
another clone cache. Credential, throttling, connection-pool, and OAuth-token behavior remain
exactly as they are for those two existing clients. No GCS generation client matrix or storage-wide
user setting is introduced.

No existing non-CAS user-facing configuration is renamed, removed, or made conditional on CAS.
This includes both the established GCS interoperability path using ordinary S3 HMAC credentials
and the existing `http_client` values.

Unsupported combinations fail at CAS mount or before sending a mutation. GCS CAS never falls back
from generations to ordinary ETags.

## Context {#context}

The current branch connects GCS generation handling to authentication. `ClientFactory::create`
forces `gcs_conditional_dialect` for both `gcp_oauth` and `gcs_hmac`.
`PocoHTTPClientGCPOAuth::makeRequestInternal` then applies the adapter to every OAuth request, while
the common response path substitutes `x-goog-generation` into the AWS SDK ETag field whenever the
header is present.

Three authentication paths must not be conflated:

- GCS HMAC authentication already exists in the merge base (`5e8eaeb4d7dd`) through the ordinary
  S3 client, `access_key_id` and `secret_access_key`, AWS SigV4, and GCS's S3 interoperability
  protocol. It is an established non-CAS path and must remain unchanged.
- `gcp_oauth` is an upstream ClickHouse feature with existing users. Changing all of its requests
  and responses is an upgrade regression.
- the explicit `http_client=gcs_hmac` selector, `PocoHTTPClientGCSHMAC`, and the GOOG4 signer were
  added in this fork by `8138a8313f2` and `9604d6a5be9`. The implementation is fork-specific, but
  the user-facing selector now exists and is not renamed by this design.

“Fork-specific GOOG4 implementation” therefore does not mean that GCS HMAC authentication itself
is new. It identifies only the dedicated GOOG4 path behind `http_client=gcs_hmac`.

### Authentication paths and user impact {#authentication-paths-and-user-impact}

HMAC credentials describe key material and a cryptographic primitive, not one unique wire
protocol. ClickHouse therefore has two HMAC paths with different compatibility contracts, plus the
OAuth path:

| User configuration | Wire authentication | Target non-CAS behavior | CAS generation behavior |
|---|---|---|---|
| Ordinary S3 client with `access_key_id` and `secret_access_key` | AWS SigV4 with `x-amz-*` through GCS S3 interoperability | Existing configuration, operations, errors, and ETag semantics remain unchanged | Does not opt into generations implicitly; select `http_client=gcs_hmac` when the GCS generation contract is required |
| `http_client=gcp_oauth` | OAuth Bearer authentication | Pre-CAS OAuth behavior and server ETags | CAS-owned `NativeConditional` requests use GCS generations |
| `http_client=gcs_hmac` | GOOG4-HMAC-SHA256 with `x-goog-*` | GOOG4 authentication with server ETags; the existing selector and credential fields remain unchanged | CAS-owned `NativeConditional` requests use GCS generations |

The ordinary HMAC path is retained because it is an established user-facing S3-compatible API and
does not need Google-native generation semantics. Migrating it automatically to GOOG4 would change
authentication headers, signing, and ETag behavior for non-CAS users without providing them a
benefit.

The dedicated GOOG4 path exists because CAS must translate its exact-write and exact-delete
conditions into Google generation headers and then authenticate the translated request. It applies
the adapter before GOOG4 signing, so the final `x-goog-*` request is signed consistently. Request
mode remains independent of authentication: `Default` means authentication plus ordinary ETag
semantics, while only `NativeConditional` adds generation semantics.

Consequently, existing non-CAS users do not migrate configuration. CAS over GCS with OAuth keeps
`gcp_oauth`; CAS over GCS with HMAC generations uses the already-exposed
`http_client=gcs_hmac`. No storage-wide CAS flag changes ordinary requests sharing either client.

The OAuth behavior is already a concrete bug, not merely a future risk. HEAD and GET response
headers can yield a numeric generation through the SDK ETag field, while LIST parses its ETag from
the XML response body and retains an ordinary ETag. ClickHouse exposes that value through the
`_etag` virtual column and uses it in filesystem-cache, page-cache, and Parquet-metadata-cache keys.
The same object can therefore acquire inconsistent user-visible values and duplicate cache entries
depending on whether its metadata originated from LIST or HEAD.

Authentication does not imply that a caller wants generation semantics. Conditional-header
detection is also insufficient: CAS needs the created generation from successful unconditional
writes and generation metadata from HEAD requests that carry no conditional header. The boundary
must be explicit per request.

The existing CAS model already treats GCS tokens as `TokenType::Generation`, omits LIST-derived
tokens on generation stores, forces conditional GCS writes into the single-PUT path, and uses the
single-attempt retry profile. This design preserves those persistent token formats and strengthens
the operation routing around them.

## Goals {#goals}

- Restore pre-CAS upstream behavior for every GCS operation that is not explicitly owned by CAS.
- Preserve the existing user-facing configuration and behavior of non-CAS GCS HMAC through the
  ordinary S3 interoperability path.
- Support CAS over GCS using generation-match preconditions and generation-valued incarnation
  tokens.
- Make isolation structural so future upstream conditional operations default to ordinary ETag
  behavior unless they explicitly opt into the new request mode.
- Do not add clients, OAuth token caches, or refresh timelines merely to carry a per-request dialect
  choice; retain only the existing retry-driven single-attempt clone.
- Keep fork changes small, explicit, and concentrated in existing request wrappers and CAS-owned
  call sites.
- Preserve existing CAS configuration and persistent token formats.
- Do not rename or remove a non-CAS authentication option.
- Fail closed when generation semantics, exact deletion, single-PUT limits, metadata round trips,
  or bucket-versioning safety cannot be verified.

## Non-goals {#non-goals}

- This design does not make ordinary ClickHouse operations consume GCS generations.
- This design does not change OAuth token acquisition or refresh behavior.
- This design does not rename `http_client=gcs_hmac` or require existing non-CAS GCS HMAC users to
  select it.
- The metadata-server OAuth expiry margin and token sharing between the base client and the existing
  single-attempt retry clone are separate pre-existing concerns and are not changed here; the
  selected design creates no additional token cache or refresh timeline.
- This design does not redesign AWS conditional operations, CAS request resolution, or CAS object
  formats.
- This design does not expose a storage-wide GCS generation setting.
- This design does not infer CAS intent from an endpoint, credentials, retry profile, or the mere
  presence of `If-Match` or `If-None-Match`.
- This design does not rely on thread-local state to propagate request mode.

## Required invariants {#required-invariants}

### Pre-CAS upstream compatibility {#pre-cas-upstream-compatibility}

For a request whose mode is `Default`, behavior must be identical to the upstream behavior before
the CAS/GCS commits for that `http_client`:

- the established GCS S3-interoperability path continues to use its existing access key, secret
  key, AWS SigV4, ordinary HTTP client, `x-amz-*` headers, and ETag semantics;
- `gcp_oauth` performs its established OAuth authentication and no CAS transformation;
- standard ETag preconditions retain their upstream meaning;
- response ETags are not replaced by generations;
- existing targeted `ApiMode::GCS` transformations remain intact, including CopyObject mappings
  for `x-amz-copy-source`, `x-amz-metadata-directive`, `x-amz-storage-class`, and
  `x-amz-meta-*`, plus removal of `x-amz-api-version`;
- ordinary GET, HEAD, LIST, PUT, COPY, multipart, metadata, `_etag`, and cache-key behavior does not
  depend on whether the same object storage also serves CAS.

The compatibility requirement is not “never rename a header.” It is “perform exactly the targeted
upstream transformations that existed before CAS, and no CAS-wide blanket transformation.”

### Dedicated GOOG4 `gcs_hmac` contract {#dedicated-goog4-gcs-hmac-contract}

The dedicated `http_client=gcs_hmac` selector and its GOOG4 implementation are fork-added; GCS HMAC
authentication through the ordinary S3 client is not. The selector remains a supported user-facing
name and is not renamed. Its `Default` request performs GOOG4 authentication only, preserves the
server ETag, and does not acquire generation preconditions. Existing targeted `ApiMode::GCS`
CopyObject mappings remain active.

Before GOOG4 signing, authentication preparation removes stale AWS authentication artifacts and
uses an explicit per-operation allowlist for headers needed by current ClickHouse requests. This
includes user-visible `x-amz-meta-*` and `x-amz-storage-class`, plus SDK-generated checksum and
stream-framing headers when the SDK emits them: `x-amz-sdk-checksum-algorithm`,
`x-amz-checksum-*`, `x-amz-trailer`, and `x-amz-decoded-content-length`. Every allowed header has a
documented disposition: translate to its GCS contract, preserve it because GCS accepts it, or
consume it before transmission. There is no generic prefix rename.

Copy source, metadata directive, storage class, and CopyObject metadata continue through the
existing targeted `ApiMode::GCS` mappings. Unsupported remaining `x-amz-*` extension headers cause
an exception instead of being guessed or sent in a mixed-prefix request. Customer-supplied
encryption headers are not mechanically renamed because GCS uses a different contract. ACL headers
are not included without an actual ClickHouse call site and a validated GCS mapping.

A live GCS characterization test covers `Default` HEAD, GET, LIST, PUT with metadata, CopyObject,
DELETE, batch `DeleteObjects`, multipart, and an upload that exercises any SDK checksum or chunked
framing selected by the production configuration before the blanket adapter is removed. This is a
release gate: unit tests can prove the allowlist, but cannot prove that GCS accepts the resulting
authenticated wire requests.

This characterization is separate from the compatibility test for the established ordinary-S3
HMAC path. Neither path may require a configuration migration for non-CAS users.

### Explicit per-request selection {#explicit-per-request-selection}

Only a request explicitly set to `NativeConditional` may:

- translate a CAS `If-Match` or `If-None-Match` into a GCS generation precondition;
- interpret `x-goog-generation` as the request's result token;
- translate CAS object attributes between the AWS SDK and GCS metadata header prefixes;
- apply GCS-specific single-PUT restrictions required to return an exact generation.

`ObjectStorageRetryProfile::SingleAttempt` remains orthogonal. A future upstream user of that retry
profile does not acquire CAS semantics. Conversely, an unconditional CAS write can request native
conditional-token semantics while retaining the ordinary retry profile.

### Typed request state {#typed-request-state}

The mode is represented only by C++ fields:

- the ClickHouse AWS request wrapper is the single source of truth;
- `Client::BuildHttpRequest` copies that value to ClickHouse's typed HTTP request object on every
  attempt;
- request and response adaptation read the field from that same HTTP request object;
- headers, `extra_headers`, signatures, and serialized bytes contain no representation of the mode.

The vendored AWS SDK creates the initial HTTP request before the retry loop and unconditionally
replaces it with a fresh request at the retry tail; a redirect changes the URI used for that
replacement. It calls `Client::BuildHttpRequest` at the start of every attempt. Persistence
therefore comes from deriving and copying the mode on every attempt, not from preserving state on
an earlier HTTP request object.

### One CAS token dialect per backend {#one-cas-token-dialect-per-backend}

GCS CAS operations mint and consume generations only. AWS-compatible CAS operations keep their
established ETag tokens. A token whose `TokenType` does not match the backend is rejected before a
mutation.

GCS LIST stays on the ordinary path and returns its XML-body ETag. Because that value is not a
generation, generation backends continue to report `supportsListTokens == false`. A caller that
needs an exact GCS token issues a CAS-owned metadata request.

### Exact delete routing {#exact-delete-routing}

Every GCS `removeObjectIfTokenMatches` request uses `NativeConditional` before it reaches the
client. Its numeric token must become `x-goog-if-generation-match`; it must never be sent as an
ordinary ETag-valued `If-Match`.

This invariant is safety- and liveness-critical. Missing the mode would send the numeric generation
as an ETag-valued `If-Match`. The design does not assume whether GCS rejects, compares, or ignores
that raw header on DELETE; the presence of `SetIfMatch` alone is not a safety proof.

The production mount capability battery exercises the same
`ObjectStorageBackend::deleteExact` to `S3ObjectStorage::removeObjectIfTokenMatches` path with a
known-wrong token and verifies that the object remains, then repeats with the known-correct token
and verifies deletion. Rejection, incorrect comparison, or silent ignoring of the raw header fails
one of those checks and rejects mount. Writable generation backends prohibit
`skip_access_check=true`, so this protection applies to every writable GCS CAS mount. Read-only
mounts may skip the battery because they expose no mutating surface and run no GC.

### Exact successful-write token {#exact-successful-write-token}

A successful GCS CAS mutation must return the generation created by that response. A missing
`x-goog-generation` is an exception. The code must not perform a follow-up HEAD and claim that a
later observed generation belongs to the original write.

All GCS CAS writes whose result token enters protocol state use a single PUT. They fail before
upload when their size exceeds `gcs_max_token_producing_put_bytes`. This includes unconditional
token-producing writes such as resurrection, not only writes carrying a precondition.

The [GCS XML multipart-completion documentation](https://docs.cloud.google.com/storage/docs/xml-api/post-object-complete)
does not guarantee an `x-goog-generation` response header. Multipart may be enabled in a future
change only after a live integration test proves that the completed generation is returned reliably
and the result can be associated with the completing request without a follow-up HEAD.

### Metadata round trip {#metadata-round-trip}

CAS object attributes must survive a GCS native-conditional write followed by a CAS metadata read:

- request `x-amz-meta-*` headers emitted by the AWS SDK become `x-goog-meta-*` on
  `NativeConditional` GCS
  requests;
- already-targeted `x-goog-meta-*` headers are left unchanged;
- response `x-goog-meta-*` headers become `x-amz-meta-*` before the AWS SDK parses the result;
- the returned `ObjectMetadata::attributes` map equals the attributes supplied by CAS.

These are targeted metadata-prefix mappings, not permission for a blanket `x-amz-*` rewrite.

### Verified bucket-versioning safety {#verified-bucket-versioning-safety}

Writable CAS over GCS requires a verified result showing that object versioning is disabled. An
enabled bucket or an unverifiable versioning probe causes mount failure. Proceeding on an assumption
would violate fail-closed behavior: exact deletion on a versioned bucket archives a noncurrent
generation instead of reclaiming storage.

Soft delete is an explicit operator precondition rather than a mount-time gate. The
[GCS policy documentation](https://docs.cloud.google.com/storage/docs/use-soft-delete) exposes its
inspection through the JSON API, while this backend and both supported authentication modes use the
XML API, so the current storage path cannot inspect it uniformly. Soft delete does not leave the
deleted generation live, but it delays physical reclamation until retention expires. Mount logs and
documentation require operators to disable soft delete for CAS pools; inability to verify it does
not masquerade as a successful automated check.

## Architecture {#architecture}

### Request mode in object-storage settings {#request-mode-in-object-storage-settings}

Add a backend-neutral request mode to `WriteSettings`:

```cpp
enum class ObjectStorageRequestMode : uint8_t
{
    Default,
    NativeConditional,
};
```

`WriteSettings::object_storage_request_mode` defaults to `Default`. `NativeConditional` means that
the caller requires the backend's native token dialect suitable for a later exact conditional
operation. It does not select a retry policy and does not name GCS.

CAS sets `NativeConditional` on:

- create-if-absent writes;
- compare-and-set overwrites;
- streaming conditional writes;
- unconditional resurrection writes;
- staging promotion and CAS-owned conditional copies;
- every other write whose returned token enters CAS protocol state.

### Request wrapper state {#request-wrapper-state}

Add a small non-template interface implemented by the fork's `ExtendedRequest` wrapper in
`src/IO/S3/Requests.h`:

```cpp
class RequestWithNativeConditionalMode
{
public:
    virtual ~RequestWithNativeConditionalMode() = default;
    virtual bool isNativeConditional() const = 0;
};
```

`ExtendedRequest` stores a mutable boolean and provides `setNativeConditional` plus the interface
getter. This boolean is the single source of truth. `WriteSettings`, the CAS metadata virtual, and
the direct DELETE setter are only transport mechanisms that set this same wrapper field on the
concrete AWS request; a future operation must do the same rather than introduce another mode
carrier.

### Typed HTTP request state {#typed-http-request-state}

Add `ExtendedHttpRequest`, derived from
`Aws::Http::Standard::StandardHttpRequest`, with a boolean `native_conditional` field and typed
getter/setter. Both `PocoHTTPClientFactory::CreateHttpRequest` overloads return this class instead
of constructing `StandardHttpRequest` directly.

`PocoHTTPClientFactory` is the process-global AWS HTTP factory installed by ClickHouse, and its
`CreateHttpRequest` is the construction point used by the classic AWS S3 client on every attempt.
Presigned URL construction does not transmit an HTTP request and is outside this path.

`Client::BuildHttpRequest` receives both the polymorphic `Aws::AmazonWebServiceRequest` wrapper and
the newly-created `Aws::Http::HttpRequest`. It detects `RequestWithNativeConditionalMode` and
`ExtendedHttpRequest` with `dynamic_cast`, then copies `true` only when both of these conditions
hold:

- the request reports `NativeConditional`;
- `http_client` is explicitly `gcp_oauth` or `gcs_hmac`;

There is no storage- or CAS-capability callback from the IO layer. Capability and mode propagation
derive from the same explicit configuration independently: storage uses it to decide whether CAS
may mount, while `Client` uses it to decide whether GCS translation applies to this request.

Provider inference from endpoint text is not part of either decision. Proxied and private GCS
endpoints remain supported because the explicit `http_client` value is the operator's declaration
of protocol intent.

AWS-compatible CAS requests may carry the wrapper boolean inside ClickHouse, but
`Client::BuildHttpRequest` leaves the HTTP-request boolean false for them. Their behavior remains
the existing ETag behavior.

### HTTP-layer consumption {#http-layer-consumption}

The OAuth and GOOG4 clients inspect `ExtendedHttpRequest::native_conditional` before authentication:

1. If false, they perform authentication only.
2. If true, they apply the native-conditional GCS request adapter.
3. OAuth installs the Bearer token after adaptation; GOOG4 signs the final GCS headers after
   adaptation.
4. The common response path reads the same typed field for generation and metadata mapping.

`PocoHTTPClient::makeRequestInternalImpl` uses `chassert` to verify that ClickHouse-created requests
are `ExtendedHttpRequest` during development and sanitizer testing. Its release-build fallback reads
a failed cast as false. This direction is fail-safe: an ordinary request keeps default semantics; a
CAS GCS request cannot obtain its required generation and its caller raises an exception. The first
implementation commit runs both CAS integration coverage and an ordinary S3 lane with this assertion
enabled to expose any unanticipated construction path.

The factory creates the initial `ExtendedHttpRequest` and the replacement used by every retry. A
redirect only changes the URI before retry construction. `Client::BuildHttpRequest` re-derives the
mode from the persistent AWS operation wrapper on every attempt. Interceptors mutate the current
HTTP object; body chunking wraps only its stream.

### CAS metadata API {#cas-metadata-api}

Ordinary `getObjectMetadata` and `tryGetObjectMetadata` remain unchanged. Add one narrow virtual
operation:

```cpp
virtual std::optional<ObjectMetadata> tryGetObjectMetadataWithNativeToken(
    const std::string & path,
    bool with_tags) const
{
    return tryGetObjectMetadata(path, with_tags);
}
```

The default implementation avoids changes to unrelated object-storage implementations.
`S3ObjectStorage` overrides the method and builds a `HeadObjectRequest` with
`NativeConditional`. `CasObjectStorageBackend::nativeHead` uses this method. A missing object remains
the normal `nullopt` outcome; unrelated authentication, container, network, and service errors
continue to propagate.

`probeSentinelRaw` keeps calling the throwing `getObjectMetadata`: it must tell no-such-key from
no-such-bucket from resource-not-found from a transient failure, and an optional return collapses all
of them into one `nullopt`. It also discards the metadata, so it consumes no token and gains nothing
from the mark.

The S3 implementation extends `getObjectInfoIfExists` or its immediate request-building helper with
an explicit request mode so the field is set on the actual `HeadObjectRequest`. It must not emulate
the new API by calling ordinary `tryGetObjectMetadata` and rewriting the result afterward.

### Capability selection {#capability-selection}

`S3ObjectStorage::conditionalOpsUseGenerationTokens` becomes a pure predicate over the explicitly
configured `http_client`:

- `gcp_oauth` and `gcs_hmac` support GCS generation tokens;
- the established GCS S3-interoperability HMAC path does not acquire generation semantics merely
  from its credentials or endpoint;
- other modes do not acquire generation semantics implicitly;
- endpoint substring matching and the base client's former dialect flag are not consulted.

CAS derives `TokenType::Generation` from this capability as before. A proxied GCS endpoint with an
explicit supported `http_client` therefore keeps working. Existing unrelated uses of
`Client::getProviderType` and `Client::getGCSOAuthToken` are not broadened by this design.

## GCS adapter responsibilities {#gcs-adapter-responsibilities}

### Generation preconditions {#generation-preconditions}

For a `NativeConditional` GCS request only:

```text
If-None-Match: *       -> x-goog-if-generation-match: 0
If-Match: <generation> -> x-goog-if-generation-match: <generation>
```

A non-numeric CAS `If-Match`, a CAS `If-None-Match` other than `*`, and conditional multipart
completion are rejected before network I/O. The same standard headers on a `Default` request retain
ordinary upstream ETag semantics.

### Request metadata {#request-metadata}

For a `NativeConditional` GCS request, the adapter converts only `x-amz-meta-*` extension headers to
`x-goog-meta-*`. Existing targeted `ApiMode::GCS` CopyObject mappings run independently and remain
unchanged. If both prefixes specify the same metadata key with different values, the request is
rejected instead of selecting one silently.

### Authentication preparation {#authentication-preparation}

Authentication cleanup is separate from generation semantics. OAuth removes stale authentication
artifacts as required and installs its Bearer token. GOOG4 preparation may translate additional
headers only through an explicit, documented allowlist needed by that authentication protocol. It
must not use a blanket `x-amz-*` to `x-goog-*` loop.

### Response adaptation {#response-adaptation}

For a `NativeConditional` response only:

- `x-goog-generation` is surfaced through the SDK ETag field;
- `x-goog-meta-*` is presented to the AWS SDK parser as `x-amz-meta-*`;
- conflicting duplicate metadata keys are rejected;
- absence of generation is permitted for non-token-producing `NativeConditional` requests, but the
  CAS caller that requires a newly-created token rejects a successful write result without one.

`Default` responses preserve the upstream ETag and response-header behavior even if GCS also sends
`x-goog-generation`.

## CAS operation flows {#cas-operation-flows}

### Metadata read and compare {#metadata-read-and-compare}

1. CAS calls `tryGetObjectMetadataWithNativeToken`.
2. S3 creates a `NativeConditional` `HeadObjectRequest`.
3. The GCS response adapter maps generation into the SDK ETag field and maps custom metadata into
   the prefix understood by the SDK.
4. CAS wraps the value as `TokenType::Generation` and retains the returned attributes.
5. A later mutation places the generation in the existing `If-Match` plumbing and marks its request
   `NativeConditional`.
6. The GCS adapter emits `x-goog-if-generation-match`.

### Create if absent {#create-if-absent}

1. CAS sets `NativeConditional`, `If-None-Match: *`, and `SingleAttempt`.
2. GCS translation emits generation match `0`.
3. `Expect: 100-continue` detection recognizes the translated
   `x-goog-if-generation-match` and retains its existing CAS body-upload optimization.
4. A verified precondition failure maps to a normal CAS conflict.
5. Success returns the generation from that PUT response.

### Exact delete {#exact-delete}

1. `removeObjectIfTokenMatches` constructs `DeleteObjectRequest` with `If-Match` equal to the CAS
   token value.
2. It explicitly sets `NativeConditional` on the request.
3. GCS translation replaces the ETag header with generation match.
4. A verified precondition failure maps to `TokenMismatch`; no other error is treated as a normal
   mismatch.

### Unconditional token-producing write {#unconditional-token-producing-write}

1. CAS sets the write to `NativeConditional` without adding a precondition.
2. On a generation backend, write settings force a single PUT and enforce
   `gcs_max_token_producing_put_bytes`.
3. The response supplies the new generation; absence is an exception.

The ordinary retry policy may reissue an unconditional PUT after an unexpected error. If an earlier
attempt actually committed, a retry can mint another generation and leave the first generation
retained by soft-delete or versioning policy. The final successful response still identifies the
current incarnation, so this is a possible storage leak rather than a false CAS success. Versioning
is prohibited by the mount check; operators should retain the existing recommendation to disable
soft delete for CAS pools. Changing unconditional retry policy is outside this design.

### Listing {#listing}

LIST always uses `Default`. It retains the upstream XML-body ETag and ordinary cache behavior. CAS
does not accept that value as a generation and re-HEADs an entry through the CAS metadata API when
an exact token is required.

## Write-settings decomposition {#write-settings-decomposition}

CAS builds token-producing write settings in two layers:

- native-token settings set `object_storage_request_mode = NativeConditional`; on generation
  backends they force single PUT and set the maximum permitted size;
- conditional-write settings extend native-token settings with the single-attempt retry profile,
  one WriteBuffer-level unexpected-error attempt, and the existing post-upload-check policy.

This ensures unconditional resurrection gets the same GCS single-PUT/token guarantees without
incorrectly inheriting the conditional write retry policy.

An object larger than the configured GCS token-producing cap is rejected before multipart upload
creation. The error identifies the object size and configured cap. No code path silently drops the
request mode or proceeds with multipart completion.

## Error handling {#error-handling}

- Unsupported GCS authentication modes fail the CAS capability check.
- A writable generation backend configured with `skip_access_check=true` fails before pool open.
- An unknown or enabled bucket-versioning state fails a writable GCS CAS mount.
- A wrong token type or invalid generation is rejected before mutation.
- A conditional multipart request is rejected before network I/O.
- Any GCS CAS token-producing write above the single-PUT cap is rejected before multipart creation.
- A successful token-producing response without generation is unverifiable and raises an exception.
- Conflicting AWS/GCS metadata-prefix values raise an exception.
- Authentication, transport, and service errors preserve their existing typed paths. Only a
  verified precondition failure becomes a normal CAS conflict or token mismatch.

## Compatibility and upgrade behavior {#compatibility-and-upgrade-behavior}

### GCS without CAS {#gcs-without-cas}

Removing authentication-wide dialect activation restores upstream behavior without a config
change. New upstream code defaults to `ObjectStorageRequestMode::Default`, and ordinary metadata
APIs do not mark requests. It therefore cannot acquire CAS semantics accidentally.

Existing non-CAS configurations keep the same public API: ordinary GCS HMAC continues through the
standard S3 client, `gcp_oauth` remains OAuth, and `http_client=gcs_hmac` remains the dedicated
GOOG4 selector. No option is renamed, no new option is required, and CAS request mode is not exposed
as a user setting.

The fix also restores one stable ETag domain for `_etag` and cache consumers: HEAD-derived metadata
no longer changes to generation while LIST retains an ETag.

### Existing CAS over GCS {#existing-cas-over-gcs}

CAS continues to use numeric generations and `TokenType::Generation`; no persistent format or
configuration migration is needed. Operations previously relying on the globally-mutated client
receive explicit `NativeConditional` request state instead.

Writable mounts that previously proceeded when bucket-versioning state was unknown now fail. This
is an intentional fail-closed tightening, not a compatibility promise: proceeding could make exact
deletion archive data instead of reclaiming it.

Writable generation backends also reject `skip_access_check=true`, because it would bypass the
production conditional-operation battery that validates the exact DELETE path. Read-only mounts
remain permitted to skip it because they cannot delete or run GC.

All token-producing GCS writes, including unconditional resurrection, are subject to
`gcs_max_token_producing_put_bytes`. This restriction is required until multipart completion is
proven to return an attributable generation. The old pre-release name
`gcs_max_conditional_put_bytes` is renamed because its scope is no longer limited to conditional
writes; no compatibility alias is retained.

### Existing CAS over AWS-compatible storage {#existing-cas-over-aws-compatible-storage}

CAS sets the request mode in its generic settings, but `Client::BuildHttpRequest` leaves
`ExtendedHttpRequest::native_conditional` false. ETag tokens, headers, retries, and persistent
formats remain unchanged.

## Testing strategy {#testing-strategy}

### Ordinary OAuth characterization {#ordinary-oauth-characterization}

Add regression tests proving that a `Default` `gcp_oauth` request preserves pre-CAS behavior:

- non-numeric `If-Match` and non-star `If-None-Match` remain standard ETag preconditions;
- arbitrary headers are not blanket-renamed;
- existing targeted `ApiMode::GCS` CopyObject and metadata mappings still occur;
- a response with both `ETag` and `x-goog-generation` returns the original ETag;
- ordinary GET, HEAD, LIST, PUT, COPY, and multipart requests retain `native_conditional=false` and
  contain no generation preconditions.

### Existing GCS HMAC compatibility {#existing-gcs-hmac-compatibility}

Add characterization coverage for the HMAC path present in merge base `5e8eaeb4d7dd`: ordinary S3
client selection, `access_key_id` and `secret_access_key`, AWS SigV4, and `x-amz-*` interoperability
headers. For non-CAS HEAD, GET, LIST, PUT with metadata, CopyObject, DELETE, and multipart, assert
that configuration parsing, authentication selection, request headers, response ETags, and errors
are unchanged. Include batch `DeleteObjects` and the SDK-generated checksum headers it requires. This
path must retain `native_conditional=false` and must not require `http_client=gcs_hmac`.

### Dedicated GOOG4 characterization {#dedicated-goog4-characterization}

Add a separate live-GCS characterization for `gcs_hmac` `Default` requests that proves the explicit
authentication allowlist supports HEAD, GET, LIST, PUT with metadata, CopyObject, DELETE, batch
`DeleteObjects`, multipart, and production checksum/chunked upload behavior. It preserves response
ETags and never applies generation preconditions. The existing `http_client=gcs_hmac`
configuration spelling and credential fields remain accepted unchanged.

Unit tests enumerate the disposition of every allowed SDK-generated `x-amz-*` header, including
checksum and stream-framing headers, and prove that an unknown extension still raises an exception.

### User-visible ETag and cache consistency {#user-visible-etag-and-cache-consistency}

For one object returned once through LIST metadata and once through ordinary HEAD metadata, assert:

- `_etag` has the same ordinary ETag value;
- filesystem-cache and page-cache key construction receives the same ETag;
- Parquet metadata cache constructs the same `(path, etag)` key;
- no numeric generation is exposed to these non-CAS consumers.

### Typed-mode isolation and retry derivation {#typed-mode-isolation-and-retry-derivation}

Capture wire requests while alternating through one shared client:

```text
ordinary HEAD
CAS metadata HEAD
ordinary conditional PUT with an ETag
CAS conditional PUT with a generation
ordinary HEAD
```

Assert that the factory creates `ExtendedHttpRequest` for every operation, only CAS operations have
its typed field set, and the final ordinary operation is unaffected by prior CAS traffic. Force an
ordinary retry and a redirect; assert that the factory creates the retry request in both cases and
that `Client::BuildHttpRequest` copies the mode again on every attempt from the AWS operation
wrapper.

Run the initial CAS integration and ordinary S3 lane with the `ExtendedHttpRequest` cast assertion
enabled. A dedicated unit test also supplies a foreign `HttpRequest` and verifies the release-path
interpretation is `Default`.

### Metadata round-trip test {#metadata-round-trip-test}

Send CAS attributes through a `NativeConditional` GCS PUT, return them as `x-goog-meta-*` on a
`NativeConditional` HEAD, and assert exact equality in `HeadResult::attributes`. Include an
already-targeted CopyObject request and a conflicting dual-prefix case.

### Exact-delete test {#exact-delete-test}

Capture `removeObjectIfTokenMatches` and prove:

- GCS sends `x-goog-if-generation-match` and no ETag-valued `If-Match`;
- matching generation removes the object;
- stale generation maps to `TokenMismatch`;
- a `Default` ordinary DELETE remains unchanged;
- the production capability battery uses this exact method, verifies both wrong-token preservation
  and correct-token deletion, and rejects a missing typed mode whether GCS rejects or ignores the
  resulting raw `If-Match`;
- writable generation mode rejects `skip_access_check=true`, while read-only mode remains allowed.

### Single-PUT and multipart tests {#single-put-and-multipart-tests}

For every GCS token-producing CAS write kind, including unconditional resurrection:

- a body at or below the cap uses PUT and returns its response generation;
- a body above the cap fails before CreateMultipartUpload;
- conditional CompleteMultipartUpload remains rejected as defense in depth;
- missing generation on a successful PUT raises an exception without follow-up HEAD.

A future multipart relaxation requires a live GCS test that captures the complete response and
proves an attributable generation. Unit tests may not manufacture the missing contract.

### Capability and mount tests {#capability-and-mount-tests}

- `gcp_oauth` and `gcs_hmac` select generation capability through explicit configuration even on a
  proxy endpoint with no `storage.googleapis.com` substring.
- Other clients do not select generation capability merely because the URL resembles GCS.
- Ordinary GCS S3-interoperability HMAC does not select generation capability from its endpoint or
  credentials and retains its existing ETag behavior.
- Enabled and unknown versioning states both reject writable GCS CAS mount; verified disabled state
  passes.
- Writable generation mode rejects `skip_access_check=true`; read-only generation mode may skip the
  mutating battery.
- AWS-compatible CAS remains on ETag semantics.

### OAuth client-count test {#oauth-client-count-test}

Assert that ordinary traffic uses the base OAuth client and that conditional CAS traffic reuses the
one existing single-attempt clone. Alternating request modes must not construct a third
generation-specific client or create further bearer-token caches. The base and single-attempt clone
may each fetch their own token under the current architecture; sharing those two token caches is not
an acceptance criterion for this change.

### Expect-continue test {#expect-continue-test}

Assert that a `NativeConditional` GCS PUT is translated before the existing
`Expect: 100-continue` gate inspects headers and that `x-goog-if-generation-match` still activates
the configured body-size threshold. `Default` conditional requests preserve upstream behavior.

## Expected source footprint {#expected-source-footprint}

The implementation should remain within these responsibilities:

- `src/IO/WriteSettings.h`: request-mode enum and defaulted field;
- `src/IO/S3/Requests.h`: shared request-mode interface and wrapper state;
- `src/IO/S3/PocoHTTPClientFactory.h` and `PocoHTTPClientFactory.cpp`: `ExtendedHttpRequest` and
  typed HTTP request construction;
- `src/IO/S3/Client.h` and `Client.cpp`: per-attempt typed-state propagation, explicit `http_client`
  capability, and removal of authentication-wide dialect activation;
- `src/IO/S3/PocoHTTPClient.h` and `PocoHTTPClient.cpp`: typed-mode inspection, response
  generation/metadata handling, the dedicated `gcs_hmac` authentication allowlist, and preservation
  of the existing ordinary-S3 HMAC and Expect paths;
- `src/IO/S3/GCSConditionalDialect.h` and `GCSConditionalDialect.cpp`: targeted generation and
  metadata mappings;
- `src/IO/S3/getObjectInfo.h` and `getObjectInfo.cpp`: `NativeConditional` CAS HEAD request
  construction;
- `src/IO/WriteBufferFromS3.h` and `WriteBufferFromS3.cpp`: propagation from `WriteSettings` to the
  actual PUT request and fail-closed single-PUT enforcement;
- `src/IO/S3/copyS3File.h` and `copyS3File.cpp`: request mode on CAS-owned conditional copies;
- `src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h`: defaulted native-token `try`
  metadata API;
- `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h` and `S3ObjectStorage.cpp`:
  conditional metadata override, exact DELETE marking, capability reporting, and settings routing;
- `CasObjectStorageBackend.h` and `CasObjectStorageBackend.cpp`: native-token settings, conditional
  settings, strict versioning gate, and CAS metadata API selection;
- `ContentAddressedSettings.cpp`, `ContentAddressedMetadataStorage.h`, and
  `ContentAddressedMetadataStorage.cpp`: rename the GCS token-producing PUT cap and reject skipped
  writable generation probes;
- the existing CAS configuration documentation: rename the pre-release cap and document the
  versioning/soft-delete preconditions;
- focused S3, OAuth, cache-key, and CAS tests.

No new S3 client cache, bearer-token provider abstraction, storage-wide setting, or duplicate S3
object-storage implementation belongs in this change.
No existing non-CAS `http_client`, credential field, or ordinary GCS HMAC configuration is renamed
or removed.

## Merge and maintenance properties {#merge-and-maintenance-properties}

- New upstream operations retain upstream behavior because `ObjectStorageRequestMode` defaults to
  `Default`.
- Existing GCS HMAC through the ordinary S3 client remains outside the GOOG4 and generation
  adapters.
- Authentication changes can evolve independently of CAS token semantics.
- Retry strategy remains client-level and continues to use the existing single-attempt clone;
  request dialect remains request-level.
- The mode is carried by typed request fields, following the request-wrapper pattern already used
  for API mode, URI override, region override, and checksum policy.
- The default virtual metadata method prevents mechanical changes to unrelated object-storage
  implementations.
- Capability derives from explicit configuration, so endpoint aliases and proxies do not expand the
  fork diff or create URL heuristics.
- Persistent formats and normal GCS configuration remain unchanged.

## Rejected alternatives {#rejected-alternatives}

### Client-per-mode matrix {#rejected-client-per-mode-matrix}

Separate base, native-conditional, and native-conditional single-attempt clients would turn a
per-request distinction into client state. The existing single-attempt clone already owns an
independent HTTP client and OAuth token cache; adding a native-conditional clone would create a
third one. The design would also require another ABA-safe clone cache, credential-rotation
rebuilding, routing tables, and indirect capability checks. Typed per-request state provides
stronger isolation with less code and keeps the one retry-driven clone that is genuinely required.

### Storage-wide opt-in flag {#rejected-storage-wide-opt-in-flag}

A default-off setting protects installations without CAS but changes every ordinary operation on an
enabled CAS disk. Future upstream ETag users on that disk could regress. Per-request selection
removes this class of failure structurally.

### Renaming `gcs_hmac` {#rejected-renaming-gcs-hmac}

A name such as `gcs_goog4` would distinguish the dedicated GOOG4 implementation from the
established ordinary-S3 HMAC path more explicitly. It is rejected because `http_client` is
user-facing configuration and `gcs_hmac` already exists on this branch. The specification explains
the distinction without requiring non-CAS users to rename or migrate configuration.

### Conditional-header detection {#rejected-conditional-header-detection}

Header detection misses CAS HEAD and unconditional token-producing writes. It also captures future
non-CAS ETag preconditions.

### Thread-local request scope {#rejected-thread-local-request-scope}

A scope similar to `Expect404ResponseScope` would avoid a request field but would not reliably
propagate into parallel upload workers. Request semantics must travel with the request object.

### Private HTTP marker header {#rejected-private-http-marker-header}

An internal header could transport the mode, but it would require reserved-name validation,
signature exclusions, serialization filters, spoofing defenses, and wire-level non-leakage tests.
`ExtendedHttpRequest` carries the same bit as typed process-local state, making those invariants
unnecessary by construction.

### CAS-owned duplicate S3 implementation {#rejected-cas-owned-duplicate-s3-implementation}

Issuing AWS SDK requests directly from CAS would duplicate endpoint handling, credential refresh,
logging, throttling, retry policy, and error mapping. It creates a larger and more fragile fork
surface than one request-mode field.

### Assuming multipart returns generation {#rejected-assuming-multipart-returns-generation}

The generic existence of `x-goog-generation` does not establish that CompleteMultipartUpload returns
it. Until a live test proves that contract, accepting multipart would either lose the exact result
token or require a racy follow-up HEAD.

## Review acceptance criteria {#review-acceptance-criteria}

The design is acceptable only if reviewers can confirm all of the following:

- the current landed behavior and the provenance of all three GCS authentication paths are stated
  accurately;
- the two HMAC paths are distinguished by their signing protocol and compatibility contract, not
  described as different kinds of credentials;
- existing ordinary-S3 GCS HMAC authentication is recognized as a merge-base feature and remains
  behaviorally and configurationally unchanged for non-CAS use;
- ordinary `gcp_oauth` traffic is identical to pre-CAS upstream behavior;
- `Default` `gcs_hmac` traffic has an explicit authentication-only contract and live
  characterization;
- the dedicated GOOG4 allowlist covers SDK-generated checksum and stream-framing headers, while
  unknown `x-amz-*` extensions still fail closed;
- live `Default` `gcs_hmac` coverage includes DELETE and batch `DeleteObjects`;
- no existing non-CAS authentication selector or credential field is renamed or removed;
- request mode, retry profile, and authentication are independent;
- every transmitted S3 request is an `ExtendedHttpRequest`, and `Client::BuildHttpRequest` derives
  its typed mode again on every retry and post-redirect attempt;
- capability uses explicit `http_client`, not endpoint substring matching or a base-client flag;
- CAS metadata absence remains a non-throwing `nullopt` outcome;
- targeted upstream `ApiMode::GCS` transformations remain intact;
- CAS attributes round-trip through GCS metadata prefixes;
- exact GCS DELETE always uses generation match;
- the production battery covers exact DELETE and cannot be skipped on a writable generation mount;
- the existing Expect gate recognizes translated generation preconditions;
- every GCS token-producing write is single-PUT and fails above the cap;
- missing write generation never triggers a follow-up HEAD;
- enabled or unknown bucket versioning rejects writable GCS CAS mount;
- the soft-delete limitation and operator precondition are explicit;
- no generation-specific client or OAuth token cache is added beyond the existing base and
  single-attempt clients;
- existing CAS token formats and normal GCS configuration require no migration;
- tests assert `_etag` and cache-key consistency, not only raw response headers.
