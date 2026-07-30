#include <gtest/gtest.h>

#include "config.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <Common/MemoryTracker.h>
#include <Common/SipHash.h>
#include <base/hex.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/// Stage A task 5 (INV-4): the `_ckpt` object.
///
/// `_ckpt` exists because prefix cleaning made the ref stream unreadable from a LIST alone, so it is
/// simultaneously the thing recovery point-reads to find its base AND the gate on what cleanup may
/// delete. Both roles are only safe while three properties hold, and this suite pins exactly those:
///
///   1. the codec is STRICT in both directions -- a body that only partly decoded would be a cleanup
///      decision taken from a partly-read object;
///   2. there is ONE merge, by semantic maximum per field, used by BOTH writers -- a writer that
///      wrote back the value it sampled earlier regresses the other writer's progress, which is TLC
///      counterexample `_sab_sealclobbersbase` and costs an acked transaction;
///   3. every CAS attempt re-checks the admitted fence generation AFTER its read and BEFORE its write,
///      so a writer whose mount incarnation moved advances nothing.
///
/// The suite name is prefixed `Cas` so it is covered by the `Cas*` unit-test gate filter.

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int MEMORY_LIMIT_EXCEEDED;
extern const int NETWORK_ERROR;
extern const int UNKNOWN_FORMAT_VERSION;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::namespaceBirthOp;
using DB::Cas::tests::publishCommittedOps;

namespace
{

const RefTxnId ID_1_1{1, 1};
const RefTxnId ID_1_2{1, 2};
const RefTxnId ID_2_1{2, 1};

PoolPtr openPool(const BackendPtr & backend)
{
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    return Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

/// The same one-transaction publish the other ref suites drive, so a namespace reaches `Live` through
/// the REAL append lane (which is also what creates its `_ckpt`).
RefTxnId publishRef(const PoolPtr & store, const RootNamespace & ns, const String & ref, uint64_t ordinal)
{
    return store->appendRefOps(ns, MutationScope::ref(ref),
        [&ref, ordinal](const RefTableState & state)
        {
            std::vector<RefOp> ops;
            if (state.getLifecycle() != RefLifecycle::Live)
                ops.push_back(namespaceBirthOp());
            for (const RefOp & op : publishCommittedOps(ref, ManifestRef{1, ordinal, 1}))
                ops.push_back(op);
            return ops;
        },
        RootMutationOrigin::Writer, RootMutationKind::Publish);
}

/// A fence that never refuses, for the tests whose subject is not the fence.
const std::function<void(uint64_t)> ALWAYS_ADMITTED = [](uint64_t) {};

/// A deadline far enough out that only the test's own contention decides the outcome. The clock is
/// frozen (a constant `now`), which is what makes every non-exhaustion test independent of wall time.
CkptDeadline generousDeadline()
{
    return CkptDeadline{[] { return uint64_t{1000}; }, 60000};
}

/// Reads the namespace's `_ckpt` and returns its body, or a default-constructed one after failing the
/// current test when the object is absent. Every assertion below goes through this rather than
/// dereferencing the optional directly: a bare `->` on a disengaged optional ABORTS the whole test
/// binary, so one regression would take every later suite's result with it instead of failing a test.
RefCkpt readCkptOrFail(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    const std::optional<CkptSample> sample = readCkpt(backend, layout, ns);
    if (!sample)
    {
        ADD_FAILURE() << "expected a _ckpt for namespace '" << ns.string() << "', found none";
        return RefCkpt{};
    }
    return sample->ckpt;
}

/// Replaces the whole body of one key, minting a new incarnation -- how a test installs a deliberately
/// malformed or concurrently-advanced object.
void overwriteObject(Backend & backend, const String & key, const String & bytes)
{
    const HeadResult h = backend.head(key);
    ASSERT_TRUE(h.exists) << "overwriteObject expects " << key << " to exist";
    ASSERT_EQ(backend.putOverwrite(key, bytes, h.token).outcome, PutOutcome::Done);
}

/// Runs `on_get` right after every `get` of `watched_key` -- the deterministic way to act inside
/// another component's read-then-write window without a sleep or a second thread. The hook is a public
/// member rather than a constructor argument so it can be installed AFTER the backend exists (every
/// interesting hook writes through that same backend) and only once the test's setup writes are done.
class GetHookBackend : public CountingBackend
{
public:
    using CountingBackend::get;

    explicit GetHookBackend(String watched_key_) : watched_key(std::move(watched_key_)) {}

    std::function<void()> on_get;

    std::optional<GetResult> get(const String & key, Range range) override
    {
        auto result = CountingBackend::get(key, range);
        if (key == watched_key && on_get)
            on_get();
        return result;
    }

private:
    String watched_key;
};

}

/// ---------------------------------------------------------------------------------------------
/// The codec
/// ---------------------------------------------------------------------------------------------

/// Every combination of the two optionals survives a round trip. Both-absent is the shape a namespace
/// carries from creation until its first snapshot, so it is a real state and not a degenerate one.
TEST(CasRefCkpt, RoundTripsEveryFieldCombination)
{
    const std::vector<RefCkpt> cases = {
        RefCkpt{.life_epoch = std::optional<uint64_t>{7}, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt},
        RefCkpt{.life_epoch = std::optional<uint64_t>{7}, .checkpoint_snapshot_id = ID_1_2,       .last_epoch_seal = std::nullopt},
        RefCkpt{.life_epoch = std::optional<uint64_t>{7}, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = ID_2_1},
        RefCkpt{.life_epoch = std::optional<uint64_t>{7}, .checkpoint_snapshot_id = ID_1_2,       .last_epoch_seal = ID_2_1},
    };
    for (const RefCkpt & ckpt : cases)
        EXPECT_EQ(decodeRefCkpt(encodeRefCkpt(ckpt)), ckpt);
}

/// STRICT means an unknown key is corruption, not something to skip. A `_ckpt` decides deletions, so a
/// reader that ignored a field it did not understand would be authorizing them from a body it only
/// partly read.
TEST(CasRefCkpt, RejectsAnUnknownKey)
{
    const String good = encodeRefCkpt(RefCkpt{.life_epoch = std::optional<uint64_t>{7}, .checkpoint_snapshot_id = ID_1_1,
                                              .last_epoch_seal = std::nullopt});
    String with_unknown = good;
    with_unknown.replace(with_unknown.rfind('}'), 1, R"(,"zz":"1"})");
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCkpt(with_unknown); });

    /// A `!`-prefixed key is a REQUIRED extension and reports the version, not corruption -- the
    /// distinction is what lets an operator tell "this build is too old" from "this object is broken".
    String with_critical = good;
    with_critical.replace(with_critical.rfind('}'), 1, R"(,"!zz":"1"})");
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { decodeRefCkpt(with_critical); });
}

