#include <gtest/gtest.h>
#include "config.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobIntentFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasIntentRules.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <Poco/Exception.h>
#include <base/defines.h>

#include <array>
#include <variant>

using namespace DB::Cas;
using DB::Cas::tests::BatchCapabilityBackend;
using DB::Cas::tests::DialectOverrideBackend;
using DB::Cas::tests::FakeClock;
using DB::Cas::tests::JournalVerb;
using DB::Cas::tests::OperationForTest;

namespace DB::ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace
{

const char * name(IntentStep step)
{
    switch (step)
    {
        case IntentStep::None: return "None";
        case IntentStep::Create: return "Create";
        case IntentStep::Candidate: return "Candidate";
        case IntentStep::Carry: return "Carry";
        case IntentStep::Drop: return "Drop";
        case IntentStep::DeleteBody: return "DeleteBody";
        case IntentStep::Standing: return "Standing";
        case IntentStep::TransitionCreate: return "TransitionCreate";
        case IntentStep::TransitionCandidate: return "TransitionCandidate";
        case IntentStep::DeleteIntent: return "DeleteIntent";
        case IntentStep::Anomaly: return "Anomaly";
    }
    return "?";
}

/// An ETag store that answers every `HEAD` with the weak form of the value: well-formed, but no incarnation.
class WeakEtagBackend : public DialectOverrideBackend
{
public:
    WeakEtagBackend() : DialectOverrideBackend(Dialect::ETag) {}

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        auto meta = DialectOverrideBackend::head(key, access);
        if (meta)
            meta->value = "W/\"" + meta->value + "\"";
        return meta;
    }
};

Meta liveHead(Backend & backend, const String & key, const String & bytes)
{
    OperationForTest op(backend);
    if (!(*op).head(key, Retry::once()))
        EXPECT_TRUE(std::holds_alternative<Committed>((*op).create(key, bytes, Retry::once())));
    auto meta = (*op).head(key, Retry::once());
    chassert(meta.has_value());
    return *meta;
}

String unquoted(const String & value)
{
    return value.size() >= 2 && value.front() == '"' && value.back() == '"' ? value.substr(1, value.size() - 2) : value;
}

BlobRef blobRef(uint64_t n)
{
    return BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(n))};
}

/// Three live answers of one store: the row's incarnation, another one, and a weak token.
struct Observations
{
    Meta own;
    Meta other;
    Meta weak;
    PersistedEtag canonical_row_token;
    PersistedEtag uncanonical_row_token;

    static Observations make()
    {
        DialectOverrideBackend store(Dialect::ETag);
        WeakEtagBackend weak_store;
        Meta own = liveHead(store, "p/blobs/a", "x");
        Meta other = liveHead(store, "p/blobs/b", "y");
        Meta weak = liveHead(weak_store, "p/blobs/w", "z");
        const PersistedEtag canonical_row_token = PersistedEtag::capture(own.etag);
        const PersistedEtag uncanonical_row_token = PersistedEtag::capture(weak.etag);
        return Observations{std::move(own), std::move(other), std::move(weak), canonical_row_token, uncanonical_row_token};
    }
};

enum class RowState : uint8_t { None, Candidate, Pending, Standing, NotABlob, StandingNotPending };
enum class HeadKind : uint8_t { NotSent, Same, Absent, Other, Uncanonical };

constexpr std::array kRowStates{RowState::None, RowState::Candidate, RowState::Pending, RowState::Standing,
                                RowState::NotABlob, RowState::StandingNotPending};
constexpr std::array kHeadKinds{HeadKind::NotSent, HeadKind::Same, HeadKind::Absent, HeadKind::Other, HeadKind::Uncanonical};

const char * name(RowState state)
{
    switch (state)
    {
        case RowState::None: return "none";
        case RowState::Candidate: return "candidate";
        case RowState::Pending: return "pending";
        case RowState::Standing: return "standing";
        case RowState::NotABlob: return "not a blob row";
        case RowState::StandingNotPending: return "standing without delete_pending";
    }
    return "?";
}

const char * name(HeadKind kind)
{
    switch (kind)
    {
        case HeadKind::NotSent: return "not sent";
        case HeadKind::Same: return "Same";
        case HeadKind::Absent: return "Absent";
        case HeadKind::Other: return "Other";
        case HeadKind::Uncanonical: return "Uncanonical";
    }
    return "?";
}

/// One point of the product of every input dimension.
struct Cell
{
    RowState row = RowState::None;
    bool canonical_token = true;
    bool zero = true;
    bool fresh = false;
    bool touched = false;
    HeadKind head = HeadKind::NotSent;
    bool suppressed = false;
    bool create_budget = true;
    bool redelete_budget = true;
};

String describe(const Cell & c)
{
    return fmt::format(
        "row {}{}, {}, {}, {}, HEAD {}, {}, create budget {}, redelete budget {}",
        name(c.row), c.row == RowState::None ? "" : (c.canonical_token ? " (canonical token)" : " (uncanonical token)"),
        c.zero ? "at zero" : "above zero", c.fresh ? "fresh transition" : "no fresh transition",
        c.touched ? "touched" : "untouched", name(c.head), c.suppressed ? "suppressed" : "unsuppressed",
        c.create_budget ? "admits" : "spent",
        c.redelete_budget ? "admits" : "spent");
}

