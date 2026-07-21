#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;

namespace
{

/// ===================================================================================
/// Small builders (mirrors gtest_cas_ref_codecs.cpp's local helpers)
/// ===================================================================================

ManifestRef manifestRef(uint64_t epoch, uint64_t seq, uint32_t ordinal)
{
    return ManifestRef{epoch, seq, ordinal};
}

RefLogTxn makeTxn(const String & ns, RefTxnId id, std::vector<RefOp> ops)
{
    RefLogTxn txn;
    txn.ns = ns;
    txn.txn_id = id;
    txn.ops = std::move(ops);
    return txn;
}

RefOp birthOp()
{
    RefOp op;
    op.kind = RefOpKind::NamespaceBirth;
    return op;
}

RefOp addPrecommitOp(const String & name, const ManifestRef & mref)
{
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, name, mref};
    return op;
}

RefOp removePrecommitOp(const String & name, const ManifestRef & mref)
{
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, name, mref};
    return op;
}

RefOp promoteOp(const String & name, const ManifestRef & mref)
{
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, name, mref};
    op.new_binding = RefOwnerBinding{RefOwnerKind::Committed, name, mref};
    return op;
}

RefOp removeCommittedOp(const String & name, const ManifestRef & mref)
{
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.old_binding = RefOwnerBinding{RefOwnerKind::Committed, name, mref};
    return op;
}

RefOp setPayloadOp(const String & name, const ManifestRef & mref, const String & payload, uint64_t ts = 0)
{
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = name;
    op.expected_manifest_ref = mref;
    op.payload = payload;
    op.published_at_ms = ts;
    return op;
}

RefOp removeNamespaceOp()
{
    RefOp op;
    op.kind = RefOpKind::RemoveNamespace;
    return op;
}

/// Field-by-field comparison (via getters) rather than a `RefTableState::operator==` addition: the
/// class is the plan's verbatim-normative interface and gains no member beyond what it specifies.
void expectStatesEqual(const RefTableState & a, const RefTableState & b)
{
    EXPECT_EQ(a.getLifecycle(), b.getLifecycle());
    EXPECT_EQ(a.getRemoveTxnId(), b.getRemoveTxnId());
    EXPECT_EQ(a.getGreatestApplied(), b.getGreatestApplied());
    EXPECT_EQ(a.getCommitted(), b.getCommitted());
    EXPECT_EQ(a.getPrecommits(), b.getPrecommits());
}

/// The spec's own construction for a hypothetical `remove_namespace` transaction (§Remove Namespace):
/// an exact owner-removal op for every committed ref and precommit, then `remove_namespace`. Built
/// independently of `CasRefStateMachine.cpp`'s internal helper of the same shape, purely from the
/// public `RefTableState` fields, so the admission-budget property tests below measure against a
/// ground truth this test file derives on its own.
RefLogTxn buildRemovalTxnForTest(const RefTableState & state, const String & ns, RefTxnId id)
{
    std::vector<RefOp> ops;
    for (const auto [name, row] : state.getCommitted())
        ops.push_back(removeCommittedOp(name, row.manifest_ref));
    for (const auto & [name, mref] : state.getPrecommits())
        ops.push_back(removePrecommitOp(name, mref));
    ops.push_back(removeNamespaceOp());
    return makeTxn(ns, id, std::move(ops));
}

constexpr const char * kNs = "srv1/db/table@cas@";

}

/// ===================================================================================
/// NamespaceBirth
/// ===================================================================================

TEST(CasRefStateMachine, BirthFromNeverBornAccepts)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    EXPECT_EQ(state.getLifecycle(), RefLifecycle::Live);
    EXPECT_FALSE(state.getRemoveTxnId().has_value());
    EXPECT_EQ(state.getGreatestApplied(), (RefTxnId{1, 1}));
}

TEST(CasRefStateMachine, BirthWhileLiveRejectedAndStateUnchanged)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    const RefTableState before = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {birthOp()})); });
    expectStatesEqual(before, state);
}

TEST(CasRefStateMachine, BirthAfterRemovalAccepts)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {removeNamespaceOp()}));
    ASSERT_EQ(state.getLifecycle(), RefLifecycle::Removed);
    ASSERT_TRUE(state.getRemoveTxnId().has_value());

    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 3}, {birthOp()}));
    EXPECT_EQ(state.getLifecycle(), RefLifecycle::Live);
    EXPECT_FALSE(state.getRemoveTxnId().has_value());
}

/// ===================================================================================
/// Ops rejected outside Live (never-born and Removed) except birth
/// ===================================================================================

TEST(CasRefStateMachine, OwnerTransitionWhileNeverBornRejected)
{
    RefTableState state;
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {addPrecommitOp("a", manifestRef(1, 1, 1))})); });
}

TEST(CasRefStateMachine, SetPayloadWhileNeverBornRejected)
{
    RefTableState state;
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {setPayloadOp("a", manifestRef(1, 1, 1), "x")})); });
}

TEST(CasRefStateMachine, RemoveNamespaceWhileNeverBornRejected)
{
    RefTableState state;
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {removeNamespaceOp()})); });
}

TEST(CasRefStateMachine, OpsWhileRemovedRejectedExceptBirth)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {removeNamespaceOp()}));
    const RefTableState after_removal = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 3}, {addPrecommitOp("a", manifestRef(1, 1, 1))})); });
    expectStatesEqual(after_removal, state);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 4}, {setPayloadOp("a", manifestRef(1, 1, 1), "x")})); });
    expectStatesEqual(after_removal, state);

    /// Repeated removal is corruption at THIS layer (spec §Remove Namespace: idempotent-success is
    /// the API layer's job, not the state machine's).
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 5}, {removeNamespaceOp()})); });
    expectStatesEqual(after_removal, state);
}