/// A duplicate key has no single meaning, so it can never be resolved by a reader's preference.
TEST(CasRefCkpt, RejectsADuplicateKey)
{
    const String good = encodeRefCkpt(RefCkpt{.life_epoch = std::optional<uint64_t>{7}, .checkpoint_snapshot_id = std::nullopt,
                                              .last_epoch_seal = std::nullopt});
    String duplicated = good;
    duplicated.replace(duplicated.rfind('}'), 1, R"(,"le":"9"})");
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCkpt(duplicated); });
}

/// Truncation in each of its shapes. Half an optional pair is the dangerous one: silently dropping it
/// would turn a truncated body into a well-formed `_ckpt` with NO checkpoint, which reads as
/// "recovery has no base" and would be trusted.
TEST(CasRefCkpt, RejectsTruncation)
{
    const String good = encodeRefCkpt(RefCkpt{.life_epoch = std::optional<uint64_t>{7}, .checkpoint_snapshot_id = ID_1_2,
                                              .last_epoch_seal = std::nullopt});

    const String header_only = good.substr(0, good.find('\n') + 1);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCkpt(header_only); });

    /// The body line without its terminator: a read that stopped mid-object.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCkpt(good.substr(0, good.size() - 1)); });

    /// An EMPTY body is not truncation, it is the legitimate "nobody knows anything yet" object -- the
    /// shape a namespace carries between its creation and its first checkpoint. Asserted here, next to
    /// the truncation cases, because the two are one character apart on the wire.
    const String empty_body = good.substr(0, good.find('\n') + 1) + "{}\n";
    EXPECT_EQ(decodeRefCkpt(empty_body), RefCkpt{});

    const String half_pair = good.substr(0, good.find('\n') + 1) + R"({"le":"7","cse":"1"})" + "\n";
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCkpt(half_pair); });

    const String other_half = good.substr(0, good.find('\n') + 1) + R"({"le":"7","lss":"2"})" + "\n";
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCkpt(other_half); });
}

TEST(CasRefCkpt, RejectsTrailingBytes)
{
    const String good = encodeRefCkpt(RefCkpt{.life_epoch = std::optional<uint64_t>{7}, .checkpoint_snapshot_id = std::nullopt,
                                              .last_epoch_seal = std::nullopt});
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCkpt(good + "junk\n"); });
}

/// The field-validity rule runs in BOTH directions: a struct this build refuses to read can never be
/// written by it either, so a bug on the write side surfaces at the writer and not as an unreadable
/// object discovered by a future recovery.
TEST(CasRefCkpt, RejectsInvalidFieldsOnEncodeAndOnDecode)
{
    /// PRESENT means REAL: an absent field is legal, a present-but-impossible one is not. A zero
    /// `life_epoch` would give the field two meanings ("unknown" and "epoch zero") on an object that
    /// gates deletions.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [] { encodeRefCkpt(RefCkpt{.life_epoch = std::optional<uint64_t>{0}, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt}); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [] { encodeRefCkpt(RefCkpt{.life_epoch = std::optional<uint64_t>{7}, .checkpoint_snapshot_id = RefTxnId{1, 0}, .last_epoch_seal = std::nullopt}); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [] { encodeRefCkpt(RefCkpt{.life_epoch = std::optional<uint64_t>{7}, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = RefTxnId{0, 1}}); });

    const String header = encodeRefCkpt(RefCkpt{.life_epoch = std::optional<uint64_t>{7}, .checkpoint_snapshot_id = std::nullopt,
                                                .last_epoch_seal = std::nullopt});
    const String prefix = header.substr(0, header.find('\n') + 1);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCkpt(prefix + R"({"le":"0"})" + "\n"); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeRefCkpt(prefix + R"({"le":"7","cse":"1","css":"0"})" + "\n"); });
}

/// The registry row is part of the contract: Control/Strict decides how the decoder treats unknown
/// keys, and the caps are the first thing that fires if a foreign object ever lands at the key.
TEST(CasRefCkpt, RegistryRowIsControlStrictWithTightCaps)
{
    const FormatTraits & traits = traitsFor(FormatId::RefCkpt);
    EXPECT_EQ(traits.type, "cas_ref_ckpt");
    EXPECT_EQ(traits.family, TextFamily::Control);
    EXPECT_EQ(traits.strictness, KeyStrictness::Strict);
    EXPECT_EQ(traits.object_cap, 64u * 1024u);
    EXPECT_EQ(traits.line_cap, 4u * 1024u);
    EXPECT_EQ(traitsForType("cas_ref_ckpt"), &traits);
    /// Raw, so the key has no suffix -- the Stage A shape is exactly `<ns>/_ckpt`. This line is also
    /// the TRIPWIRE for the codec's shortcut: `encodeRefCkpt`/`decodeRefCkpt` hand bytes to and from
    /// the backend directly, bypassing `sealObject`/`openObject` because both are the identity under
    /// `CompressionPolicy::Never`. Flip the policy to `Always` and that bypass would silently write
    /// uncompressed bodies under a `.zst` key -- which this assertion catches first.
    EXPECT_EQ(storedSuffix(FormatId::RefCkpt), "");
    EXPECT_EQ(traits.compression, CompressionPolicy::Never);
}

/// ---------------------------------------------------------------------------------------------
/// The key
/// ---------------------------------------------------------------------------------------------

