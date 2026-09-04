#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Common/ProfileEvents.h>

namespace DB::Cas
{

/// Per-namespace and per-operation instrumentation for the content-addressed storage seam.
///
/// Every content-addressed storage operation flows through the abstract `Backend` seam.
/// `InstrumentedBackend` is a transparent decorator: it owns an inner `BackendPtr`, delegates every
/// operation, and increments a `ProfileEvent` keyed by the key's namespace and the operation's
/// outcome. The pool wraps its backend once in `Pool::open`, which includes operations issued by
/// background writers and GC as well as foreground calls, for both object-storage and in-memory
/// backends. This backend-level chokepoint is needed because background PUTs are not attributable
/// through the foreground request that scheduled them.

/// Namespace of a CA key, classified by substring of the key path (6 classes; `Server` is currently
/// unreachable through this classifier — the per-server control subtree lives under
/// `/gc/server-roots/<server_root_id>/...` and classifies as Gc).
///   <prefix>/blobs/..        → Blob
///   <prefix>/cas/ns/..        → Root  (immutable streams and point/path-addressed namespace state)
///   <prefix>/cas/manifests/.. → Manifest
///   <prefix>/roots/..        → Root  (loose mountpoint objects)
///   <prefix>/gc/..           → Gc
///   else (e.g. _pool_meta, _probe) → Other
enum class CasNs : uint8_t
{
    Blob = 0,
    Manifest,
    Root,
    Gc,
    Server,
    Other,
};
static constexpr size_t CAS_NS_COUNT = 6;

/// Operation + outcome class, mapped from the `Backend` method and its result.
///
/// The decorator counts BOTH surfaces for the migration window. It must: it forwards a legacy call
/// to the inner backend AS a legacy call, so that a double wrapped by a `Pool` still intercepts it,
/// and the conversion to a primitive happens one level down, inside that double. Each request is
/// therefore counted exactly once, on whichever surface its caller used.
///   putIfAbsent                → Done ⇒ Put ; PreconditionFailed ⇒ PutDeduplicated
///   putOverwrite               → Done ⇒ Overwrite ; PreconditionFailed ⇒ CasConflict
///   casPut                     → Committed ⇒ Cas ; Conflict ⇒ CasConflict
///   write, no expected value   → a value ⇒ Put       ; RawConflict ⇒ PutDeduplicated
///   write, an expected value   → a value ⇒ Overwrite ; RawConflict ⇒ CasConflict
///   head, head(key)            → present ⇒ Head ; absent ⇒ HeadMiss (the 404 signal)
///   read, get                  → Read (all calls, hit or miss)
///   getStream                  → GetStream
///   remove, deleteExact        → Delete (all outcomes)
///   list                       → List
///   publish, publishBlob       → Put
///
/// `Cas` therefore counts only what a caller sent as a `casPut`: the primitive cannot tell a
/// compare-and-set from any other replacement, so a migrated replace counts as `Overwrite`. That
/// distinction, and the `CAS*CompareSwap`/`CAS*GetStream` events it feeds, go when the legacy
/// surface does.
enum class CasOp : uint8_t
{
    Put = 0,
    PutDeduplicated,
    Overwrite,
    Cas,
    CasConflict,
    Head,
    HeadMiss,
    Read,
    GetStream,
    Delete,
    List,
};
static constexpr size_t CAS_OP_COUNT = 11;

/// Classify a key into its namespace by substring. The order is significant where a more specific
/// layout such as `cas/ns/` must be recognized before a generic fallback; unknown key families
/// are intentionally counted as `Other`.
CasNs classifyCasNs(const String & key);

/// Increment the `ProfileEvent` corresponding to `(ns, op)`. The row-major table is defined in the
/// implementation and must remain aligned with the `CasNs` and `CasOp` enum values.
void incrementCasEvent(CasNs ns, CasOp op);

/// Transparent `Backend` decorator that records operation counts without changing the wrapped
/// backend's results, exceptions, or state transitions. The inner backend is owned by this object.
class InstrumentedBackend final : public Backend
{
public:
    /// Unhide the base overloads this class's own declarations would otherwise shadow: the
    /// convenience forms that omit Range/ObjectMeta/expected-token.
    using Backend::get;
    using Backend::getStream;
    using Backend::head;
    using Backend::list;
    using Backend::probeSentinelRaw;
    using Backend::putIfAbsent;
    using Backend::putOverwrite;
    using Backend::casPut;

    explicit InstrumentedBackend(BackendPtr inner_) : inner(std::move(inner_)) {}

    /// Capability checks are deliberately uninstrumented: they do not represent storage operations.
    void checkPoolPreconditions() override { inner->checkPoolPreconditions(); }
    void checkSkipAccessCheckSupport() override { inner->checkSkipAccessCheckSupport(); }
    void checkConditionalWriteSingleAttemptSupport() override { inner->checkConditionalWriteSingleAttemptSupport(); }

