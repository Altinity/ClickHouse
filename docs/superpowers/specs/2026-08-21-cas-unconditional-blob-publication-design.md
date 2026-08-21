---
description: 'Design for replacing CAS blob conditional creation with one provider-neutral HEAD-then-unconditional-publication protocol'
sidebar_label: 'CAS unconditional blob publication'
sidebar_position: 3
slug: /superpowers/specs/cas-unconditional-blob-publication-design
title: 'CAS unconditional blob publication design'
doc_type: 'design'
---

# CAS unconditional blob publication design {#cas-unconditional-blob-publication-design}

**Status:** DRAFT for review, rev.1 (2026-08-21). This specification defines a pre-release target
state. It deliberately does not require compatibility with older writer binaries or compatibility
aliases for settings that have not shipped.

## Decision {#decision}

Every CAS writer that must physically ensure a content-addressed blob uses one protocol, independent
of storage provider and source location:

1. make the build's precommit durable;
2. `HEAD` the blob;
3. adopt a present, non-condemned body;
4. otherwise publish the writer's own payload unconditionally under a fresh CAS envelope;
5. reconcile the per-blob freshness metadata to `Clean`;
6. record an explicit materialization proof and continue.

Concurrent writers may both publish the same content-addressed key after racing `HEAD` misses. This
is accepted. The key fixes the logical payload, durable references name the content hash rather than
an incarnation token, and the winning physical incarnation is immaterial. A fresh envelope protects
the winner from exact-token deletes already queued for a condemned predecessor.

The correctness protocol is uniform; byte transport is not. A re-readable local source uses an
ordinary streaming PUT and may use multipart. An object already held in S3 staging uses an ordinary
native same-store server-side copy. Both are unconditional implementations of the same
`Backend::publishBlob` contract.

Blob publication neither consumes nor produces an ETag/generation token. Tokens remain mandatory
where their values carry concurrency meaning: mutable CAS objects, freshness metadata, native-token
`HEAD`, GC condemnation, and exact-token delete.

This decision removes conditional creation only from the blob-body lane. It does not weaken or
replace conditional semantics for manifests, ref-log/control objects, leases, checkpoints, the
registry, GC state, freshness metadata, or exact-token deletion.

## Goals {#goals}

1. Remove the GCS single-PUT size cliff from ordinary blob creation and condemned-body replacement.
2. Give AWS-compatible storage, GCS, local storage, and a future Azure backend one blob correctness
   protocol.
3. Preserve content deduplication without provider-specific conditional-create branches.
4. Remove writer-side dependence on write-response ETags or GCS generations.
5. Reduce fork surface in generic object-storage and S3 code.
6. Keep non-CAS GCP behavior and user-facing configuration unchanged.
7. Make ambiguous writes, GC races, and fence loss explicit and model-checkable.
8. Keep large blob publication streaming and multipart-capable.
9. Measure and explicitly accept the S3 request-count and latency cost of the mandatory `HEAD`.

## Non-goals {#non-goals}

- Compatibility with older CAS writer binaries. CAS has not been released.
- A durable pool-format migration. The blob key, envelope, manifest, ref-log, and pool formats do not
  change.
- Removal of conditional operations from the mutable CAS lane.
- Design of Azure's version-token dialect.
- A new distributed presence cache or cross-writer request coalescer.
- Making every backend move bytes identically.
- Reworking the sparse `Clean`/`Condemned` metadata protocol beyond what publication requires.

## Context and problem statement {#context-and-problem-statement}

`PartWriteTxn::uploadBlobDetached` currently selects between several paths:

- a cache- or size-triggered `HEAD` followed by adoption;
- `Backend::putIfAbsentStream` for a conditional streaming create;
- `Backend::promoteStaged` for a conditional server-side copy;
- a post-`412` `HEAD` and adopt-or-resurrect branch;
- `Backend::resurrect` for an unconditional condemned-body replacement.

The conditional create is routed through
`CasRequestController::conditionalCreateControlled`,
`CasRefLedger::stagingConditionalCreate`, and `Pool::stagingConditionalCreate`. Its successful result
must return an incarnation token even though the writer later uses only the fact that a token is
present. `BlobDepRecord::token.has_value` distinguishes an uploaded/observed blob from
manifest-trusted evidence; the token value is not a promotion precondition.

