#include <gtest/gtest.h>

#include "config.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/tests/cas_test_helpers.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

/// Stage B task 4c: the `_ckpt` JOIN and the `O(1)` SIZE invariant (Constraint 15).
///
/// `mergeCkpt` already has a suite (`CasRefCkpt` in `gtest_cas_ref_ckpt.cpp`) covering it as one step of
/// the publish algorithm. This suite's subject is narrower and different: the JOIN LAW itself, per
/// field, stated so that a later change to any one field's rule fails here rather than being absorbed
/// into a publish-path assertion; plus the size invariant, which no existing test constrains at all.
///
/// The size half is a REGRESSION FENCE, not a fix -- nothing about today's `_ckpt` is non-`O(1)`. It
/// exists to fail the day someone adds a map, a collection, or any per-ref/per-file term to an object
/// that has no repair path and gates destructive cleanup.
///
/// Constraint 15 names four dimensions (refs, files, transactions, writer epochs) and they do NOT
/// behave the same way, so they get two different assertions rather than one claim covering both:
///
///   - REFS and FILES never enter the body in any form, so the encoded size is BYTE-EQUAL between a
///     namespace holding one and a namespace holding ten thousand. That is `EncodedCkptSizeIs...`
///     below, and it drives the REAL append lane on purpose: a hand-built pair of `RefCkpt` structs
///     would leave a newly-added collection field EMPTY in both and the equality would still hold,
///     so the fence would not fire on the very change it exists to catch. Only a real producer
///     populates a real field.
///   - TRANSACTIONS and WRITER EPOCHS enter as the DECIMAL WIDTH of the two id pairs. That is not
///     equality: `{cse=1,css=1}` and `{cse=1,css=10000}` differ by four bytes. It is `O(1)` because
///     the fields are `uint64_t` and so the width is ceilinged at twenty digits, which is a bound a
///     test asserts on a constructed worst case -- `EncodedCkptSizeHasAConstantCeiling...` below.

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
}

using namespace DB::Cas;
using DB::Cas::tests::namespaceBirthOp;
using DB::Cas::tests::publishCommittedOps;

/// Constraint 15's COMPILE-TIME half: `_ckpt` is a fixed-size product of scalar monotone facts. Any
/// field that owns heap storage -- a map, a vector, a `String` -- makes `RefCkpt` non-trivially-copyable
/// and fails the build here, which is the earliest and cheapest place the constraint can be enforced.
/// The two runtime size tests below are the rest of the fence: this one cannot see a fixed-capacity
/// array, and they cannot see a field that is never populated by the producers they drive.
static_assert(std::is_trivially_copyable_v<RefCkpt>,
              "Constraint 15: _ckpt is a fixed-size product of scalar monotone facts, so its encoded size is "
              "O(1) in refs, files, transactions and writer epochs. A field with heap storage (a map, a "
              "vector, a String) breaks that and belongs in a separate immutable object or ledger.");