/// The input of `cell`, or nothing when no `classifyDecisionHead` answer has its `HEAD` kind: `Same`
/// without a canonical row token.
std::optional<IntentRuleInput> inputOf(const Cell & c, const Observations & o)
{
    IntentRuleInput input;
    input.at_zero = c.zero;
    input.fresh_transition = c.fresh;
    input.key_touched = c.touched;
    input.suppressed = c.suppressed;
    input.create_budget_admits = c.create_budget;
    input.redelete_budget_admits = c.redelete_budget;
    if (c.row != RowState::None)
    {
        RetiredEntry row;
        row.kind = c.row == RowState::NotABlob ? static_cast<ObjectKind>(2) : ObjectKind::Blob;
        row.ref = blobRef(1);
        row.token = c.canonical_token ? o.canonical_row_token : o.uncanonical_row_token;
        row.condemn_round = 3;
        row.delete_pending = c.row == RowState::Pending || c.row == RowState::Standing;
        row.standing = c.row == RowState::Standing || c.row == RowState::StandingNotPending;
        input.row = row;
    }
    if (c.head == HeadKind::NotSent)
        return input;

    std::optional<Meta> observed;
    switch (c.head)
    {
        case HeadKind::NotSent: break;
        case HeadKind::Same: observed = o.own; break;
        case HeadKind::Absent: break;
        case HeadKind::Other: observed = o.other; break;
        case HeadKind::Uncanonical: observed = o.weak; break;
    }
    DecisionHead head = input.row ? classifyDecisionHead(input.row->token, observed) : classifyDecisionHead(observed);
    const HeadSeen wanted = c.head == HeadKind::Same ? HeadSeen::Same
        : c.head == HeadKind::Absent                 ? HeadSeen::Absent
        : c.head == HeadKind::Other                  ? HeadSeen::Other
                                                     : HeadSeen::Uncanonical;
    if (head.seen() != wanted)
        return std::nullopt;
    input.head = std::move(head);
    return input;
}

/// The oracle, written from the GC rules and their handling of request failures and uncanonical tokens,
/// not from the code. First: which inputs a round can produce at all. A HEAD the rules do not require can still
/// arrive, for example a read-ahead answer shared by two rows of one key.
bool headIsRequired(const Cell & c)
{
    if (c.row == RowState::None)
        return c.fresh;   /// the condemn HEAD of a fresh transition
    if (!c.zero)          /// the spare HEAD of a row with a confirmed intent it may delete
        return (c.row == RowState::Pending || c.row == RowState::Standing) && c.canonical_token && !c.suppressed;
    const bool body_delete_head = c.row == RowState::Pending && !c.suppressed && c.redelete_budget;
    return c.touched || body_delete_head;   /// the supersede HEAD of a touched key, or the body-delete HEAD
}

bool reachable(const Cell & c)
{
    if (c.row == RowState::NotABlob || c.row == RowState::StandingNotPending)
        return false;
    /// A fresh transition is a touched key reaching zero with no row.
    if (c.fresh != (c.row == RowState::None && c.zero && c.touched))
        return false;
    return c.head != HeadKind::NotSent || !headIsRequired(c);
}

enum class IntentOf : uint8_t { Nothing, RowToken, Observed };

struct Expected
{
    IntentStep step;
    IntentOf create = IntentOf::Nothing;
    IntentOf remove = IntentOf::Nothing;
    bool new_token = false;
};

/// The step a reachable cell must take.
Expected expected(const Cell & c)
{
    using enum IntentStep;
    /// Destruction needs an unsuppressed round.
    const bool destruction_allowed = !c.suppressed;

    if (c.row == RowState::None)
    {
        if (!c.fresh)
            return {None};
        /// Condemn a body whose token canonicalises; absent, or no identity: no row. Runs in any round.
        if (c.head != HeadKind::Other)
            return {None};
        if (c.create_budget)
            return {Create, IntentOf::Observed, IntentOf::Nothing, true};
        return {Candidate, IntentOf::Nothing, IntentOf::Nothing, true};
    }

    /// Spare: any row goes. Its intent goes too only when the round may destroy, the row's intent was confirmed
    /// for a canonical token, and the HEAD shows another incarnation; an absent body proves nothing.
    if (!c.zero)
    {
        const bool confirmed = c.row == RowState::Pending || c.row == RowState::Standing;
        if (confirmed && c.canonical_token && destruction_allowed && c.head == HeadKind::Other)
            return {Drop, IntentOf::Nothing, IntentOf::RowToken};
        return {Drop};
    }

    /// Transition, before any other rule; its intent delete waits for an unsuppressed round.
    if (c.head == HeadKind::Other)
    {
        const IntentOf remove = !c.suppressed && c.canonical_token ? IntentOf::RowToken : IntentOf::Nothing;
        if (c.create_budget)
            return {TransitionCreate, IntentOf::Observed, remove, true};
        return {TransitionCandidate, IntentOf::Nothing, remove, true};
    }

    switch (c.row)
    {
        case RowState::Candidate:
            /// No intent is created for a body that is gone; one an earlier create left is an orphan for the sweep.
            if (c.head == HeadKind::Absent)
                return {Drop};
            /// Retry the create. A token with no identity has no intent key to create.
            if (destruction_allowed && c.create_budget && c.canonical_token)
                return {Create, IntentOf::RowToken};
            return {Carry};
        case RowState::Pending:
            if (!destruction_allowed || !c.redelete_budget)
                return {Carry};
            if (c.head == HeadKind::Same)
                return {DeleteBody};
            if (c.head == HeadKind::Absent)
                return {Standing};
            return {Carry};   /// a token with no identity never shows that the body is gone
        case RowState::Standing:
            /// A HEAD that shows the row's own token, after its body delete answered.
            if (c.head == HeadKind::Same)
                return {Anomaly};
            /// The intent removal takes no HEAD: an uncanonical supersede HEAD is no transition.
            if (!destruction_allowed)
                return {Carry};
            if (c.canonical_token)
                return {DeleteIntent, IntentOf::Nothing, IntentOf::RowToken};
            return {Drop};
        default:
            chassert(false);
            return {None};
    }
}

