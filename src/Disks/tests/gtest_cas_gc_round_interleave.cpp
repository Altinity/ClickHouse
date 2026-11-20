#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedExchange.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <base/scope_guard.h>

#include <atomic>
#include <filesystem>

/// A copy of a part that adopts its entries without reading the bodies, and the drop of its source, at every point
/// of a GC round, for every state of the blob's GC row, through the three copy paths. The oracle of every cell: no
/// committed part ever names a missing body, and once nothing names the blob it is reclaimed.

namespace DB::ContentAddressedSetting
{
extern const ContentAddressedSettingsUInt64 gc_io_concurrency;
extern const ContentAddressedSettingsUInt64 gc_round_intent_create_budget;
extern const ContentAddressedSettingsUInt64 gc_shards;
}

namespace DB::ErrorCodes
{
extern const int NO_REPLICA_HAS_PART;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

enum class CopyPath : uint8_t
{
    /// `createHardLink` into another table's part, then the transaction commit.
    HardLink,
    /// `CachedPartFolderAccess::republishRef` into another namespace; it drops its own source.
    Republish,
    /// A relink fetch: the sender's offer, `prepareAdoptFromManifest`, the sender's `confirmExactRef`, the promote.
    Relink,
};

enum class SourceDrop : uint8_t
{
    /// The copy and the drop of its source both land before the round.
    BeforeTheRound,
    /// At the point: the copy reads its source, the source is dropped, then the copy tries to commit.
    InsideTheCopy,
    /// The copy reads its source before the round; the source is dropped and rounds reclaim its body; the copy tries
    /// to commit at the point.
    ReclaimedInsideTheCopy,
    /// At the point: the copy commits, then its source is dropped.
    AfterTheCopy,
    /// The copy commits at the point; its source is dropped after the round.
    AfterTheRound,
};

enum class RowState : uint8_t
{
    None,
    Candidate,
    Pending,
    Standing,
};

enum class Hops : uint8_t
{
    /// One copy from the writer's table into an existing table.
    One,
    /// The writer's part is copied into a middle table in one round and from there into a last table in the next.
    Relay,
    /// One copy into a table that does not exist at the round's catalog read.
    Newborn,
};

/// Where in the round the copy lands. `pass` and `destination_first` matter only for `BetweenNamespaces`: the walk of
/// the first namespace of the pair, then the event, then the walk of the second.
struct EventPoint
{
    RoundInterleavePoint point;
    uint8_t pass = 1;
    bool destination_first = true;
};

const std::vector<EventPoint> kEventPoints = {
    {.point = RoundInterleavePoint::AfterCatalogRead},
    {.point = RoundInterleavePoint::BetweenNamespaces, .pass = 1, .destination_first = true},
    {.point = RoundInterleavePoint::BetweenNamespaces, .pass = 1, .destination_first = false},
    {.point = RoundInterleavePoint::BetweenNamespaces, .pass = 2, .destination_first = true},
    {.point = RoundInterleavePoint::BetweenNamespaces, .pass = 2, .destination_first = false},
    {.point = RoundInterleavePoint::AfterIntake},
    {.point = RoundInterleavePoint::AfterRequestsBeforeCommit},
};

/// The GC shard count of a sharded cell; its blob's row is in a shard other than the first.
constexpr uint64_t kShardedGc = 2;

struct Cell
{
    CopyPath path;
    SourceDrop drop;
    RowState row;
    Hops hops;
    /// Ignored for `SourceDrop::BeforeTheRound`.
    EventPoint at;
    uint64_t gc_shards = 1;
};

const char * name(CopyPath v)
{
    switch (v)
    {
        case CopyPath::HardLink: return "HardLink";
        case CopyPath::Republish: return "Republish";
        case CopyPath::Relink: return "Relink";
    }
    return "?";
}

const char * name(SourceDrop v)
{
    switch (v)
    {
        case SourceDrop::BeforeTheRound: return "DroppedBeforeTheRound";
        case SourceDrop::InsideTheCopy: return "DroppedInsideTheCopy";
        case SourceDrop::ReclaimedInsideTheCopy: return "ReclaimedInsideTheCopy";
        case SourceDrop::AfterTheCopy: return "DroppedAfterTheCopy";
        case SourceDrop::AfterTheRound: return "DroppedAfterTheRound";
    }
    return "?";
}

const char * name(RowState v)
{
    switch (v)
    {
        case RowState::None: return "NoRow";
        case RowState::Candidate: return "Candidate";
        case RowState::Pending: return "Pending";
        case RowState::Standing: return "Standing";
    }
    return "?";
}

const char * name(Hops v)
{
    switch (v)
    {
        case Hops::One: return "OneHop";
        case Hops::Relay: return "Relay";
        case Hops::Newborn: return "Newborn";
    }
    return "?";
}

String name(const EventPoint & at)
{
    String out = roundInterleavePointName(at.point);
    if (at.point == RoundInterleavePoint::BetweenNamespaces)
        out += fmt::format("Pass{}{}", at.pass, at.destination_first ? "DestinationFirst" : "SourceFirst");
    return out;
}

String cellName(const Cell & cell)
{
    String out = fmt::format("{}_{}_{}_{}", name(cell.path), name(cell.drop), name(cell.row), name(cell.hops));
    if (cell.drop != SourceDrop::BeforeTheRound)
        out += "_" + name(cell.at);
    if (cell.gc_shards != 1)
        out += fmt::format("_GcShards{}", cell.gc_shards);
    return out;
}

/// Why a cell of the cross product cannot occur; empty when it can.
String exclusionReason(const Cell & cell)
{
    if (cell.gc_shards != 1 && cell.path != CopyPath::HardLink)
        return "the shard of a blob's row depends only on its hash, not on the path that copied it, so several GC shards "
               "run with `HardLink` only";
    if (cell.drop == SourceDrop::BeforeTheRound && cell.hops == Hops::Newborn)
        return "a destination copied before the round exists at the round's catalog read";
    if (cell.drop == SourceDrop::AfterTheRound && cell.path == CopyPath::Republish)
        return "`republishRef` drops its source in the call that copies it";
    if ((cell.drop == SourceDrop::InsideTheCopy || cell.drop == SourceDrop::ReclaimedInsideTheCopy) && cell.hops == Hops::Relay)
        return "the first copy is refused, so there is nothing to relay";
    if (cell.drop == SourceDrop::ReclaimedInsideTheCopy && cell.path == CopyPath::Republish)
        return "`republishRef` reads and commits in one call, so no round runs between them";
    return {};
}

/// Every cell of the cross product: the `BeforeTheRound` cells once, the others once per event point.
std::vector<Cell> crossProduct()
{
    std::vector<Cell> out;
    for (uint64_t gc_shards : {uint64_t{1}, kShardedGc})
        for (CopyPath path : {CopyPath::HardLink, CopyPath::Republish, CopyPath::Relink})
            for (SourceDrop drop : {SourceDrop::BeforeTheRound, SourceDrop::InsideTheCopy, SourceDrop::ReclaimedInsideTheCopy,
                                    SourceDrop::AfterTheCopy, SourceDrop::AfterTheRound})
                for (RowState row : {RowState::None, RowState::Candidate, RowState::Pending, RowState::Standing})
                    for (Hops hops : {Hops::One, Hops::Relay, Hops::Newborn})
                    {
                        if (drop == SourceDrop::BeforeTheRound)
                        {
                            out.push_back({.path = path, .drop = drop, .row = row, .hops = hops, .at = kEventPoints.front(),
                                           .gc_shards = gc_shards});
                            continue;
                        }
                        for (const EventPoint & at : kEventPoints)
                            out.push_back({.path = path, .drop = drop, .row = row, .hops = hops, .at = at, .gc_shards = gc_shards});
                    }
    return out;
}

std::vector<Cell> generatedCells()
{
    std::vector<Cell> out;
    for (const Cell & cell : crossProduct())
        if (exclusionReason(cell).empty())
            out.push_back(cell);
    return out;
}

/// One table of the fixture. The first character of `uuid` sets where the table's namespace sorts in the walk.
struct Table
{
    String uuid;
    String dir;
    RootNamespace ns;
};

/// A part that names the blob.
struct Holder
{
    const Table * table;
    String ref;
};

const UInt128 kLeader = hexToU128("000000000000000000000000000000d1");

/// One storage over a local object store, a GC leader over its pool, and the tables of a cell.
class InterleaveFixture
{
public:
    explicit InterleaveFixture(const Cell & cell_) : cell(cell_)
    {
        auto settings = makeSettingsForTest("test", std::filesystem::temp_directory_path() / "cas_interleave_scratch");
        /// Sequential reads: every read of the walk happens at its place in the round, none ahead of a point.
        settings[DB::ContentAddressedSetting::gc_io_concurrency] = 1;
        /// One create per round: the decoy takes it, so the blob stays a candidate.
        if (cell.row == RowState::Candidate)
            settings[DB::ContentAddressedSetting::gc_round_intent_create_budget] = 1;
        settings[DB::ContentAddressedSetting::gc_shards] = cell.gc_shards;
        storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
            makeLocalObjectStorageForTest(), "pool", "srv1", "", nullptr, settings);
        storage->startup();
        store = storage->store();
        gc = std::make_unique<Gc>(store, kLeader);
        body_key = store->layout().blobKey(blob);
        store->setEventSink([this](CasEvent e)
        {
            if (e.type == CasEventType::BlobDelete && e.object_hash == blobIdOf(blob))
            {
                ++body_deletes;
                if (named.load())
                    ++body_deletes_while_named;
            }
        });