/// ===================================================================================
/// Add precommit (spec §Add Precommit)
/// ===================================================================================

TEST(CasRefStateMachine, AddPrecommitAccepts)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1))}));
    EXPECT_TRUE(state.getPrecommits().contains({"a", manifestRef(1, 1, 1)}));
}

TEST(CasRefStateMachine, AddPrecommitRejectsExactDuplicate)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1))}));
    const RefTableState before = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {addPrecommitOp("a", manifestRef(1, 1, 1))})); });
    expectStatesEqual(before, state);
}

TEST(CasRefStateMachine, AddPrecommitRejectsConflictingManifestUnderDifferentName)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1))}));
    const RefTableState before = state;

    /// Same manifest_ref, a DIFFERENT ref_name: "no conflicting owner may name the same manifest".
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {addPrecommitOp("b", manifestRef(1, 1, 1))})); });
    expectStatesEqual(before, state);
}

TEST(CasRefStateMachine, AddPrecommitRejectsManifestAlreadyCommittedElsewhere)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1)), promoteOp("a", manifestRef(1, 1, 1))}));
    ASSERT_TRUE(state.getCommitted().contains("a"));
    const RefTableState before = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {addPrecommitOp("b", manifestRef(1, 1, 1))})); });
    expectStatesEqual(before, state);
}

TEST(CasRefStateMachine, AddPrecommitAllowsDifferentManifestsRacingForSameName)
{
    /// Two builds racing for the same final ref name (same shape gtest_cas_ref_codecs.cpp's
    /// RoundTripPrecommitsSameNameDifferentManifest round-trips): distinct manifest_ref, no conflict.
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("same", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {addPrecommitOp("same", manifestRef(1, 2, 1))}));
    EXPECT_TRUE(state.getPrecommits().contains({"same", manifestRef(1, 1, 1)}));
    EXPECT_TRUE(state.getPrecommits().contains({"same", manifestRef(1, 2, 1)}));
}

/// ===================================================================================
/// Remove precommit / remove committed (spec §Remove Precommit, §Remove Committed Ref)
/// ===================================================================================

TEST(CasRefStateMachine, RemovePrecommitAccepts)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {removePrecommitOp("a", manifestRef(1, 1, 1))}));
    EXPECT_TRUE(state.getPrecommits().empty());
}

TEST(CasRefStateMachine, RemovePrecommitRejectsAbsentBinding)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    const RefTableState before = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {removePrecommitOp("a", manifestRef(1, 1, 1))})); });
    expectStatesEqual(before, state);
}

TEST(CasRefStateMachine, RemovePrecommitRejectsWrongManifest)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1))}));
    const RefTableState before = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {removePrecommitOp("a", manifestRef(1, 2, 1))})); });
    expectStatesEqual(before, state);
}

TEST(CasRefStateMachine, RemoveCommittedAccepts)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1)), promoteOp("a", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {removeCommittedOp("a", manifestRef(1, 1, 1))}));
    EXPECT_TRUE(state.getCommitted().empty());
}

TEST(CasRefStateMachine, RemoveCommittedRejectsAbsentRef)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    const RefTableState before = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {removeCommittedOp("a", manifestRef(1, 1, 1))})); });
    expectStatesEqual(before, state);
}

TEST(CasRefStateMachine, RemoveCommittedRejectsWrongManifest)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1)), promoteOp("a", manifestRef(1, 1, 1))}));
    const RefTableState before = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {removeCommittedOp("a", manifestRef(9, 9, 9))})); });
    expectStatesEqual(before, state);
}

/// ===================================================================================
/// Promote (spec §Promote): exact precommit required, atomicity, invalid shapes
/// ===================================================================================

TEST(CasRefStateMachine, PromoteRejectsAbsentPrecommit)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    const RefTableState before = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {promoteOp("a", manifestRef(1, 1, 1))})); });
    expectStatesEqual(before, state);
}

TEST(CasRefStateMachine, PromoteAtomicityNoOwnerlessIntermediateEmptyPayload)
{
    /// A bare promote (no set_payload in the same transaction) is itself a complete, valid, and
    /// OBSERVABLE transaction -- there is no partial-op state exposed here, only the choice of
    /// whether the payload arrives in this txn or a later one (spec §Promote).
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {promoteOp("a", manifestRef(1, 1, 1))}));

    EXPECT_FALSE(state.getPrecommits().contains({"a", manifestRef(1, 1, 1)}));
    ASSERT_TRUE(state.getCommitted().contains("a"));
    EXPECT_EQ(state.getCommitted().at("a").manifest_ref, manifestRef(1, 1, 1));
    EXPECT_EQ(state.getCommitted().at("a").payload, "");
}

TEST(CasRefStateMachine, PromoteWithPayloadInSameTxnInstallsPayload)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2},
        {promoteOp("a", manifestRef(1, 1, 1)), setPayloadOp("a", manifestRef(1, 1, 1), "initial-payload", 42)}));

    ASSERT_TRUE(state.getCommitted().contains("a"));
    EXPECT_EQ(state.getCommitted().at("a").payload, "initial-payload");
    EXPECT_EQ(state.getCommitted().at("a").published_at_ms, 42u);
}

