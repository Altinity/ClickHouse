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
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

/// The `O(1)` SIZE invariant of `_ckpt`.
///
/// This suite is a REGRESSION FENCE, not a fix -- nothing about today's `_ckpt` is non-`O(1)`. It
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
///     equality: `{snapshot_epoch=1,snapshot_seq=1}` and `{snapshot_epoch=1,snapshot_seq=10000}` differ by four bytes. It is `O(1)` because
///     the fields are `uint64_t` and so the width is ceilinged at twenty digits, which is a bound a
///     test asserts on a constructed worst case -- `EncodedCkptSizeHasAConstantCeiling...` below.

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
/// adding a field, or widening one, fails a test rather than quietly moving the bound. The shared
/// format-version header is the single-digit `v:1` baseline; a future generation bump that widens it
/// moves this constant too.
constexpr size_t CKPT_WORST_CASE_ENCODED_BYTES = 296;

/// The high-cardinality side of the size fence, in ONE transaction. Bounded above by the append lane's
/// 5000-operation cap on a normal-class item (`publishCommittedOps` emits two ops per ref), and kept at
/// one transaction on purpose: spreading a larger namespace over several of them costs tens of seconds
/// in a debug build, and a size fence that cannot finish inside the harness budget fences nothing. Any
/// per-ref term in `_ckpt` is as visible at this count as at any larger one.
constexpr size_t MANY_REFS = 2000;

/// Fixed-width, so the refs themselves cannot be what differs between the two namespaces: the claim
/// under test is that ref cardinality does not reach `_ckpt`, and a name that grew with `i` would
/// confound a size comparison if it ever did.
String refName(size_t i)
{
    return fmt::format("r{:08}", i);
}

/// `boot_ms_fn` defaults to the real clock. A caller whose test body does enough CPU-bound work
/// against ONE open pool to risk outrunning `mount_lease_ttl_ms` on a slow sanitizer build should
/// pass a frozen one instead of widening the TTL: the mount fence and the ref-log request controller
/// both read time through this same seam (see `CasRefLedger`'s `controller_boot_ms_fn`), so freezing
/// it removes the wall-clock race rather than merely giving it more room.
PoolPtr openPool(const BackendPtr & backend, std::function<uint64_t()> boot_ms_fn = {})
{
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.boot_ms_fn = std::move(boot_ms_fn);
    return Pool::open(backend, std::move(config));
}

/// The incarnation the production birth wiring minted for `ns`, learned back from the catalog the way a
/// real reader does. Fails the current test rather than dereferencing a disengaged optional, so one
/// regression cannot abort the binary and take every later suite's result with it.
NamespaceLifeId liveLifeOrFail(CasOperation & op, const Layout & layout, const RootNamespace & ns)
{
    const CasRefCatalog::Snapshot snap = CasRefCatalog::read(op, layout);
    for (const CatalogEntry & entry : snap.catalog.entries)
        if (entry.ns.string() == ns.string())
            return NamespaceLifeId::fromCatalogEntry(entry.ns, entry.incarnation);
    ADD_FAILURE() << "expected a catalog entry for namespace '" << ns.string() << "', found none";
    return DB::Cas::tests::fixture::fixtureLife(ns);
}

/// Births `ns` and publishes `ref_count` committed refs through the REAL append lane, in ONE
/// transaction, and returns that namespace's durable `_ckpt` as encoded bytes.
///
/// One transaction also holds every OTHER dimension fixed while `ref_count` varies: two namespaces
/// built this way end at the same transaction id, so a difference in their `_ckpt` bodies can only be
/// the refs. `ref_count` must therefore stay within the append lane's per-item operation cap.
String encodedCkptOfNamespaceWithRefs(const PoolPtr & store, CasOperation & op, const Layout & layout,
                                      const RootNamespace & ns, size_t ref_count)
{
    store->appendRefOps(ns, MutationScope::wholeShard(),
        [ref_count](const RefTableState & state)
        {
            std::vector<RefOp> ops;
            if (state.getLifecycle() != RefLifecycle::Live)
                ops.push_back(namespaceBirthOp());
            for (size_t i = 0; i < ref_count; ++i)
                for (const RefOp & ref_op : publishCommittedOps(refName(i), ManifestRef{1, i + 1, 1}))
                    ops.push_back(ref_op);
            return ops;
        },
        RootMutationOrigin::Writer, RootMutationKind::Publish);

    const NamespaceLifeId life = liveLifeOrFail(op, layout, ns);
    const std::optional<CkptSample> sample = readCkpt(op, layout, life);
    if (!sample)
    {
        ADD_FAILURE() << "expected a _ckpt for namespace '" << ns.string() << "' after its birth transaction";
        return {};
    }
    return encodeRefCkptAtGeneration(sample->ckpt, 1);
}

}

/// ---------------------------------------------------------------------------------------------
/// Constraint 15: the `O(1)` size invariant
/// ---------------------------------------------------------------------------------------------