That accidental result-token requirement is expensive on GCS. Generation-token writes are marked
`NativeConditional`, forced into a single PUT, and limited by
`gcs_max_token_producing_put_bytes`. GCS does not enforce preconditions on multipart completion, so
the current code fails before multipart rather than silently lose the condition. A blob above the
cap therefore cannot be created, and the same token-producing restriction currently reaches the
otherwise unconditional resurrect path.

The cap is not a blob-format limit. Raising it increases the amount buffered by each concurrent
single-part upload and merely moves the failure. Temporary upload plus conditional GCS Compose would
retain the conditional protocol, add a new temporary-object debris class, and enlarge the shared S3
fork surface.

The existing unconditional resurrect rationale already establishes the simpler invariant: all
writers of a content-addressed key own equivalent logical payloads; replacing one equivalent
incarnation with another is safe; and freshness comes from the envelope plus exact-token deletion,
not from write exclusivity. The same reasoning applies to fresh local uploads and S3-staging
promotion.

### Existing evidence and its limit {#existing-evidence-and-its-limit}

`CaIncarnationCore::WOverwrite` already models an unconditional same-content re-PUT by any actor and
requires safety not to depend on PUT conditions. The `EnableOverwrite = TRUE` configurations are
evidence for equivalent overwrite and fresh-incarnation safety.

They are not sufficient for this change. `CaIncarnationCore::WCreate` checks absence atomically, and
the existing configurations do not model two writers splitting observation from publication,
ambiguous late landing, the freshness-meta transition, or writer readiness without a token. A
focused model is required before release.

## Chosen architecture {#chosen-architecture}

### One state machine {#one-state-machine}

`PartWriteTxn::ensureBlobPresent` owns the complete blob lifecycle decision:

```text
durable precommit
        |
        v
    HEAD blob
      /     \
 present   absent
    |         |
 GET meta     +------------------+
  /    \                         |
Clean  Condemned                 |
 |        |                       |
adopt     +----> unconditional publish
                         |
                         v
                reconcile meta to Clean
                         |
                         v
                       Ready
```

The flow has these invariants:

- Every transaction-level publication decision starts from a current blob `HEAD`. Retries performed
  internally by the storage client remain part of that one transport attempt.
- Adoption of an existing body requires the build's precommit to be durable.
- The writer never reads a condemned destination body to recreate it. Publication always uses the
  writer's own re-readable source.
- A present body with absent metadata is semantically non-condemned, as today. It is adopted and the
  existing best-effort `putMetaIfAbsent(Clean)` backfill is attempted; the backfill does not select
  publication because absence already means `Clean`.
- A present `Condemned` body is never adopted. It is replaced by an equivalent body under a fresh
  envelope and the marker is reconciled to `Clean`.
- An absent body is published without first reading metadata. The existing `putMetaIfAbsent(Clean)`
  path handles the common case; a conflict then reads and reconciles a stale marker.
- No dependency is recorded until the body is observed safe or publication and metadata
  reconciliation have completed.
- `PartWriteTxn::adoptEvidence` remains an I/O-free proof backed by a durable source manifest. It is
  not a physical publication path and does not issue a `HEAD`.

### One transport API {#one-transport-api}

The backend exposes one blob-specific operation:

```cpp
struct StreamingBlobSource
{
    uint64_t payload_size;
    String fresh_envelope;
    std::function<std::unique_ptr<ReadBuffer>()> open_payload;
};

struct StagedBlobSource
{
    String object_key;
    uint64_t object_size;
};

using BlobPublishSource = std::variant<StreamingBlobSource, StagedBlobSource>;

struct BlobPublishRequest
{
    String destination_key;
    BlobPublishSource source;
};

virtual void publishBlob(const BlobPublishRequest & request) = 0;
```

The names may be adjusted during planning to match local conventions, but the semantic shape is
fixed:

- `publishBlob` is an unconditional rewrite;
- it does not issue a `HEAD` or read freshness metadata;
- it does not return an ETag, generation, or other incarnation token;
- it makes a complete object atomically visible or throws;
- a streaming source is `[fresh_envelope][payload]` and is eligible for ordinary multipart;
- a staged source is already a complete CAS object with a fresh envelope and is copied as-is;
- the backend verifies the source byte count before visibility or relies on a staging write that
  already performed that verification;
- no conditional-create, client-side re-upload, or unconditional-delete fallback is permitted.