TEST(CasRefStateMachine, PromoteRejectsDisplacingAnotherCommittedManifest)
{
    /// A challenger precommit under the SAME ref_name as an already-committed (different) manifest is
    /// legal to stage (spec §Add Precommit only restricts manifest identity, not ref_name), but a bare
    /// promote of it must not silently displace the stale committed row.
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1)), promoteOp("a", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {addPrecommitOp("a", manifestRef(1, 2, 1))}));
    const RefTableState before = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 3}, {promoteOp("a", manifestRef(1, 2, 1))})); });
    expectStatesEqual(before, state);
}

TEST(CasRefStateMachine, PromoteAcceptsAfterExplicitRemovalOfStaleCommitted)
{
    /// The correct atomic-replace sequence: an explicit removal of the old committed row, followed by
    /// the promote, in the SAME transaction -- both ops are recorded, so GC sees the old manifest's
    /// "-1" edge explicitly rather than losing it to a silent displacement.
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1)), promoteOp("a", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {addPrecommitOp("a", manifestRef(1, 2, 1))}));

    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 3},
        {removeCommittedOp("a", manifestRef(1, 1, 1)), promoteOp("a", manifestRef(1, 2, 1))}));

    ASSERT_TRUE(state.getCommitted().contains("a"));
    EXPECT_EQ(state.getCommitted().at("a").manifest_ref, manifestRef(1, 2, 1));
    EXPECT_FALSE(state.getPrecommits().contains({"a", manifestRef(1, 2, 1)}));
}

TEST(CasRefStateMachine, OwnerTransitionRejectsInvalidCombinations)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));

    /// old=None, new=Committed: not a recognized shape (committed rows are only reached via promote).
    {
        RefOp op;
        op.kind = RefOpKind::OwnerTransition;
        op.new_binding = RefOwnerBinding{RefOwnerKind::Committed, "a", manifestRef(1, 1, 1)};
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
            [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {op})); });
    }

    /// A promote-shaped op (Precommit -> Committed) with mismatched ref_name is not a legal promote.
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 3}, {addPrecommitOp("a", manifestRef(1, 1, 1))}));
    {
        RefOp op;
        op.kind = RefOpKind::OwnerTransition;
        op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, "a", manifestRef(1, 1, 1)};
        op.new_binding = RefOwnerBinding{RefOwnerKind::Committed, "b", manifestRef(1, 1, 1)};
        const RefTableState before = state;
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
            [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 4}, {op})); });
        expectStatesEqual(before, state);
    }

    /// old=Committed, new=Precommit: moving a committed ref "backwards" is not a recognized shape.
    {
        RefOp op;
        op.kind = RefOpKind::OwnerTransition;
        op.old_binding = RefOwnerBinding{RefOwnerKind::Committed, "a", manifestRef(1, 1, 1)};
        op.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, "a", manifestRef(1, 1, 1)};
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
            [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 5}, {op})); });
    }
}

/// ===================================================================================
/// SetPayload (spec §Update Payload)
/// ===================================================================================

TEST(CasRefStateMachine, SetPayloadRejectsWhenRefAbsent)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    const RefTableState before = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {setPayloadOp("a", manifestRef(1, 1, 1), "x")})); });
    expectStatesEqual(before, state);
}

TEST(CasRefStateMachine, SetPayloadRejectsManifestMismatch)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1)), promoteOp("a", manifestRef(1, 1, 1))}));
    const RefTableState before = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {setPayloadOp("a", manifestRef(9, 9, 9), "x")})); });
    expectStatesEqual(before, state);
}

TEST(CasRefStateMachine, SetPayloadAcceptsAndReplacesPayload)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1)), promoteOp("a", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {setPayloadOp("a", manifestRef(1, 1, 1), "v1", 10)}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 3}, {setPayloadOp("a", manifestRef(1, 1, 1), "v2", 20)}));

    EXPECT_EQ(state.getCommitted().at("a").payload, "v2");
    EXPECT_EQ(state.getCommitted().at("a").published_at_ms, 20u);
    EXPECT_EQ(state.getCommitted().at("a").manifest_ref, manifestRef(1, 1, 1));   /// unchanged: no edge move
}

/// ===================================================================================
/// RemoveNamespace ordering lens (spec §Remove Namespace; codec deliberately doesn't check this)
/// ===================================================================================

TEST(CasRefStateMachine, RemoveNamespaceAloneOnEmptyTableAccepted)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {removeNamespaceOp()}));
    EXPECT_EQ(state.getLifecycle(), RefLifecycle::Removed);
    ASSERT_TRUE(state.getRemoveTxnId().has_value());
    EXPECT_EQ(*state.getRemoveTxnId(), (RefTxnId{1, 2}));
}

TEST(CasRefStateMachine, RemoveNamespaceDrainingOwnersInSameTxnAccepted)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1)), addPrecommitOp("b", manifestRef(1, 2, 1)),
         promoteOp("b", manifestRef(1, 2, 1))}));
    ASSERT_TRUE(state.getPrecommits().contains({"a", manifestRef(1, 1, 1)}));
    ASSERT_TRUE(state.getCommitted().contains("b"));

    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2},
        {removePrecommitOp("a", manifestRef(1, 1, 1)), removeCommittedOp("b", manifestRef(1, 2, 1)),
         removeNamespaceOp()}));
    EXPECT_EQ(state.getLifecycle(), RefLifecycle::Removed);
    EXPECT_TRUE(state.getCommitted().empty());
    EXPECT_TRUE(state.getPrecommits().empty());
}