namespace
{

constexpr uint64_t U64_MAX = std::numeric_limits<uint64_t>::max();

/// Constraint 15's bound, as a number: the encoded size of the WIDEST `_ckpt` this build can produce
/// (all three fields present, every integer component at `UINT64_MAX`). Pinned as a literal so that
/// adding a field, or widening one, fails a test rather than quietly moving the bound.
constexpr size_t CKPT_WORST_CASE_ENCODED_BYTES = 176;

/// Refs published per transaction by the helper below. The append lane caps a normal-class item at 5000
/// operations and `publishCommittedOps` emits two per ref, so a namespace holding ten thousand refs is
/// necessarily built over several transactions -- the cardinality under test cannot be reached in one.
constexpr size_t REFS_PER_TXN = 2000;

PoolPtr openPool(const BackendPtr & backend)
{
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    return Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

/// The incarnation the production birth wiring minted for `ns`, learned back from the catalog the way a
/// real reader does. Fails the current test rather than dereferencing a disengaged optional, so one
/// regression cannot abort the binary and take every later suite's result with it.
NamespaceLifeId liveLifeOrFail(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    const CasRefCatalog::Snapshot snap = CasRefCatalog::read(backend, layout);
    for (const CatalogEntry & entry : snap.catalog.entries)
        if (entry.ns.string() == ns.string())
            return NamespaceLifeId::fromCatalogEntry(entry.ns, entry.incarnation);
    ADD_FAILURE() << "expected a catalog entry for namespace '" << ns.string() << "', found none";
    return NamespaceLifeId::stageATransition(ns);
}

/// Births `ns`, publishes `ref_count` committed refs through the REAL append lane in transactions of
/// `REFS_PER_TXN`, and returns that namespace's durable `_ckpt` as encoded bytes.
String encodedCkptOfNamespaceWithRefs(const PoolPtr & store, Backend & backend, const Layout & layout,
                                      const RootNamespace & ns, size_t ref_count)
{
    for (size_t base = 0; base < ref_count; base += REFS_PER_TXN)
    {
        const size_t end = std::min(base + REFS_PER_TXN, ref_count);
        store->appendRefOps(ns, MutationScope::wholeShard(),
            [base, end](const RefTableState & state)
            {
                std::vector<RefOp> ops;
                if (state.getLifecycle() != RefLifecycle::Live)
                    ops.push_back(namespaceBirthOp());
                for (size_t i = base; i < end; ++i)
                {
                    /// Fixed-width names so the refs themselves cannot be the thing that differs: this
                    /// helper's whole claim is that ref cardinality does not reach `_ckpt`, and a ref
                    /// whose NAME grew with `i` would confound a size comparison if it ever did.
                    const String ref = fmt::format("r{:08}", i);
                    for (const RefOp & op : publishCommittedOps(ref, ManifestRef{1, i + 1, 1}))
                        ops.push_back(op);
                }
                return ops;
            },
            RootMutationOrigin::Writer, RootMutationKind::Publish);
    }

    const NamespaceLifeId life = liveLifeOrFail(backend, layout, ns);
    const std::optional<CkptSample> sample = readCkpt(backend, layout, life);
    if (!sample)
    {
        ADD_FAILURE() << "expected a _ckpt for namespace '" << ns.string() << "' after its birth transaction";
        return {};
    }
    return encodeRefCkpt(sample->ckpt);
}

}

/// ---------------------------------------------------------------------------------------------
/// The join law, per field
/// ---------------------------------------------------------------------------------------------

/// An absence is "this writer knew nothing", never "this writer says none". Exactly one writer ever
/// knows a namespace's genesis epoch, so every other contribution is `nullopt` and must leave what is
/// on record alone -- in BOTH argument orders, because the two `_ckpt` writers have no ordering
/// between them and the merge is what makes that safe.
TEST(CasRefCkptJoin, JoinUnknownLifeEpochWithPresentYieldsPresent)
{
    const RefCkpt unknown{.life_epoch = std::nullopt, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt};
    const RefCkpt present{.life_epoch = 7, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt};

    EXPECT_EQ(mergeCkpt(unknown, present).life_epoch, std::optional<uint64_t>{7});
    EXPECT_EQ(mergeCkpt(present, unknown).life_epoch, std::optional<uint64_t>{7})
        << "the merge is commutative -- a writer that knows nothing must not be able to erase the genesis epoch";

    /// The other half of "absent loses": two absences stay absent. `life_epoch` has no floor to fall
    /// back to, and a fabricated one is permanent -- the semantic-max merge can never lower it again.
    EXPECT_EQ(mergeCkpt(unknown, unknown).life_epoch, std::nullopt);
}

/// The ordinary steady state, and the case the re-key made the only present-vs-present one a namespace
/// life should ever see: both writers agree. Asserted for its own sake because it is what a stricter
/// conflict rule must keep admitting -- an equal republish is not a conflict.
TEST(CasRefCkptJoin, JoinEqualLifeEpochsYieldsSame)
{
    const RefCkpt a{.life_epoch = 9, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt};
    const RefCkpt b{.life_epoch = 9, .checkpoint_snapshot_id = RefTxnId{9, 4}, .last_epoch_seal = std::nullopt};

    EXPECT_EQ(mergeCkpt(a, b).life_epoch, std::optional<uint64_t>{9});
    EXPECT_EQ(mergeCkpt(b, a).life_epoch, std::optional<uint64_t>{9});
    const std::optional<RefTxnId> b_checkpoint = RefTxnId{9, 4};
    EXPECT_EQ(mergeCkpt(a, b).checkpoint_snapshot_id, b_checkpoint)
        << "an equal life_epoch must not disturb the other fields' own join";
}

/// `checkpoint_snapshot_id` and `last_epoch_seal` continue to merge by SEMANTIC MAXIMUM. Unlike
/// `life_epoch` these two genuinely advance over a namespace's life, and the max is what stops a writer
/// that sampled an older body from regressing the other writer's progress (TLC counterexample
/// `_sab_sealclobbersbase`, which costs an acked transaction). Both directions and present-beats-absent,
/// since the two writers have no ordering between them.
TEST(CasRefCkptJoin, CheckpointAndSealStillMergeBySemanticMaximum)
{
    const RefCkpt lower{.life_epoch = std::nullopt, .checkpoint_snapshot_id = RefTxnId{3, 5}, .last_epoch_seal = RefTxnId{3, 4}};
    const RefCkpt higher{.life_epoch = std::nullopt, .checkpoint_snapshot_id = RefTxnId{4, 1}, .last_epoch_seal = RefTxnId{4, 2}};

    const std::optional<RefTxnId> higher_checkpoint = higher.checkpoint_snapshot_id;
    const std::optional<RefTxnId> higher_seal = higher.last_epoch_seal;
    const std::optional<RefTxnId> lower_checkpoint = lower.checkpoint_snapshot_id;
    const std::optional<RefTxnId> lower_seal = lower.last_epoch_seal;

    /// Ordered by writer_epoch FIRST: `{4,1}` beats `{3,5}` even though its sequence is smaller, which
    /// is the intended timeline across an epoch restart that resets the sequence.
    EXPECT_EQ(mergeCkpt(lower, higher).checkpoint_snapshot_id, higher_checkpoint);
    EXPECT_EQ(mergeCkpt(higher, lower).checkpoint_snapshot_id, higher_checkpoint);
    EXPECT_EQ(mergeCkpt(lower, higher).last_epoch_seal, higher_seal);
    EXPECT_EQ(mergeCkpt(higher, lower).last_epoch_seal, higher_seal);

    /// Present beats absent, both directions and both fields.
    const RefCkpt nothing;
    EXPECT_EQ(mergeCkpt(nothing, lower).checkpoint_snapshot_id, lower_checkpoint);
    EXPECT_EQ(mergeCkpt(lower, nothing).checkpoint_snapshot_id, lower_checkpoint);
    EXPECT_EQ(mergeCkpt(nothing, lower).last_epoch_seal, lower_seal);
    EXPECT_EQ(mergeCkpt(lower, nothing).last_epoch_seal, lower_seal);
    EXPECT_EQ(mergeCkpt(nothing, nothing).checkpoint_snapshot_id, std::nullopt);
    EXPECT_EQ(mergeCkpt(nothing, nothing).last_epoch_seal, std::nullopt);
}

/// ---------------------------------------------------------------------------------------------
/// Constraint 15: the `O(1)` size invariant
/// ---------------------------------------------------------------------------------------------

/// REFS and FILES: byte-equal, because they never enter the body. Driven through the REAL append lane
/// (see `encodedCkptOfNamespaceWithRefs` on why a hand-built struct pair would not fence anything).
TEST(CasRefCkptJoin, EncodedCkptSizeIsIndependentOfCardinality)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    Layout layout("p");