std::optional<CanonicalIdentity> identityOf(IntentOf of, const Cell & c, const Observations & o)
{
    switch (of)
    {
        case IntentOf::Nothing: return std::nullopt;
        case IntentOf::RowToken: return (c.canonical_token ? o.canonical_row_token : o.uncanonical_row_token).canonical();
        case IntentOf::Observed:
        {
            const Meta & meta = c.head == HeadKind::Same ? o.own : o.other;
            return meta.etag.canonical();
        }
    }
    return std::nullopt;
}

String render(const std::optional<CanonicalIdentity> & identity)
{
    return identity ? String(dialectWord(identity->dialect)) + ":" + identity->tag_hex : "nothing";
}

/// Calls `visit(cell, input)` for every cell of the product whose `HEAD` kind can be built; returns
/// the count of the others.
template <typename Visit>
size_t forEachCell(const Observations & o, Visit && visit)
{
    size_t unbuildable = 0;
    for (const RowState row : kRowStates)
        for (const bool canonical_token : {true, false})
        {
            if (row == RowState::None && !canonical_token)
                continue;   /// no row, no token
            for (const bool zero : {true, false})
                for (const bool fresh : {true, false})
                    for (const bool touched : {true, false})
                        for (const HeadKind head : kHeadKinds)
                            for (const bool suppressed : {true, false})
                                for (const bool create_budget : {true, false})
                                    for (const bool redelete_budget : {true, false})
                                    {
                                        const Cell cell{
                                            .row = row,
                                            .canonical_token = canonical_token,
                                            .zero = zero,
                                            .fresh = fresh,
                                            .touched = touched,
                                            .head = head,
                                            .suppressed = suppressed,
                                            .create_budget = create_budget,
                                            .redelete_budget = redelete_budget};
                                        const auto input = inputOf(cell, o);
                                        if (!input)
                                            ++unbuildable;
                                        else
                                            visit(cell, *input);
                                    }
        }
    return unbuildable;
}

/// Row states x own token, in-degree, fresh transition, touched, HEAD, suppressed, create budget,
/// redelete budget.
constexpr size_t kProductCells = (1 + (kRowStates.size() - 1) * 2) * 2 * 2 * 2 * kHeadKinds.size() * 2 * 2 * 2;

/// The cell counts, derived from the dimensions and from which `HEAD` each row state requires, not from `forEachCell`,
/// `reachable` or `headIsRequired`: a change to the rules has to be argued with this arithmetic.
struct CellCounts
{
    size_t reachable = 0;
    size_t required_heads = 0;
    size_t extra_heads = 0;
    size_t unbuildable = 0;
    size_t deletes_shown_by_an_extra_head = 0;
};

