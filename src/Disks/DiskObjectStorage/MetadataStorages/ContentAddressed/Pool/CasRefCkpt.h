#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCatalogFormat.h>
#include <cstdint>
#include <optional>

namespace DB::Cas
{

/// The pure, catalog-authoritative input to both recovery entry points. `LIST` cannot supply a base,
/// life epoch, frontier, or stopping condition.
struct RecoveryGrounding
{
    std::optional<RefTxnId> base;
    /// Always set for a recoverable life: the base's successor, or `{life_epoch,1}`.
    std::optional<RefTxnId> walk_from;
    /// Set only from a generation 1 or 2 body: the chain may not end below it.
    std::optional<RefTxnId> committed_through;
};

/// Choose where recovery starts (the base and the first key to walk) and, for a generation 1 or 2
/// `_ckpt`, the frontier the chain may not end below, from catalog lifecycle authority and `_ckpt`. Throws
/// `INVALID_STATE` for absent/Creating names and `CORRUPTED_DATA` when a Live/Removing life lacks
/// the checkpoint or genesis fact it must have. This helper performs no I/O and never trusts LIST.
RecoveryGrounding chooseRecoveryGrounding(const std::optional<CatalogEntry> & catalog_state,
                                          const std::optional<RefCkpt> & ckpt);

/// The writers of one namespace's `_ckpt` object and what a reader consults when the base it sampled
/// turns out to be gone. The object itself and its codec live in `Formats/CasRefCkptFormat.h`.

/// The legacy semantic-maximum merge of generation 1 and 2 bodies, used only by `publishCkpt`. Per
/// field: the greater `life_epoch`, the greater present `checkpoint_snapshot_id`, the greater present
/// `last_epoch_seal`; an absent optional loses to a present one, and two absents stay absent. A
/// cross-epoch `committed_through` pair must be numerically adjacent and carry its seal evidence.
/// It does not hold the `life_epoch` decrease refusal or the frontier refusals: a commutative function
/// cannot tell which argument is durable, so those live in `publishCkpt`.
RefCkpt mergeCkpt(const RefCkpt & a, const RefCkpt & b);

/// What one `_ckpt` publication did: `publishCkpt`, `createCkptAtCreation` or `publishCkptSnapshot`.
enum class CkptPublishOutcome : uint8_t
{
    Published,      /// the contribution is durable, and this call sent at least one write before it
                    /// was; which write made it durable -- this one or a competitor's -- is not claimed
    IdenticalSkip,  /// the contribution added nothing to what was already there; NO write was issued
    FencedOut,      /// this actor's admission is gone. Whether its last attempt landed is UNRESOLVED:
                    /// admission can be lost before the write is sent, and equally after the write is
                    /// proven durable. Re-read before assuming either way
    SuccessorSeal,  /// CONCLUSIVE rejection of a non-seal frontier: a successor sealed this epoch at that
                    /// id, so the record is not history even if its object exists. No further write
                    /// follows the refusal. The late record stays below the checkpoint: no reader visits
                    /// it, and ref cleanup deletes it
    Superseded,     /// UNRESOLVED: a later epoch moved past this non-seal frontier, and `_ckpt` cannot tell
                    /// whether the successor adopted it. No write follows the verdict, but an earlier
                    /// attempt of this call may have landed. Handle like `FencedOut`
    NotAdvanced,    /// nothing was written and the caller's fact is not in `_ckpt`
};

/// True when the contribution is durable in `_ckpt`: the only outcomes after which a caller may act on it.
inline bool ckptContributionRecorded(CkptPublishOutcome outcome)
{
    return outcome == CkptPublishOutcome::Published || outcome == CkptPublishOutcome::IdenticalSkip;
}

/// The legacy publisher of generation 1 and 2 bodies; no production code calls it, and it refuses a
/// generation 3 body with `CORRUPTED_DATA`. It merges `contribution` into `life`'s `_ckpt` and makes the
/// result durable, as ONE read-modify-write on `op`; an ABSENT object is created from `contribution`.
///
/// The `life_epoch` and identical-body verdicts consult `op.admitted()` before they speak, because a writer
/// the fence is about to refuse has landed nothing AT THAT POINT: it is told `FencedOut` rather than `IdenticalSkip`
/// or a corruption verdict. `FencedOut` is returned rather than thrown because it is an expected,
/// transient control signal. It does NOT promise the object is unchanged -- a lost
/// admission is also reported for a write already proven durable.
///
/// A NON-seal frontier contribution (`committed_through` present and not equal to its own
/// `last_epoch_seal`) is checked against the durable body before the merge, independently of admission,
/// because the merge accepts a lower-epoch frontier by numeric comparison alone. Only such a
/// contribution can get these outcomes; one without `committed_through` never does:
///   - its `committed_through` equals the durable `last_epoch_seal` -> `SuccessorSeal`;
///   - its epoch is below the durable frontier's, and the durable seal is not in its epoch strictly
///     above it -> `Superseded`. An id below the seal of its own epoch was adopted by the successor's
///     walk and still merges.
///
/// FAILS CLOSED, never open:
///   - an existing `_ckpt` that does not decode PROPAGATES `CORRUPTED_DATA` and is never overwritten.
///     It is the only record of recovery's base and of what cleanup may delete; replacing it with a
///     body derived from `contribution` alone would erase the base while leaving a well-formed object
///     behind -- corruption laundered into something a reader would trust.
///   - a contribution whose `life_epoch` is BELOW the durable one raises `CORRUPTED_DATA`, decided
///     before the merge, so no body is built and no write is sent. This rule and the frontier verdicts
///     HAVE to live here rather than in `mergeCkpt`: only this function knows which side is durable.
///   - a contribution `mergeCkpt` rejects (a frontier that skips an epoch, a broken seal invariant) raises
///     `CORRUPTED_DATA` for an admitted writer, decided before any write. A writer that already lost
///     admission gets `FencedOut` instead.
///   - exhausting the policy under persistent conflict throws the retry-later class. No partial state
///     exists to clean up: every attempt either committed the complete merged body or changed nothing.
CkptPublishOutcome publishCkpt(CasOperation & op, const Layout & layout, const NamespaceLifeId & life,
                               const RefCkpt & contribution, const Retry & policy = Retry::standard());

/// One observation of a namespace's `_ckpt`: the decoded body and the incarnation it was read at. The
/// incarnation is what the missing-base revalidation adjudicates against, so a reader that keeps only
/// the body cannot apply the rule.
struct CkptSample
{
    RefCkpt ckpt;
    Etag etag;
};

/// Point-read of `life`'s `_ckpt`. `nullopt` means the object is absent (a namespace whose creation has
/// not published one yet); a present-but-undecodable object throws `CORRUPTED_DATA`.
std::optional<CkptSample> readCkpt(CasOperation & op, const Layout & layout, const NamespaceLifeId & life);

/// Create-if-absent of the generation 3 body `{life_epoch}`; the only production writer that creates `_ckpt`.
/// Absent -> created: `Published`. Present, whatever it holds, or a lost create race: `IdenticalSkip`,
/// so a durable `life_epoch` is never replaced. Fence lost: `FencedOut`.
CkptPublishOutcome createCkptAtCreation(CasOperation & op, const Layout & layout, const NamespaceLifeId & life,
                                        uint64_t life_epoch, const Retry & policy = Retry::standard());

/// Conditional replace of a present generation 3 body that raises `checkpoint_snapshot_id`.
/// Replaced: `Published`. Durable id equal to `snapshot_id`: `IdenticalSkip`. Object absent, below
/// generation 3, or durable id above `snapshot_id`: `NotAdvanced`. Fence lost: `FencedOut`.
CkptPublishOutcome publishCkptSnapshot(CasOperation & op, const Layout & layout, const NamespaceLifeId & life,
                                       const RefTxnId & snapshot_id, const Retry & policy = Retry::standard());

enum class CkptStampOutcome : uint8_t
{
    Stamped,
    TokenMismatch,
    Unresolved,
};

struct CkptStampResult
{
    CkptStampOutcome outcome;
    /// Set iff `Stamped`: the body now durable and the token it was written under.
    std::optional<CkptSample> stamped;
};

/// One conditional replace on `accepted.etag` with `{life_epoch, checkpoint_snapshot_id}` at generation 3.
/// An absent object or another token: `TokenMismatch`. A write that did not commit for any other reason,
/// an unknown outcome included: `Unresolved`.
CkptStampResult stampCkptGeneration3(CasOperation & op, const Layout & layout, const NamespaceLifeId & life,
                                     const CkptSample & accepted);

/// The verdict of INV-4's three-way revalidation, for the one leg that is not simply "it is there".
enum class MissingBaseVerdict : uint8_t
{
    RestartRecovery,  /// the checkpoint moved under us -- re-read `_ckpt` and start over from the new base
    Corrupted,        /// the base is gone under a checkpoint that still names it -- fail closed
};

/// Adjudicate a sampled recovery anchor that turned out to be unavailable, by comparing the `_ckpt`
/// incarnation this recovery sampled against the one a fresh re-read observes. The anchor is the
/// checkpoint-named snapshot and its retained same-id non-seal log witness; the caller supplies this
/// verdict after either exact read is absent.
///
///   - incarnation ADVANCED  -> `RestartRecovery`. Cleanup legitimately advanced the checkpoint and
///     deleted the previous anchor while we were reading. Nothing is wrong; restart from the newer
///     base (bounded by the caller's own restart budget).
///   - incarnation UNCHANGED -> `Corrupted`. The checkpoint still names an object that is not there, and
///     the deletion gate makes that unreachable in an honest run: the named snapshot and matching log
///     are both retained. Something deleted a live anchor.
///   - `_ckpt` itself ABSENT on the re-read -> `Corrupted` for the same reason, and more bluntly: the
///     namespace has a base we sampled and no checkpoint at all. `_ckpt` is deleted only as part of
///     namespace removal, which cannot be racing a live recovery of that same namespace.
///
/// Pure, so it is decided the same way at every call site; the caller raises `CORRUPTED_DATA` on
/// `Corrupted` with its own context.
MissingBaseVerdict classifyMissingSampledBase(const Etag & sampled, const std::optional<Etag> & current);

/// INV-4's snapshot-deletion gate: a snapshot is deletable only STRICTLY BELOW the checkpoint. Strict
/// rather than at-or-below because the checkpoint names the snapshot a recovery is entitled to fetch
/// by exact key; deleting THAT one is what turns a stale-but-harmless pointer into the corruption
/// `classifyMissingSampledBase` has to report (TLC counterexample `_sab_staleckptcorruption`).
///
/// Fail-closed on `nullopt`: a namespace with no checkpoint yet has NOTHING deletable, because no
/// snapshot has been established as a covering base.
bool snapshotDeletableUnderCkpt(const RefTxnId & snapshot_id, const std::optional<RefTxnId> & checkpoint_snapshot_id);

}
