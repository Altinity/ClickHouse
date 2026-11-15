#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include "cas_test_helpers.h"

#include <functional>
#include <set>
#include <stdexcept>
#include <utility>

using namespace DB::Cas;

using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::FakeClock;
using DB::Cas::tests::OperationForTest;
using DB::Cas::tests::ScopedReadCeiling;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::tombstoneTxn;

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
}

namespace
{

CasRequests makeRequests(BackendPtr backend, FakeClock & clock, Fence fence = Fence::open())
{
    return CasRequests(std::move(backend), std::move(fence), clock.nowFn(), clock.sleepFn(), nullptr, DB::Cas::kDefaultRequestWindowMs);
}

}

TEST(CASRefProtocol, CrossEpochFromSealShortCircuitsWithoutAnyRequest)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    Layout layout("pool");
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    const RootNamespace ns("t");
    const auto life = DB::Cas::tests::fixture::fixtureLife(ns);

    /// `from_seal == RefTxnId{}`: nothing consumed yet, so there is no seal to cross from -- proved
    /// without reading anything.
    {
        const EpochCrossResult r = crossEpochFromSeal(
            op, layout, ns, RefTxnId{}, std::nullopt, RefTxnId{2, 1}, life);
        EXPECT_EQ(r.outcome, EpochCrossOutcome::NothingConsumed);
    }

    /// The caller already decoded the record at `from_seal` and knows it is not an `EpochSeal` --
    /// also proved without any read here.
    {
        const EpochCrossResult r = crossEpochFromSeal(
            op, layout, ns, RefTxnId{1, 5}, /*seal_proven=*/false, RefTxnId{2, 1}, life);
        EXPECT_EQ(r.outcome, EpochCrossOutcome::NotASeal);
    }

    EXPECT_EQ(backend->getTotal(), 0u);
}

namespace
{

const RootNamespace kWalkNs("w");

/// Counts every request and fires a one-shot hook after the read of an exact key.
class ChainHookBackend : public CountingBackend
{
public:
    String watched_key;
    std::function<void()> after_read;

    std::optional<Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        std::optional<Raw> result = CountingBackend::read(key, access);
        if (after_read && key == watched_key)
            std::exchange(after_read, {})();
        return result;
    }
};

struct ChainFixture
{
    std::shared_ptr<ChainHookBackend> backend = std::make_shared<ChainHookBackend>();
    Layout layout{"pool"};
    NamespaceLifeId life = DB::Cas::tests::fixture::fixtureLife(kWalkNs);

    String logKey(RefTxnId id) const { return layout.refLogKey(life, id); }
    String ckptKey() const { return layout.refCkptKey(life); }

    /// `generation` 0 stamps the header the content needs.
    void putLog(const RefLogTxn & txn, uint32_t generation = 0)
    {
        OperationForTest op(*backend);
        const String text = generation ? encodeRefLogTxnAtGeneration(txn, generation) : encodeRefLogTxn(txn);
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).create(logKey(txn.txn_id), sealObject(FormatId::RefLog, text), Retry::once())));
    }

    void putSnapshot(RefTxnId id)
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(layout.refSnapshotKey(life, id),
            sealObject(FormatId::RefSnapshot, encodeRefTableSnapshot(DB::Cas::tests::minimalLiveSnapshot(kWalkNs.string(), id))),
            Retry::once())));
    }

    void remove(const String & key)
    {
        OperationForTest op(*backend);
        const auto head = (*op).head(key, Retry::once());
        ASSERT_TRUE(head.has_value());
        ASSERT_EQ((*op).remove(key, head->etag, Retry::once()), Removal::Removed);
    }

    CkptSample putCkpt(const RefCkpt & body, uint32_t generation)
    {
        OperationForTest op(*backend);
        EXPECT_TRUE(std::holds_alternative<Committed>(
            (*op).create(ckptKey(), encodeRefCkptAtGeneration(body, generation), Retry::once())));
        return *readCkpt(*op, layout, life);
    }

    void replaceCkpt(const RefCkpt & body, uint32_t generation)
    {
        OperationForTest op(*backend);
        const auto current = (*op).read(ckptKey(), Retry::once());
        ASSERT_TRUE(current.has_value());
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).replace(ckptKey(), encodeRefCkptAtGeneration(body, generation), current->etag, Retry::once())));
    }

    CatalogEntry catalogEntry() const
    {
        return CatalogEntry{.ns = kWalkNs, .state = NsState::Live, .incarnation = life.incarnation};
    }
};