CellCounts expectedCellCounts()
{
    constexpr size_t kinds = kHeadKinds.size();
    constexpr size_t sent_kinds = kinds - 1;             /// all but `NotSent`
    constexpr size_t suppression_budget_flags = 2 * 2 * 2;   /// suppressed, create budget, redelete budget
    constexpr size_t zero_touched_fresh_flags = 2 * 2 * 2;
    /// `Same` needs a canonical row token, so a row without one, and no row, cannot show it.
    constexpr size_t kinds_without_same = kinds - 1;
    constexpr size_t sent_kinds_without_same = sent_kinds - 1;
    /// Of the flags, those whose round sends a pending row's body-delete `HEAD`: unsuppressed, redelete budget admits.
    constexpr size_t pending_head_flags = 2;   /// either create budget
    /// Of the flags, those whose round sends a spare `HEAD`: unsuppressed.
    constexpr size_t spare_head_flags = 2 * 2;   /// either budget

    /// `fresh` is fixed by the other dimensions in a reachable cell, so (at zero, touched) gives one cell each.
    /// `spare_head`: the row holds a confirmed intent of a canonical token, which a spare may delete.
    const auto row_cells = [&](size_t head_kinds, size_t sent_head_kinds, bool is_pending, bool spare_head, CellCounts & out)
    {
        for (const bool zero : {true, false})
            for (const bool touched : {true, false})
            {
                const size_t required_flags = !zero ? (spare_head ? spare_head_flags : 0)
                    : touched                      ? suppression_budget_flags
                    : is_pending                   ? pending_head_flags
                                                   : 0;
                const size_t free_flags = suppression_budget_flags - required_flags;
                out.reachable += required_flags * sent_head_kinds + free_flags * head_kinds;
                out.required_heads += required_flags * sent_head_kinds;
                out.extra_heads += free_flags * (head_kinds - 1);
            }
    };

    CellCounts counts;
    row_cells(kinds_without_same, sent_kinds_without_same, false, false, counts);   /// no row
    /// Candidate, standing, pending; a spare `HEAD` only for the two with a confirmed intent and a canonical token.
    for (const auto [is_pending, confirmed] : {std::pair{false, false}, std::pair{false, true}, std::pair{true, true}})
    {
        row_cells(kinds, sent_kinds, is_pending, confirmed, counts);                                  /// canonical token
        row_cells(kinds_without_same, sent_kinds_without_same, is_pending, false, counts);   /// uncanonical token
    }

    counts.unbuildable = kRowStates.size() * zero_touched_fresh_flags * suppression_budget_flags;
    /// A `HEAD` showing another incarnation deletes the intent of an untouched row at zero that was not going to:
    /// a candidate (any create or redelete budget) and a pending row whose body-delete `HEAD` was not required
    /// (redelete budget spent), both unsuppressed.
    constexpr size_t candidate_flags = 2 * 2;
    counts.deletes_shown_by_an_extra_head = candidate_flags + pending_head_flags;
    return counts;
}

IntentDecision decide(const Cell & c, const Observations & o)
{
    return decideIntentStep(inputOf(c, o).value());
}

}

