# CAS GCS Generation-token binding — design {#gcs-generation-binding}

2026-07-03. Release-gate #1, GCS leg. Target level: **validation-grade** — enough to run the full
validation cycle (probe → replication → churn → two-phase reclaim → fsck → DROP-to-zero) on live
GCS; GCS ships marked experimental. Production-grade follow-ups are listed at the end.

## Measured ground truth (live bucket, 2026-07-03) {#measured-facts}

All probes: `utils/ca-soak/scripts/gcs_goog4_probe.py` + `gcs_goog4_mp_probe.py`.

| Fact | Consequence |
|---|---|
| GCS S3-compat (sigv4/`x-amz`) silently ignores `If-None-Match`, `If-Match` on PUT and DELETE (wrong-ETag DELETE deletes) | The AWS-style surface is unusable for CAS; the capability probe correctly refuses the mount today (fail-close, exit 48) |
| `x-goog-*` headers cannot be mixed into sigv4 requests (`ExcessHeaderValues`) | Native preconditions require non-`x-amz` auth: GOOG4-HMAC or OAuth Bearer |
| GOOG4-HMAC-SHA256 over the same XML endpoint with the same HMAC pair enforces the FULL battery (12/12) | The binding is a signing-dialect problem, not an API-migration problem |
| The incarnation token arrives in every PUT/HEAD response as `x-goog-generation` | No post-PUT HEAD needed (better than AWS ETag capture) |
| `CompleteMultipartUpload` **silently ignores** `x-goog-if-generation-match` (200 + overwrite) | Conditional writes must never take the multipart path on GCS |
| `Compose` **enforces** `x-goog-if-generation-match` (5/5) | Production-grade big-blob path exists: unconditional multipart to a temp key → conditional `Compose(temp → final)` → delete temp |
| The 412 XML body code is literally `PreconditionFailed` | Existing ClickHouse-side detection (`finalizeConditionalWrite`, `removeObjectIfTokenMatches`) needs no change |
| GOOG4 is structurally sigv4 with renamed constants (`GOOG4` key prefix, `goog4_request` scope, `x-goog-date`/`x-goog-content-sha256`) and supports `UNSIGNED-PAYLOAD` | The signer is ~150 lines and streaming-friendly (no body hashing) |

## Architecture {#architecture}

**A GCS conditional dialect — auth-agnostic, applied at the wire boundary — plus two auth modes
under it.** Everything above the HTTP layer keeps speaking AWS: `ETag` tokens,
`If-Match`/`If-None-Match` conditions. The dialect translates both directions at the last moment.

```
CasObjectStorageBackend / WriteBufferFromS3 / S3ObjectStorage      (UNCHANGED)
        | SDK request: x-amz-*, If-Match/If-None-Match, ETag plumbing
+-- PocoHTTPClient: GCS conditional dialect (new flag in client configuration) --+
| request:  strip AWS auth artifacts; rename x-amz-* -> x-goog-*;                |
|           If-None-Match: *        -> x-goog-if-generation-match: 0            |
|           If-Match: "<digits>"    -> x-goog-if-generation-match: <digits>     |
| auth:     (a) GOOG4-HMAC-SHA256, UNSIGNED-PAYLOAD   <- NEW signer             |
|           (b) Authorization: Bearer                 <- existing GCPOAuth      |
| response: ETag := "<x-goog-generation>"  (quoted, when the header is present) |
+--------------------------------------------------------------------------------+
        | storage.googleapis.com (XML API)
```

The symmetric response rewrite is the load-bearing trick: token capture from PUT responses,
`HeadResult.token`, `removeObjectIfTokenMatches` — the entire existing token plumbing works
unchanged because on GCS the "ETag" *is* the generation. The request-side mapping is its exact
inverse (strip the quotes, put the digits into `x-goog-if-generation-match`).

Follows the established in-tree pattern: `PocoHTTPClientGCPOAuth` already rewrites requests at
this exact layer (Bearer injection), and `ApiMode::GCS` already renames `x-amz-*` headers for
`CopyObject` and deletes `x-amz-api-version` in `Client::BuildHttpRequest`.