RefCkpt generationThreeCkpt(uint64_t life_epoch, std::optional<RefTxnId> snapshot = std::nullopt)
{
    RefCkpt ckpt = DB::Cas::tests::refCkptOf(life_epoch, std::nullopt, snapshot);
    ckpt.generation = G_REF_CHAIN;
    return ckpt;
}

RefLogTxn birthAt(RefTxnId id)
{
    return RefLogTxn{kWalkNs.string(), id, {DB::Cas::tests::namespaceBirthOp()}, std::nullopt};
}

RefLogTxn ordinaryAt(RefTxnId id, std::optional<RefTxnId> backlink = std::nullopt)
{
    const ManifestRef m{id.writer_epoch, id.ref_sequence, 1};
    return RefLogTxn{kWalkNs.string(), id, DB::Cas::tests::publishCommittedOps("r" + std::to_string(id.ref_sequence), m), backlink};
}

RefLogTxn terminalAt(RefTxnId id)
{
    RefOp remove;
    remove.kind = RefOpKind::RemoveNamespace;
    return RefLogTxn{kWalkNs.string(), id, {remove}, std::nullopt};
}

RefChainOrigin originAt(RefTxnId first, const CkptSample & sample, std::optional<RefTxnId> old_frontier = std::nullopt)
{
    return RefChainOrigin{
        .first_key = first, .open_transition = std::nullopt, .ckpt_token = sample.etag,
        .confirm_base_after_end = true, .old_frontier = old_frontier};
}

const RefChainVisitor kVisitAll = [](RefLogTxn &&, uint64_t) { return true; };

RefChainWalk walk(ChainFixture & f, const RefChainOrigin & origin, const RefChainVisitor & visit = kVisitAll,
                  KeyReader * reader = nullptr)
{
    OperationForTest op(*f.backend);
    return walkRefChain(*op, f.layout, f.life, origin, visit, reader);
}

/// A reader that serves takes inline and records which keys are hinted and not yet taken.
class CountingKeyReader final : public KeyReader
{
public:
    explicit CountingKeyReader(CasOperation & op_) : op(op_) {}
    void hint(const String & key) override { outstanding.insert(key); ++hints; }
    std::optional<Object> take(const String & key) override
    {
        outstanding.erase(key);
        return op.read(key, Retry::standard());
    }
    void discard(const String & key) override { outstanding.erase(key); }
    size_t pending() const override { return outstanding.size(); }
    size_t window() const override { return 4; }

    uint64_t hints = 0;

private:
    CasOperation & op;
    std::set<String> outstanding;
};

}

TEST(CASRefChainWalk, EndsAtTheFirstAbsentKeyAndConfirmsTheBase)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    const CkptSample sample = f.putCkpt(generationThreeCkpt(1), 3);
    f.putLog(birthAt({1, 1}));
    f.putLog(ordinaryAt({1, 2}));
    f.backend->resetCounts();

    const RefChainWalk result = walk(f, originAt({1, 1}, sample));
    EXPECT_EQ(result.end, RefChainEnd::AbsentKey);
    EXPECT_EQ(result.next_key, (RefTxnId{1, 3}));
    EXPECT_EQ(result.records, 2u);
    EXPECT_EQ(f.backend->getCount(f.logKey({1, 1})) + f.backend->getCount(f.logKey({1, 2}))
              + f.backend->getCount(f.logKey({1, 3})), 3u);
    EXPECT_EQ(f.backend->getCount(f.ckptKey()), 1u);
}