/// Every cell of the product a round can produce takes the oracle's step, intents and token.
/// Global properties over the same cells: the body delete, the intent delete and the transition's
/// precedence, each stated by itself.
TEST(CASIntentRules, EveryReachableCellFollowsTheSpec)
{
    const Observations o = Observations::make();
    ASSERT_TRUE(o.canonical_row_token.canonical().has_value());
    ASSERT_FALSE(o.uncanonical_row_token.canonical().has_value());
    ASSERT_NE(o.own.etag.canonical(), o.other.etag.canonical()) << "positive control: two incarnations";

    size_t reachable_cells = 0;
    size_t unreachable_cells = 0;
    size_t body_deletes = 0;
    size_t intent_deletes = 0;
    size_t spare_deletes = 0;
    size_t transitions = 0;
    size_t extra_heads = 0;
    size_t deletes_shown_by_an_extra_head = 0;
    size_t heads_required = 0;
    const size_t unbuildable = forEachCell(o, [&](const Cell & c, const IntentRuleInput & input)
    {
        if (!reachable(c))
        {
            ++unreachable_cells;
            return;
        }
        ++reachable_cells;
        /// The fold sends the HEAD this names, and the rules require exactly that one.
        const DecisionHeadNeed need = decisionHeadNeed(input);
        EXPECT_EQ(need != DecisionHeadNeed::None, headIsRequired(c)) << describe(c);
        if (need == DecisionHeadNeed::TouchedKey)
            EXPECT_TRUE(c.zero && c.touched) << describe(c);
        if (need == DecisionHeadNeed::PendingBody)
            EXPECT_TRUE(c.row == RowState::Pending && c.zero && !c.touched) << describe(c);
        if (need == DecisionHeadNeed::Spare)
            EXPECT_TRUE((c.row == RowState::Pending || c.row == RowState::Standing) && !c.zero && c.canonical_token
                        && !c.suppressed) << describe(c);
        heads_required += need != DecisionHeadNeed::None;
        const IntentDecision got = decideIntentStep(input);
        const Expected want = expected(c);
        const String where = describe(c);

        /// An unrequired HEAD never enables a body delete. It enables an intent delete only as a
        /// transition: a HEAD showing another incarnation is itself the evidence that `t` is gone.
        if (c.head != HeadKind::NotSent && !headIsRequired(c))
        {
            ++extra_heads;
            Cell without = c;
            without.head = HeadKind::NotSent;
            const IntentDecision baseline = decide(without, o);
            EXPECT_NE(got.step, IntentStep::DeleteBody) << where;
            if (got.intent_to_delete && got.intent_to_delete != baseline.intent_to_delete)
            {
                ++deletes_shown_by_an_extra_head;
                EXPECT_EQ(c.head, HeadKind::Other) << where;
                EXPECT_TRUE(got.step == IntentStep::TransitionCreate || got.step == IntentStep::TransitionCandidate) << where;
            }
        }

        EXPECT_EQ(got.step, want.step) << where << ": got " << name(got.step) << ", expected " << name(want.step);
        EXPECT_EQ(got.intent_to_create, identityOf(want.create, c, o))
            << where << ": creates " << render(got.intent_to_create);
        EXPECT_EQ(got.intent_to_delete, identityOf(want.remove, c, o))
            << where << ": deletes " << render(got.intent_to_delete);
        EXPECT_EQ(got.new_token.has_value(), want.new_token) << where;
        /// Every new token is the one the HEAD showed, another incarnation than the row's.
        if (want.new_token && got.new_token)
            EXPECT_EQ(got.new_token->canonical(), o.other.etag.canonical()) << where;
        if (got.new_token && input.head && input.head->observed())
            EXPECT_EQ(got.new_token->value, PersistedEtag::capture(input.head->observed()->etag).value) << where;

        /// The body delete: only a pending row with a canonical token, at an unsuppressed zero cut, within the
        /// redelete budget, whose HEAD shows its own token.
        if (got.step == IntentStep::DeleteBody)
        {
            ++body_deletes;
            EXPECT_TRUE(c.row == RowState::Pending && c.canonical_token && c.zero && !c.suppressed
                        && c.redelete_budget && c.head == HeadKind::Same)
                << where;
        }
        /// The intent delete: a standing row at an unsuppressed zero cut with no transition, a transition in an
        /// unsuppressed round, or a spare of a pending or standing row in an unsuppressed round whose HEAD shows
        /// another incarnation; always the row's own canonical token.
        if (got.intent_to_delete)
        {
            ++intent_deletes;
            const bool by_removal = c.row == RowState::Standing && c.zero && !c.suppressed
                && c.head != HeadKind::Other && c.head != HeadKind::Same;
            const bool by_transition = c.row != RowState::None && c.zero && c.head == HeadKind::Other && !c.suppressed;
            const bool by_spare = (c.row == RowState::Pending || c.row == RowState::Standing) && !c.zero
                && c.head == HeadKind::Other && !c.suppressed;
            if (by_spare)
                ++spare_deletes;
            EXPECT_TRUE(by_removal || by_transition || by_spare) << where;
            EXPECT_TRUE(c.canonical_token) << where;
            EXPECT_EQ(got.intent_to_delete, o.canonical_row_token.canonical()) << where;
        }
        /// Another incarnation wins over both deletes.
        if (c.row != RowState::None && c.zero && c.head == HeadKind::Other)
        {
            ++transitions;
            EXPECT_TRUE(got.step == IntentStep::TransitionCreate || got.step == IntentStep::TransitionCandidate) << where;
        }
    });

    EXPECT_EQ(reachable_cells + unreachable_cells + unbuildable, kProductCells);
    const CellCounts want = expectedCellCounts();
    EXPECT_EQ(reachable_cells, want.reachable);
    EXPECT_EQ(heads_required, want.required_heads);
    EXPECT_EQ(extra_heads, want.extra_heads) << "a HEAD that is not required is accepted";
    EXPECT_EQ(deletes_shown_by_an_extra_head, want.deletes_shown_by_an_extra_head)
        << "transitions of untouched rows whose intent stays without the HEAD";
    EXPECT_EQ(unbuildable, want.unbuildable) << "Same is built only for a canonical row token";
    EXPECT_GT(heads_required, 0u) << "positive control";
    EXPECT_GT(body_deletes, 0u) << "positive control";
    EXPECT_GT(intent_deletes, 0u) << "positive control";
    /// Pending and standing, each over (touched) x (create budget) x (redelete budget).
    EXPECT_EQ(spare_deletes, 2u * 2 * 2 * 2) << "a spare whose HEAD shows another incarnation";
    EXPECT_GT(transitions, 0u) << "positive control";
}

#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASIntentRules, EveryUnreachableCellThrows)
{
    const Observations o = Observations::make();
    size_t unreachable_cells = 0;
    forEachCell(o, [&](const Cell & c, const IntentRuleInput & input)
    {
        if (reachable(c))
            return;
        ++unreachable_cells;
        try
        {
            const IntentDecision got = decideIntentStep(input);
            ADD_FAILURE() << describe(c) << ": expected LOGICAL_ERROR, got " << name(got.step);
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR) << describe(c);
        }
    });
    const CellCounts want = expectedCellCounts();
    EXPECT_EQ(unreachable_cells, kProductCells - want.reachable - want.unbuildable);
}
#endif