`ObjectStorageBackend` implements the streaming variant through ordinary `writeObject` with
`WriteMode::Rewrite`, Default request mode, and the ordinary retry profile. It implements the staged
variant through native same-store `copyObject`. `InMemoryBackend` provides a deterministic test
implementation; local object storage must retain atomic final visibility without materializing an
unbounded number of concurrent bodies.

`PartWriteTxn` decides whether and why to publish. The backend decides only how bytes move. A future
Azure backend therefore supplies ordinary streaming/copy transport without adding another blob
state machine.

### Explicit writer proof {#explicit-writer-proof}

`BlobDepRecord` no longer encodes control state through an optional token and an `adopted` boolean.
It carries an explicit proof:

```cpp
enum class BlobDependencyProof
{
    Materialized,
    TrustedManifest,
};

struct BlobDepRecord
{
    ObjectKind kind;
    uint64_t size;
    BlobDependencyProof proof;
};
```

`Materialized` means a non-condemned current body was observed or a publication completed and its
metadata was reconciled. `TrustedManifest` is produced only by `adoptEvidence`. A pending upload is
not stored in `deps`; it becomes `Materialized` only on success. Promotion tests the proof enum
instead of `token.has_value` and `adopted`.

Tokens remain part of `HeadResult`, metadata CAS, GC catalogs, diagnostic events where available,
and exact-token deletion. Removing them from writer dependency state does not remove incarnation
identity from the storage protocol.

### Orthogonal diagnostics {#orthogonal-diagnostics}

The current `BlobUploadOutcome` combines decision and transport into cache-hit, head-hit,
head-miss-adopt, fresh local, staged promote, and two resurrect variants. The target diagnostics use
independent dimensions:

- action: `Observed` or `Published`;
- publication reason: `Absent` or `Condemned`;
- publication transport: `Streaming` or `ServerSideCopy`.

Only applicable dimensions are emitted. This avoids multiplying enum variants when a new backend or
transport is added.

## Detailed protocol {#detailed-protocol}

### Normal attempt {#normal-attempt}

For each unique blob in the existing blob-upload fan-out:

1. `requireAlive` verifies the transaction and mount.
2. The build's precommit must be `Durable` before an existing incarnation can be adopted or a new
   physical incarnation can become a dependency.
3. `Backend::head` reads the current body state and logical size.
4. If the body exists, `loadMeta` determines whether it is `Clean`/absent or `Condemned`.
5. A present non-condemned body with the expected logical size yields `Materialized` without moving
   bytes.
6. An absent or condemned body selects publication. The transaction captures the current fence
   generation at that decision.
7. Immediately before the durable write, `checkFenceOrThrow` confirms that the mount still owns the
   same fence generation.
8. `Backend::publishBlob` publishes the writer's source unconditionally.
9. `reconcileBlobMetaClean` establishes a compatible `Clean` marker with the declared size.
10. The method returns a `Materialized` dependency.

The bounded outer loop is owned by `PartWriteTxn`, not the transport backend. It may reuse the
existing request-budget and error-classification helpers, but it must not recreate a generic
conditional-create controller under a new name.

### Metadata reconciliation {#metadata-reconciliation}

The common absent-body path does not pay a metadata read before publication:

1. try `putMetaIfAbsent(Clean)`;
2. if it commits, finish;
3. on a real conflict or unresolved outcome, load the current marker;
4. an existing compatible `Clean` marker is success;
5. CAS a `Condemned` marker to `Clean` using the marker version just read;
6. reject a size disagreement as `CORRUPTED_DATA`;
7. repeat conflicts within the existing bounded metadata budget;
8. after budget exhaustion, surface the existing retry-later error class.

Failure to reconcile does not produce a `Materialized` dependency. A body that landed first is
safe debris and a subsequent attempt starts again from `HEAD`.

### Retry and ambiguity {#retry-and-ambiguity}

Deterministic local errors, invalid configuration, authorization errors, source corruption, and
unsupported transport propagate immediately. Only retryable or outcome-ambiguous transport errors
restart the bounded attempt.

Every such restart begins at `HEAD`; it never blindly reissues a state transition based on the old
observation. If the previous write landed, the new observation sees an equivalent body. If it did
not land, the body remains absent. If the body or its marker is condemned, the normal publication
path runs again.

An object-storage client may internally retry an unconditional PUT or copy. Repeating the same CAS
object is safe: the payload remains content-identical, and the envelope is fresh relative to the
condemned incarnation being displaced. GCS may mint multiple generations; no writer consumes their
values.