    const String one = encodedCkptOfNamespaceWithRefs(store, *backend, layout, RootNamespace{"srv1/one"}, 1);
    const String many = encodedCkptOfNamespaceWithRefs(store, *backend, layout, RootNamespace{"srv1/many"}, 2000);

    ASSERT_FALSE(one.empty());
    ASSERT_FALSE(many.empty());
    EXPECT_EQ(one, many)
        << "not merely equal in SIZE: refs and files reach `_ckpt` in no form at all, so the two bodies "
           "are byte-identical.";
    /// The same claim stated so that it does not depend on the chosen cardinality at all: no ref
    /// PUBLISHED into the namespace appears anywhere in its `_ckpt`. A count-based comparison can only
    /// catch a term that grows; this catches one that is merely there.
    EXPECT_EQ(many.find("r00000000"), String::npos)
        << "a published ref's NAME appears in `_ckpt`: " << many;
    EXPECT_EQ(many.find("r00001999"), String::npos)
        << "a published ref's NAME appears in `_ckpt`: " << many;
    EXPECT_EQ(one.size(), many.size())
        << "Constraint 15: `_ckpt`'s encoded size must not grow with the number of refs or files in the "
           "namespace. A collection or per-ref term was added to an object that has NO repair path and "
           "gates destructive cleanup; it belongs in a separate immutable object or ledger instead.\n"
           "  1 ref:     " << one << "  10000 refs: " << many;
}