TEST(CasRefStateMachine, RemoveNamespaceRejectsWhenOwnersRemain)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1)), addPrecommitOp("b", manifestRef(1, 2, 1))}));
    const RefTableState before = state;

    /// Only "a" is drained; "b" remains -- remove_namespace's own precondition (empty owner sets)
    /// must fail, and the WHOLE transaction (including the "a" removal) must not apply.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2},
            {removePrecommitOp("a", manifestRef(1, 1, 1)), removeNamespaceOp()})); });
    expectStatesEqual(before, state);
}

TEST(CasRefStateMachine, RemoveNamespaceMustBeFinalOp)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    const RefTableState before = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {removeNamespaceOp(), birthOp()})); });
    expectStatesEqual(before, state);
}

TEST(CasRefStateMachine, RemoveNamespaceRejectsNonRemovalEarlierOp)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1)), promoteOp("a", manifestRef(1, 1, 1))}));
    const RefTableState before = state;

    /// set_payload before remove_namespace: not an owner-removal transition.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2},
            {setPayloadOp("a", manifestRef(1, 1, 1), "x"), removeCommittedOp("a", manifestRef(1, 1, 1)),
             removeNamespaceOp()})); });
    expectStatesEqual(before, state);

    /// An ADD (not a removal) owner_transition before remove_namespace: also rejected.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 3},
            {addPrecommitOp("c", manifestRef(1, 3, 1)), removeCommittedOp("a", manifestRef(1, 1, 1)),
             removeNamespaceOp()})); });
    expectStatesEqual(before, state);
}

/// ===================================================================================
/// Whole-transaction atomicity: a failing LAST op leaves the whole txn (and earlier ops) unapplied
/// ===================================================================================

TEST(CasRefStateMachine, WholeTxnAtomicityLastOpFailureLeavesStateUntouched)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    const RefTableState before = state;

    /// ops[0] (add "a") would succeed in isolation; ops[1] (remove absent "b") fails -- the whole
    /// transaction, including "a", must be rejected.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2},
            {addPrecommitOp("a", manifestRef(1, 1, 1)), removePrecommitOp("b", manifestRef(9, 9, 9))})); });

    expectStatesEqual(before, state);
    EXPECT_FALSE(state.getPrecommits().contains({"a", manifestRef(1, 1, 1)}));
}

/// ===================================================================================
/// Strictly increasing txn ids
/// ===================================================================================

TEST(CasRefStateMachine, StrictlyIncreasingTxnIdsRejectsEqualAndLower)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 5}, {birthOp()}));
    const RefTableState before = state;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 5}, {addPrecommitOp("a", manifestRef(1, 1, 1))})); });
    expectStatesEqual(before, state);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 4}, {addPrecommitOp("a", manifestRef(1, 1, 1))})); });
    expectStatesEqual(before, state);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { applyRefLogTxn(state, makeTxn(kNs, RefTxnId{0, 999}, {addPrecommitOp("a", manifestRef(1, 1, 1))})); });
    expectStatesEqual(before, state);

    /// A gap is fine -- only strict increase is required (spec §Ordered Ref Transaction Identifier).
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 100}, {addPrecommitOp("a", manifestRef(1, 1, 1))}));
    EXPECT_EQ(state.getGreatestApplied(), (RefTxnId{1, 100}));
}

/// ===================================================================================
/// snapshotOf: canonical sort + Removed shape
/// ===================================================================================

TEST(CasRefStateMachine, SnapshotOfSortsCommittedAndPrecommits)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("zzz", manifestRef(1, 3, 1)), addPrecommitOp("aaa", manifestRef(1, 1, 1)),
         promoteOp("aaa", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {addPrecommitOp("mmm", manifestRef(1, 2, 1))}));

    const RefTableSnapshot snap = snapshotOf(state, kNs);
    ASSERT_EQ(snap.committed.size(), 1u);
    EXPECT_EQ(snap.committed[0].ref_name, "aaa");
    ASSERT_EQ(snap.precommits.size(), 2u);
    EXPECT_EQ(snap.precommits[0].ref_name, "mmm");
    EXPECT_EQ(snap.precommits[1].ref_name, "zzz");
    EXPECT_EQ(snap.snapshot_id, (RefTxnId{1, 2}));

    /// The result must actually be encodable (canonical shape) -- a real round trip through the codec.
    const String bytes = encodeRefTableSnapshot(snap);
    const RefTableSnapshot decoded = decodeRefTableSnapshot(bytes, kNs, snap.snapshot_id);
    EXPECT_EQ(decoded, snap);
}

TEST(CasRefStateMachine, SnapshotOfRemovedIsEmptyWithRemoveTxnId)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {removeNamespaceOp()}));

    const RefTableSnapshot snap = snapshotOf(state, kNs);
    EXPECT_EQ(snap.lifecycle, RefLifecycle::Removed);
    ASSERT_TRUE(snap.remove_txn_id.has_value());
    EXPECT_EQ(*snap.remove_txn_id, (RefTxnId{1, 2}));
    EXPECT_TRUE(snap.committed.empty());
    EXPECT_TRUE(snap.precommits.empty());
}

/// ===================================================================================
/// replay: TableState = Replay(S_X.state, tail(X))
/// ===================================================================================