### Fence loss {#fence-loss}

- Fence loss before the pre-write check prevents publication.
- Fence loss after publication prevents the transaction from promoting. The body remains equivalent
  unreferenced debris and is reclaimed by the normal protocol.
- A lost response followed by fence loss does not authorize adoption or commit. The caller surfaces
  the retry-later/fenced result.

No fence failure falls back to an unguarded write or delete.

### Size and atomicity {#size-and-atomicity}

For a streaming source, byte counting happens while writing and before finalize. A mismatch cancels
the incomplete upload and publishes nothing. A post-write `HEAD` is not an acceptable substitute:
after finalize it could inspect a racing writer's incarnation and would detect a short write only
after it became current.

An S3-staged object is size-checked when the staging object is created. It already contains the
complete envelope and payload. Native same-store copy is the atomic publication point. Partial
multipart uploads and failed copies are invisible and remain transport-level debris handled by the
storage implementation.

## Request budget and performance contract {#request-budget-and-performance-contract}

The common request shapes are:

| State | Requests |
|---|---|
| Fresh absent | blob `HEAD` + blob PUT/copy + existing `Clean`-meta create |
| Existing `Clean` | blob `HEAD` + meta GET; no body write |
| Existing with absent meta | blob `HEAD` + meta GET + best-effort `Clean`-meta create; no body write |
| `Condemned` | blob `HEAD` + meta GET + blob PUT/copy + meta CAS |
| Metadata conflict | additional metadata GET/CAS only on the conflict path |

Compared with the current cold-cache path, a fresh small blob adds exactly one blob `HEAD` and one
serial network phase before upload. It does not add a common-path metadata GET. A cold duplicate
instead avoids the doomed conditional upload and its `412` path.

All blob `HEAD` requests for a part remain in the existing bounded fan-out. The design must not
serialize one blob's complete protocol before beginning the next blob.

The additional `HEAD` is an explicit architectural cost, not an unmeasured footnote. Before merge,
the implementation records before/after S3-compatible measurements for:

- a lone insert;
- the established wide-insert workload;
- total blob `HEAD`, body PUT/copy, and metadata request counts;
- the number and proportion of new small/cold external blobs;
- wall time and peak memory.

The report must demonstrate that the implementation adds no hidden metadata GET and preserves
fan-out. Merge requires explicit acceptance of the measured S3 price. A deterministic unit test or
a mock service cannot replace this performance decision.

## Concurrency argument {#concurrency-argument}

### Racing fresh writers {#racing-fresh-writers}

Two writers may both observe absence and publish. Their payloads are equal by the content-addressed
key. Their envelopes may differ, so the final ETag may differ; GCS generations differ regardless.
The last publication wins physically, while both writers hold a valid `Materialized` proof for the
same logical content.

### Racing adoption and publication {#racing-adoption-and-publication}

A writer may adopt a non-condemned body while another writer replaces it with an equivalent body.
No durable reference names the replaced token, and the payload identity is unchanged. The durable
precommit edge and freshness marker still prevent GC from treating the adopted logical blob as
unreferenced.

### Condemnation and exact deletion {#condemnation-and-exact-deletion}

GC writes `Condemned` before scheduling a body deletion. A writer that observes that marker must
publish from its own source. The fresh envelope changes an ETag-based incarnation; any GCS rewrite
mints a new generation. A queued `deleteExact` for the old token therefore misses the replacement.

The design never relies on an unconditional delete. It also never adopts a condemned body merely
because its payload hash is correct: that body may still be removed by an already authorized exact
delete.

### Late landing {#late-landing}

A timed-out PUT or copy may become visible after the caller has begun recovery. Late landing is
safe because it contains equivalent content, but it is not proof of success. Only a new `HEAD`, the
freshness-meta decision, and successful reconciliation can produce `Materialized`.

## S3 staging {#s3-staging}

`staging_backend=s3` remains opt-in. It selects where bytes are staged, not a distinct correctness
protocol.

The staged object is complete `[fresh envelope][payload]` content owned by the current transaction.
After the destination `HEAD` selects publication, `ObjectStorageBackend::publishBlob` uses ordinary
native same-store `copyObject`. There is no destination precondition and no destination token result.