TEST(CasRefCkpt, KeyIsTheLifeLeafAndParsesBack)
{
    const Layout layout{"p"};
    const RootNamespace ns{"srv1/ckpt_key"};
    const RefNamespaceId ns_id = RefNamespaceId::stageATransition(ns);
    EXPECT_EQ(layout.refCkptKey(ns_id),
        "p/cas/refs/srv1/ckpt_key/" + renderRefIncarnation(ns_id.incarnation) + "/_ckpt");
    EXPECT_EQ(layout.parseRefCkptKey(layout.refCkptKey(ns_id)), ns_id);

    /// `_ckpt` has no kind directory, so the id-bearing parser must NOT claim it -- and the `_ckpt`
    /// parser must not claim the id-bearing keys either. Each key has exactly one classifier.
    EXPECT_FALSE(layout.parseRefObjectKey(layout.refCkptKey(ns_id)).has_value());
    EXPECT_FALSE(layout.parseRefCkptKey(layout.refLogKey(ns_id, ID_1_1)).has_value());
    EXPECT_FALSE(layout.parseRefCkptKey(layout.refSnapshotKey(ns_id, ID_1_1)).has_value());
    EXPECT_FALSE(layout.parseRefCkptKey(layout.refCkptKey(ns_id) + ".zst").has_value());
    EXPECT_FALSE(layout.parseRefCkptKey("p/cas/refs/_ckpt").has_value());
    EXPECT_FALSE(layout.parseRefCkptKey("q" + layout.refCkptKey(ns_id).substr(1)).has_value());
}

/// A `_ckpt` is a LEGITIMATE ref object, and `groupRefKeys` aborts ref folding for the whole GC round
/// on any key under the ref prefix it cannot classify. Without this the first namespace to get a
/// checkpoint would stop ref folding in every subsequent round, pool-wide.
TEST(CasRefCkpt, GroupRefKeysClassifiesTheCkptInsteadOfAbortingTheRound)
{
    const Layout layout{"p"};
    const RootNamespace ns{"srv1/ckpt_group"};
    const std::vector<String> keys = {
        layout.refLogKey(RefNamespaceId::stageATransition(ns), ID_1_1),
        layout.refSnapshotKey(RefNamespaceId::stageATransition(ns), ID_1_1),
        layout.refCkptKey(RefNamespaceId::stageATransition(ns)),
    };

    const auto grouped = groupRefKeys(layout, keys);
    ASSERT_EQ(grouped.size(), 1u);
    const RefTableListing & listing = grouped.at(ns.string());
    EXPECT_EQ(listing.logs, std::vector<RefTxnId>{ID_1_1});
    EXPECT_EQ(listing.snapshots, std::vector<RefTxnId>{ID_1_1});
    EXPECT_TRUE(listing.has_ckpt) << "the checkpoint carries no txn id, so the listing must record it "
                                     "separately or a table with one looks identical to a table without";

    /// A genuinely unrecognizable key is still corruption -- the `_ckpt` arm must not have widened the
    /// gate into "ignore anything odd".
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { groupRefKeys(layout, {"p/cas/refs/srv1/ckpt_group/_bogus"}); });
}

/// ---------------------------------------------------------------------------------------------
/// The merge -- per field, both directions
/// ---------------------------------------------------------------------------------------------

/// The per-field table the ledger obligation from the TLA phase asks for: each field independently
/// newer on either side, plus both-absent and equal bodies. A merge that is not per-field would pass
/// some rows and fail others, which is the point of enumerating them.
TEST(CasRefCkpt, MergeTakesThePerFieldSemanticMaximum)
{
    const RefCkpt low{.life_epoch = std::optional<uint64_t>{3}, .checkpoint_snapshot_id = ID_1_1, .last_epoch_seal = ID_1_1};
    const RefCkpt high_ckpt{.life_epoch = std::optional<uint64_t>{3}, .checkpoint_snapshot_id = ID_1_2, .last_epoch_seal = ID_1_1};
    const RefCkpt high_seal{.life_epoch = std::optional<uint64_t>{3}, .checkpoint_snapshot_id = ID_1_1, .last_epoch_seal = ID_2_1};
    const RefCkpt high_life{.life_epoch = std::optional<uint64_t>{9}, .checkpoint_snapshot_id = ID_1_1, .last_epoch_seal = ID_1_1};

    /// Each field newer on the RIGHT, then the same case mirrored to the LEFT: the merge is symmetric,
    /// which is exactly why the two writers need no ordering between them.
    EXPECT_EQ(mergeCkpt(low, high_ckpt), high_ckpt);
    EXPECT_EQ(mergeCkpt(high_ckpt, low), high_ckpt);
    EXPECT_EQ(mergeCkpt(low, high_seal), high_seal);
    EXPECT_EQ(mergeCkpt(high_seal, low), high_seal);
    EXPECT_EQ(mergeCkpt(low, high_life), high_life);
    EXPECT_EQ(mergeCkpt(high_life, low), high_life);

    /// Fields advance INDEPENDENTLY: a merge of two bodies each newer in a different field keeps both.
    const RefCkpt both = mergeCkpt(high_ckpt, high_seal);
    EXPECT_EQ(both.checkpoint_snapshot_id, ID_1_2);
    EXPECT_EQ(both.last_epoch_seal, ID_2_1);

    /// An absent optional loses to a present one, whichever side it is on, and two absents stay absent.
    const RefCkpt none{.life_epoch = std::optional<uint64_t>{3}, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt};
    EXPECT_EQ(mergeCkpt(none, low), low);
    EXPECT_EQ(mergeCkpt(low, none), low);
    EXPECT_EQ(mergeCkpt(none, none), none);

    /// Identical bodies merge to themselves -- the property `publishCkpt` turns into "no write".
    EXPECT_EQ(mergeCkpt(low, low), low);

    /// A contribution that knows NOTHING about `life_epoch` (the snapshot publisher's shape) must not
    /// erase it. This is the case a plain assignment would get wrong.
    const RefCkpt publisher_only{.life_epoch = std::nullopt, .checkpoint_snapshot_id = ID_1_2, .last_epoch_seal = std::nullopt};
    const RefCkpt advanced = mergeCkpt(low, publisher_only);
    EXPECT_EQ(advanced.life_epoch, 3u);
    EXPECT_EQ(advanced.checkpoint_snapshot_id, ID_1_2);
    EXPECT_EQ(advanced.last_epoch_seal, ID_1_1) << "the publisher knows nothing about the seal and must "
                                                   "not drag it backwards";
}