        /// The destination sorts before the source when `destination_first`, after it otherwise.
        const bool destination_first = cell.at.destination_first;
        source = makeTable(destination_first ? 'c' : 'a', 1);
        middle = makeTable('b', 2);
        last = makeTable(destination_first ? 'a' : 'c', 3);
        bystander = makeTable(destination_first ? 'a' : 'c', 4);
        newborn = makeTable('d', 5);
    }

    /// The sink writes into members that are destroyed before the storage.
    ~InterleaveFixture() { store->setEventSink({}); }

    InterleaveFixture(const InterleaveFixture &) = delete;
    InterleaveFixture & operator=(const InterleaveFixture &) = delete;

    /// The tables a hop copies from and into, in hop order.
    std::vector<std::pair<const Table *, const Table *>> hopTables() const
    {
        switch (cell.hops)
        {
            case Hops::One: return {{&source, &last}};
            case Hops::Relay: return {{&source, &middle}, {&middle, &last}};
            case Hops::Newborn: return {{&source, &newborn}};
        }
        return {};
    }

    /// The pair whose walks the `BetweenNamespaces` event sits between: the hop's tables, or for a newborn
    /// destination, which is not walked, the bystander in its place.
    std::pair<const Table *, const Table *> walkPair(const Table * from, const Table * to) const
    {
        return {from, cell.hops == Hops::Newborn ? &bystander : to};
    }