This makes the path usable for generation-token GCS. The current generation-store exclusion and
conditional-copy capability probe are removed. The replacement capability is narrower and purely
transportal: explicit `staging_backend=s3` requires native same-store copy. If configuration or the
backend cannot provide it, writable mount fails closed rather than silently moving bytes through a
client-side read/write fallback. Default `staging_backend=local` is unchanged.

Ordinary GCS copy, multipart-sized copy, and staged-envelope preservation must pass a live GCS gate.
A fake service can prove request construction but not Google XML API behavior.

## GCS and conditional request isolation {#gcs-and-conditional-request-isolation}

The typed `NativeConditional` request mode introduced by the GCS request-isolation design remains
valid. This design narrows its callers:

- blob `HEAD` that needs a native generation continues to use it;
- mutable conditional PUT and CAS continue to use it;
- token-exact DELETE continues to use it;
- blob PUT and blob copy use Default mode;
- blob publication ignores write-response generation;
- LIST remains unmarked.

Consequently, a blob above the conditional single-PUT cap can use ordinary multipart on GCS. The cap
is retained only for writes that actually carry a condition and must preserve it through the one
request that publishes the object.

Non-CAS `gcp_oauth`, dedicated `gcs_hmac`, and ordinary S3-interoperability traffic remain on their
existing Default contracts. This design adds no non-CAS setting and no authentication-wide behavior.

## API and setting removals {#api-and-setting-removals}

The implementation removes the following blob-only surfaces after an exhaustive caller audit:

- `Backend::putIfAbsentStream`;
- `Backend::promoteStaged`;
- `Backend::resurrect`;
- `CasRequestController::conditionalCreateControlled` and its result types;
- `CasRefLedger::stagingConditionalCreate`;
- `Pool::stagingConditionalCreate`;
- `IObjectStorage::copyObjectConditional` and the S3/copy helper plumbing added solely for it;
- the conditional-copy mount probe and `conditional_copy_supported` state;
- `ObjectStorageBackend::tokenProducingWriteSettings`;
- the blob presence cache and its cache hit/miss events;
- `deduplication_cache_bytes`;
- `deduplication_head_first_min_bytes`.

`ObjectStorageBackend::conditionalWriteSettings` becomes self-contained and remains for genuine
conditional operations. `gcs_max_token_producing_put_bytes` is renamed to
`gcs_max_conditional_put_bytes` with no compatibility alias. The renamed setting does not apply to
blob publication.

If the caller audit finds a non-blob production consumer of a listed surface, implementation stops
and the design is revised. A compatibility wrapper with a misleading old contract is not an
acceptable shortcut.

## Compatibility {#compatibility}

### Durable data {#durable-data}

No durable encoding changes. Existing pre-release pools remain readable because blob keys,
envelopes, metadata records, manifests, ref logs, and pool metadata are unchanged. Existing `Clean`
and `Condemned` markers retain their meanings.

### Writers and settings {#writers-and-settings}

Old writer-binary coexistence is out of scope. No mixed-version protocol test is required. Removed
and renamed pre-release settings intentionally have no aliases.

### Non-CAS users {#non-cas-users}

No non-CAS user-facing setting, authentication selector, request header, ETag behavior, or cache-key
behavior changes. Removing fork-specific conditional-copy API must leave ordinary
`IObjectStorage::copyObject` behavior byte-for-byte unchanged.

## TLA+ release gate {#tla-release-gate}

### Focused model {#focused-model}

Add `docs/superpowers/models/CaBlobPublishCore.tla` with two writers and one GC. It models:

- durable and absent precommits;
- per-writer `HEAD`, meta-read, publishing, response-lost, meta-clean, ready, committed, and aborted
  phases;
- body presence, logical payload identity, incarnation token, and next token;
- metadata `Absent`, `Clean`, and `Condemned` plus a marker version;
- queued exact-token deletes;
- fence generation and fence loss;
- a publish that may land after its response is lost;
- streaming and staged sources as equivalent correctness inputs.

The model abstracts AWS, GCS, local storage, and future providers to the same publish action. Wire
semantics are tested live, not encoded as provider branches in TLA+.

### Invariants {#tla-invariants}

- A committed ref implies a present body with the content identity named by the ref.
- A writer reaches `Ready` only after durable precommit and a valid materialization proof.
- A condemned body is not adopted without a fresh publication.
- A delete authorized for a retired token cannot remove a different fresh incarnation.
- An unconditional writer cannot publish different logical content under the same modelled key.
- A fence-lost writer cannot commit.
- An unresolved absent publication cannot be reported as ready.