/// ---------------------------------------------------------------------------------------------
/// publishCkpt
/// ---------------------------------------------------------------------------------------------

TEST(CasRefCkpt, CreatesTheObjectWhenItIsAbsent)
{
    auto backend = std::make_shared<CountingBackend>();
    const Layout layout{"p"};
    const RootNamespace ns{"srv1/ckpt_create"};
    const RefCkpt birth{.life_epoch = std::optional<uint64_t>{5}, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt};

    EXPECT_EQ(publishCkpt(*backend, layout, ns, birth, 1, ALWAYS_ADMITTED, generousDeadline()),
              CkptPublishOutcome::Published);
    const auto sample = readCkpt(*backend, layout, ns);
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(sample->ckpt, birth);
}

/// Any writer may CREATE the object, and none of them may complete it. A publisher knows only the
/// checkpoint, so it creates an object that knows only the checkpoint; the birth transaction's
/// `life_epoch` merges in afterwards. Order does not matter -- the merge is a per-field maximum, and
/// no writer ever supplies a field it does not know (a guess here would be permanent, since the merge
/// can never lower it).
TEST(CasRefCkpt, EachWriterCreatesWithOnlyWhatItKnowsAndTheOtherFieldsMergeInLater)
{
    auto backend = std::make_shared<CountingBackend>();
    const Layout layout{"p"};
    const RootNamespace ns{"srv1/ckpt_partial_create"};
    const RefCkpt publisher{.life_epoch = std::nullopt, .checkpoint_snapshot_id = ID_1_1, .last_epoch_seal = std::nullopt};

    ASSERT_EQ(publishCkpt(*backend, layout, ns, publisher, 1, ALWAYS_ADMITTED, generousDeadline()),
              CkptPublishOutcome::Published);
    const auto created = readCkpt(*backend, layout, ns);
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(created->ckpt.checkpoint_snapshot_id, ID_1_1);
    EXPECT_FALSE(created->ckpt.life_epoch.has_value()) << "the publisher must not invent a genesis epoch";

    const RefCkpt birth{.life_epoch = std::optional<uint64_t>{5}, .checkpoint_snapshot_id = std::nullopt,
                        .last_epoch_seal = std::nullopt};
    ASSERT_EQ(publishCkpt(*backend, layout, ns, birth, 1, ALWAYS_ADMITTED, generousDeadline()),
              CkptPublishOutcome::Published);
    const auto completed = readCkpt(*backend, layout, ns);
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->ckpt.life_epoch, 5u);
    EXPECT_EQ(completed->ckpt.checkpoint_snapshot_id, ID_1_1) << "and must not lose the checkpoint on the way in";
}

/// The conflict path is the whole reason the algorithm re-READS instead of retrying its bytes: the
/// winner's field must survive the loser's retry. Here a concurrent writer advances the seal between
/// our read and our CAS; our retry must merge onto the new body, not overwrite it.
TEST(CasRefCkpt, TokenConflictRereadsAndMergesOntoTheWinner)
{
    const Layout layout{"p"};
    const RootNamespace ns{"srv1/ckpt_conflict"};
    const String key = layout.refCkptKey(RefNamespaceId::stageATransition(ns));
    const RefCkpt base{.life_epoch = std::optional<uint64_t>{5}, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt};

    auto backend = std::make_shared<GetHookBackend>(key);
    ASSERT_EQ(backend->casPut(key, encodeRefCkpt(base), std::nullopt).outcome, CasOutcome::Committed);

    /// The concurrent sealer lands exactly ONCE, immediately after our first read -- so our first CAS
    /// carries a token that is no longer current, and our retry has to merge onto its body.
    bool interfered = false;
    backend->on_get = [&]
    {
        if (interfered)
            return;
        interfered = true;
        const RefCkpt sealer{.life_epoch = std::optional<uint64_t>{5}, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = ID_2_1};
        const HeadResult h = backend->head(key);
        ASSERT_EQ(backend->putOverwrite(key, encodeRefCkpt(mergeCkpt(base, sealer)), h.token).outcome,
                  PutOutcome::Done);
    };

    const RefCkpt publisher{.life_epoch = std::nullopt, .checkpoint_snapshot_id = ID_1_2, .last_epoch_seal = std::nullopt};
    EXPECT_EQ(publishCkpt(*backend, layout, ns, publisher, 1, ALWAYS_ADMITTED, generousDeadline()),
              CkptPublishOutcome::Published);

    const auto sample = readCkpt(*backend, layout, ns);
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(sample->ckpt.checkpoint_snapshot_id, ID_1_2) << "our own contribution must land";
    EXPECT_EQ(sample->ckpt.last_epoch_seal, ID_2_1)
        << "the concurrent writer's seal must survive our retry -- a retry that reused the body read "
           "before the conflict would silently drop it (TLC `_sab_sealclobbersbase`)";
    EXPECT_EQ(sample->ckpt.life_epoch, 5u);
    EXPECT_GE(backend->casPutCount(key), 2u) << "the first CAS must have been rejected, not skipped";
}

/// A contribution that adds nothing issues NO write. This is a correctness property, not a saving:
/// both writers publish on every snapshot and every seal, and a no-op write would mint a fresh token
/// each time, turning every other writer's in-flight CAS into a conflict for identical bytes.
TEST(CasRefCkpt, AnIdenticalMergedBodyIssuesNoWrite)
{
    auto backend = std::make_shared<CountingBackend>();
    const Layout layout{"p"};
    const RootNamespace ns{"srv1/ckpt_noop"};
    const String key = layout.refCkptKey(RefNamespaceId::stageATransition(ns));
    const RefCkpt full{.life_epoch = std::optional<uint64_t>{5}, .checkpoint_snapshot_id = ID_1_2, .last_epoch_seal = ID_2_1};

    ASSERT_EQ(publishCkpt(*backend, layout, ns, full, 1, ALWAYS_ADMITTED, generousDeadline()),
              CkptPublishOutcome::Published);
    const uint64_t writes_after_create = backend->casPutCount(key);
    const Token token_after_create = backend->head(key).token;

    /// The same contribution again, and a strictly OLDER one: neither adds anything.
    EXPECT_EQ(publishCkpt(*backend, layout, ns, full, 1, ALWAYS_ADMITTED, generousDeadline()),
              CkptPublishOutcome::IdenticalSkip);
    const RefCkpt older{.life_epoch = std::optional<uint64_t>{5}, .checkpoint_snapshot_id = ID_1_1, .last_epoch_seal = std::nullopt};
    EXPECT_EQ(publishCkpt(*backend, layout, ns, older, 1, ALWAYS_ADMITTED, generousDeadline()),
              CkptPublishOutcome::IdenticalSkip);

    EXPECT_EQ(backend->casPutCount(key), writes_after_create) << "a skip must issue no CAS at all";
    EXPECT_EQ(backend->head(key).token, token_after_create) << "and must not mint a new incarnation";
}

