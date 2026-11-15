#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Common/Exception.h>
#include <base/defines.h>
#include <limits>
#include <variant>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int INVALID_STATE;
}
}

namespace DB::Cas
{

RecoveryGrounding chooseRecoveryGrounding(const std::optional<CatalogEntry> & catalog_state,
                                          const std::optional<RefCkpt> & ckpt)
{
    if (!catalog_state || catalog_state->state == NsState::Creating)
        throw Exception(ErrorCodes::INVALID_STATE, "CAS recovery grounding: namespace is absent or Creating");
    if (catalog_state->state != NsState::Live && catalog_state->state != NsState::Removing)
        throw Exception(ErrorCodes::INVALID_STATE, "CAS recovery grounding: namespace has an unsupported lifecycle state");
    if (!ckpt || !ckpt->life_epoch)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS recovery grounding: a Live or Removing namespace requires a readable _ckpt with life_epoch");

    checkRefCkptInvariants(*ckpt, "recovery grounding");
    RecoveryGrounding result;
    if (ckpt->generation < G_REF_CHAIN)
    {
        result.committed_through = ckpt->committed_through;
        if (!result.committed_through && (ckpt->checkpoint_snapshot_id || ckpt->last_epoch_seal))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS recovery grounding: a checkpoint without committed_through cannot name a snapshot or epoch seal");
        if (ckpt->checkpoint_snapshot_id && ckpt->last_epoch_seal
            && *ckpt->checkpoint_snapshot_id == *ckpt->last_epoch_seal)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS recovery grounding: checkpoint_snapshot_id must not name last_epoch_seal");
    }

    result.base = ckpt->checkpoint_snapshot_id;
    if (result.base)
    {
        if (result.base->ref_sequence == std::numeric_limits<uint64_t>::max())
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS recovery grounding: checkpoint base has no representable successor");
        result.walk_from = RefTxnId{result.base->writer_epoch, result.base->ref_sequence + 1};
    }
    else
    {
        result.walk_from = RefTxnId{*ckpt->life_epoch, 1};
    }
    return result;
}

std::optional<CkptSample> readCkpt(CasOperation & op, const Layout & layout, const NamespaceLifeId & life)
{
    const std::optional<Object> got = op.read(layout.refCkptKey(life), Retry::standard());
    if (!got)
        return std::nullopt;
    /// Materialized read, then decode: the object is MUTABLE, so the body must be fixed before it is
    /// parsed, and the incarnation must be the one that labels exactly these bytes.
    return CkptSample{decodeRefCkpt(got->bytes), got->etag};
}

namespace
{

String encodeGenerationThreeBody(std::optional<uint64_t> life_epoch, std::optional<RefTxnId> checkpoint_snapshot_id)
{
    return encodeRefCkptAtGeneration(RefCkpt{.life_epoch = life_epoch, .committed_through = std::nullopt,
                                             .checkpoint_snapshot_id = checkpoint_snapshot_id,
                                             .last_epoch_seal = std::nullopt}, G_REF_CHAIN);
}

[[noreturn]] void throwUnpublished(WriteResult && result, const NamespaceLifeId & life)
{
    orThrow(std::move(result), "CAS _ckpt for namespace '" + life.ns.string() + "'");
    throwCasWriteRetryLater("CAS _ckpt for namespace '" + life.ns.string() + "': the write did not commit");
}

}

CkptPublishOutcome createCkptAtCreation(CasOperation & op, const Layout & layout, const NamespaceLifeId & life,
                                        uint64_t life_epoch, const Retry & policy)
{
    WriteResult result = op.create(layout.refCkptKey(life), encodeGenerationThreeBody(life_epoch, std::nullopt), policy);
    if (std::holds_alternative<Committed>(result))
        return CkptPublishOutcome::Published;
    if (const auto * conflict = std::get_if<Conflict>(&result))
    {
        /// The verdict "already there" speaks only for an admitted writer, as in `publishCkptSnapshot`.
        if (!op.admitted())
            return CkptPublishOutcome::FencedOut;
        /// A refused create whose settling read found nothing is a deletion under the create, not a body.
        if (!std::holds_alternative<ProvenAbsent>(conflict->seen))
            return CkptPublishOutcome::IdenticalSkip;
    }
    if (const auto * gave_up = std::get_if<GaveUp>(&result); gave_up && gave_up->why == GaveUp::Why::FenceLost)
        return CkptPublishOutcome::FencedOut;
    throwUnpublished(std::move(result), life);
}

CkptPublishOutcome publishCkptSnapshot(CasOperation & op, const Layout & layout, const NamespaceLifeId & life,
                                       const RefTxnId & snapshot_id, const Retry & policy)
{
    std::optional<CkptPublishOutcome> decline;
    size_t decisions = 0;
    const auto decide = [&](const std::optional<Object> & current) -> std::optional<String>
    {
        ++decisions;
        decline.reset();
        if (!current)
        {
            decline = CkptPublishOutcome::NotAdvanced;
            return std::nullopt;
        }
        const RefCkpt durable = decodeRefCkpt(current->bytes);
        /// Below generation 3 the lane's stamp owns the conversion; an old body never names a new snapshot.
        if (durable.generation < G_REF_CHAIN
            || (durable.checkpoint_snapshot_id && snapshot_id < *durable.checkpoint_snapshot_id))
        {
            decline = CkptPublishOutcome::NotAdvanced;
            return std::nullopt;
        }
        if (durable.checkpoint_snapshot_id == snapshot_id)
        {
            decline = op.admitted() ? CkptPublishOutcome::IdenticalSkip : CkptPublishOutcome::FencedOut;
            return std::nullopt;
        }
        return encodeGenerationThreeBody(durable.life_epoch, snapshot_id);
    };

    WriteResult result = op.readModifyWrite(layout.refCkptKey(life), decide, policy);
    if (std::holds_alternative<Committed>(result))
        return CkptPublishOutcome::Published;
    if (std::holds_alternative<Declined>(result))
    {
        chassert(decline);
        /// An identical body after a refused attempt of this call: that attempt or a competitor made it durable.
        if (*decline == CkptPublishOutcome::IdenticalSkip && decisions > 1)
            return CkptPublishOutcome::Published;
        return *decline;
    }
    if (const auto * gave_up = std::get_if<GaveUp>(&result); gave_up && gave_up->why == GaveUp::Why::FenceLost)
        return CkptPublishOutcome::FencedOut;
    throwUnpublished(std::move(result), life);
}

CkptStampResult stampCkptGeneration3(CasOperation & op, const Layout & layout, const NamespaceLifeId & life,
                                     const CkptSample & accepted)
{
    RefCkpt stamped{.life_epoch = accepted.ckpt.life_epoch, .committed_through = std::nullopt,
                    .checkpoint_snapshot_id = accepted.ckpt.checkpoint_snapshot_id, .last_epoch_seal = std::nullopt};
    stamped.generation = G_REF_CHAIN;
    /// One attempt: a caller that meets anything but a commit re-derives from a fresh read.
    const WriteResult result = op.replace(layout.refCkptKey(life),
        encodeGenerationThreeBody(stamped.life_epoch, stamped.checkpoint_snapshot_id), accepted.etag, Retry::once());
    if (const auto * committed = std::get_if<Committed>(&result))
        return CkptStampResult{.outcome = CkptStampOutcome::Stamped,
                               .stamped = CkptSample{.ckpt = std::move(stamped), .etag = committed->etag}};
    if (std::holds_alternative<Conflict>(result))
        return CkptStampResult{.outcome = CkptStampOutcome::TokenMismatch, .stamped = std::nullopt};
    return CkptStampResult{.outcome = CkptStampOutcome::Unresolved, .stamped = std::nullopt};
}

MissingBaseVerdict classifyMissingSampledBase(const Etag & sampled, const std::optional<Etag> & current)
{
    if (current && !(*current == sampled))
        return MissingBaseVerdict::RestartRecovery;
    return MissingBaseVerdict::Corrupted;
}

bool snapshotDeletableUnderCkpt(const RefTxnId & snapshot_id, const std::optional<RefTxnId> & checkpoint_snapshot_id)
{
    return checkpoint_snapshot_id.has_value() && snapshot_id < *checkpoint_snapshot_id;
}

}