Liveness may be checked under bounded failures and weak fairness, but safety is the release blocker.

### Sabotage configurations {#tla-sabotage-configurations}

Each sabotage must fail for the named reason:

- adopt `Condemned` without publication;
- reuse the condemned incarnation instead of minting a fresh one;
- replace exact-token delete with unconditional delete;
- report ready after a lost response without a new observation;
- publish before durable precommit;
- skip metadata reconciliation;
- commit after fence loss;
- allow a source whose logical content does not match the key.

The runner must assert that TLC failed on the expected invariant rather than accepting any nonzero
exit.

### Existing-model audit {#existing-model-audit}

Audit every model, config, runner, and result document containing `WCreate`, `WOverwrite`, resurrect,
tokened writer dependencies, or token equality in `NoDangle`.

In particular, `CaGcCondemnMarkerGate::NoDangle` cannot remain a general invariant requiring the
current body token to equal a token adopted earlier. Equivalent replacement legitimately changes
that token. Update the invariant to logical presence/content identity or explicitly narrow and name
the older model's scope. Update `CaIncarnationCore` comments to point to the focused split-phase
model.

Changed models are rerun and their result documents regenerated. Historical results are not left
beside changed inputs as if they described the new state space.

## Test strategy {#test-strategy}

### Deterministic C++ tests {#deterministic-cpp-tests}

Cover at least these state-machine cases:

- fresh miss: `HEAD`, publish, and `Clean`, with no pre-publication metadata GET;
- existing `Clean`: `HEAD` plus metadata read and no body publish;
- existing body with absent metadata: adoption and the selected backfill behavior;
- absent body with a stale `Condemned` marker: publish and reconcile to `Clean`;
- present `Condemned`: fresh publication and old exact-delete miss;
- two writers racing after the same miss;
- ambiguous publication that landed and one that did not;
- fence loss before and after durable publication;
- streaming source-size mismatch publishes nothing;
- streaming and staged-copy sources produce the same dependency proof;
- `Materialized` and `TrustedManifest` replace every writer-side token-presence branch;
- `adoptEvidence` performs no blob I/O;
- blob publication never invokes a conditional-create API.

Fault fixtures must distinguish a write that did not land from a write that landed but lost its
response. Tests must assert request counts so a green path cannot hide an accidental metadata GET.

### Backend and HTTP tests {#backend-and-http-tests}

- AWS/ETag and GCS/generation blob publication above the conditional cap takes multipart.
- A successful blob publication with no response ETag/generation still succeeds.
- Ordinary staged copy carries no conditional request mode or destination precondition.
- Genuine mutable conditional PUT, native-token `HEAD`, and exact DELETE remain marked and translated.
- Default `gcp_oauth` and `gcs_hmac` requests keep the non-CAS request/response contract.
- Explicit `staging_backend=s3` refuses a backend without native same-store copy.
- Removed `copyObjectConditional` and conditional multipart-copy behavior have no surviving call site.

### Live storage gates {#live-storage-gates}

Run on an AWS-compatible service and real GCS:

- fresh streaming publication;
- duplicate adoption without a second body upload;
- concurrent equivalent publishers;
- a blob above the former GCS cap;
- multipart publication;
- native staged copy;
- condemned replacement followed by the queued old-token delete;
- ordinary non-CAS `HEAD`, GET, LIST, PUT, copy, single DELETE, and batch DELETE.

The GCS gate is mandatory for release. A deterministic fixture cannot establish that Google accepts
the ordinary multipart/copy wire shape.

### Existing gates {#existing-gates}

The full `CAS*` unit gate, relevant S3/WriteBuffer unit suites, `test_cas_gcs`, and ordinary S3
integration lane must pass. Every gtest command asserts a nonzero discovered-test count. Build and
test output follows the repository rule: redirect to unique build-directory logs and have a
subagent summarize each log.

## Documentation migration {#documentation-migration}

Documentation is part of the implementation change, not deferred cleanup.

### User and operator documentation {#user-and-operator-documentation}

Under `docs/en/antalya/cas`:

- rewrite `architecture/blob-protocol.md` around `HEAD` then unconditional publication;
- update `architecture/backend.md` for `publishBlob`, explicit dependency proof, and the reduced token
  role;