TEST(CASRefChainWalk, FollowsAForwardLinkAndSkipsUnwrittenEpochs)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    const CkptSample sample = f.putCkpt(generationThreeCkpt(1), 3);
    f.putLog(birthAt({1, 1}));
    f.putLog(tombstoneTxn(kWalkNs.string(), {1, 2}, 5));
    f.putLog(ordinaryAt({5, 1}, RefTxnId{1, 2}), 3);
    f.backend->resetCounts();

    const RefChainWalk result = walk(f, originAt({1, 1}, sample));
    EXPECT_EQ(result.end, RefChainEnd::AbsentKey);
    EXPECT_EQ(result.next_key, (RefTxnId{5, 2}));
    for (const RefTxnId id : {RefTxnId{2, 1}, RefTxnId{3, 1}, RefTxnId{4, 1}, RefTxnId{1, 3}})
        EXPECT_EQ(f.backend->getCount(f.logKey(id)), 0u);
}

TEST(CASRefChainWalk, EndsAtATombstoneWhoseSuccessorIsAbsent)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    const CkptSample sample = f.putCkpt(generationThreeCkpt(1), 3);
    f.putLog(birthAt({1, 1}));
    f.putLog(tombstoneTxn(kWalkNs.string(), {1, 2}, 5));

    const RefChainWalk result = walk(f, originAt({1, 1}, sample));
    EXPECT_EQ(result.end, RefChainEnd::AbsentKey);
    EXPECT_EQ(result.next_key, (RefTxnId{5, 1}));
    EXPECT_EQ(result.open_transition, (RefEpochTransition{RefTxnId{1, 2}, 5}));
}

TEST(CASRefChainWalk, StopsAtATerminalRecordAndReadsNoKeyAfterIt)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    const CkptSample sample = f.putCkpt(generationThreeCkpt(1), 3);
    f.putLog(birthAt({1, 1}));
    f.putLog(terminalAt({1, 2}));
    f.putLog(ordinaryAt({1, 3}));
    f.backend->resetCounts();

    const RefChainWalk result = walk(f, originAt({1, 1}, sample));
    EXPECT_EQ(result.end, RefChainEnd::Terminal);
    EXPECT_EQ(f.backend->getCount(f.logKey({1, 3})), 0u);
    EXPECT_EQ(f.backend->getCount(f.ckptKey()), 1u);

    f.backend->watched_key = f.logKey({1, 2});
    f.backend->after_read = [&] { f.replaceCkpt(generationThreeCkpt(1, RefTxnId{1, 1}), 3); };
    EXPECT_EQ(walk(f, originAt({1, 1}, sample)).end, RefChainEnd::BaseMoved);
}

TEST(CASRefChainWalk, VisitorCanStopTheWalk)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    const CkptSample sample = f.putCkpt(generationThreeCkpt(1), 3);
    f.putLog(birthAt({1, 1}));
    f.putLog(ordinaryAt({1, 2}));
    f.putLog(ordinaryAt({1, 3}));
    f.backend->resetCounts();

    const RefChainWalk result = walk(f, originAt({1, 1}, sample),
        [](RefLogTxn && txn, uint64_t) { return txn.txn_id != RefTxnId{1, 2}; });
    EXPECT_EQ(result.end, RefChainEnd::Stopped);
    EXPECT_EQ(result.next_key, (RefTxnId{1, 3}));
    EXPECT_EQ(result.records, 2u);
    EXPECT_EQ(f.backend->getCount(f.logKey({1, 3})), 0u);
    EXPECT_EQ(f.backend->getCount(f.ckptKey()), 0u);
}

TEST(CASRefChainWalk, BaseMovedUnderTheWalkIsARestart)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    const CkptSample sample = f.putCkpt(generationThreeCkpt(1), 3);
    f.putLog(birthAt({1, 1}));
    f.putLog(ordinaryAt({1, 2}));
    f.backend->watched_key = f.logKey({1, 3});
    f.backend->after_read = [&] { f.replaceCkpt(generationThreeCkpt(1, RefTxnId{1, 2}), 3); };

    EXPECT_EQ(walk(f, originAt({1, 1}, sample)).end, RefChainEnd::BaseMoved);
}

