#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace DB::Cas
{

/// THE update and read algorithms for one namespace's `_ckpt` object (spec INV-4). The object itself
/// and its codec live in `Formats/CasRefCkptFormat.h`; this header is what WRITES it and what a reader
/// consults when the base it sampled turns out to be gone.

/// The one join, shared by BOTH `_ckpt` writers (the snapshot publisher and the sealer). An absent
/// optional loses to a present one on every field, and two absents stay absent. Per field:
///   - `checkpoint_snapshot_id`, `last_epoch_seal`: the SEMANTIC MAXIMUM. These genuinely advance over a
///     namespace's life, and the max is what stops a writer that sampled an older body from regressing
///     the other writer's progress.
///   - `life_epoch`: the contribution wins when it is at or ABOVE the durable value, and a contribution
///     BELOW it is `CORRUPTED_DATA` -- see `joinLifeEpoch` in the `.cpp` for why that specific case, and
///     only that case, is the one worth refusing. Two present-and-different values are ordinary and must
///     stay admitted; a decrease is a fence violation the old per-field max absorbed silently.
///
/// The ARGUMENTS ARE ORDERED, which is what the `life_epoch` rule needs and what the two id fields do
/// not care about: `stored` is the body just read from the object, `contribution` is what this writer
/// wants to add. `what` names the object in the exception message (the same role it plays in
/// `checkRefCkptInvariants`); `publishCkpt` passes the `_ckpt` key.
///
/// There is deliberately ONE of these rather than a surgical per-writer update. A `_ckpt` write is a
/// whole-body read-modify-write, so a writer that wrote back only the field it knows about would carry
/// its STALE reading of the other field along with it and silently regress the other writer's
/// progress. That regression is not hypothetical: it is TLC counterexample `_sab_sealclobbersbase`,
/// where a sealer writing its sampled body back verbatim drops a concurrently published base and the
/// next recovery loses an ACKED transaction.
///
/// The two writers still need NO ORDERING between them, which is what the old "the merge is commutative"
/// sentence was really claiming, but the reason is now narrower and worth stating exactly. On the two id
/// fields the join is a commutative, associative max, so their outcome is order-independent outright. On
/// `life_epoch` the outcome is order-independent because the ORDER ITSELF is constrained: contributions
/// only ever rise (writer epochs are durable-monotone per server root, and a namespace has one), so the
/// reversed sequence that would be refused is the one that cannot occur. The token-CAS is what makes
/// each read-modify-write atomic; that part is unchanged.
RefCkpt mergeCkpt(const RefCkpt & stored, const RefCkpt & contribution, std::string_view what);

/// What one `publishCkpt` call did.
enum class CkptPublishOutcome : uint8_t
{
    Published,      /// the merged body is durable -- this call's CAS committed it
    IdenticalSkip,  /// the contribution added nothing to what was already there; NO write was issued
    FencedOut,      /// the admitted fence generation moved before the CAS; NOTHING was written
};

/// The retry bound for `publishCkpt`: an absolute point on a monotonic millisecond clock, plus that
/// clock. Both are required and must be the SAME clock -- the caller passes its own injectable boot
/// clock (`CasRefLedger`'s `boot_ms_fn`), so a test drives the exhaustion arm deterministically
/// instead of sleeping, and a VM suspend cannot shorten the window.
struct CkptDeadline
{
    std::function<uint64_t()> now_ms;
    uint64_t deadline_ms = 0;
};

/// Merge `contribution` into `ns`'s `_ckpt` and make the result durable.
///
/// One attempt is: GET the object -> decode it -> merge -> (identical? return without a CAS) ->
/// re-check the fence -> token-CAS. A CAS conflict means another writer's read-modify-write landed
/// between our GET and our CAS, so the whole attempt repeats against the NEW body -- never against the
/// one we already read, which is the point of re-reading rather than retrying the same bytes.
///
/// An ABSENT object is created from `contribution` as it stands. Every writer may create it and none
/// may complete it: a publisher that knows only the checkpoint creates one that knows only the
/// checkpoint, and the field a different writer knows merges in whenever it arrives, in either order.
/// That is the whole reason each field is optional rather than defaulted.
///
/// FENCE DISCIPLINE (spec §3, the same value at every site of the trio): `check_fence_or_throw` is
/// re-run on EVERY attempt, AFTER that attempt's read and immediately BEFORE its CAS -- not once at
/// entry. A generation that moved means the mount lease incarnation changed since this work was
/// admitted, so this writer's body is stale even if the fence happens to be live again; the CAS must
/// not be sent. That refusal is returned as `FencedOut` rather than thrown: it is an expected,
/// transient control signal (the same class the request controller reports as `Unresolved`), and the
/// snapshot publisher that calls this sits after a durable PUT where an exception would be worse than
/// a value. NOTHING has been written when it is returned -- the check precedes the CAS.
///
/// FAILS CLOSED, never open:
///   - an existing `_ckpt` that does not decode PROPAGATES `CORRUPTED_DATA` and is never overwritten.
///     It is the only record of recovery's base and of what cleanup may delete; replacing it with a
///     body derived from `contribution` alone would erase the base while leaving a well-formed object
///     behind -- corruption laundered into something a reader would trust.
///   - a contribution whose `life_epoch` is BELOW the durable one PROPAGATES `CORRUPTED_DATA` from
///     `mergeCkpt`, before the CAS is built, so nothing is written. That refusal is raised where the
///     merge is, not here, so it cannot be reached by any other path into this object.
///   - exhausting the deadline (or the live-lock brake) under persistent conflict throws the
///     retry-later class. No partial state exists to clean up: every attempt either committed the
///     complete merged body or changed nothing.
///
/// `admitted_generation` is the fence generation the CALLER captured when its work was admitted, and
/// `check_fence_or_throw` is the callback the pool wires from `CasMountRuntime::checkFenceOrThrow`
/// (the ledger never owns a `CasMountRuntime`; it receives the pair the way `CasPlainObjects` does).
CkptPublishOutcome publishCkpt(Backend & backend, const Layout & layout, const NamespaceLifeId & life,
                               const RefCkpt & contribution, uint64_t admitted_generation,
                               const std::function<void(uint64_t)> & check_fence_or_throw,
                               const CkptDeadline & deadline);

/// One observation of a namespace's `_ckpt`: the decoded body and the incarnation TOKEN it was read
/// at. The token is what the missing-base revalidation adjudicates against, so a reader that keeps
/// only the body cannot apply the rule.
struct CkptSample
{
    RefCkpt ckpt;
    Token token;
};

/// Point-read of `life`'s `_ckpt`. `nullopt` means the object is absent (a namespace whose creation has
/// not published one yet); a present-but-undecodable object throws `CORRUPTED_DATA`.
std::optional<CkptSample> readCkpt(Backend & backend, const Layout & layout, const NamespaceLifeId & life);

/// The verdict of INV-4's three-way revalidation, for the one leg that is not simply "it is there".
enum class MissingBaseVerdict : uint8_t
{
    RestartRecovery,  /// the checkpoint moved under us -- re-read `_ckpt` and start over from the new base
    Corrupted,        /// the base is gone under a checkpoint that still names it -- fail closed
};

/// Adjudicate a sampled base that turned out to be MISSING (its exact-key GET returned 404), by
/// comparing the `_ckpt` token this recovery sampled against the token a fresh re-read observes.
///
///   - token ADVANCED  -> `RestartRecovery`. Cleanup legitimately advanced the checkpoint and deleted
///     a snapshot strictly below the new base while we were reading. Nothing is wrong; restart from
///     the newer base (bounded by the caller's own restart budget).
///   - token UNCHANGED -> `Corrupted`. The checkpoint still names the object that is not there, and
///     the deletion gate makes that unreachable in an honest run: snapshots are deletable only
///     STRICTLY BELOW the checkpoint. Something deleted a live base.
///   - `_ckpt` itself ABSENT on the re-read -> `Corrupted` for the same reason, and more bluntly: the
///     namespace has a base we sampled and no checkpoint at all. `_ckpt` is deleted only as part of
///     namespace removal, which cannot be racing a live recovery of that same namespace.
///
/// Pure, so it is decided the same way at every call site; the caller raises `CORRUPTED_DATA` on
/// `Corrupted` with its own context.
MissingBaseVerdict classifyMissingSampledBase(const Token & sampled_token, const std::optional<Token> & current_token);

/// INV-4's snapshot-deletion gate: a snapshot is deletable only STRICTLY BELOW the checkpoint. Strict
/// rather than at-or-below because the checkpoint names the snapshot a recovery is entitled to fetch
/// by exact key; deleting THAT one is what turns a stale-but-harmless pointer into the corruption
/// `classifyMissingSampledBase` has to report (TLC counterexample `_sab_staleckptcorruption`).
///
/// Fail-closed on `nullopt`: a namespace with no checkpoint yet has NOTHING deletable, because no
/// snapshot has been established as a covering base.
bool snapshotDeletableUnderCkpt(const RefTxnId & snapshot_id, const std::optional<RefTxnId> & checkpoint_snapshot_id);

}