TEST(CasRefStateMachine, ReplayFromNoSnapshot)
{
    std::vector<RefLogTxn> tail{
        makeTxn(kNs, RefTxnId{1, 1}, {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1))}),
        makeTxn(kNs, RefTxnId{1, 2}, {promoteOp("a", manifestRef(1, 1, 1))}),
    };
    const RefTableState state = replay(std::nullopt, tail);
    EXPECT_EQ(state.getLifecycle(), RefLifecycle::Live);
    EXPECT_TRUE(state.getCommitted().contains("a"));
    EXPECT_EQ(state.getGreatestApplied(), (RefTxnId{1, 2}));
}

TEST(CasRefStateMachine, ReplayFromSnapshotPlusTail)
{
    RefTableState built;
    applyRefLogTxn(built, makeTxn(kNs, RefTxnId{1, 1}, {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1))}));
    const RefTableSnapshot snap = snapshotOf(built, kNs);

    std::vector<RefLogTxn> tail{makeTxn(kNs, RefTxnId{1, 2}, {promoteOp("a", manifestRef(1, 1, 1))})};
    const RefTableState state = replay(snap, tail);
    EXPECT_TRUE(state.getCommitted().contains("a"));
    EXPECT_EQ(state.getGreatestApplied(), (RefTxnId{1, 2}));
}

TEST(CasRefStateMachine, ReplayRejectsTailNsMismatchAgainstSnapshot)
{
    RefTableState built;
    applyRefLogTxn(built, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    const RefTableSnapshot snap = snapshotOf(built, kNs);

    std::vector<RefLogTxn> tail{makeTxn("other-ns", RefTxnId{1, 2}, {addPrecommitOp("a", manifestRef(1, 1, 1))})};
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { replay(snap, tail); });
}

TEST(CasRefStateMachine, ReplayRejectsTailNsMismatchAcrossEntries)
{
    std::vector<RefLogTxn> tail{
        makeTxn("ns-a", RefTxnId{1, 1}, {birthOp()}),
        makeTxn("ns-b", RefTxnId{1, 2}, {addPrecommitOp("a", manifestRef(1, 1, 1))}),
    };
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { replay(std::nullopt, tail); });
}

TEST(CasRefStateMachine, ReplayRejectsHandBuiltSnapshotWithDuplicateCommittedName)
{
    /// A hand-built RefTableSnapshot (never passed through decodeRefTableSnapshot -- exactly what
    /// fsck hands to replay) with two committed rows sharing one ref_name must be rejected, not
    /// silently collapsed to one row via std::map::emplace (the phantom-alive class of bug fixed in
    /// stateFromSnapshot).
    RefTableSnapshot snap;
    snap.ns = kNs;
    snap.snapshot_id = RefTxnId{1, 1};
    snap.lifecycle = RefLifecycle::Live;
    RefCommittedRow row1;
    row1.ref_name = "a";
    row1.manifest_ref = manifestRef(1, 1, 1);
    RefCommittedRow row2;
    row2.ref_name = "a";
    row2.manifest_ref = manifestRef(1, 2, 1);
    snap.committed.push_back(row1);
    snap.committed.push_back(row2);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { replay(snap, {}); });
}

TEST(CasRefStateMachine, ReplayRejectsHandBuiltSnapshotWithUnsortedPrecommits)
{
    RefTableSnapshot snap;
    snap.ns = kNs;
    snap.snapshot_id = RefTxnId{1, 1};
    snap.lifecycle = RefLifecycle::Live;
    snap.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "b", manifestRef(1, 1, 1)});
    snap.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "a", manifestRef(1, 2, 1)});

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { replay(snap, {}); });
}