- update the blob flow in `architecture/part-lifecycle.md`;
- add the focused model and invariants to `architecture/correctness.md`;
- record the replaced conditional-create design in `architecture/design-history.md`;
- remove blob conditional-create requirements from `bucket-requirements.md` while preserving mutable
  CAS and exact-delete requirements;
- remove the two cache settings and rename the GCS cap in `configuration.md`;
- update `roadmap.md` and every other current page that describes write-once blob creation,
  resurrect, S3 staging, or a GCS blob single-PUT limit.

All new or changed headings retain explicit anchors as required by repository documentation rules.

### Internal CAS corpus {#internal-cas-corpus}

Under `docs/superpowers/cas` and `docs/superpowers/specs`:

- make this specification the current source for blob publication;
- add a supersession note to the GCS request-isolation design: typed conditional mode remains, but
  blob PUT/copy no longer use it;
- revise the object-storage conditional-operations proposal so it does not generalize removed blob
  conditional methods;
- update the CAS implementation `README` and consolidation coverage matrix;
- preserve historical reviews and reports as historical evidence, adding concise supersession notes
  where they are still navigation entry points rather than rewriting past facts.

### Backlog {#backlog}

- Close `[gcs-conditional-overwrite-rethink]` with links to this specification, implementation, live
  results, and performance report.
- Close or reformulate `[emulated-resurrect-should-spill-to-disk]`; there is no separate resurrect API
  in the target state.
- Update the performance entry for `HEAD-before-PUT` with the explicit 2026-08-21 user decision and
  measured before/after result.
- Close or remove cache-specific implementation findings and stale ProfileEvents work.
- Audit `formats-and-storage.md`, `performance.md`, `operability-and-introspection.md`,
  `ref-protocol.md`, `testing-and-ci.md`, and `docs-and-cleanup.md`.
- Preserve backlog identifiers and decision history rather than deleting them silently.

### Model documentation and code comments {#model-documentation-and-code-comments}

Update `docs/superpowers/models/README.md`, all affected model comments/configs/results, the CAS
implementation `README`, production comments, test fixture comments, event reasons, log messages,
and exception text.

The audit is semantic. It checks not only removed identifiers but also prose containing write-once
blob, conditional create, `412`, tokened leaf, resurrect, single PUT, conditional copy, `HEAD` first,
and dedup cache. “Resurrection” may remain as a lifecycle description for replacement of a condemned
incarnation, but not as a separate backend API or correctness branch.

### Negative-search gate {#negative-search-gate}

Run an exhaustive, untruncated search for:

```text
putIfAbsentStream
promoteStaged
conditionalCreateControlled
stagingConditionalCreate
copyObjectConditional
deduplication_cache_bytes
deduplication_head_first_min_bytes
gcs_max_token_producing_put_bytes
```

Every remaining hit must be classified as one of:

- historical text carrying an explicit supersession marker;
- a genuine non-blob conditional operation whose name remains accurate;
- a defect that blocks completion.

No unexplained hit is accepted. Exact-name search is followed by manual classification of the
semantic terms above so renamed stale prose cannot pass the gate.

## Implementation footprint and fork value {#implementation-footprint-and-fork-value}

The mechanical footprint is broad because the current interfaces appear in many test doubles and
documents. The production concept is narrow, and the final C++ diff should be net-negative.

Expected areas are:

- `CasPartWriteTxn` state machine and dependency representation;
- `Backend`, `InMemoryBackend`, `ObjectStorageBackend`, and `InstrumentedBackend`;
- request-controller/ref-ledger/pool removal of the blob conditional-create seam;
- S3-staging selection and mount checks;
- CAS settings, ProfileEvents, tests, models, and documentation;
- deletion of fork-specific conditional-copy API from `IObjectStorage`, `S3ObjectStorage`,
  `copyS3File`, and their tests.

The last deletion is the main upstream-maintenance win. CAS-specific `publishBlob` stays under the
content-addressed backend. Frequently changed generic object-storage code returns closer to
upstream, reducing future merge conflicts. The remaining typed GCS conditional machinery has a
smaller and more honest caller set.

## Risks and mitigations {#risks-and-mitigations}