/// TRANSACTIONS and WRITER EPOCHS: not equality -- they enter as the decimal width of the id pairs --
/// but ceilinged, because the fields are `uint64_t`. The worst case is constructible exactly (every
/// field present at `UINT64_MAX`), so the bound is asserted on it rather than believed about it.
TEST(CasRefCkptJoin, EncodedCkptSizeHasAConstantCeilingAcrossTransactionsAndEpochs)
{
    /// The true worst case over every namespace history: all three fields present, every component at
    /// the widest value its type can hold. No real `_ckpt` can encode larger, because there is no field
    /// that is not one of these five integers.
    const RefCkpt worst{.life_epoch = U64_MAX,
                        .checkpoint_snapshot_id = RefTxnId{U64_MAX, U64_MAX},
                        .last_epoch_seal = RefTxnId{U64_MAX, U64_MAX}};
    const size_t worst_bytes = encodeRefCkpt(worst).size();

    /// Pinned as a literal, not merely compared against itself: this is the number Constraint 15's
    /// `O(1)` claim reduces to, and a change to it means a field was added, removed or rewidened.
    EXPECT_EQ(worst_bytes, CKPT_WORST_CASE_ENCODED_BYTES)
        << "the widest `_ckpt` this build can encode changed size -- a field was added, removed, or "
           "given a wider type. Constraint 15's O(1) bound is exactly this constant.";

    /// The growth term is the decimal width, and it is bounded by that ceiling rather than proportional
    /// to the number of transactions: four orders of magnitude of `ref_sequence` cost four bytes.
    const RefCkpt at_sequence_1{.life_epoch = 1, .checkpoint_snapshot_id = RefTxnId{1, 1}, .last_epoch_seal = RefTxnId{1, 1}};
    const RefCkpt at_sequence_10k{.life_epoch = 1, .checkpoint_snapshot_id = RefTxnId{1, 10000}, .last_epoch_seal = RefTxnId{1, 10000}};
    EXPECT_EQ(encodeRefCkpt(at_sequence_10k).size(), encodeRefCkpt(at_sequence_1).size() + 8);
    EXPECT_LE(encodeRefCkpt(at_sequence_10k).size(), worst_bytes);
    EXPECT_LE(encodeRefCkpt(at_sequence_1).size(), worst_bytes);

    /// And the ceiling is far below the format registry's own object cap, so the cap is what it is
    /// documented to be -- a corruption brake this object cannot approach -- and never the thing that
    /// makes the size bounded.
    EXPECT_LT(worst_bytes, traitsFor(FormatId::RefCkpt).object_cap);
}