/// Randomized replay equation: replay(snapshotOf(mid-state), tail) == full replay (spec §Table State).
TEST(CasRefStateMachine, ReplayEquationPropertyTest)
{
    std::mt19937 rng(4242); // NOLINT(cert-msc32-c,cert-msc51-cpp): deterministic seed is required for reproducible property coverage.
    const std::vector<String> names{"a", "b", "c"};

    for (int trial = 0; trial < 30; ++trial)
    {
        std::vector<RefLogTxn> history;
        uint64_t seq = 1;
        history.push_back(makeTxn(kNs, RefTxnId{1, seq++}, {birthOp()}));

        /// Track our own model of legal next actions so every generated op is guaranteed valid --
        /// this test exercises the replay equation, not the rejection paths (covered above).
        std::vector<std::pair<String, ManifestRef>> open_precommits;
        std::vector<std::pair<String, ManifestRef>> open_committed;
        uint64_t next_build_seq = 1;

        const int steps = 15;
        for (int step = 0; step < steps; ++step)
        {
            const uint32_t choice = rng() % 4;
            if (choice == 0 || (open_precommits.empty() && open_committed.empty()))
            {
                /// Add precommit under a fresh manifest_ref (never collides, so always legal).
                const String & name = names[rng() % names.size()];
                const ManifestRef mref = manifestRef(1, next_build_seq++, 1);
                history.push_back(makeTxn(kNs, RefTxnId{1, seq++}, {addPrecommitOp(name, mref)}));
                open_precommits.emplace_back(name, mref);
            }
            else if (choice == 1 && !open_precommits.empty())
            {
                /// Only a name NOT already committed is eligible for a BARE promote: promoting into an
                /// already-committed name requires an explicit prior removal in the same transaction
                /// (spec §Promote; see PromoteRejectsDisplacingAnotherCommittedManifest) -- a distinct
                /// scenario from the one this equation test exercises.
                std::vector<size_t> eligible;
                for (size_t i = 0; i < open_precommits.size(); ++i)
                {
                    const bool already_committed = std::any_of(open_committed.begin(), open_committed.end(),
                        [&](const auto & c) { return c.first == open_precommits[i].first; });
                    if (!already_committed)
                        eligible.push_back(i);
                }
                if (!eligible.empty())
                {
                    const size_t idx = eligible[rng() % eligible.size()];
                    const auto [name, mref] = open_precommits[idx];
                    open_precommits.erase(open_precommits.begin() + static_cast<int64_t>(idx));
                    history.push_back(makeTxn(kNs, RefTxnId{1, seq++}, {promoteOp(name, mref)}));
                    open_committed.emplace_back(name, mref);
                }
            }
            else if (choice == 2 && !open_committed.empty())
            {
                const size_t idx = rng() % open_committed.size();
                const auto & [name, mref] = open_committed[idx];
                const uint64_t this_id = seq++;
                history.push_back(makeTxn(kNs, RefTxnId{1, this_id},
                    {setPayloadOp(name, mref, "payload-" + std::to_string(this_id))}));
            }
            else if (!open_precommits.empty())
            {
                const size_t idx = rng() % open_precommits.size();
                const auto [name, mref] = open_precommits[idx];
                open_precommits.erase(open_precommits.begin() + static_cast<int64_t>(idx));
                history.push_back(makeTxn(kNs, RefTxnId{1, seq++}, {removePrecommitOp(name, mref)}));
            }
            else if (!open_committed.empty())
            {
                const size_t idx = rng() % open_committed.size();
                const auto [name, mref] = open_committed[idx];
                open_committed.erase(open_committed.begin() + static_cast<int64_t>(idx));
                history.push_back(makeTxn(kNs, RefTxnId{1, seq++}, {removeCommittedOp(name, mref)}));
            }
        }

        const RefTableState full = replay(std::nullopt, history);

        const size_t cut = rng() % (history.size() + 1);
        const std::vector<RefLogTxn> head(history.begin(), history.begin() + static_cast<int64_t>(cut));
        const std::vector<RefLogTxn> tail(history.begin() + static_cast<int64_t>(cut), history.end());
        const RefTableState mid = replay(std::nullopt, head);
        const std::optional<RefTableSnapshot> mid_snapshot =
            cut == 0 ? std::nullopt : std::make_optional(snapshotOf(mid, kNs));
        const RefTableState resumed = replay(mid_snapshot, tail);

        expectStatesEqual(full, resumed);
    }
}

/// ===================================================================================
/// admits(): dual-bound admission budget (spec §Snapshot Format)
/// ===================================================================================

TEST(CasRefStateMachine, AdmitsAcceptsWellUnderBudget)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    EXPECT_TRUE(admits(state, addPrecommitOp("a", manifestRef(1, 1, 1)), 1'000'000, 1'000'000));
}

TEST(CasRefStateMachine, AdmitsRejectsGrowthPastSnapshotBudgetOwnerTransitionAdd)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));

    const RefOp op = addPrecommitOp("a", manifestRef(1, 1, 1));
    RefTableState scratch = state;
    applyRefLogTxn(scratch, makeTxn(kNs, RefTxnId{1, 2}, {op}));
    const size_t true_size = encodeRefTableSnapshot(snapshotOf(scratch, "")).size();

    EXPECT_TRUE(admits(state, op, true_size, 1'000'000));
    EXPECT_FALSE(admits(state, op, true_size - 1, 1'000'000));
}

TEST(CasRefStateMachine, AdmitsRejectsGrowthPastSnapshotBudgetSetPayload)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1)), promoteOp("a", manifestRef(1, 1, 1))}));

    const RefOp op = setPayloadOp("a", manifestRef(1, 1, 1), String(200, 'x'));
    RefTableState scratch = state;
    applyRefLogTxn(scratch, makeTxn(kNs, RefTxnId{1, 2}, {op}));
    const size_t true_size = encodeRefTableSnapshot(snapshotOf(scratch, "")).size();

    EXPECT_TRUE(admits(state, op, true_size, 1'000'000));
    EXPECT_FALSE(admits(state, op, true_size - 1, 1'000'000));
}

TEST(CasRefStateMachine, AdmitsRejectsGrowthPastSnapshotBudgetPromoteWithPayload)
{
    /// The "promote-with-payload" growth class: the owner_transition half of a promote is admitted
    /// cheaply (empty payload), but the immediately-following set_payload that installs the REAL
    /// initial payload is where the growth actually happens.
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {promoteOp("a", manifestRef(1, 1, 1))}));
    ASSERT_TRUE(state.getCommitted().contains("a"));
    ASSERT_EQ(state.getCommitted().at("a").payload, "");

    const RefOp op = setPayloadOp("a", manifestRef(1, 1, 1), String(500, 'y'), 99);
    RefTableState scratch = state;
    applyRefLogTxn(scratch, makeTxn(kNs, RefTxnId{1, 3}, {op}));
    const size_t true_size = encodeRefTableSnapshot(snapshotOf(scratch, "")).size();

    EXPECT_TRUE(admits(state, op, true_size, 1'000'000));
    EXPECT_FALSE(admits(state, op, true_size - 1, 1'000'000));
}