    /// The tables that exist before the scenario.
    std::vector<const Table *> existingTables() const
    {
        switch (cell.hops)
        {
            case Hops::One: return {&source, &last};
            case Hops::Relay: return {&source, &middle, &last};
            case Hops::Newborn: return {&source, &bystander};
        }
        return {};
    }

    String nextPart()
    {
        const size_t n = ++part_counter;
        return fmt::format("all_{}_{}_0", n, n);
    }

    void commitPart(const Table & table, const String & part, const String & bytes)
    {
        auto tx = storage->createTransaction();
        auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
        auto buf = ca_tx.writeFile(table.dir + "tmp_insert_" + part + "/data.bin", 65536, DB::WriteMode::Rewrite, {});
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
        tx->moveDirectory(table.dir + "tmp_insert_" + part, table.dir + part);
        tx->commit(DB::NoCommitOptions{});
    }

    void filler(const Table & table)
    {
        const String part = nextPart();
        commitPart(table, part, "a filler " + part + " of " + table.uuid);
    }

    void drop(const Holder & holder)
    {
        store->dropRef(holder.table->ns, holder.ref);
        forget(holder);
    }

    /// For a part that is already gone.
    void forget(const Holder & holder)
    {
        std::erase_if(holders, [&](const Holder & h) { return h.table == holder.table && h.ref == holder.ref; });
        named = !holders.empty();
    }