| Risk | Mitigation |
|---|---|
| One additional S3 `HEAD` and RTT for a fresh small/cold blob | Request-count assertions, preserved fan-out, before/after lone and wide insert benchmarks, explicit acceptance before merge |
| Hidden writer-side consumer of the token value | Exhaustive caller audit, explicit proof enum, compile failures after token removal, focused promotion tests |
| GC race in the split `HEAD`/publish/meta flow | Focused two-writer TLA+ model, sabotage configurations, deterministic late-landing and exact-delete tests |
| GCS rejects ordinary multipart or copy wire behavior | Mandatory real-GCS live gate |
| Server-side copy silently falls back to client transfer | Explicit native-copy capability and fail-closed `staging_backend=s3` mount |
| Source length changes after hashing | Count before finalize; staged source verified when created |
| Local backend loses atomic visibility or accumulates unbounded memory | Backend contract test and large streaming test; no whole-body materialization in the production local object-storage path |
| Stale prose causes a later reintroduction of conditional blob logic | Documentation workstream and exhaustive negative-search gate |

## Rejected alternatives {#rejected-alternatives}

### Provider-specific create protocols {#provider-specific-create-protocols}

Keep conditional create on AWS and use `HEAD` plus unconditional PUT only on GCS. This saves the
extra fresh-object `HEAD` on AWS, but preserves two correctness state machines, conditional copy,
writer result-token plumbing, and a new policy decision for Azure. It is rejected in favor of one
protocol plus an explicit performance gate.

### Unconditional publication without HEAD {#unconditional-publication-without-head}

Always overwrite every referenced blob. This is logically safe for equivalent content but destroys
the bandwidth and request savings of deduplication. It is rejected.

### GCS temporary upload and conditional Compose {#gcs-temporary-upload-and-conditional-compose}

Upload to a temporary key and conditionally Compose onto the destination. This keeps the
conditional protocol, adds Compose-specific shared S3 changes, creates a temporary-key debris class,
and still needs provider-specific behavior. It is rejected.

### Presence cache skips HEAD {#presence-cache-skips-head}

The current cache is only a hint that a blob was recently observed. GC may have deleted it since.
Skipping `HEAD` on a cache hit can therefore create a dangling reference. Retaining the cache while
still issuing every `HEAD` has no decision or request-budget value. It is removed. A future
in-process request-coalescing design would be a separate measured optimization.

### Compatibility wrappers around the old API {#compatibility-wrappers-around-the-old-api}

Make `putIfAbsentStream`, `promoteStaged`, and `resurrect` call `publishBlob`. Their names and return
contracts would be false, and the old 412/token states would remain visible to callers. CAS is
pre-release, so this compatibility debt has no beneficiary. It is rejected.

### Generalize every CAS object under one immutable API {#generalize-every-cas-object-under-one-immutable-api}

Manifests and control objects genuinely depend on write-once or compare-and-swap semantics. Treating
them like content blobs would mix immutable identity with mutable coordination and enlarge the
change without solving the blob problem. It is rejected.

## Acceptance criteria {#acceptance-criteria}

The design is implemented only when all of the following hold:

1. Every transaction-level blob publication decision begins with `HEAD` and uses unconditional
   `publishBlob` only when the body is absent or condemned; internal retries of that transport
   attempt do not create another correctness decision.
2. `adoptEvidence` remains I/O-free.
3. Blob publication works above the former GCS cap through ordinary multipart.
4. S3 staging uses unconditional native same-store copy on AWS-compatible storage and GCS.
5. Writer dependency state contains no incarnation token and uses an explicit proof.
6. Mutable CAS conditional operations and exact-token deletion retain their token semantics.
7. The blob conditional-create controller, conditional-copy API, presence cache, and obsolete
   settings are removed with no unexplained call sites.
8. Fresh-miss request counting shows one added blob `HEAD` and no added common-path metadata GET.
9. The focused TLA+ safe model passes and every sabotage fails on its named invariant.
10. Existing affected TLA+ models and result documents are audited and made consistent.
11. Deterministic C++ gates, AWS-compatible integration, and real-GCS live gates pass.
12. Non-CAS GCP behavior remains on the Default request/response contract.
13. The S3 performance report is reviewed and the measured cost explicitly accepted.
14. Public docs, internal CAS docs, backlog, model docs, comments, logs, and exception text describe
    the target protocol.
15. The negative-search gate has no unexplained result.

## Short user-facing summary {#short-user-facing-summary}

CAS blobs are deduplicated by checking whether their content-addressed object already exists. When
it does not exist, ClickHouse uploads it normally; racing equivalent uploads are harmless. On GCS,
large blobs may use multipart and are no longer limited by the conditional single-PUT cap. Existing
non-CAS GCS and S3 behavior does not change.