TEST(CASRefChainWalk, FailedValidationWithAMovedBaseIsARestartNotCorruption)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    const CkptSample sample = f.putCkpt(generationThreeCkpt(1), 3);
    f.putLog(birthAt({1, 1}));
    f.putLog(tombstoneTxn(kWalkNs.string(), {1, 2}, 5));
    f.putLog(ordinaryAt({5, 1}, RefTxnId{1, 1}), 3);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { walk(f, originAt({1, 1}, sample)); });

    f.backend->watched_key = f.logKey({5, 1});
    f.backend->after_read = [&] { f.replaceCkpt(generationThreeCkpt(1, RefTxnId{1, 1}), 3); };
    EXPECT_EQ(walk(f, originAt({1, 1}, sample)).end, RefChainEnd::BaseMoved);

    ChainFixture g;
    const CkptSample g_sample = g.putCkpt(generationThreeCkpt(1), 3);
    g.putLog(birthAt({1, 1}));
    g.backend->resetCounts();
    EXPECT_THROW(walk(g, originAt({1, 1}, g_sample), [](RefLogTxn &&, uint64_t) -> bool { throw std::runtime_error("visit"); }),
                 std::runtime_error);
    EXPECT_EQ(g.backend->getCount(g.ckptKey()), 0u);
}

TEST(CASRefChainWalk, OldFrontierAboveTheEndIsCorruption)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    const CkptSample sample = f.putCkpt(DB::Cas::tests::refCkptOf(1, RefTxnId{1, 3}), 1);
    f.putLog(birthAt({1, 1}));
    f.putLog(ordinaryAt({1, 2}));
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { walk(f, originAt({1, 1}, sample, RefTxnId{1, 3})); });

    f.putLog(ordinaryAt({1, 3}));
    f.putLog(ordinaryAt({1, 4}));
    const RefChainWalk result = walk(f, originAt({1, 1}, sample, RefTxnId{1, 3}));
    EXPECT_EQ(result.end, RefChainEnd::AbsentKey);
    EXPECT_EQ(result.next_key, (RefTxnId{1, 5}));
    EXPECT_EQ(result.records, 4u);
}

namespace
{

/// Generation 1 `_ckpt` with frontier `{2,1}`; the chain is a birth and a terminal record at `{1,2}`.
CkptSample terminalBelowFrontier(ChainFixture & f)
{
    const CkptSample sample = f.putCkpt(DB::Cas::tests::refCkptOf(1, RefTxnId{2, 1}, std::nullopt, RefTxnId{1, 3}), 1);
    f.putLog(birthAt({1, 1}));
    f.putLog(terminalAt({1, 2}));
    return sample;
}

}

TEST(CASRefChainWalk, TerminalBelowTheOldFrontierIsCorruption)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    const CkptSample sample = terminalBelowFrontier(f);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { walk(f, originAt({1, 1}, sample, RefTxnId{2, 1})); });

    f.backend->watched_key = f.logKey({1, 2});
    f.backend->after_read = [&] { f.replaceCkpt(DB::Cas::tests::refCkptOf(1, RefTxnId{2, 2}, std::nullopt, RefTxnId{1, 3}), 1); };
    EXPECT_EQ(walk(f, originAt({1, 1}, sample, RefTxnId{2, 1})).end, RefChainEnd::BaseMoved);
}

TEST(CASRefChainWalk, ReadAheadIsDroppedWhenTheWalkThrows)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    const CkptSample sample = terminalBelowFrontier(f);
    OperationForTest op(*f.backend);
    CountingKeyReader reader(*op);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { walkRefChain(*op, f.layout, f.life, originAt({1, 1}, sample, RefTxnId{2, 1}), kVisitAll, &reader); });
    EXPECT_GT(reader.hints, 0u) << "the walk must have read ahead, or this proves nothing";
    EXPECT_EQ(reader.pending(), 0u);
}