    void addHolder(const Holder & holder)
    {
        holders.push_back(holder);
        named = true;
    }

    RoundReport round()
    {
        const RoundReport report = runRegularRoundReclaiming(*gc);
        store->renewWatermarkOnce();
        return report;
    }

    std::optional<RetiredEntry> row() const
    {
        for (const RetiredEntry & entry : currentRetiredSet(*store->poolBackendPtr(), store->layout(), blobShard(blob, cell.gc_shards)))
            if (entry.ref == blob)
                return entry;
        return std::nullopt;
    }

    bool bodyStored()
    {
        auto op = store->mountRequests().admit();
        return op.head(body_key, Retry::standard()).has_value();
    }

    /// No committed part names a missing body.
    void check(const String & where)
    {
        const FsckReport report = runFsck(*store, /*detail=*/true);
        EXPECT_EQ(report.dangling, 0u) << where << ": " << formatFsckSummary(report);
        if (!holders.empty())
            EXPECT_TRUE(bodyStored()) << where << ": the body is gone while " << holders.size() << " part(s) name it";
    }

    /// The writer's part `all_1_1_0` names the blob, and the blob's row is in `cell.row`.
    void prepareRow()
    {
        for (const Table * table : existingTables())
            filler(*table);
        const Holder writer{&source, "all_1_1_0"};
        const String decoy_payload = decoyPayload();
        switch (cell.row)
        {
            case RowState::None:
                commitPart(source, writer.ref, payload);
                addHolder(writer);
                round();
                ASSERT_FALSE(row().has_value());
                break;
            case RowState::Candidate:
            case RowState::Pending:
            {
                /// The writer adopts the stored body after the cut of the condemning round, before the intent exists:
                /// that round misses its edge and condemns the blob.
                commitPart(source, "all_2_2_0", payload);
                if (cell.row == RowState::Candidate)
                    commitPart(source, "all_3_3_0", decoy_payload);
                round();
                store->dropRef(source.ns, "all_2_2_0");
                if (cell.row == RowState::Candidate)
                    store->dropRef(source.ns, "all_3_3_0");
                store->renewWatermarkOnce();
                const RoundWithCallback condemning = runRoundWithCallback(*gc, RoundInterleavePoint::AfterIntake, {}, [&]
                {
                    commitPart(source, writer.ref, payload);
                    addHolder(writer);
                });
                store->renewWatermarkOnce();
                ASSERT_EQ(condemning.fired, 1u);
                ASSERT_TRUE(row().has_value()) << "the condemning round wrote no row";
                if (cell.row == RowState::Pending)
                    ASSERT_TRUE(row()->delete_pending && !row()->standing) << "the row is not pending";
                else
                    ASSERT_TRUE(!row()->delete_pending && !row()->standing) << "the row is not a candidate";
                break;
            }
            case RowState::Standing:
                commitPart(source, "all_2_2_0", payload);
                round();
                store->dropRef(source.ns, "all_2_2_0");
                store->renewWatermarkOnce();
                round();
                round();
                ASSERT_TRUE(row() && row()->standing) << "the body was not deleted";
                /// The writer finds no body and uploads a new incarnation; the round of the scenario reads its edge.
                commitPart(source, writer.ref, payload);
                addHolder(writer);
                ASSERT_TRUE(row() && row()->standing);
                break;
        }
        check("after the row setup");
    }