namespace
{

/// One input per check that refuses an unreachable shape, with the text that check reports.
struct Shape
{
    String what;
    IntentRuleInput input;
    String message;
};

std::vector<Shape> unreachableShapes(const Observations & o)
{
    std::vector<Shape> shapes;
    const auto add = [&](const String & what, const Cell & c, const String & message)
    {
        chassert(!reachable(c));
        shapes.push_back(Shape{what, inputOf(c, o).value(), message});
    };
    add("not a blob row", Cell{.row = RowState::NotABlob, .zero = false}, "the row is not a blob row");
    add("standing without delete_pending", Cell{.row = RowState::StandingNotPending, .zero = false},
        "a standing row without delete_pending");
    add("fresh transition without its HEAD", Cell{.fresh = true, .touched = true}, "the decision HEAD of this input is missing");
    add("pending row whose body-delete HEAD was not sent", Cell{.row = RowState::Pending},
        "the decision HEAD of this input is missing");
    add("fresh transition with a row", Cell{.row = RowState::Candidate, .fresh = true, .touched = true, .head = HeadKind::Absent},
        "fresh_transition is not");
    add("touched standing row without its HEAD", Cell{.row = RowState::Standing, .touched = true},
        "the decision HEAD of this input is missing");
    /// Without the spare `HEAD` the rules could not tell another incarnation from the row's own.
    add("spared pending row without its HEAD", Cell{.row = RowState::Pending, .zero = false, .touched = true},
        "the decision HEAD of this input is missing");
    add("spared standing row without its HEAD", Cell{.row = RowState::Standing, .zero = false},
        "the decision HEAD of this input is missing");

    /// A HEAD classified with no row, handed in beside a row whose token it shows.
    IntentRuleInput misclassified = inputOf(Cell{.row = RowState::Standing, .touched = true, .head = HeadKind::Absent}, o).value();
    misclassified.head = classifyDecisionHead(std::optional<Meta>(o.own));
    shapes.push_back(Shape{"HEAD classified against another token", misclassified, "classified against another token"});
    return shapes;
}

}

#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASIntentRules, EachUnreachableShapeThrows)
{
    const Observations o = Observations::make();
    for (const auto & shape : unreachableShapes(o))
    {
        try
        {
            const IntentDecision got = decideIntentStep(shape.input);
            ADD_FAILURE() << shape.what << ": expected LOGICAL_ERROR, got " << name(got.step);
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR) << shape.what;
            EXPECT_NE(e.message().find(shape.message), String::npos) << shape.what << ": " << e.message();
        }
    }
}
#else
TEST(CASIntentRulesDeathTest, EachUnreachableShapeThrows)
{
    const Observations o = Observations::make();
    for (const auto & shape : unreachableShapes(o))
        EXPECT_DEATH({ (void)decideIntentStep(shape.input); }, shape.message) << shape.what;
}
#endif

TEST(CASIntentRules, RespelledTokenIsTheSameIncarnation)
{
    DialectOverrideBackend backend(Dialect::ETag);
    const Meta live = liveHead(backend, "p/blobs/a", "x");
    const PersistedEtag captured = PersistedEtag::capture(live.etag);
    ASSERT_EQ(captured.dialect, "etag");
    const String bare = unquoted(captured.value);

    for (const String & spelling : {bare, "\"" + bare + "\""})
    {
        const PersistedEtag row{"etag", spelling};
        ASSERT_TRUE(sameIncarnation(row, live.etag)) << spelling;
        const DecisionHead head = classifyDecisionHead(row, live);
        EXPECT_EQ(head.seen(), HeadSeen::Same) << spelling;
        ASSERT_TRUE(head.observed().has_value());
        EXPECT_EQ(head.observed()->etag.render(), live.etag.render()) << "the raw token of this HEAD is kept for If-Match";
    }
}

TEST(CASIntentRules, HeadClassification)
{
    DialectOverrideBackend backend(Dialect::ETag);
    const Meta a = liveHead(backend, "p/blobs/a", "x");
    const Meta b = liveHead(backend, "p/blobs/b", "y");
    ASSERT_NE(a.etag.canonical(), b.etag.canonical()) << "positive control: two incarnations";
    const PersistedEtag row_a = PersistedEtag::capture(a.etag);

    EXPECT_EQ(classifyDecisionHead(row_a, a).seen(), HeadSeen::Same);
    EXPECT_EQ(classifyDecisionHead(row_a, b).seen(), HeadSeen::Other);
    EXPECT_EQ(classifyDecisionHead(row_a, std::nullopt).seen(), HeadSeen::Absent);
    EXPECT_FALSE(classifyDecisionHead(row_a, std::nullopt).observed().has_value());

    WeakEtagBackend weak;
    const Meta w = liveHead(weak, "p/blobs/w", "z");
    ASSERT_FALSE(w.etag.canonical().has_value()) << "positive control: the weak token names no incarnation";
    EXPECT_EQ(classifyDecisionHead(row_a, w).seen(), HeadSeen::Uncanonical);
    EXPECT_EQ(classifyDecisionHead(PersistedEtag::capture(w.etag), w).seen(), HeadSeen::Uncanonical)
        << "a token that names no incarnation never shows the row's own";

    /// A row whose token does not canonicalise: a canonical body is another incarnation.
    const PersistedEtag uncanonical_row{"etag", "W/\"abc\""};
    EXPECT_EQ(classifyDecisionHead(uncanonical_row, a).seen(), HeadSeen::Other);
    EXPECT_EQ(classifyDecisionHead(uncanonical_row, std::nullopt).seen(), HeadSeen::Absent);
    EXPECT_EQ(classifyDecisionHead(PersistedEtag{"nonsense", "abc"}, a).seen(), HeadSeen::Other);

    /// With no row there is no own token: a canonical body is another incarnation.
    EXPECT_EQ(classifyDecisionHead(std::optional<Meta>(a)).seen(), HeadSeen::Other);
    EXPECT_EQ(classifyDecisionHead(std::optional<Meta>(w)).seen(), HeadSeen::Uncanonical);
    EXPECT_EQ(classifyDecisionHead(std::optional<Meta>()).seen(), HeadSeen::Absent);
}