/// The fence is re-checked AFTER the read and BEFORE the write, on every attempt. A generation that
/// moved means this writer's lease incarnation is gone, so its merged body is stale even if the fence
/// is live again under a fresh incarnation.
TEST(CasRefCkpt, AFenceBumpBetweenTheReadAndTheCasWritesNothing)
{
    auto backend = std::make_shared<CountingBackend>();
    const Layout layout{"p"};
    const RootNamespace ns{"srv1/ckpt_fenced"};
    const String key = layout.refCkptKey(RefNamespaceId::stageATransition(ns));
    const RefCkpt base{.life_epoch = std::optional<uint64_t>{5}, .checkpoint_snapshot_id = ID_1_1, .last_epoch_seal = std::nullopt};
    ASSERT_EQ(publishCkpt(*backend, layout, ns, base, 1, ALWAYS_ADMITTED, generousDeadline()),
              CkptPublishOutcome::Published);
    const Token token_before = backend->head(key).token;
    const uint64_t writes_before = backend->casPutCount(key);

    /// The callback the pool wires from `CasMountRuntime::checkFenceOrThrow`: it throws when the
    /// generation moved since admission. Mirrors the real site's class (the transient, upstream-retryable
    /// one) so the stub cannot drift into testing a shape production never produces.
    const auto moved_fence = [](uint64_t admitted)
    {
        throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR,
            "fence generation moved since admission ({})", admitted);
    };

    const RefCkpt advance{.life_epoch = std::nullopt, .checkpoint_snapshot_id = ID_1_2, .last_epoch_seal = std::nullopt};
    EXPECT_EQ(publishCkpt(*backend, layout, ns, advance, 1, moved_fence, generousDeadline()),
              CkptPublishOutcome::FencedOut);
    EXPECT_EQ(backend->casPutCount(key), writes_before) << "the check precedes the CAS, so nothing is sent";
    EXPECT_EQ(backend->head(key).token, token_before);
    EXPECT_EQ(readCkptOrFail(*backend, layout, ns), base);
}

/// Persistent contention fails CLOSED and says so. There is no partial state to clean up -- every
/// attempt either committed the complete merged body or changed nothing -- but the caller must be told
/// its contribution is unpublished rather than left to assume it landed.
TEST(CasRefCkpt, AnExhaustedDeadlineUnderPersistentConflictThrowsRetryLater)
{
    const Layout layout{"p"};
    const RootNamespace ns{"srv1/ckpt_exhausted"};
    const String key = layout.refCkptKey(RefNamespaceId::stageATransition(ns));
    const RefCkpt base{.life_epoch = std::optional<uint64_t>{5}, .checkpoint_snapshot_id = ID_1_1, .last_epoch_seal = std::nullopt};

    auto backend = std::make_shared<GetHookBackend>(key);
    ASSERT_EQ(backend->casPut(key, encodeRefCkpt(base), std::nullopt).outcome, CasOutcome::Committed);

    /// Every read is followed by a rewrite of the SAME body under a fresh incarnation, so the token this
    /// call holds is always stale and every CAS it issues conflicts. The clock advances one step per
    /// read, so the DEADLINE is what ends the loop -- deterministically, with no sleeping and well
    /// before the live-lock brake.
    uint64_t now = 0;
    backend->on_get = [&]
    {
        ++now;
        const HeadResult h = backend->head(key);
        if (h.exists)
            backend->putOverwrite(key, encodeRefCkpt(base), h.token);
    };

    const RefCkpt advance{.life_epoch = std::nullopt, .checkpoint_snapshot_id = ID_1_2, .last_epoch_seal = std::nullopt};
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR,
        [&] { publishCkpt(*backend, layout, ns, advance, 1, ALWAYS_ADMITTED, CkptDeadline{[&] { return now; }, 5}); });
    EXPECT_EQ(readCkptOrFail(*backend, layout, ns), base) << "no partial state: every attempt either "
                                                             "committed the complete merged body or wrote nothing";
}

/// A `_ckpt` that does not decode is NEVER overwritten. It is the only record of recovery's base and
/// of what cleanup may delete, so replacing it with a body derived from the contribution alone would
/// erase the base and leave a well-formed object a reader would trust.
TEST(CasRefCkpt, ACorruptCheckpointIsNeverOverwritten)
{
    auto backend = std::make_shared<CountingBackend>();
    const Layout layout{"p"};
    const RootNamespace ns{"srv1/ckpt_corrupt"};
    const String key = layout.refCkptKey(RefNamespaceId::stageATransition(ns));
    ASSERT_EQ(publishCkpt(*backend, layout, ns,
                          RefCkpt{.life_epoch = std::optional<uint64_t>{5}, .checkpoint_snapshot_id = ID_1_2, .last_epoch_seal = std::nullopt},
                          1, ALWAYS_ADMITTED, generousDeadline()), CkptPublishOutcome::Published);

    const String garbage = "not a cas object\n";
    overwriteObject(*backend, key, garbage);

    const RefCkpt birth{.life_epoch = std::optional<uint64_t>{5}, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt};
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { publishCkpt(*backend, layout, ns, birth, 1, ALWAYS_ADMITTED, generousDeadline()); });
    EXPECT_EQ(backend->get(key)->bytes, garbage) << "corruption must be surfaced, never laundered into a "
                                                    "well-formed object";
}

/// ---------------------------------------------------------------------------------------------
/// The reader-side rules Task 6 and the cleanup call sites consume
/// ---------------------------------------------------------------------------------------------