    /// Starts the copy of `from` into `to`: what the copy reads of its source happens now, the commit happens when
    /// the returned function runs. It returns the new holder, or nothing when the copy was refused. For a republish,
    /// which reads and commits in one call, `drop_inside` drops the source between the two.
    std::function<std::optional<Holder>()> beginCopy(const Holder & from, const Table & to, bool drop_inside)
    {
        const String part = "all_1_1_0";
        const auto refused = [](const DB::Exception & e)
        {
            if (e.code() != DB::ErrorCodes::NO_REPLICA_HAS_PART)
                throw;
            return std::optional<Holder>{};
        };
        switch (cell.path)
        {
            case CopyPath::HardLink:
            {
                std::shared_ptr<DB::IMetadataTransaction> tx = storage->createTransaction();
                tx->createHardLink(from.table->dir + from.ref + "/data.bin", to.dir + "tmp_clone_" + part + "/data.bin");
                tx->moveDirectory(to.dir + "tmp_clone_" + part, to.dir + part);
                return [tx, refused, holder = Holder{&to, part}]() -> std::optional<Holder>
                {
                    try
                    {
                        tx->commit(DB::NoCommitOptions{});
                    }
                    catch (const DB::Exception & e)
                    {
                        return refused(e);
                    }
                    return holder;
                };
            }
            case CopyPath::Republish:
                return [this, from, refused, drop_inside, holder = Holder{&to, part}]() -> std::optional<Holder>
                {
                    auto access = storage->partAccess();
                    if (drop_inside)
                        access->setHookForTest(CachedPartFolderAccess::TestHook::BeforeDestinationPublish, [&] { drop(from); });
                    SCOPE_EXIT(access->setHookForTest(CachedPartFolderAccess::TestHook::BeforeDestinationPublish, nullptr));
                    try
                    {
                        EXPECT_TRUE(access->republishRef({from.table->ns, from.ref}, {holder.table->ns, holder.ref}));
                    }
                    catch (const DB::Exception & e)
                    {
                        return refused(e);
                    }
                    /// The rename dropped its source.
                    forget(from);
                    return holder;
                };
            case CopyPath::Relink:
            {
                auto * exchange = dynamic_cast<DB::IContentAddressedExchange *>(storage.get());
                const auto sent = exchange->getRelinkOffer(from.table->dir + from.ref);
                if (!sent)
                {
                    ADD_FAILURE() << "the sender made no offer for " << from.ref;
                    return [] { return std::optional<Holder>{}; };
                }
                return [this, exchange, offer = *sent, holder = Holder{&to, "tmp-fetch_" + part}]() -> std::optional<Holder>
                {
                    /// A receiver that cannot adopt the entries falls back to fetching bytes: a refusal, not a copy.
                    std::unique_ptr<DB::ICaPreparedRelink> prepared;
                    if (exchange->prepareAdoptFromManifest(holder.table->dir + holder.ref, offer.manifest_bytes, prepared)
                        != DB::CaRelinkPrepare::Prepared)
                    {
                        relink_refused_by = RelinkRefusal::Prepare;
                        return std::nullopt;
                    }
                    const auto token = DB::decodeCasRelinkSourceToken(offer.confirm_token);
                    if (!token)
                    {
                        ADD_FAILURE() << "the sender's confirm token does not decode";
                        prepared->abort();
                        return std::nullopt;
                    }
                    if (exchange->confirmExactRef(token->root_namespace, token->ref_name, token->manifest_ref_text)
                        != DB::CasConfirmAnswer::Yes)
                    {
                        relink_refused_by = RelinkRefusal::Confirm;
                        prepared->abort();
                        return std::nullopt;
                    }
                    if (prepared->promote() != DB::CaRelinkPromote::Committed)
                    {
                        ADD_FAILURE() << "the promote of a confirmed relink did not commit";
                        return std::nullopt;
                    }
                    return holder;
                };
            }
        }
        return [] { return std::optional<Holder>{}; };
    }

