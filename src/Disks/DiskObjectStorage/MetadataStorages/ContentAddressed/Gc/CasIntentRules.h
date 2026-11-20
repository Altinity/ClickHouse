#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <cstdint>
#include <optional>

/// The per-blob decision rules of the deletion-intent GC, as pure functions over what one round
/// observed. A row of the adopted state is `candidate(t)` (no `delete_pending`), `pending(t)`
/// (`delete_pending`) or `standing(t)` (`standing`), where `t` is the row's token.
namespace DB::Cas
{

/// What the decision `HEAD B` of this round showed, relative to the row's own token.
enum class HeadSeen : uint8_t
{
    Same,          /// the row's own incarnation, in any spelling
    Absent,        /// an answered 404
    Other,         /// an incarnation that canonicalises and is not the row's
    Uncanonical,   /// a well-formed token that names no incarnation: it never shows that `t` is gone
};

/// Built only by `classifyDecisionHead`, so every `seen` other than `Absent` carries the token the
/// `HEAD` observed.
class DecisionHead
{
public:
    HeadSeen seen() const { return seen_; }
    const std::optional<Meta> & observed() const { return observed_; }

private:
    DecisionHead(HeadSeen seen, std::optional<Meta> observed) : seen_(seen), observed_(std::move(observed)) {}

    friend DecisionHead classifyDecisionHead(const PersistedEtag & row_token, const std::optional<Meta> & observed);
    friend DecisionHead classifyDecisionHead(const std::optional<Meta> & observed);

    HeadSeen seen_;
    std::optional<Meta> observed_;
};

/// `row_token` is the token of the row the `HEAD` decides; a token that does not canonicalise is
/// never `Same`, so a canonical body behind such a row is `Other`.
DecisionHead classifyDecisionHead(const PersistedEtag & row_token, const std::optional<Meta> & observed);
/// For a key with no row: a canonical body is `Other`.
DecisionHead classifyDecisionHead(const std::optional<Meta> & observed);

/// What the caller does for one blob.
enum class IntentStep : uint8_t
{
    None,                /// no row, no request
    Create,              /// create `intent_to_create`; the row is `pending` if the create is confirmed, else `candidate`
    Candidate,           /// over the create budget: record `candidate(new_token)`, send no request
    Carry,               /// keep the row unchanged
    Drop,                /// drop the row; a spare also deletes `intent_to_delete` when it names one
    DeleteBody,          /// row to `standing(t)`, and `DELETE B If-Match` the raw token of `head->observed()`
    Standing,            /// the body is absent: row to `standing(t)`, no request
    TransitionCreate,    /// create `intent_to_create`; row to `pending(new_token)` if confirmed, else `candidate(new_token)`
    TransitionCandidate, /// over the create budget: row to `candidate(new_token)`, no create
    DeleteIntent,        /// delete `intent_to_delete`; drop the row after the answer
    Anomaly,             /// `standing(t)` whose `HEAD` shows `t`: carried, nothing deleted, one ERROR
};

struct IntentRuleInput
{
    /// From the adopted state, so written by an earlier round: the fold never passes a row it minted.
    std::optional<RetiredEntry> row;
    bool at_zero = false, fresh_transition = false, suppressed = false;
    bool key_touched = false;          /// the key had an edge or a delta in this round's window
    bool create_budget_admits = false, redelete_budget_admits = false;
    std::optional<DecisionHead> head;  /// the decision HEAD of this round, when one was sent
};

/// The decision `HEAD` an input needs before `decideIntentStep`, from everything but `head`: the `HEAD` of a
/// touched key at zero, which serves every rule of the key, the `HEAD` before a pending row's body delete, or
/// the `HEAD` of a spared `pending` or `standing` row whose token canonicalises, in an unsuppressed round.
enum class DecisionHeadNeed : uint8_t
{
    None,
    TouchedKey,
    PendingBody,
    Spare,
};
DecisionHeadNeed decisionHeadNeed(const IntentRuleInput & input);

struct IntentDecision
{
    IntentStep step = IntentStep::None;
    std::optional<CanonicalIdentity> intent_to_create = {};
    /// Goes to the delete batch, beside a transition too; never to the job that deletes the body.
    std::optional<CanonicalIdentity> intent_to_delete = {};
    /// The token of the row this step writes, when it is the observed one rather than the row's own.
    std::optional<PersistedEtag> new_token = {};
};

/// Every destructive step needs an unsuppressed round, and every one but a spare's intent delete needs zero.
/// `DeleteBody` also needs the redelete budget, a `HEAD` that shows the row's own token and a key this round did
/// not touch: only an untouched zero, on a complete intake, proves the blob had no owner at the round's catalog
/// read, so a touched `pending` row with that `HEAD` is carried. `DeleteIntent` needs a
/// `standing` row with a canonical token; the `intent_to_delete` of a transition, or of a spared `pending` or
/// `standing` row, a `HEAD` that shows another incarnation.
/// Throws `LOGICAL_ERROR` on an input no round produces: a row that is not a blob row, `standing`
/// without `delete_pending`, `fresh_transition` other than "no row, at zero, touched", no `HEAD` where
/// one is required (a fresh transition, a touched key at zero with a row, a pending row whose body
/// delete runs, a spare that `decisionHeadNeed` names), or a `HEAD` whose classification against the row's
/// token gives another result. A `HEAD` that is not required is used like any other.
IntentDecision decideIntentStep(const IntentRuleInput & input);

}