/// INV-4's three-way revalidation of a base that turned out to be missing.
TEST(CasRefCkpt, AMissingSampledBaseRestartsOnAnAdvancedTokenAndIsCorruptionOnAnUnchangedOne)
{
    const Token sampled{"t1", TokenType::Emulated};
    const Token advanced{"t2", TokenType::Emulated};

    EXPECT_EQ(classifyMissingSampledBase(sampled, advanced), MissingBaseVerdict::RestartRecovery)
        << "cleanup legitimately moved the checkpoint while we read; restart from the newer base";
    EXPECT_EQ(classifyMissingSampledBase(sampled, sampled), MissingBaseVerdict::Corrupted)
        << "the checkpoint still names an object that is not there, which the strictly-below deletion "
           "gate makes unreachable in an honest run";
    EXPECT_EQ(classifyMissingSampledBase(sampled, std::nullopt), MissingBaseVerdict::Corrupted)
        << "a namespace with a sampled base and no checkpoint at all is worse, not better";
}

/// The deletion gate is STRICTLY below, because the checkpoint names the snapshot a recovery is
/// entitled to fetch by exact key. At-or-below is TLC counterexample `_sab_staleckptcorruption`.
TEST(CasRefCkpt, SnapshotsAreDeletableStrictlyBelowTheCheckpoint)
{
    EXPECT_TRUE(snapshotDeletableUnderCkpt(ID_1_1, ID_1_2));
    EXPECT_FALSE(snapshotDeletableUnderCkpt(ID_1_2, ID_1_2)) << "the checkpoint's own base is off limits";
    EXPECT_FALSE(snapshotDeletableUnderCkpt(ID_2_1, ID_1_2));
    /// Fail closed: a namespace with no checkpoint has established no covering base, so nothing is
    /// deletable -- a stale or absent pointer may only ever under-clean.
    EXPECT_FALSE(snapshotDeletableUnderCkpt(ID_1_1, std::nullopt));
}

/// ---------------------------------------------------------------------------------------------
/// The REAL call sites, through the ledger
/// ---------------------------------------------------------------------------------------------

/// The namespace-birth transaction creates the checkpoint, and it is the only writer that can: the
/// `life_epoch` is this transaction's own writer epoch.
TEST(CasRefCkpt, NamespaceBirthCreatesTheCheckpointCarryingItsLifeEpoch)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/ckpt_birth"};

    EXPECT_FALSE(backend->head(store->layout().refCkptKey(RefNamespaceId::stageATransition(ns))).exists)
        << "nothing exists before the birth";
    ASSERT_EQ(publishRef(store, ns, "ref_1", 1), (RefTxnId{store->writerEpoch(), 1}));

    const auto sample = readCkpt(*backend, store->layout(), ns);
    ASSERT_TRUE(sample.has_value()) << "spec §3 creates the _ckpt before the namespace becomes Live";
    EXPECT_EQ(sample->ckpt.life_epoch, store->writerEpoch());
    EXPECT_FALSE(sample->ckpt.checkpoint_snapshot_id.has_value()) << "a newborn namespace has no base yet";
    EXPECT_FALSE(sample->ckpt.last_epoch_seal.has_value());
}

/// The snapshot publisher is INV-4's second writer: the body PUT commits, then the checkpoint names it.
TEST(CasRefCkpt, ACommittedSnapshotPublishAdvancesTheCheckpoint)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    const uint64_t epoch = store->writerEpoch();
    const RootNamespace ns{"srv1/ckpt_publish"};

    ASSERT_EQ(publishRef(store, ns, "ref_1", 1), (RefTxnId{epoch, 1}));
    ASSERT_FALSE(readCkptOrFail(*backend, store->layout(), ns).checkpoint_snapshot_id.has_value());

    ASSERT_TRUE(store->trySnapshotPublishOnce(ns));
    const auto published = store->newestPublishedSnapshotIdForTest(ns);
    ASSERT_TRUE(published.has_value());

    const auto sample = readCkpt(*backend, store->layout(), ns);
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(sample->ckpt.checkpoint_snapshot_id, published);
    EXPECT_EQ(sample->ckpt.life_epoch, epoch) << "the publisher contributes nothing about life_epoch, so "
                                                 "the merge must preserve what the birth wrote";
    /// And the snapshot body it names really is there -- the checkpoint may never point at a key that
    /// does not exist, which is the premise the missing-base rule reasons from.
    EXPECT_TRUE(backend->head(store->layout().refSnapshotKey(RefNamespaceId::stageATransition(ns), *published)).exists);
}

/// The body-PUT/cleanup/`_ckpt` race, decided by the ORDER of the two writes: cleanup planned in the
/// window between the snapshot body PUT and the checkpoint CAS still reads the OLD checkpoint, and the
/// gate is strictly below it -- so it cannot delete the snapshot just published.
TEST(CasRefCkpt, CleanupPlannedBetweenTheBodyPutAndTheCkptCasCannotDeleteTheNewSnapshot)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    const uint64_t epoch = store->writerEpoch();
    const RootNamespace ns{"srv1/ckpt_race"};

    ASSERT_EQ(publishRef(store, ns, "ref_1", 1), (RefTxnId{epoch, 1}));
    ASSERT_TRUE(store->trySnapshotPublishOnce(ns));
    const RefTxnId first_snapshot = *store->newestPublishedSnapshotIdForTest(ns);

    ASSERT_EQ(publishRef(store, ns, "ref_2", 2), (RefTxnId{epoch, 2}));
    /// The checkpoint a cleanup pass sampled BEFORE the second publication -- the stale reading the
    /// race hands it.
    const std::optional<RefTxnId> stale_checkpoint = readCkptOrFail(*backend, store->layout(), ns).checkpoint_snapshot_id;
    ASSERT_EQ(stale_checkpoint, first_snapshot);

    ASSERT_TRUE(store->trySnapshotPublishOnce(ns));
    const RefTxnId second_snapshot = *store->newestPublishedSnapshotIdForTest(ns);
    ASSERT_LT(first_snapshot, second_snapshot);

    /// Planning against the STALE checkpoint: the just-published snapshot is not deletable, and neither
    /// is the one the stale checkpoint itself names. A stale pointer can only under-clean.
    EXPECT_FALSE(snapshotDeletableUnderCkpt(second_snapshot, stale_checkpoint));
    EXPECT_FALSE(snapshotDeletableUnderCkpt(first_snapshot, stale_checkpoint));
    /// Once the checkpoint is re-read, the older snapshot becomes reclaimable and the base does not.
    const std::optional<RefTxnId> fresh_checkpoint = readCkptOrFail(*backend, store->layout(), ns).checkpoint_snapshot_id;
    EXPECT_TRUE(snapshotDeletableUnderCkpt(first_snapshot, fresh_checkpoint));
    EXPECT_FALSE(snapshotDeletableUnderCkpt(second_snapshot, fresh_checkpoint));
}