    /// Where hop `index` lands. The pass-2 order of a pair depends on the round number and the hops of a relay run in
    /// consecutive rounds, so only the second hop of a relay takes a pass-2 point; the first takes the pass-1 point of
    /// the same walk order.
    EventPoint pointOfHop(size_t index) const
    {
        if (cell.hops == Hops::Relay && index == 0 && cell.at.point == RoundInterleavePoint::BetweenNamespaces)
            return {.point = cell.at.point, .pass = 1, .destination_first = cell.at.destination_first};
        return cell.at;
    }

    /// One hop in one round, with the copy and the drop where the cell puts them. Returns the new holder.
    std::optional<Holder> hop(size_t index, const Holder & from, const Table & to)
    {
        const EventPoint at = pointOfHop(index);
        const bool pass_two = passTwo(at);
        const auto [walk_first, walk_second] = walkPair(from.table, &to);

        /// The copy reads its source before the round; the source is dropped and its body reclaimed before the commit.
        std::function<std::optional<Holder>()> finish;
        if (cell.drop == SourceDrop::ReclaimedInsideTheCopy)
        {
            finish = beginCopy(from, to, /*drop_inside=*/false);
            drop(from);
            for (int i = 0; i < 16 && bodyStored(); ++i)
                round();
            if (bodyStored())
            {
                ADD_FAILURE() << "the dropped source's body was not reclaimed";
                return std::nullopt;
            }
            check("after the reclaim of the source's body");
            if (pass_two && !passTwoWalksThePairInOrder(nextRound()))
            {
                filler(*walk_first);
                round();
            }
        }

        /// Two new records in the source carry its walk into pass 2. The other table of the pair gets them only for
        /// a pass-2 event; otherwise it stays quiet, and pass 1 reads it to its end.
        filler(*walk_first);
        if (pass_two)
        {
            filler(*walk_second);
            EXPECT_TRUE(passTwoWalksThePairInOrder(nextRound())) << "pass 2 of round " << nextRound() << " does not walk the pair in order";
        }

        std::optional<Holder> made;
        bool copied = false;
        const bool refusal_expected = cell.drop == SourceDrop::InsideTheCopy || cell.drop == SourceDrop::ReclaimedInsideTheCopy;
        const auto event = [&]
        {
            check("before the copy");
            if (!finish)
            {
                const bool drop_inside = cell.drop == SourceDrop::InsideTheCopy;
                finish = beginCopy(from, to, drop_inside);
                if (drop_inside && cell.path != CopyPath::Republish)
                    drop(from);
            }
            made = finish();
            copied = true;
            if (made)
            {
                addHolder(*made);
                if (cell.drop == SourceDrop::AfterTheCopy && cell.path != CopyPath::Republish)
                    drop(from);
            }
            check("after the copy");
        };

        if (cell.drop == SourceDrop::BeforeTheRound)
        {
            event();
            round();
        }
        else
        {
            RoundInterleaveSelector selector;
            if (at.point == RoundInterleavePoint::BetweenNamespaces)
            {
                /// The destination sorts first exactly when `destination_first`, so the pass walks the pair in name order.
                const bool to_first = walk_second->ns.string() < walk_first->ns.string();
                selector = {.first = to_first ? walk_second->ns.string() : walk_first->ns.string(),
                            .second = to_first ? walk_first->ns.string() : walk_second->ns.string(),
                            .pass = at.pass};
            }
            const RoundWithCallback scenario = runRoundWithCallback(*gc, at.point, selector, event);
            store->renewWatermarkOnce();
            EXPECT_EQ(scenario.fired, 1u) << "the round never reached " << name(at);
        }
        check("after the round of the copy");
        if (!copied)
            return std::nullopt;

        if (refusal_expected)
        {
            EXPECT_FALSE(made.has_value()) << "a copy whose source was dropped before its promote committed";
            /// A reclaimed body does not fail the receiver's prepare; the sender's confirm refuses both drops.
            if (cell.path == CopyPath::Relink)
                EXPECT_EQ(relink_refused_by, RelinkRefusal::Confirm) << "another mechanism refused the relink";
        }
        else
            EXPECT_TRUE(made.has_value()) << "a copy of a live source was refused";

        if (made && cell.drop == SourceDrop::AfterTheRound)
        {
            drop(from);
            check("after the drop that follows the round");
        }
        return made;
    }