TEST(CASRefProtocol, BaseAtSequenceOneNeedsATombstoneThatNamesItsEpoch)
{
    ScopedReadCeiling c(3);
    const RefCkpt ckpt = generationThreeCkpt(1, RefTxnId{5, 1});
    const auto base_with = [&](const std::optional<RefLogTxn> & predecessor, RefTxnId backlink)
    {
        auto f = std::make_unique<ChainFixture>();
        f->putCkpt(ckpt, 3);
        f->putSnapshot({5, 1});
        f->putLog(ordinaryAt({5, 1}, backlink), 3);
        if (predecessor)
            f->putLog(*predecessor);
        return f;
    };
    const auto read_base = [&](ChainFixture & f)
    {
        OperationForTest op(*f.backend);
        return readCheckpointSnapshotBase(*op, f.layout, f.life, ckpt);
    };

    {
        auto f = base_with(tombstoneTxn(kWalkNs.string(), {1, 2}, 5), {1, 2});
        EXPECT_EQ(read_base(*f).predecessor_seal_id, (RefTxnId{1, 2}));
    }
    {
        auto f = base_with(tombstoneTxn(kWalkNs.string(), {1, 2}, 4), {1, 2});
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { read_base(*f); });
    }
    {
        auto f = base_with(ordinaryAt({1, 2}), {1, 2});
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { read_base(*f); });
    }
    {
        auto f = base_with(std::nullopt, {1, 2});
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { read_base(*f); });
    }
    {
        auto f = base_with(tombstoneTxn(kWalkNs.string(), {4, 7}, std::nullopt), {4, 7});
        EXPECT_EQ(read_base(*f).predecessor_seal_id, (RefTxnId{4, 7}));
    }
}

TEST(CASRefProtocol, BaseMovedWhileLoadingTheBaseIsARestart)
{
    ScopedReadCeiling c(3);
    const auto build = [](ChainFixture & f)
    {
        f.putLog(birthAt({1, 1}));
        for (uint64_t s = 2; s <= 4; ++s)
            f.putLog(ordinaryAt({1, s}));
        f.putSnapshot({1, 2});
        f.putSnapshot({1, 4});
        return f.putCkpt(generationThreeCkpt(1, RefTxnId{1, 2}), 3);
    };

    {
        ChainFixture f;
        const CkptSample sample = build(f);
        f.backend->watched_key = f.logKey({1, 2});
        f.backend->after_read = [&]
        {
            f.remove(f.layout.refSnapshotKey(f.life, {1, 2}));
            f.replaceCkpt(generationThreeCkpt(1, RefTxnId{1, 4}), 3);
        };
        OperationForTest op(*f.backend);
        EXPECT_FALSE(recoverRefTableDetailedFromAuthority(*op, f.layout, f.catalogEntry(), sample).has_value());
    }
    {
        ChainFixture f;
        const CkptSample sample = build(f);
        f.backend->watched_key = f.logKey({1, 2});
        f.backend->after_read = [&] { f.remove(f.layout.refSnapshotKey(f.life, {1, 2})); };
        OperationForTest op(*f.backend);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
            [&] { recoverRefTableDetailedFromAuthority(*op, f.layout, f.catalogEntry(), sample); });
    }
}

namespace
{

RefCursorResume resume(ChainFixture & f, RefTxnId cursor, std::optional<RefTxnId> snapshot)
{
    OperationForTest op(*f.backend);
    return resolveRefCursorSuccessor(*op, f.layout, f.life, cursor, snapshot);
}

}