/// One `_ckpt` write per publication and not one more: the checkpoint is written where the snapshot is
/// published, and a publisher with nothing above its newest snapshot touches it at all.
TEST(CasRefCkpt, TheCheckpointIsWrittenOncePerPublicationAndNotOnIdleAttempts)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const uint64_t epoch = store->writerEpoch();
    const RootNamespace ns{"srv1/ckpt_republish"};
    const String key = store->layout().refCkptKey(RefNamespaceId::stageATransition(ns));

    ASSERT_EQ(publishRef(store, ns, "ref_1", 1), (RefTxnId{epoch, 1}));
    const uint64_t writes_after_birth = backend->casPutCount(key);
    EXPECT_EQ(writes_after_birth, 1u) << "the birth creates the object with exactly one CAS";

    ASSERT_TRUE(store->trySnapshotPublishOnce(ns));
    EXPECT_EQ(backend->casPutCount(key), writes_after_birth + 1) << "one publication, one checkpoint CAS";
    const uint64_t writes_after_publish = backend->casPutCount(key);
    const auto after_publish = readCkpt(*backend, store->layout(), ns);
    ASSERT_TRUE(after_publish.has_value());

    /// Nothing was appended since, so there is nothing above the newest snapshot: the publisher declines
    /// before it reaches the checkpoint at all, and repeating the attempt changes nothing.
    EXPECT_FALSE(store->trySnapshotPublishOnce(ns));
    EXPECT_FALSE(store->trySnapshotPublishOnce(ns));
    EXPECT_EQ(backend->casPutCount(key), writes_after_publish);
    EXPECT_EQ(readCkptOrFail(*backend, store->layout(), ns), after_publish->ckpt);
}

/// Publication replays a `NeedsRecovery` lane before it captures a snapshot and advances `_ckpt`.
TEST(CasRefCkpt, NeedsRecoveryReplaysBeforeCheckpointAdvance)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/ckpt_poisoned"};
    const String key = store->layout().refCkptKey(RefNamespaceId::stageATransition(ns));

    ASSERT_EQ(publishRef(store, ns, "ref_1", 1), (RefTxnId{store->writerEpoch(), 1}));
    const auto before = readCkpt(*backend, store->layout(), ns);
    ASSERT_TRUE(before.has_value());
    ASSERT_FALSE(before->ckpt.checkpoint_snapshot_id.has_value());
    const uint64_t writes_before = backend->casPutCount(key);

    /// Enter `NeedsRecovery`: an install throws after its transaction is
    /// durable, leaving this cached table missing a transaction the log contains.
    auto planned = std::make_exception_ptr(DB::Exception(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED,
        "simulated allocation failure inside the post-durable install region"));
    auto fired = std::make_shared<std::atomic<bool>>(false);
    store->setInstallRegionProbeForTest([planned, fired]
    {
        if (fired->exchange(true))
            return;
        ALLOW_ALLOCATIONS_IN_SCOPE;
        std::rethrow_exception(planned);
    });
    expectThrowsCode(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED, [&] { publishRef(store, ns, "ref_2", 2); });
    store->setInstallRegionProbeForTest(nullptr);
    ASSERT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);

    /// The publish entry point recovers first, so the snapshot covers the stranded transaction.
    EXPECT_TRUE(store->trySnapshotPublishOnce(ns));
    EXPECT_TRUE(store->resolveRef(ns, "ref_2", /*allow_stale=*/false).has_value())
        << "the stranded transaction is durable; the re-derivation must have applied it";
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Ready);
    EXPECT_GT(backend->casPutCount(key), writes_before)
        << "and the checkpoint advances -- truthfully, over a snapshot that is not missing anything";
    EXPECT_TRUE(readCkptOrFail(*backend, store->layout(), ns).checkpoint_snapshot_id.has_value());

}

/// A publish admitted under an incarnation that is replaced mid-attempt advances NOTHING, and does not
/// adopt the snapshot either -- adopting would suppress every later publication for it while the
/// checkpoint still pointed below it, leaving recovery on an older base with nothing to fix it.
TEST(CasRefCkpt, APublishFencedOutMidAttemptDoesNotAdvanceTheCheckpoint)
{
    const Layout probe_layout{"p"};
    const RootNamespace ns{"srv1/ckpt_stale_gen"};
    const String ckpt_key = probe_layout.refCkptKey(RefNamespaceId::stageATransition(ns));

    auto backend = std::make_shared<GetHookBackend>(ckpt_key);
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    PoolPtr store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    ASSERT_EQ(publishRef(store, ns, "ref_1", 1), (RefTxnId{store->writerEpoch(), 1}));

    /// Installed only NOW, so the birth's own checkpoint creation is untouched: from here the mount is
    /// re-armed on the `_ckpt` READ, i.e. inside the read-then-CAS window of exactly the call under
    /// test. A re-arm bumps the fence GENERATION, which is what an admitted publish presents back.
    backend->on_get = [&] { DB::Cas::tests::rearmMountFenceAfterAnomalyForTest(store); };

    const auto before = readCkpt(*backend, store->layout(), ns);
    ASSERT_TRUE(before.has_value());
    ASSERT_FALSE(before->ckpt.checkpoint_snapshot_id.has_value());
    const uint64_t writes_before = backend->casPutCount(ckpt_key);

    EXPECT_FALSE(store->trySnapshotPublishOnce(ns))
        << "a publish whose checkpoint could not be advanced must not report success";
    EXPECT_EQ(backend->casPutCount(ckpt_key), writes_before) << "nothing may be sent after the fence moved";
    EXPECT_FALSE(readCkptOrFail(*backend, store->layout(), ns).checkpoint_snapshot_id.has_value());
    EXPECT_FALSE(store->newestPublishedSnapshotIdForTest(ns).has_value())
        << "the snapshot must not be adopted as the newest while its checkpoint is unpublished";
}