    /// The round number of the next round.
    uint64_t nextRound()
    {
        OperationForTest op(*store->poolBackendPtr());
        const auto state = (*op).read(store->layout().gcStateKey(), Retry::standard());
        return (state ? decodeGcState(state->bytes).round : 0) + 1;
    }

    void run()
    {
        if (passTwo(cell.at) && cell.drop != SourceDrop::ReclaimedInsideTheCopy)
        {
            /// Each round of the row setup commits, and so does the first hop of a relay.
            const size_t setup_rounds = cell.row == RowState::None ? 1 : (cell.row == RowState::Standing ? 3 : 2);
            const size_t rounds_before_the_pass_two_hop = setup_rounds + (cell.hops == Hops::Relay ? 1 : 0);
            size_t padding = 0;
            while (!passTwoWalksThePairInOrder(nextRound() + padding + rounds_before_the_pass_two_hop))
                ++padding;
            pad(padding);
        }
        ASSERT_NO_FATAL_FAILURE(prepareRow());

        Holder current{&source, "all_1_1_0"};
        const auto hops = hopTables();
        for (size_t index = 0; index < hops.size(); ++index)
        {
            const auto & [from, to] = hops[index];
            ASSERT_EQ(from, current.table);
            const auto made = hop(index, current, *to);
            if (!made)
                break;
            current = *made;
        }

        for (int i = 0; i < 3; ++i)
        {
            round();
            check(fmt::format("quiet round {}", i));
        }

        /// Liveness: once nothing names the blob, the body is reclaimed.
        while (!holders.empty())
            drop(holders.front());
        for (int i = 0; i < 16 && bodyStored(); ++i)
            round();
        EXPECT_FALSE(bodyStored()) << "the body was not reclaimed after the last part naming it was dropped";
        check("after the reclaim");
        EXPECT_GE(body_deletes.load(), 1u) << "positive control: the event sink saw the reclaiming delete";
        EXPECT_EQ(body_deletes_while_named.load(), 0u) << "a body DELETE was sent while a part named the blob";
    }

    /// Whether pass 2 of `round` walks the pass-2 hop's pair in name order, one right after the other. Pass 2 starts at
    /// index `round mod n` of the unfinished walks in name order. They are the pair, and for a relay whose first source
    /// is dropped after that hop's round also that source, whose drop record the pass-2 round reads.
    bool passTwoWalksThePairInOrder(uint64_t round) const
    {
        const bool three_walks = cell.hops == Hops::Relay && cell.drop == SourceDrop::AfterTheRound;
        const uint64_t walks = three_walks ? 3 : 2;
        /// The relay's first source sorts last when the destination is walked first, and first otherwise.
        const uint64_t first_of_the_pair = three_walks && !cell.at.destination_first ? 1 : 0;
        return round % walks != (first_of_the_pair + 1) % walks;
    }

    bool passTwo(const EventPoint & at) const
    {
        return cell.drop != SourceDrop::BeforeTheRound && at.point == RoundInterleavePoint::BetweenNamespaces && at.pass == 2;
    }

    /// Committed rounds before the row setup.
    void pad(size_t rounds)
    {
        for (size_t i = 0; i < rounds; ++i)
        {
            filler(source);
            round();
        }
    }

    const Cell cell;
    const String payload = payloadInAShardOtherThanTheFirst(cell.gc_shards);
    const BlobRef blob = idOf(payload);
    String body_key;

    std::shared_ptr<DB::ContentAddressedMetadataStorage> storage;
    PoolPtr store;
    std::unique_ptr<Gc> gc;

    Table source;
    Table middle;
    Table last;
    Table bystander;
    Table newborn;

    std::vector<Holder> holders;
    std::atomic<bool> named{false};
    std::atomic<uint64_t> body_deletes{0};
    std::atomic<uint64_t> body_deletes_while_named{0};