TEST(CASRefCursorSuccessor, CursorAtOrAboveTheSnapshotIsReadForItsKind)
{
    ScopedReadCeiling c(3);
    const auto at_cursor = [](const std::optional<RefLogTxn> & record)
    {
        auto f = std::make_unique<ChainFixture>();
        if (record)
            f->putLog(*record);
        return f;
    };
    const RefTxnId cursor{1, 2};
    {
        auto f = at_cursor(ordinaryAt(cursor));
        const RefCursorResume r = resume(*f, cursor, std::nullopt);
        EXPECT_EQ(r.kind, RefCursorResumeKind::Key);
        EXPECT_EQ(r.next_key, (RefTxnId{1, 3}));
        EXPECT_EQ(r.body_gets, 1u);
    }
    {
        auto f = at_cursor(tombstoneTxn(kWalkNs.string(), cursor, 5));
        const RefCursorResume r = resume(*f, cursor, std::nullopt);
        EXPECT_EQ(r.kind, RefCursorResumeKind::Key);
        EXPECT_EQ(r.next_key, (RefTxnId{5, 1}));
        EXPECT_EQ(r.open_transition, (RefEpochTransition{cursor, 5}));
        EXPECT_EQ(r.body_gets, 1u);
    }
    {
        auto f = at_cursor(tombstoneTxn(kWalkNs.string(), cursor, std::nullopt));
        const RefCursorResume r = resume(*f, cursor, std::nullopt);
        EXPECT_EQ(r.kind, RefCursorResumeKind::Key);
        EXPECT_EQ(r.next_key, (RefTxnId{2, 1}));
        EXPECT_EQ(r.body_gets, 1u);
    }
    {
        auto f = at_cursor(terminalAt(cursor));
        const RefCursorResume r = resume(*f, cursor, std::nullopt);
        EXPECT_EQ(r.kind, RefCursorResumeKind::Terminal);
        EXPECT_EQ(r.body_gets, 1u);
    }
    {
        auto f = at_cursor(std::nullopt);
        EXPECT_EQ(resume(*f, cursor, std::nullopt).kind, RefCursorResumeKind::Hold);
    }
}

TEST(CASRefCursorSuccessor, CursorBelowTheSnapshotIsNeverRead)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    f.putLog(ordinaryAt({5, 1}, RefTxnId{1, 2}), 3);
    f.putLog(ordinaryAt({1, 2}));
    f.putLog(ordinaryAt({1, 3}));
    f.backend->resetCounts();

    const RefCursorResume r = resume(f, {1, 2}, RefTxnId{5, 3});
    EXPECT_EQ(r.kind, RefCursorResumeKind::Key);
    EXPECT_EQ(r.next_key, (RefTxnId{5, 1}));
    EXPECT_EQ(r.open_transition, (RefEpochTransition{RefTxnId{1, 2}, 5}));
    EXPECT_EQ(f.backend->getCount(f.logKey({1, 2})), 0u);
    EXPECT_EQ(f.backend->getCount(f.logKey({1, 3})), 0u);
}

TEST(CASRefCursorSuccessor, OrdinaryCursorBelowTheSnapshotResumesInItsEpoch)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    const RefCursorResume r = resume(f, {1, 4}, RefTxnId{1, 9});
    EXPECT_EQ(r.kind, RefCursorResumeKind::Key);
    EXPECT_EQ(r.next_key, (RefTxnId{1, 5}));
    EXPECT_EQ(r.body_gets, 0u);
}

TEST(CASRefCursorSuccessor, BackChainCrossesIntermediateEpochs)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    f.putLog(ordinaryAt({3, 1}, RefTxnId{1, 2}), 3);
    f.putLog(ordinaryAt({5, 1}, RefTxnId{3, 2}), 3);
    const RefCursorResume r = resume(f, {1, 2}, RefTxnId{5, 2});
    EXPECT_EQ(r.kind, RefCursorResumeKind::Key);
    EXPECT_EQ(r.next_key, (RefTxnId{3, 1}));
    EXPECT_EQ(r.body_gets, 2u);
}

TEST(CASRefCursorSuccessor, BacklinkBelowOrBeforeTheCursorHolds)
{
    ScopedReadCeiling c(3);
    ChainFixture f;
    f.putLog(ordinaryAt({5, 1}, RefTxnId{1, 2}), 3);
    EXPECT_EQ(resume(f, {2, 7}, RefTxnId{5, 2}).kind, RefCursorResumeKind::Hold);
    EXPECT_EQ(resume(f, {1, 5}, RefTxnId{5, 2}).kind, RefCursorResumeKind::Hold);

    ChainFixture empty;
    EXPECT_EQ(resume(empty, {1, 2}, RefTxnId{5, 2}).kind, RefCursorResumeKind::Hold);
}