/// ===================================================================================
/// Equivalence fences for the `prepareRefChunk` extraction (Stage B `{#extract-prepare-ref-chunk}`)
/// ===================================================================================
///
/// An extraction is only safe to review if something pins what crosses its boundary. These three
/// fences are deliberately NOT red-first: they pass on the PRE-extraction tree and must keep passing
/// after it, which is the whole point -- the literals below were captured from a real append on the
/// pre-extraction tree and pasted in, so re-deriving them afterwards cannot silently measure the
/// change against itself.
///
/// They live in this TU rather than beside the pure preparation tests because all three need a real
/// backend and the real append lane, which this suite already drives through `publishRef` (including
/// the namespace birth, the one chunk shape whose first durable effect is the `_ckpt` and not the
/// ref-log `PUT`).
TEST(CasRefCkpt, CommitRefChunkDurableBytesUnchangedByExtraction)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"test/golden@cas@"};

    const RefTxnId id = publishRef(store, ns, "gold_ref", 7);
    ASSERT_EQ(id.writer_epoch, 1u);
    ASSERT_EQ(id.ref_sequence, 1u);

    /// The KEY carries the namespace incarnation, so its life segment is rendered rather than pasted
    /// (Task 1c re-keys it); every other segment is literal.
    const String key = store->layout().refLogKey(RefNamespaceId::stageATransition(ns), id);
    EXPECT_EQ(key, "p/cas/refs/test/golden@cas@/"
                   + renderRefIncarnation(RefNamespaceId::stageATransition(ns).incarnation)
                   + "/_log/0000000000000001-0000000000000001.zst")
        << "the canonical ref-log key the append lane derives";

    /// The BODY is pinned by exact length plus a 128-bit digest, which together pin it byte for byte
    /// while staying readable. It is a function of `{ns, id, ops, chain_link}` only -- no incarnation
    /// reaches it -- so these two literals survive Task 1c's re-keying as well.
    const auto got = backend->get(key);
    ASSERT_TRUE(got.has_value()) << "the birth chunk must be durable at its canonical key";
    EXPECT_EQ(got->bytes.size(), 177u) << "the sealed ref-log body changed size";
    SipHash body_hash;
    body_hash.update(got->bytes.data(), got->bytes.size());
    EXPECT_EQ(getHexUIntLowercase(body_hash.get128()), "447348741c9f402fb94767ae028a4e73")
        << "the sealed ref-log body changed content -- preparation must seal the same bytes it sealed "
           "before the extraction";
}

/// The directive's "preserve backend request counts", asserted rather than assumed: preparation is
/// pure, so lifting it out must not add, remove or reorder a single request. One birth chunk = exactly
/// one write-once `PUT` at the ref-log key, no read-back, and exactly one `_ckpt` CAS.
TEST(CasRefCkpt, AppendRequestCountUnchangedByExtraction)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"test/req@cas@"};

    const RefTxnId id = publishRef(store, ns, "req_ref", 1);
    const String log_key = store->layout().refLogKey(RefNamespaceId::stageATransition(ns), id);
    const String ckpt_key = store->layout().refCkptKey(RefNamespaceId::stageATransition(ns));

    EXPECT_EQ(backend->putCount(log_key), 1u) << "exactly one write-once PUT per committed chunk";
    EXPECT_EQ(backend->getCount(log_key), 0u) << "a Committed PUT owes no read-back";
    EXPECT_EQ(backend->casPutCount(ckpt_key), 1u)
        << "the birth contributes `life_epoch` in exactly one `_ckpt` CAS -- deciding that contribution "
           "earlier must not publish it twice, nor move the publish off the birth path";
}

/// The post-durable install region is the reason preparation has to happen where it does: everything
/// between "this object may be durable" and "the runtime records it" runs under
/// `DENY_ALLOCATIONS_IN_SCOPE` and must not allocate. The extraction moves work EARLIER, never into
/// that region.
///
/// WHAT THIS TEST PROVES, and what it does NOT -- stated precisely, because a fence trusted for more
/// than it checks is worse than no fence.
///
/// `DENY_ALLOCATIONS_IN_SCOPE` is `static_assert(true)` unless `!defined(NDEBUG)` (`MemoryTracker.h`),
/// so it is inert in every build that leaves `NDEBUG` defined -- which includes this gate and CI's
/// sanitizer lanes, since those configure `CMAKE_BUILD_TYPE=None` and `CMakeLists.txt` maps that to
/// `RelWithDebInfo`. Only a `Debug` build, or a tidy lane (which adds `-UNDEBUG`), has the
/// no-allocation half live. Nothing here proves the region does not allocate.
///
/// What is left is weaker than "the install is still guarded": `install_region_probe_for_test` fires
/// as the FIRST statement inside the guarded scope, BEFORE `rt->state.swap(*candidate)`, and the SAME
/// probe is shared by BOTH probe-instrumented post-durable install regions (`CasRefLedger.cpp`: the
/// wedge-resolution adoption and `commitRefChunk`'s `Committed` install). So `probe_hits > 0` proves
/// only that SOME probe-instrumented region was entered on this path -- which on this path can only be
/// the commit install, since nothing here wedges. It goes red if the region stops being entered at all
/// (a lost commit path, a skipped install arm); a refactor that lifted the swap out of the scope while
/// leaving the guard shell and the probe behind would keep it GREEN.
TEST(CasRefCkpt, PostDurableInstallRegionStillEnteredAfterExtraction)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"test/region@cas@"};

    unsigned probe_hits = 0;
    store->setInstallRegionProbeForTest([&probe_hits] { ++probe_hits; });
    const RefTxnId id = publishRef(store, ns, "region_ref", 1);
    store->setInstallRegionProbeForTest(nullptr);

    EXPECT_GT(probe_hits, 0u)
        << "no probe-instrumented post-durable install region was entered on a committing append -- "
           "the `Committed` install arm was not reached at all";
    EXPECT_TRUE(backend->get(store->layout().refLogKey(RefNamespaceId::stageATransition(ns), id)).has_value());
}