/// A spared `pending` or `standing` row needs its `HEAD`, and deletes the intent of its token only when that `HEAD`
/// shows another incarnation. The row's own token, an absent body and a token with no identity keep the intent.
TEST(CASIntentRules, SpareDeletesTheIntentOnlyOverAnotherIncarnation)
{
    const Observations o = Observations::make();
    for (const RowState row : {RowState::Pending, RowState::Standing})
        for (const bool touched : {false, true})
        {
            const Cell base{.row = row, .zero = false, .touched = touched};
            const String where = describe(base);
            EXPECT_EQ(decisionHeadNeed(inputOf(base, o).value()), DecisionHeadNeed::Spare) << where;

            Cell other = base;
            other.head = HeadKind::Other;
            const IntentDecision deletes = decide(other, o);
            EXPECT_EQ(deletes.step, IntentStep::Drop) << where;
            EXPECT_EQ(deletes.intent_to_delete, o.canonical_row_token.canonical()) << where;
            EXPECT_FALSE(deletes.intent_to_create.has_value()) << where;
            EXPECT_FALSE(deletes.new_token.has_value()) << where;

            for (const HeadKind kept : {HeadKind::Same, HeadKind::Absent, HeadKind::Uncanonical})
            {
                Cell c = base;
                c.head = kept;
                const IntentDecision got = decide(c, o);
                EXPECT_EQ(got.step, IntentStep::Drop) << where << ", HEAD " << name(kept);
                EXPECT_FALSE(got.intent_to_delete.has_value()) << where << ", HEAD " << name(kept);
            }
        }
}

/// No spare `HEAD` for a row that holds no confirmed intent of a canonical token, or in a suppressed round. A
/// `HEAD` that arrives anyway deletes nothing there.
TEST(CASIntentRules, SpareSendsNoHeadWithoutAConfirmedIntentOrInASuppressedRound)
{
    const Observations o = Observations::make();
    const std::vector<Cell> cells{
        Cell{.row = RowState::Candidate, .zero = false},
        Cell{.row = RowState::Pending, .canonical_token = false, .zero = false},
        Cell{.row = RowState::Standing, .canonical_token = false, .zero = false},
        Cell{.row = RowState::Pending, .zero = false, .suppressed = true},
        Cell{.row = RowState::Standing, .zero = false, .suppressed = true}};
    for (const Cell & base : cells)
    {
        const String where = describe(base);
        EXPECT_EQ(decisionHeadNeed(inputOf(base, o).value()), DecisionHeadNeed::None) << where;
        for (const HeadKind head : kHeadKinds)
        {
            Cell c = base;
            c.head = head;
            const auto input = inputOf(c, o);
            if (!input)
                continue;   /// `Same` needs a canonical row token
            const IntentDecision got = decideIntentStep(*input);
            EXPECT_EQ(got.step, IntentStep::Drop) << where << ", HEAD " << name(head);
            EXPECT_FALSE(got.intent_to_delete.has_value()) << where << ", HEAD " << name(head);
        }
    }
}

/// Two spellings of one token are one incarnation: a spare never reads a respelled row token as another.
TEST(CASIntentRules, SpareComparesCanonicalIdentities)
{
    DialectOverrideBackend backend(Dialect::ETag);
    const Meta live = liveHead(backend, "p/blobs/a", "x");
    const Meta replaced = liveHead(backend, "p/blobs/b", "y");
    const String bare = unquoted(PersistedEtag::capture(live.etag).value);
    for (const String & spelling : {bare, "\"" + bare + "\""})
    {
        IntentRuleInput input;
        RetiredEntry row;
        row.ref = blobRef(1);
        row.token = PersistedEtag{"etag", spelling};
        row.condemn_round = 3;
        row.delete_pending = true;
        input.row = row;
        input.key_touched = true;

        input.head = classifyDecisionHead(row.token, live);
        EXPECT_FALSE(decideIntentStep(input).intent_to_delete.has_value()) << spelling;

        /// Positive control: the same row over another incarnation deletes the intent of its canonical identity.
        input.head = classifyDecisionHead(row.token, replaced);
        EXPECT_EQ(decideIntentStep(input).intent_to_delete, live.etag.canonical()) << spelling;
    }
}

namespace
{

const DB::UInt128 kGcId = DB::UInt128(1);

struct IntentDeleteFixture
{
    std::shared_ptr<BatchCapabilityBackend> backend = std::make_shared<BatchCapabilityBackend>();
    PoolPtr store;
    std::unique_ptr<Gc> gc;
    std::vector<WriteOnceKey> keys;