## Mode selection (explicit opt-in, no auto-detect) {#mode-selection}

Existing disk-config mechanism `http_client`:

- `<http_client>gcp_oauth</http_client>` — existing OAuth client; NOW also enables the
  conditional dialect (it already runs with `ApiMode::GCS`).
- `<http_client>gcs_hmac</http_client>` — NEW: `PocoHTTPClientGCSHMAC` (GOOG4 signer + dialect);
  also forces `ApiMode::GCS` on the `Client` regardless of credential presence.

No auto-switching on endpoint detection: today's plain "GCS as S3 with HMAC" disks keep their wire
behavior byte-for-byte. The CAS capability probe remains the gate: without the flag it keeps
failing closed exactly as it does now; with the flag it must pass 8/8.

## Components {#components}

### 1. GOOG4 signer — `src/IO/S3/GOOG4Signer.{h,cpp}` {#signer}

`void signGOOG4(Aws::Http::HttpRequest & request, const Aws::Auth::AWSCredentials & credentials,
std::chrono::system_clock::time_point now)` — canonical request over the FINAL header set,
`x-goog-content-sha256: UNSIGNED-PAYLOAD` (bodies stream without hashing), key chain
`HMAC("GOOG4"+secret, date) → "auto" → "storage" → "goog4_request"`, canonical query string
included (multipart/list requests carry query params). `now` is a parameter — unit tests use fixed
vectors (recorded from the live-validated python signer), no wall clock in tests.

### 2. `PocoHTTPClientGCSHMAC` — sibling of `PocoHTTPClientGCPOAuth` {#http-client}

Selected by `http_client = gcs_hmac` in `PocoHTTPClientFactory`. Its `makeRequestInternal`
override: apply the dialect (below), then `signGOOG4`, then delegate to the base implementation.
Credentials come from the same provider chain the AWS path uses (`use_environment_credentials`,
inline keys, …) — reused, not duplicated.

### 3. The conditional dialect — shared helper in `PocoHTTPClient.{h,cpp}` {#dialect}

One function pair, used by BOTH GCS modes (`gcs_hmac` calls it before signing; `gcp_oauth` calls
it before Bearer injection):

Request side (`applyGcsConditionalDialectToRequest`):
- Drop AWS auth artifacts: `Authorization`, `X-Amz-Date`, `x-amz-content-sha256`,
  `x-amz-security-token` (they must not survive into a GOOG4/Bearer request).
- Rename every remaining `x-amz-*` header to `x-goog-*` (generalizes the 3-header CopyObject
  rename; that special case stays as-is — renaming twice is a no-op).
- `If-None-Match: *` → `x-goog-if-generation-match: 0`. An `If-None-Match` with any value other
  than `*` throws `LOGICAL_ERROR` — nothing in the tree sends one, and GCS has no equivalent, so
  fail closed rather than silently change semantics.
- `If-Match: "<digits>"` (quotes optional) → `x-goog-if-generation-match: <digits>`. A non-numeric
  `If-Match` value throws `LOGICAL_ERROR`: it means an ETag-kind token leaked into a
  generation-dialect client (mixed-mode misconfiguration) — fail closed.
- GUARD (multipart-ignores-preconditions): a `CompleteMultipartUpload` (POST with `uploadId` query
  and no `partNumber`) that carries either conditional header throws `LOGICAL_ERROR`. GCS would
  silently accept-and-ignore it (measured); the guard turns a silent-data-loss path into an
  immediate exception.
- `amz-sdk-invocation-id`/`amz-sdk-request` (no `x-` prefix) are left as-is; the live probe run
  validates GCS tolerates them — if it does not, they get dropped here too.

Response side (`applyGcsConditionalDialectToResponse`):
- When `x-goog-generation` is present, set the response `ETag` header to the quoted generation
  (`"1783078552147137"`), overwriting the MD5-style ETag GCS returns. Quoting keeps the value
  round-trippable through the existing ETag plumbing; the request-side mapping strips the quotes.

### 4. `TokenType::Generation` stamping {#token-type}

