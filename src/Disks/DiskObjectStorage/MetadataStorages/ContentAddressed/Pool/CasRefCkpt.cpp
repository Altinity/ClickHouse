#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h>
#include <Common/Exception.h>
#include <algorithm>

namespace DB::Cas
{

namespace
{

/// Live-lock brake, the same shape and for the same reason as `CasPlainObjects`': the deadline is the
/// real bound, and this only stops an unexpected continuous conflict from spinning until it elapses.
constexpr size_t MAX_CKPT_CAS_ATTEMPTS = 100;

/// The per-field semantic maximum for an OPTIONAL field: a present value beats an absent one (an
/// absence is "this writer knew nothing", never "this writer says none"), and two present values
/// resolve by the field's own order -- for `RefTxnId` that is writer_epoch then ref_sequence, the
/// intended timeline even across an epoch restart that resets the sequence.
template <typename T>
std::optional<T> maxKnown(const std::optional<T> & a, const std::optional<T> & b)
{
    if (!a)
        return b;
    if (!b)
        return a;
    return std::max(*a, *b);
}

}

RefCkpt mergeCkpt(const RefCkpt & a, const RefCkpt & b)
{
    RefCkpt merged;
    /// `life_epoch` is a namespace-lifetime constant, so in the steady state one side knows it and the
    /// other does not, and the max keeps the one that does. It is still merged like every other field:
    /// taking it from either side by name is how a writer that knows nothing about it would erase it.
    merged.life_epoch = maxKnown(a.life_epoch, b.life_epoch);
    merged.checkpoint_snapshot_id = maxKnown(a.checkpoint_snapshot_id, b.checkpoint_snapshot_id);
    merged.last_epoch_seal = maxKnown(a.last_epoch_seal, b.last_epoch_seal);
    return merged;
}

std::optional<CkptSample> readCkpt(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    std::optional<GetResult> got = backend.get(layout.refCkptKey(RefNamespaceId::stageATransition(ns)));
    if (!got)
        return std::nullopt;
    /// Materialized read, then decode: the object is MUTABLE, so the body must be fixed before it is
    /// parsed, and the token must be the one that labels exactly these bytes.
    return CkptSample{decodeRefCkpt(got->bytes), got->token};
}

CkptPublishOutcome publishCkpt(Backend & backend, const Layout & layout, const RootNamespace & ns,
                               const RefCkpt & contribution, uint64_t admitted_generation,
                               const std::function<void(uint64_t)> & check_fence_or_throw,
                               const CkptDeadline & deadline)
{
    const String key = layout.refCkptKey(RefNamespaceId::stageATransition(ns));

    for (size_t attempt = 0; attempt < MAX_CKPT_CAS_ATTEMPTS; ++attempt)
    {
        if (deadline.now_ms() >= deadline.deadline_ms)
            break;

        /// Read the WHOLE body every attempt. A retry after a conflict must merge against what is
        /// there NOW: reusing the previous attempt's reading is precisely the read-modify-write with
        /// the merge left out, one round later.
        const std::optional<CkptSample> current = readCkpt(backend, layout, ns);
        /// ANY writer may create the object; none of them may invent a field. An absent `_ckpt` is
        /// created from the contribution as it stands, so a publisher that knows only the checkpoint
        /// creates one that knows only the checkpoint, and the birth transaction's `life_epoch` merges
        /// into it whenever it arrives -- in either order, because the merge is a per-field maximum.
        const RefCkpt merged = current ? mergeCkpt(current->ckpt, contribution) : contribution;

        /// Nothing new: return WITHOUT a CAS. This is not an optimization -- both writers publish on
        /// every snapshot and every seal, and most of those carry a checkpoint the object already has,
        /// so issuing the write anyway would mint a fresh token per no-op and turn every other writer's
        /// in-flight CAS into a conflict, for a body byte-identical to the one already stored.
        if (current && merged == current->ckpt)
            return CkptPublishOutcome::IdenticalSkip;

        /// AFTER the read, BEFORE the CAS, on EVERY attempt (spec §3). A generation that moved since
        /// admission means this writer's lease incarnation is gone and the body it just merged is
        /// stale, so the CAS must never be sent -- and because the check precedes it, nothing was.
        try
        {
            check_fence_or_throw(admitted_generation);
        }
        catch (...)
        {
            /// Typed, not propagated: the caller asked "did this land", and "the fence moved, so
            /// nothing was sent" is an answer, not a failure of the operation. Only the fence check is
            /// wrapped, so nothing else can be mistaken for it.
            return CkptPublishOutcome::FencedOut;
        }

        const std::optional<Token> expected =
            current ? std::optional<Token>{current->token} : std::nullopt;
        if (backend.casPut(key, encodeRefCkpt(merged), expected).outcome == CasOutcome::Committed)
            return CkptPublishOutcome::Published;
        /// `Conflict`: the incarnation we read is no longer current, so another writer's merge landed
        /// first. Nothing of ours was written; re-read and merge against the winner.
    }

    /// Fail closed. Every attempt was all-or-nothing, so there is no partial state -- only an
    /// unpublished contribution, which the caller must be told about rather than left to assume.
    throwCasWriteRetryLater("CAS _ckpt for namespace '" + ns.string()
        + "': persistent CAS contention, the checkpoint contribution was not published");
}

MissingBaseVerdict classifyMissingSampledBase(const Token & sampled_token, const std::optional<Token> & current_token)
{
    if (current_token && !(*current_token == sampled_token))
        return MissingBaseVerdict::RestartRecovery;
    return MissingBaseVerdict::Corrupted;
}

bool snapshotDeletableUnderCkpt(const RefTxnId & snapshot_id, const std::optional<RefTxnId> & checkpoint_snapshot_id)
{
    return checkpoint_snapshot_id.has_value() && snapshot_id < *checkpoint_snapshot_id;
}

}
