#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasIntentRules.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace DB::Cas
{

DecisionHead classifyDecisionHead(const PersistedEtag & row_token, const std::optional<Meta> & observed)
{
    if (!observed)
        return {HeadSeen::Absent, observed};
    if (!observed->etag.canonical())
        return {HeadSeen::Uncanonical, observed};
    return {sameIncarnation(row_token, observed->etag) ? HeadSeen::Same : HeadSeen::Other, observed};
}

DecisionHead classifyDecisionHead(const std::optional<Meta> & observed)
{
    if (!observed)
        return {HeadSeen::Absent, observed};
    return {observed->etag.canonical() ? HeadSeen::Other : HeadSeen::Uncanonical, observed};
}

DecisionHeadNeed decisionHeadNeed(const IntentRuleInput & input)
{
    if (input.at_zero && input.key_touched)
        return DecisionHeadNeed::TouchedKey;
    const RetiredEntry * row = input.row ? &*input.row : nullptr;
    if (row && input.at_zero && row->delete_pending && !row->standing && !input.suppressed && input.redelete_budget_admits)
        return DecisionHeadNeed::PendingBody;
    return DecisionHeadNeed::None;
}

namespace
{

[[noreturn]] void refuse(std::string_view what)
{
    throw Exception(ErrorCodes::LOGICAL_ERROR, "GC intent rules: {}", what);
}

/// Without the `HEAD` `decisionHeadNeed` names the rules could create or destroy on a stale view. A `HEAD` that
/// is not required, such as a shared read-ahead answer, is used like any other.
void checkInput(const IntentRuleInput & input)
{
    const RetiredEntry * row = input.row ? &*input.row : nullptr;
    if (row && row->kind != ObjectKind::Blob)
        refuse("the row is not a blob row");
    if (row && row->standing && !row->delete_pending)
        refuse("a standing row without delete_pending");
    if (input.fresh_transition != (!row && input.at_zero && input.key_touched))
        refuse("fresh_transition is not 'no row, at zero, touched'");

    if (decisionHeadNeed(input) != DecisionHeadNeed::None && !input.head)
        refuse("the decision HEAD of this input is missing");

    if (input.head)
    {
        const DecisionHead again = row ? classifyDecisionHead(row->token, input.head->observed())
                                       : classifyDecisionHead(input.head->observed());
        if (again.seen() != input.head->seen())
            refuse("the decision HEAD was classified against another token");
    }
}

/// Condemn a body: `Create` within the create budget, else `over_budget`; the row takes the observed token.
IntentDecision condemnObserved(const IntentRuleInput & input, IntentStep within_budget, IntentStep over_budget)
{
    const Etag & observed = input.head->observed()->etag;
    IntentDecision out{.step = over_budget, .new_token = PersistedEtag::capture(observed)};
    if (input.create_budget_admits)
    {
        out.step = within_budget;
        out.intent_to_create = observed.canonical();
    }
    return out;
}

}

IntentDecision decideIntentStep(const IntentRuleInput & input)
{
    checkInput(input);

    const std::optional<HeadSeen> seen = input.head ? std::optional(input.head->seen()) : std::nullopt;
    if (!input.row)
    {
        /// A fresh transition condemns a body whose token canonicalises, in any round.
        if (input.fresh_transition && seen == HeadSeen::Other)
            return condemnObserved(input, IntentStep::Create, IntentStep::Candidate);
        return {};
    }

    const RetiredEntry & row = *input.row;
    if (!input.at_zero)
        return {.step = IntentStep::Drop};

    const std::optional<CanonicalIdentity> own = row.token.canonical();
    /// The transition first: a plain intent delete first can spend the round that folded the key's
    /// last transition, and then the new incarnation is never condemned. Its intent delete waits for an
    /// unsuppressed round.
    if (seen == HeadSeen::Other)
    {
        IntentDecision out = condemnObserved(input, IntentStep::TransitionCreate, IntentStep::TransitionCandidate);
        if (!input.suppressed)
            out.intent_to_delete = own;
        return out;
    }

    /// A body or intent delete on a suppressed cut can miss a writer's edge.
    const bool destruction_allowed = !input.suppressed;

    if (row.standing)
    {
        /// The body delete of this row was answered, so its own token cannot come back.
        if (seen == HeadSeen::Same)
            return {.step = IntentStep::Anomaly};
        if (!destruction_allowed)
            return {.step = IntentStep::Carry};
        /// The removal needs no HEAD, so a supersede HEAD with no identity does not hold it back.
        if (!own)
            return {.step = IntentStep::Drop};
        return {.step = IntentStep::DeleteIntent, .intent_to_delete = own};
    }

    if (row.delete_pending)
    {
        if (!destruction_allowed || !input.redelete_budget_admits)
            return {.step = IntentStep::Carry};
        if (seen == HeadSeen::Same)
            return {.step = IntentStep::DeleteBody};
        if (seen == HeadSeen::Absent)
            return {.step = IntentStep::Standing};
        /// A token with no identity never shows that the body is gone.
        return {.step = IntentStep::Carry};
    }

    /// No intent is created for a body that is gone; one an earlier create left is an orphan for the sweep.
    if (seen == HeadSeen::Absent)
        return {.step = IntentStep::Drop};
    if (destruction_allowed && input.create_budget_admits && own)
        return {.step = IntentStep::Create, .intent_to_create = own};
    return {.step = IntentStep::Carry};
}

}