TEST(CasRefStateMachine, AdmitsRejectsGrowthPastRemovalBudget)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(), addPrecommitOp("a", manifestRef(1, 1, 1)), promoteOp("a", manifestRef(1, 1, 1))}));

    const RefOp op = setPayloadOp("a", manifestRef(1, 1, 1), String(300, 'z'));
    RefTableState scratch = state;
    applyRefLogTxn(scratch, makeTxn(kNs, RefTxnId{1, 2}, {op}));
    const String removal_bytes = encodeRefLogTxn(buildRemovalTxnForTest(scratch, "", RefTxnId{1, 1}));
    const size_t true_removal_size = removal_bytes.size();

    /// A generous snapshot budget isolates the removal-budget bound specifically.
    EXPECT_TRUE(admits(state, op, 1'000'000, true_removal_size));
    EXPECT_FALSE(admits(state, op, 1'000'000, true_removal_size - 1));
}

/// Randomized exactness property test: admits()'s internal size computation must exactly match the
/// real encoders' output, for both bounds, across randomized states and candidate growing ops.
TEST(CasRefStateMachine, AdmitsExactnessPropertyTest)
{
    std::mt19937 rng(777); // NOLINT(cert-msc32-c,cert-msc51-cpp): deterministic seed is required for reproducible property coverage.

    for (int trial = 0; trial < 20; ++trial)
    {
        RefTableState state;
        applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
        uint64_t seq = 2;
        uint64_t next_build_seq = 1;
        std::vector<std::pair<String, ManifestRef>> open_precommits;
        std::vector<std::pair<String, ManifestRef>> open_committed;

        /// Build up a random but valid mid-state (a handful of precommits/committed rows/payloads).
        const int setup_steps = 1 + static_cast<int>(rng() % 5);
        for (int i = 0; i < setup_steps; ++i)
        {
            const String name = "ref" + std::to_string(rng() % 4);
            const ManifestRef mref = manifestRef(1, next_build_seq++, 1);
            applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, seq++}, {addPrecommitOp(name, mref)}));
            open_precommits.emplace_back(name, mref);

            /// A bare promote may not target a name already committed under a different manifest
            /// (spec §Promote; see PromoteRejectsDisplacingAnotherCommittedManifest) -- skip promoting
            /// this iteration's precommit when an earlier iteration already committed the same name.
            const bool name_already_committed = std::any_of(open_committed.begin(), open_committed.end(),
                [&](const auto & c) { return c.first == name; });
            if (!name_already_committed && rng() % 2 == 0)
            {
                const auto [pname, pmref] = open_precommits.back();
                open_precommits.pop_back();
                applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, seq++}, {promoteOp(pname, pmref)}));
                open_committed.emplace_back(pname, pmref);
            }
        }

        /// Pick a random candidate growing op against this state.
        RefOp candidate;
        const uint32_t kind = rng() % 3;
        if (kind == 0 || open_committed.empty())
        {
            candidate = addPrecommitOp("fresh-" + std::to_string(trial), manifestRef(1, next_build_seq++, 1));
        }
        else if (kind == 1)
        {
            const auto & [name, mref] = open_committed[rng() % open_committed.size()];
            candidate = setPayloadOp(name, mref, String(1 + rng() % 64, 'q'), rng());
        }
        else
        {
            /// A genuinely distinct third shape: a racing precommit under an ALREADY-committed name
            /// (legal -- spec §Add Precommit only restricts manifest identity, never ref_name).
            const String & name = open_committed[rng() % open_committed.size()].first;
            candidate = addPrecommitOp(name, manifestRef(1, next_build_seq++, 1));
        }

        RefTableState scratch = state;
        applyRefLogTxn(scratch, makeTxn(kNs, RefTxnId{1, seq}, {candidate}));
        const size_t true_snapshot_size = encodeRefTableSnapshot(snapshotOf(scratch, "")).size();
        const size_t true_removal_size =
            encodeRefLogTxn(buildRemovalTxnForTest(scratch, "", RefTxnId{1, 1})).size();

        EXPECT_TRUE(admits(state, candidate, true_snapshot_size, true_removal_size));
        EXPECT_FALSE(admits(state, candidate, true_snapshot_size - 1, true_removal_size));
        EXPECT_FALSE(admits(state, candidate, true_snapshot_size, true_removal_size - 1));
    }
}

/// ===================================================================================
/// Snapshot size helpers: framing + Σ per-row must equal a full encode, byte for byte.
/// ===================================================================================
TEST(CasRefSnapshotSizeHelpers, FramingPlusRowsEqualsFullEncode)
{
    /// Build a non-trivial Live table: two committed rows (one with a payload) and one precommit.
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(),
         addPrecommitOp("alpha", manifestRef(1, 1, 1)), promoteOp("alpha", manifestRef(1, 1, 1)),
         addPrecommitOp("beta", manifestRef(1, 2, 1)), promoteOp("beta", manifestRef(1, 2, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2},
        {setPayloadOp("alpha", manifestRef(1, 1, 1), String(123, 'p'), 42)}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 3}, {addPrecommitOp("gamma", manifestRef(1, 3, 1))}));

    const RefTableSnapshot snap = snapshotOf(state, "");
    const size_t full = encodeRefTableSnapshot(snap).size();

    size_t rebuilt = snapshotFramingSize("", snap.snapshot_id, snap.lifecycle,
                                         snap.remove_txn_id, snap.sealed_from,
                                         snap.committed.size() + snap.precommits.size());
    for (const RefCommittedRow & row : snap.committed)
        rebuilt += committedRowEncodedSize(row);
    for (const RefOwnerBinding & pc : snap.precommits)
        rebuilt += precommitRowEncodedSize(pc);

    EXPECT_EQ(rebuilt, full);
}

