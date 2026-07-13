#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Common/ProfileEvents.h>

namespace DB::Cas
{

/// B168 P0: per-namespace S3 op instrumentation.
///
/// Every CA S3 op flows through the abstract `Backend` seam. `InstrumentedBackend` is a transparent
/// decorator that wraps an inner `BackendPtr`, delegates every method faithfully, and increments a
/// ProfileEvent keyed by the key's NAMESPACE × the OPERATION+OUTCOME. Wrapping the pool backend once
/// in `Store::open` makes writer AND GC ops attributable, for both the S3 and in-memory backends.
///
/// Motivation: `part_log` shows `put=0` because PUTs ride a background threadpool, so we need a
/// backend-level chokepoint to attribute the S3 op-count (PUTs/HEADs/404s/412s/LISTs) by CA op type.

/// Namespace of a CA key, classified by substring of the key path (6 classes).
///   <prefix>/blobs/..        → Blob
///   <prefix>/cas/manifests/.. → Manifest
///   <prefix>/roots/<server-hex>/_watermark     → Server  (checked before the generic /roots/)
///   <prefix>/roots/..        → Root  (incl. /roots/<ns>/_files/ and mountpoint objects)
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

/// Operation + outcome class (10 classes), mapped from the Backend method and its return value.
///   putIfAbsent / putIfAbsentStream finalize → Done ⇒ Put ; PreconditionFailed ⇒ PutDedup
///   putOverwrite                              → Done ⇒ Overwrite ; PreconditionFailed ⇒ CasConflict
///   casPut                                    → Committed ⇒ Cas ; Conflict ⇒ CasConflict
///   head                                      → exists ⇒ Head ; !exists ⇒ HeadMiss (the 404 signal)
///   get                                       → Get (all calls, hit or miss)
///   getStream                                 → GetStream (all calls, hit or miss)
///   deleteExact                               → Delete (all outcomes)
///   list                                      → List
enum class CasOp : uint8_t
{
    Put = 0,
    PutDedup,
    Overwrite,
    Cas,
    CasConflict,
    Head,
    HeadMiss,
    Get,
    GetStream,
    Delete,
    List,
};
static constexpr size_t CAS_OP_COUNT = 11;

/// Classify a key into its namespace by substring. See CasNs.
CasNs classifyCasNs(const String & key);

/// Increment the ProfileEvent for (ns, op). The table lives in the .cpp.
void incrementCasEvent(CasNs ns, CasOp op);

class InstrumentedBackend final : public Backend
{
public:
    explicit InstrumentedBackend(BackendPtr inner_) : inner(std::move(inner_)) {}

    void checkStorePreconditions() override { inner->checkStorePreconditions(); }
    void checkConditionalWriteSingleAttemptSupport() override { inner->checkConditionalWriteSingleAttemptSupport(); }

    std::optional<GetResult> get(const String & key, Range range = {}) override
    {
        auto result = inner->get(key, range);
        incrementCasEvent(classifyCasNs(key), CasOp::Get);
        return result;
    }

    std::optional<GetStreamResult> getStream(const String & key, Range range = {}) override
    {
        auto result = inner->getStream(key, range);
        incrementCasEvent(classifyCasNs(key), CasOp::GetStream);
        return result;
    }

    HeadResult head(const String & key) override
    {
        HeadResult result = inner->head(key);
        incrementCasEvent(classifyCasNs(key), result.exists ? CasOp::Head : CasOp::HeadMiss);
        return result;
    }

    PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta = {}) override
    {
        PutResult result = inner->putIfAbsent(key, bytes, meta);
        incrementCasEvent(classifyCasNs(key), result.outcome == PutOutcome::Done ? CasOp::Put : CasOp::PutDedup);
        return result;
    }

    WriteSinkPtr putIfAbsentStream(const String & key, const ObjectMeta & meta = {}) override;

    PutResult putOverwrite(const String & key, const String & bytes, const Token & expected,
                           const ObjectMeta & meta = {}) override
    {
        PutResult result = inner->putOverwrite(key, bytes, expected, meta);
        incrementCasEvent(classifyCasNs(key), result.outcome == PutOutcome::Done ? CasOp::Overwrite : CasOp::CasConflict);
        return result;
    }

    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                     const ObjectMeta & meta = {}) override
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

    bool supportsListTokens() const override { return inner->supportsListTokens(); }

    PutResult promoteStaged(const String & staging_key, const String & blob_key) override
    {
        PutResult result = inner->promoteStaged(staging_key, blob_key);
        /// A write-once server-side copy is a create attempt on the BLOB key: Done ⇒ Put, 412 ⇒ PutDedup.
        incrementCasEvent(classifyCasNs(blob_key), result.outcome == PutOutcome::Done ? CasOp::Put : CasOp::PutDedup);
        return result;
    }

    Token resurrectStaged(const String & staging_key, const String & blob_key,
                          const String & fresh_header, uint64_t staging_payload_offset) override
    {
        Token token = inner->resurrectStaged(staging_key, blob_key, fresh_header, staging_payload_offset);
        /// An unconditional resurrect re-upload overwrites the (condemned) BLOB key.
        incrementCasEvent(classifyCasNs(blob_key), CasOp::Overwrite);
        return token;
    }

private:
    BackendPtr inner;
};

}