    /// The typed sentinel probe is a diagnostic/authoritative read, not a routine storage operation —
    /// deliberately uninstrumented (no ProfileEvent), like the capability checks above. MUST still be
    /// forwarded explicitly: `Backend::probeSentinelRaw`'s generic default derives its classification from
    /// THIS object's own `head`/`read` (virtual dispatch would otherwise resolve back to
    /// `InstrumentedBackend`'s plain, non-typed overrides above), silently discarding whatever sharper
    /// container/permission evidence the wrapped `inner` backend (e.g. `ObjectStorageBackend`'s S3/Local
    /// classification) is able to provide.
    SentinelProbeResult probeSentinelRaw(const String & key, TransportAccess & access) override
    {
        return inner->probeSentinelRaw(key, access);
    }
    SentinelProbeResult probeSentinelRaw(const String & key) override { return inner->probeSentinelRaw(key); }

    /// Delegate the read and count it after the inner call succeeds or returns absent. Exceptions
    /// propagate unchanged and therefore do not produce a separate outcome event.
    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto result = inner->read(key, access);
        incrementCasEvent(classifyCasNs(key), CasOp::Read);
        return result;
    }

    /// Count `Head` or `HeadMiss` from the returned presence after delegating to the backend.
    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        auto result = inner->head(key, access);
        incrementCasEvent(classifyCasNs(key), result ? CasOp::Head : CasOp::HeadMiss);
        return result;
    }

    /// Delegate one paginated listing and classify the prefix used for the request.
    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        auto page = inner->list(prefix, cursor, limit, access);
        incrementCasEvent(classifyCasNs(prefix), CasOp::List);
        return page;
    }

    /// Delegate the conditional removal and count every returned outcome as `Delete`.
    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        auto outcome = inner->remove(key, expected_value, access);
        incrementCasEvent(classifyCasNs(key), CasOp::Delete);
        return outcome;
    }

    /// Count a create and a replacement separately, and each of them separately from its refusal:
    /// they cost the same one request, but a pool whose creates are mostly refused and one whose
    /// replacements mostly conflict are different problems.
    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        auto result = inner->write(key, bytes, expected_value, access);
        const CasOp op = expected_value ? (result ? CasOp::Overwrite : CasOp::CasConflict)
                                        : (result ? CasOp::Put : CasOp::PutDeduplicated);
        incrementCasEvent(classifyCasNs(key), op);
        return result;
    }

    /// Delegate a forward-only read stream and count the request after the stream is acquired.
    std::unique_ptr<ReadBuffer> stream(const String & key, TransportAccess & access) override
    {
        auto result = inner->stream(key, access);
        incrementCasEvent(classifyCasNs(key), CasOp::GetStream);
        return result;
    }
    std::optional<GetStreamResult> getStream(const String & key, Range range) override
    {
        auto result = inner->getStream(key, range);
        incrementCasEvent(classifyCasNs(key), CasOp::GetStream);
        return result;
    }

    /// ---- The legacy surface, forwarded AS legacy ----
    ///
    /// Not inherited from `Backend`: its forwarder would call the primitive on THIS object, so the
    /// inner backend would receive a primitive and any legacy override it carries -- which is how
    /// almost every fault injection in the test suite is written -- would never run.
    std::optional<GetResult> get(const String & key, Range range) override
    {
        auto result = inner->get(key, range);
        incrementCasEvent(classifyCasNs(key), CasOp::Read);
        return result;
    }

    HeadResult head(const String & key) override
    {
        HeadResult result = inner->head(key);
        incrementCasEvent(classifyCasNs(key), result.exists ? CasOp::Head : CasOp::HeadMiss);
        return result;
    }

    PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta) override
    {
        PutResult result = inner->putIfAbsent(key, bytes, meta);
        incrementCasEvent(classifyCasNs(key), result.outcome == PutOutcome::Done ? CasOp::Put : CasOp::PutDeduplicated);
        return result;
    }

    PutResult putOverwrite(const String & key, const String & bytes, const Token & expected,
                           const ObjectMeta & meta) override
    {
        PutResult result = inner->putOverwrite(key, bytes, expected, meta);
        incrementCasEvent(classifyCasNs(key), result.outcome == PutOutcome::Done ? CasOp::Overwrite : CasOp::CasConflict);
        return result;
    }

    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                     const ObjectMeta & meta) override
    {
        CasResult result = inner->casPut(key, bytes, expected, meta);
        incrementCasEvent(classifyCasNs(key), result.outcome == CasOutcome::Committed ? CasOp::Cas : CasOp::CasConflict);
        return result;
    }

    DeleteOutcome deleteExact(const String & key, const Token & token) override
    {
        DeleteOutcome outcome = inner->deleteExact(key, token);
        incrementCasEvent(classifyCasNs(key), CasOp::Delete);
        return outcome;
    }

    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        ListPage page = inner->list(prefix, cursor, limit);
        incrementCasEvent(classifyCasNs(prefix), CasOp::List);
        return page;
    }

    void publishBlob(const BlobPublishRequest & request) override;

    /// Count one successful physical blob publication after delegating exactly once. The backend has
    /// no lifecycle reason to classify here; decision diagnostics remain with the writer.
    void publish(const BlobPublishRequest & request, TransportAccess & access) override;

    /// These are properties of the wrapped backend, not operations to count.
    Dialect dialect() const override { return inner->dialect(); }
    bool supportsListTokens() const override { return inner->supportsListTokens(); }
    uint64_t attemptTimeoutMs() const override { return inner->attemptTimeoutMs(); }
    bool refreshCredentials() override { return inner->refreshCredentials(); }

private:
    BackendPtr inner;
};

}