    explicit IntentDeleteFixture(size_t key_count, bool create = true)
    {
        store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_bulk_delete_chunk_keys = 2});
        gc = std::make_unique<Gc>(store, kGcId);
        const CanonicalIdentity identity{Dialect::Emulated, "616263"};
        OperationForTest op(*backend);
        for (uint64_t i = 1; i <= key_count; ++i)
        {
            keys.push_back(store->layout().writeOnceBlobIntentKey(blobRef(i), identity));
            if (create)
                EXPECT_TRUE(std::holds_alternative<Committed>((*op).create(keys.back().str(), blobIntentRecord(), Retry::once())));
        }
        backend->clearJournal();
    }

    std::vector<std::vector<String>> removeRequests() const
    {
        std::vector<std::vector<String>> out;
        for (const auto & entry : backend->journal())
            if (entry.verb == JournalVerb::RemoveMany)
                out.push_back(entry.keys);
        return out;
    }

    bool anyPresent()
    {
        OperationForTest op(*backend);
        for (const auto & key : keys)
            if ((*op).head(key.str(), Retry::once()))
                return true;
        return false;
    }
};

/// Fails every `removeManyWriteOnce` whose first key is `failing_key`, with `error`.
class FailingChunkBackend : public BatchCapabilityBackend
{
public:
    void failChunkStartingAt(const String & key, std::function<void()> raise)
    {
        failing_key = key;
        raise_error = std::move(raise);
    }

    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
    {
        if (!keys.empty() && keys.front().str() == failing_key)
            raise_error();
        BatchCapabilityBackend::removeManyWriteOnce(keys, access);
    }

private:
    String failing_key;
    std::function<void()> raise_error;
};

void expectChunkFailureNamesItsKey(const std::function<void()> & raise)
{
    auto backend = std::make_shared<FailingChunkBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_bulk_delete_chunk_keys = 2});
    Gc gc(store, kGcId);
    std::vector<WriteOnceKey> keys;
    for (uint64_t i = 1; i <= 5; ++i)
        keys.push_back(store->layout().writeOnceBlobIntentKey(blobRef(i), CanonicalIdentity{Dialect::Emulated, "616263"}));
    backend->failChunkStartingAt(keys[2].str(), raise);

    FakeClock clock;
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), nullptr, kDefaultRequestWindowMs);
    CasOperation op = requests.admit();
    try
    {
        gc.sendIntentDeletes(op, keys);
        FAIL() << "the failed chunk did not throw";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_NE(e.message().find(keys[2].str()), String::npos) << e.message();
    }
    EXPECT_EQ(backend->requestSizes(), std::vector<size_t>{2})
        << "the chunk before the failed one was sent, and none after it";
}

}

TEST(CASGcIntentDeletes, ChunksShareOneBatchRequest)
{
    IntentDeleteFixture f(5);
    OperationForTest op(*f.backend);
    f.gc->sendIntentDeletes(*op, f.keys);

    const auto requests = f.removeRequests();
    ASSERT_EQ(requests.size(), 3u);
    EXPECT_EQ(requests[0], (std::vector<String>{f.keys[0].str(), f.keys[1].str()}));
    EXPECT_EQ(requests[1], (std::vector<String>{f.keys[2].str(), f.keys[3].str()}));
    EXPECT_EQ(requests[2], (std::vector<String>{f.keys[4].str()}));
    EXPECT_FALSE(f.anyPresent());
}

TEST(CASGcIntentDeletes, AbsentKeyIsSuccess)
{
    IntentDeleteFixture f(3, /*create=*/ false);
    ASSERT_FALSE(f.anyPresent()) << "positive control: no key exists";
    OperationForTest op(*f.backend);
    EXPECT_NO_THROW(f.gc->sendIntentDeletes(*op, f.keys));
    EXPECT_EQ(f.removeRequests().size(), 2u) << "the deletes were sent";
}

TEST(CASGcIntentDeletes, RefusedChunkThrowsNamingAKey)
{
    expectChunkFailureNamesItsKey([] { throw Poco::TimeoutException("the response was lost"); });
#if USE_AWS_S3
    expectChunkFailureNamesItsKey([]
    {
        throw DB::S3Exception("simulated denial", Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied");
    });
#endif
}

TEST(CASGcIntentDeletes, AbsentKeyIsSuccessOneByOne)
{
    IntentDeleteFixture f(3, /*create=*/ false);
    f.backend->setStoreRejectsBatches(true);
    ASSERT_FALSE(f.anyPresent()) << "positive control: no key exists";
    OperationForTest op(*f.backend);
    EXPECT_NO_THROW(f.gc->sendIntentDeletes(*op, f.keys));
    EXPECT_EQ(f.removeRequests().size(), 3u) << "one delete per key was sent";
}

TEST(CASGcIntentDeletes, OneByOneOnAStoreWithoutBatchDelete)
{
    IntentDeleteFixture f(5);
    f.backend->setStoreRejectsBatches(true);
    OperationForTest op(*f.backend);
    f.gc->sendIntentDeletes(*op, f.keys);

    const auto requests = f.removeRequests();
    ASSERT_EQ(requests.size(), 5u);
    for (size_t i = 0; i < requests.size(); ++i)
        EXPECT_EQ(requests[i], std::vector<String>{f.keys[i].str()});
    EXPECT_GT(f.backend->callSizes().size(), requests.size()) << "positive control: a batch was refused first";
    EXPECT_FALSE(f.anyPresent());
}