    enum class RelinkRefusal : uint8_t { None, Prepare, Confirm };
    RelinkRefusal relink_refused_by = RelinkRefusal::None;

private:
    static String payloadInAShardOtherThanTheFirst(uint64_t gc_shards)
    {
        if (gc_shards == 1)
            return "round-interleave-payload";
        for (size_t i = 0;; ++i)
            if (String candidate = "round-interleave-payload-" + std::to_string(i); blobShard(idOf(candidate), gc_shards) != 0)
                return candidate;
    }

    Table makeTable(char order, int index)
    {
        const String uuid = fmt::format("{}0000000-0000-4000-8000-{:012}", order, index);
        const String dir = uuid.substr(0, 3) + "/" + uuid + "/";
        return Table{uuid, dir, storage->liveNamespace(uuid)};
    }

    /// A payload whose blob comes before the cell's blob in the order the round spends its creates (shard, then
    /// hash), so it takes the round's one intent create.
    String decoyPayload() const
    {
        const auto position = [&](const BlobRef & ref) { return std::make_pair(blobShard(ref, cell.gc_shards), ref); };
        for (size_t i = 0;; ++i)
            if (const String candidate = "round-interleave-decoy-" + std::to_string(i); position(idOf(candidate)) < position(blob))
                return candidate;
    }

    size_t part_counter = 10;
};

class CASInterleaveMatrix : public ::testing::TestWithParam<Cell>
{
};

}

TEST_P(CASInterleaveMatrix, CopyAndDropKeepEveryNamedBody)
{
    InterleaveFixture fixture(GetParam());
    fixture.run();
}

INSTANTIATE_TEST_SUITE_P(CASInterleave, CASInterleaveMatrix, ::testing::ValuesIn(generatedCells()),
    [](const ::testing::TestParamInfo<Cell> & param_info) { return cellName(param_info.param); });

/// The cells the matrix runs and the ones it does not generate, with the reason for each excluded group. The counts
/// are pinned, so a change to a dimension or to an exclusion shows up here.
TEST(CASInterleaveMatrixCells, GeneratedAndExcludedCellCounts)
{
    const std::vector<Cell> product = crossProduct();
    std::map<String, size_t> excluded;
    for (const Cell & cell : product)
        if (const String reason = exclusionReason(cell); !reason.empty())
            ++excluded[reason];
    for (const auto & [reason, count] : excluded)
        std::cout << "[ CASInterleave ] excluded " << count << ": " << reason << '\n';

    EXPECT_EQ(product.size(), 2088u);
    EXPECT_EQ(generatedCells().size(), 1012u);
    const auto excludedFor = [&](const Cell & cell) { return excluded[exclusionReason(cell)]; };
    const EventPoint pass_one{.point = RoundInterleavePoint::BetweenNamespaces, .pass = 1, .destination_first = true};
    EXPECT_EQ(excludedFor({.path = CopyPath::Relink, .drop = SourceDrop::AfterTheCopy, .row = RowState::None, .hops = Hops::One,
                           .at = pass_one, .gc_shards = kShardedGc}), 696u);
    EXPECT_EQ(excludedFor({.path = CopyPath::HardLink, .drop = SourceDrop::BeforeTheRound, .row = RowState::None,
                           .hops = Hops::Newborn, .at = pass_one}), 16u);
    EXPECT_EQ(excludedFor({.path = CopyPath::Republish, .drop = SourceDrop::AfterTheRound, .row = RowState::None,
                           .hops = Hops::One, .at = pass_one}), 84u);
    EXPECT_EQ(excludedFor({.path = CopyPath::HardLink, .drop = SourceDrop::InsideTheCopy, .row = RowState::None,
                           .hops = Hops::Relay, .at = pass_one}), 224u);
    EXPECT_EQ(excludedFor({.path = CopyPath::Republish, .drop = SourceDrop::ReclaimedInsideTheCopy, .row = RowState::None,
                           .hops = Hops::One, .at = pass_one}), 56u);
    EXPECT_EQ(excluded.size(), 5u);
}