/// REFS and FILES: byte-equal, because they never enter the body. Driven through the REAL append lane
/// (see `encodedCkptOfNamespaceWithRefs` on why a hand-built struct pair would not fence anything).
TEST(CASRefCheckpointJoin, EncodedCkptSizeIsIndependentOfCardinality)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// `MANY_REFS` committed through ONE `appendRefOps` call is CPU-bound encoding, not I/O -- on a
    /// slow sanitizer build (msan in particular) it can outrun the real-clock `mount_lease_ttl_ms`
    /// this pool was opened under and trip the mount fence mid-publish. Freeze the pool's clock
    /// instead of racing it (see `openPool`'s doc comment).
    auto store = openPool(backend, [] { return uint64_t{0}; });
    Layout layout("p");
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();

    const String one = encodedCkptOfNamespaceWithRefs(store, op, layout, RootNamespace{"srv1/one"}, 1);
    const String many = encodedCkptOfNamespaceWithRefs(store, op, layout, RootNamespace{"srv1/many"}, MANY_REFS);

    ASSERT_FALSE(one.empty());
    ASSERT_FALSE(many.empty());
    EXPECT_EQ(one, many)
        << "not merely equal in SIZE: refs and files reach `_ckpt` in no form at all, so the two bodies "
           "are byte-identical.";
    /// The same claim stated so that it does not depend on the chosen cardinality at all: no ref
    /// PUBLISHED into the namespace appears anywhere in its `_ckpt`. A count-based comparison can only
    /// catch a term that grows; this catches one that is merely there.
    EXPECT_EQ(many.find(refName(0)), String::npos)
        << "a published ref's NAME appears in `_ckpt`: " << many;
    EXPECT_EQ(many.find(refName(MANY_REFS - 1)), String::npos)
        << "a published ref's NAME appears in `_ckpt`: " << many;
    EXPECT_EQ(one.size(), many.size())
        << "Constraint 15: `_ckpt`'s encoded size must not grow with the number of refs or files in the "
           "namespace. A collection or per-ref term was added to an object that has NO repair path and "
           "gates destructive cleanup; it belongs in a separate immutable object or ledger instead.\n"
           "  1 ref:            " << one
        << "  " << MANY_REFS << " refs: " << many;
}

/// TRANSACTIONS and WRITER EPOCHS: not equality -- they enter as the decimal width of the id pairs --
/// but ceilinged, because the fields are `uint64_t`. The worst case is constructible exactly (every
/// field present at `UINT64_MAX`), so the bound is asserted on it rather than believed about it.
TEST(CASRefCheckpointJoin, EncodedCkptSizeHasAConstantCeilingAcrossTransactionsAndEpochs)
{
    /// The true worst case over every namespace history: all three fields present, every component at
    /// the widest value its type can hold. No real `_ckpt` can encode larger, because there is no field
    /// that is not one of these five integers.
    const RefCkpt worst{.life_epoch = U64_MAX,
                        .committed_through = RefTxnId{U64_MAX, U64_MAX},
                        .checkpoint_snapshot_id = RefTxnId{U64_MAX, U64_MAX},
                        .last_epoch_seal = RefTxnId{U64_MAX, U64_MAX}};
    const size_t worst_bytes = encodeRefCkptAtGeneration(worst, 1).size();

    /// Pinned as a literal, not merely compared against itself: this is the number Constraint 15's
    /// `O(1)` claim reduces to, and a change to it means a field was added, removed or rewidened.
    EXPECT_EQ(worst_bytes, CKPT_WORST_CASE_ENCODED_BYTES)
        << "the widest `_ckpt` this build can encode changed size -- a field was added, removed, or "
           "given a wider type. Constraint 15's O(1) bound is exactly this constant.";

    /// The growth term is the decimal width, and it is bounded by that ceiling rather than proportional
    /// to the number of transactions: four orders of magnitude of `ref_sequence` cost four bytes.
    const RefCkpt at_sequence_1{.life_epoch = 1, .committed_through = RefTxnId{1, 1}, .checkpoint_snapshot_id = RefTxnId{1, 1}, .last_epoch_seal = RefTxnId{1, 1}};
    const RefCkpt at_sequence_10k{.life_epoch = 1, .committed_through = RefTxnId{1, 10000}, .checkpoint_snapshot_id = RefTxnId{1, 10000}, .last_epoch_seal = RefTxnId{1, 10000}};
    EXPECT_EQ(encodeRefCkptAtGeneration(at_sequence_10k, 1).size(), encodeRefCkptAtGeneration(at_sequence_1, 1).size() + 12);
    EXPECT_LE(encodeRefCkptAtGeneration(at_sequence_10k, 1).size(), worst_bytes);
    EXPECT_LE(encodeRefCkptAtGeneration(at_sequence_1, 1).size(), worst_bytes);

    /// And the ceiling is far below the format registry's own object cap, so the cap is what it is
    /// documented to be -- a corruption brake this object cannot approach -- and never the thing that
    /// makes the size bounded.
    EXPECT_LT(worst_bytes, traitsFor(FormatId::RefCkpt).object_cap);
}