`S3ObjectStorage` learns `IObjectStorage::conditionalTokenKind()` (default `ETag`; `Generation`
when its client runs a GCS conditional dialect). `CasObjectStorageBackend` stamps
`TokenType::Generation` instead of `TokenType::ETag` at its three stamping sites. The value is
opaque either way ("sent back exactly as observed"); the type feeds introspection/logs and the
mixed-mode fail-close above.

### 5. Conditional writes are single-PUT on GCS {#single-put}

Since multipart finalize cannot carry a precondition, `CasObjectStorageBackend`'s conditional
write paths (`nativeConditionalPut`, `putIfAbsentStream`) must never let a conditional write
take the multipart path when the token kind is `Generation`:
- The conditional-write `WriteSettings` force the single-part path with a raised cap
  (`s3_max_single_part_upload_size`-equivalent for this buffer) of
  `gcs_max_conditional_put_bytes`, new CA disk setting, default 1 GiB.
- A conditional write larger than the cap throws (`NOT_IMPLEMENTED`, message names the compose
  follow-up and the setting). Memory implication (single-part buffers the body) is documented in
  the setting's comment. GCS's own single-PUT limit is 5 TB — not the constraint; RAM is.
- Unconditional paths (plain reads/GETs, LIST, unconditional uploads if any) are untouched.

### 6. Probe extension: bucket-versioning fail-close {#probe-versioning}

In generation-dialect mode the capability probe adds one step, surfaced to the Core through a new
optional `Backend` hook (`Backend::checkStorePreconditions`, default no-op; `ObjectStorageBackend`
implements it for the generation dialect): `GET /?versioning` on the bucket.
`<Status>Enabled</Status>` ⇒ throw `NOT_IMPLEMENTED` with an explicit message: on a versioned
bucket a token-exact DELETE archives a noncurrent generation instead of reclaiming storage — GC
"deletes" would silently stop reclaiming. (Soft delete is invisible to the XML API and is a
billing-only concern — documented, not probed.)

The existing 8-step battery runs unchanged and must pass end-to-end (this is the first real
validation of the whole binding).

## Error handling {#error-handling}

- Generation-mismatch → HTTP 412 with body code `PreconditionFailed` — identical to AWS; the two
  existing detectors work unchanged (verified live). No new mapping code.
- Malformed conditional (empty `If-Match` equivalent) cannot be produced: the dialect validates
  digits-only and the probe cleanup is already HEAD-gated.
- All dialect fail-closes are `LOGICAL_ERROR`/`NOT_IMPLEMENTED` exceptions raised client-side
  BEFORE the request leaves the process — no reliance on the store rejecting what it demonstrably
  does not reject.

## Testing {#testing}

- Unit, signer: fixed-vector tests (timestamp injected) against signatures recorded from the
  live-validated python signer; canonical-query cases (multipart-style params).
- Unit, dialect: header-mapping table tests (star, quoted/unquoted digits, non-numeric If-Match
  throw, x-amz rename, auth-artifact strip, multipart-conditional guard, response ETag rewrite).
- Unit, backend: `TokenType::Generation` stamping and the single-PUT cap throw (InMemory backend
  with a stubbed token kind).
- Live (the actual gate): `docker-compose-gcs.yml` stand with `<http_client>gcs_hmac</http_client>`
  in `storage_conf_gcs_*.xml` — probe passes 8/8+versioning, then the full AWS-parity cycle:
  replication + checksums, churn + `OPTIMIZE FINAL`, two-phase reclaim observed against the bucket,
  fsck clean, `DROP TABLE` to metadata-only. Same acceptance bar as the AWS leg.
- OAuth leg: manual probe run with ADC credentials (`gcp_oauth` + dialect) — pass = supported,
  fail = documented as "HMAC only for now" without blocking the gate.

## Explicitly out of scope (follow-ups, ROADMAP rows) {#out-of-scope}

- Compose-based conditional finalize for blobs above the single-PUT cap (production-grade GCS).
- Azure leg of gate #1.
- JSON API / resumable uploads; per-request `x-goog-user-project` (requester-pays).
- Auto-detection of the dialect from the endpoint (stays explicit opt-in until GCS support
  graduates from experimental).