/// ===================================================================================
/// Removal-txn size helpers: framing + Σ per-owner-op must equal a full removal-txn encode.
/// ===================================================================================
TEST(CasRefLogSizeHelpers, FramingPlusOpsEqualsFullRemovalEncode)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(),
         addPrecommitOp("alpha", manifestRef(1, 1, 1)), promoteOp("alpha", manifestRef(1, 1, 1)),
         addPrecommitOp("beta", manifestRef(1, 2, 1))}));

    /// Ground truth: the whole-namespace removal txn this test file already builds independently.
    const RefLogTxn removal = buildRemovalTxnForTest(state, "", RefTxnId{1, 1});
    const size_t full = encodeRefLogTxn(removal).size();

    size_t rebuilt = removalFramingSize("", RefTxnId{1, 1},
                                        state.getCommitted().size() + state.getPrecommits().size() + 1);
    for (const auto [name, row] : state.getCommitted())
        rebuilt += removalOpEncodedSize(RefOwnerKind::Committed, name, row.manifest_ref);
    for (const auto & [name, mref] : state.getPrecommits())
        rebuilt += removalOpEncodedSize(RefOwnerKind::Precommit, name, mref);

    EXPECT_EQ(rebuilt, full);
}

/// ===================================================================================
/// Body-byte counters: snapshot_body_bytes / removal_body_bytes are a pure function of the rows.
/// ===================================================================================
namespace
{
uint64_t recomputeSnapshotBody(const RefTableState & s)
{
    uint64_t total = 0;
    for (const auto [name, row] : s.getCommitted())
        total += committedRowEncodedSize(row);
    for (const auto & [name, mref] : s.getPrecommits())
        total += precommitRowEncodedSize(RefOwnerBinding{RefOwnerKind::Precommit, name, mref});
    return total;
}
uint64_t recomputeRemovalBody(const RefTableState & s)
{
    uint64_t total = 0;
    for (const auto [name, row] : s.getCommitted())
        total += removalOpEncodedSize(RefOwnerKind::Committed, name, row.manifest_ref);
    for (const auto & [name, mref] : s.getPrecommits())
        total += removalOpEncodedSize(RefOwnerKind::Precommit, name, mref);
    return total;
}
}

TEST(CasRefStateCounters, CountersTrackRowsThroughEveryOpKind)
{
    RefTableState state;
    EXPECT_EQ(state.getSnapshotBodyBytes(), 0u);
    EXPECT_EQ(state.getRemovalBodyBytes(), 0u);

    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {addPrecommitOp("a", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 3}, {promoteOp("a", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 4},
        {setPayloadOp("a", manifestRef(1, 1, 1), String(77, 'x'), 5)}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 5}, {addPrecommitOp("b", manifestRef(1, 2, 1))}));
    EXPECT_EQ(state.getSnapshotBodyBytes(), recomputeSnapshotBody(state));
    EXPECT_EQ(state.getRemovalBodyBytes(), recomputeRemovalBody(state));

    /// Shrink back down: remove the precommit, then the committed row.
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 6}, {removePrecommitOp("b", manifestRef(1, 2, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 7}, {removeCommittedOp("a", manifestRef(1, 1, 1))}));
    EXPECT_EQ(state.getSnapshotBodyBytes(), recomputeSnapshotBody(state));
    EXPECT_EQ(state.getRemovalBodyBytes(), recomputeRemovalBody(state));
    EXPECT_EQ(state.getSnapshotBodyBytes(), 0u);
    EXPECT_EQ(state.getRemovalBodyBytes(), 0u);
}

/// ===================================================================================
/// Budget-size accessors equal the real encoders across randomized states.
/// ===================================================================================
TEST(CasRefBudgetSize, AccessorsEqualFullEncodeRandomized)
{
    std::mt19937 rng(1234); // NOLINT(cert-msc32-c,cert-msc51-cpp): deterministic seed for reproducibility.
    for (int trial = 0; trial < 30; ++trial)
    {
        RefTableState state;
        applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
        uint64_t seq = 2;
        uint64_t build = 1;
        std::vector<std::pair<String, ManifestRef>> committed_names;

        const int steps = 1 + static_cast<int>(rng() % 6);
        for (int i = 0; i < steps; ++i)
        {
            const String name = "r" + std::to_string(rng() % 5);
            const ManifestRef mref = manifestRef(1, build++, 1);
            applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, seq++}, {addPrecommitOp(name, mref)}));
            const bool already = std::any_of(committed_names.begin(), committed_names.end(),
                [&](const auto & c) { return c.first == name; });
            if (!already && rng() % 2 == 0)
            {
                applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, seq++}, {promoteOp(name, mref)}));
                committed_names.emplace_back(name, mref);
                if (rng() % 2 == 0)
                    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, seq++},
                        {setPayloadOp(name, mref, String(rng() % 50, 's'), rng())}));
            }
        }

        const size_t true_snapshot = encodeRefTableSnapshot(snapshotOf(state, "")).size();
        const size_t true_removal = encodeRefLogTxn(buildRemovalTxnForTest(state, "", RefTxnId{1, 1})).size();
        EXPECT_EQ(encodedSnapshotBudgetSize(state), true_snapshot);
        EXPECT_EQ(encodedRemovalBudgetSize(state), true_removal);
    }
}
