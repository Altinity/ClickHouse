#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <condition_variable>
#include <optional>
#include <thread>
#include <vector>

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int CORRUPTED_DATA;
extern const int NOT_IMPLEMENTED;
extern const int UNKNOWN_FORMAT_VERSION;
extern const int FILE_DOESNT_EXIST;
extern const int LOGICAL_ERROR;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::idOf;
using DB::Cas::tests::publishRaw;
using DB::Cas::tests::shardOfForTest;
using DB::Cas::tests::u128Of;
using DB::Cas::tests::writeBlobRaw;
using DB::Cas::tests::writeTreeRaw;

namespace
{
/// Counts mutating backend calls so a test can assert an open path is write-free.
class WriteCountingBackend final : public DB::Cas::Backend
{
public:
    explicit WriteCountingBackend(std::shared_ptr<DB::Cas::Backend> inner_) : inner(std::move(inner_)) {}
    size_t writes = 0;

    std::optional<DB::Cas::GetResult> get(const String & k, DB::Cas::Range r = {}) override { return inner->get(k, r); }
    DB::Cas::HeadResult head(const String & k) override { return inner->head(k); }
    DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
    DB::Cas::PutResult putIfAbsent(const String & k, const String & b, const DB::Cas::ObjectMeta & meta = {}) override { ++writes; return inner->putIfAbsent(k, b, meta); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & meta = {}) override { ++writes; return inner->putIfAbsentStream(k, meta); }
    DB::Cas::PutResult putOverwrite(const String & k, const String & b, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & meta = {}) override { ++writes; return inner->putOverwrite(k, b, e, meta); }
    DB::Cas::CasResult casPut(const String & k, const String & b, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & meta = {}) override { ++writes; return inner->casPut(k, b, e, meta); }
    DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { ++writes; return inner->deleteExact(k, t); }
private:
    std::shared_ptr<DB::Cas::Backend> inner;
};
}

TEST(CasStore, ReadOnlyOpenSkipsProbe)
{
    auto shared = std::make_shared<DB::Cas::InMemoryBackend>();

    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    /// Writable open: creates _pool_meta and runs the probe (which writes+cleans up).
    DB::Cas::Store::open(std::make_shared<WriteCountingBackend>(shared), cfg);

    /// Read-only re-open over the SAME data must perform ZERO writes (no probe, meta already present).
    auto counter = std::make_shared<WriteCountingBackend>(shared);
    DB::Cas::PoolConfig ro = cfg;
    ro.read_only = true;
    auto store = DB::Cas::Store::open(counter, ro);
    EXPECT_EQ(counter->writes, 0u);
    ASSERT_NE(store, nullptr);
}

TEST(CasStore, MinActiveTracksInFlightBuilds)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    cfg.background_watermark = false;
    auto store = DB::Cas::Store::open(backend, cfg);

    ASSERT_EQ(store->minActive(), store->peekNextBuildSeq());   /// no builds: floor == next seq
    auto b1 = store->startBuild({});                            /// seq 1
    auto b2 = store->startBuild({});                            /// seq 2
    ASSERT_EQ(store->minActive(), 1u);
    b1->abandon();                                              /// finishes seq 1
    ASSERT_EQ(store->minActive(), 2u);                          /// floor advances
    b2->abandon();
    ASSERT_EQ(store->minActive(), store->peekNextBuildSeq());   /// empty again
}

TEST(CasStore, BuildSeqIsStrictlyMonotone)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    cfg.background_watermark = false;
    auto store = DB::Cas::Store::open(backend, cfg);
    auto a = store->startBuild({});
    auto sa = a->buildSeq();
    a->abandon();
    auto b = store->startBuild({});
    ASSERT_GT(b->buildSeq(), sa);                               /// never reused, never lower
}

TEST(CasPoolMeta, CreateThenReopen)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    PoolMeta created = PoolMeta::createOrValidate(*b, layout, /*root_shards*/ 8, /*blob_header_len*/ 256);
    EXPECT_NE(created.pool_id, UInt128{});
    PoolMeta reopened = PoolMeta::createOrValidate(*b, layout, /*root_shards*/ 4, /*blob_header_len*/ 512);
    EXPECT_EQ(reopened.pool_id, created.pool_id);     /// pool is authoritative — config ignored on reopen
    EXPECT_EQ(reopened.root_shards, 8u);
    EXPECT_EQ(reopened.blob_header_len, 256u);
}

TEST(CasPoolMeta, FailClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    /// A future object (framing magic CAPM + min_reader=2) must fail closed with UNKNOWN_FORMAT_VERSION.
    b->putIfAbsent(layout.poolMetaKey(), String("CAPM\x02\x00\x02\x00", 8));
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 256); });
    /// Garbage bytes => CORRUPTED_DATA.
    auto b2 = std::make_shared<InMemoryBackend>();
    b2->putIfAbsent(layout.poolMetaKey(), "garbage");
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { PoolMeta::createOrValidate(*b2, layout, 8, 256); });
}

TEST(CasPoolMeta, RoundTripAndReadability)
{
    PoolMeta pm;
    pm.pool_id = hexToU128("0123456789abcdeffedcba9876543210");
    pm.root_shards = 8;
    pm.blob_header_len = 256;

    const String encoded = encodePoolMeta(pm);
    /// Protobuf framing: starts with CAPM magic (not JSON).
    ASSERT_GE(encoded.size(), 8u);
    EXPECT_EQ(encoded.substr(0, 4), "CAPM");
    EXPECT_NE(encoded.front(), '{');

    PoolMeta decoded = decodePoolMeta(encoded);
    EXPECT_EQ(decoded.pool_id, pm.pool_id);
    EXPECT_EQ(decoded.root_shards, pm.root_shards);
    EXPECT_EQ(decoded.blob_header_len, pm.blob_header_len);
}

TEST(CasPoolMeta, RejectsBadConstantsAtCreation)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");

    /// not 8-aligned
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 100); });
    /// below the 96-byte floor
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 64); });
    /// above the 16 KiB ceiling
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 17 * 1024); });
    /// zero shards
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*b, layout, 0, 256); });

    /// A creation that fails config validation must not have written anything.
    EXPECT_FALSE(b->get(layout.poolMetaKey()).has_value());
}

TEST(CasPoolMeta, RejectsBadConstantsOnDecode)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    /// Encode a PoolMeta with blob_header_len=100 (not 8-aligned); decode must reject it as CORRUPTED_DATA.
    PoolMeta bad_pm;
    bad_pm.pool_id = hexToU128("00000000000000000000000000000001");
    bad_pm.root_shards = 8;
    bad_pm.blob_header_len = 100;   /// violates 8-alignment invariant
    b->putIfAbsent(layout.poolMetaKey(), encodePoolMeta(bad_pm));
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 256); });
}

TEST(CasPoolMeta, DecodeGarbageFails)
{
    /// Any non-CAPM framing byte sequence => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodePoolMeta(String("garbage")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodePoolMeta(String("")); });
}

TEST(CasPoolMeta, ConcurrentCreateRace)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");

    /// A racing creator already wrote a valid foreign pool_id. createOrValidate must NOT overwrite it:
    /// it re-reads (after losing the create-if-absent CAS, or seeing it present) and returns the
    /// foreign pool_id, validated like a reopen.
    const UInt128 foreign = hexToU128("0123456789abcdeffedcba9876543210");
    PoolMeta foreign_pm;
    foreign_pm.pool_id = foreign;
    foreign_pm.root_shards = 8;
    foreign_pm.blob_header_len = 256;
    b->putIfAbsent(layout.poolMetaKey(), encodePoolMeta(foreign_pm));

    PoolMeta result = PoolMeta::createOrValidate(*b, layout, /*root_shards*/ 4, /*blob_header_len*/ 512);
    EXPECT_EQ(result.pool_id, foreign);
    EXPECT_EQ(result.root_shards, 8u);     /// the foreign pool's constants win
    EXPECT_EQ(result.blob_header_len, 256u);
}

TEST(CasPoolMeta, CasConflictReReadsWinner)
{
    /// The subtlest branch: the initial GET sees ABSENT, so createOrValidate proceeds to the
    /// create-if-absent casPut — and loses, because a racing creator committed in between. The loser
    /// must then re-read and return the WINNER's pool identity, not LOGICAL_ERROR. A single-threaded
    /// `failNextCasPut` alone cannot exercise this: it returns Conflict without leaving the object
    /// readable, so the re-read would fire the LOGICAL_ERROR guard. We model the real interleaving
    /// with a backend whose casPut commits the winner's object (via the public putIfAbsent) and THEN
    /// reports Conflict — exactly what the loser observes.
    class RacingBackend : public InMemoryBackend
    {
    public:
        String winner_bytes;
        CasResult casPut(const String & key, const String & bytes,
            const std::optional<Token> & expected, const ObjectMeta & meta = {}) override
        {
            if (!winner_committed)
            {
                winner_committed = true;
                /// The winner lands first; our create-if-absent now necessarily conflicts.
                putIfAbsent(key, winner_bytes);
                return {CasOutcome::Conflict, {}};
            }
            return InMemoryBackend::casPut(key, bytes, expected, meta);
        }
    private:
        bool winner_committed = false;
    };

    const UInt128 winner = hexToU128("0123456789abcdeffedcba9876543210");
    PoolMeta winner_pm;
    winner_pm.pool_id = winner;
    winner_pm.root_shards = 8;
    winner_pm.blob_header_len = 256;

    auto b = std::make_shared<RacingBackend>();
    b->winner_bytes = encodePoolMeta(winner_pm);
    Layout layout("p");

    /// Our config (4 / 512) is what we WOULD have minted, but we lose the race and inherit the winner.
    PoolMeta result = PoolMeta::createOrValidate(*b, layout, /*root_shards*/ 4, /*blob_header_len*/ 512);
    EXPECT_EQ(result.pool_id, winner);
    EXPECT_EQ(result.root_shards, 8u);
    EXPECT_EQ(result.blob_header_len, 256u);
}

TEST(CasStore, OpenFailsClosedOnNonEnforcingBackend)
{
    auto b = std::make_shared<InMemoryBackend>();
    b->setEnforceTokens(false);
    expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED,
        [&] { Store::open(b, PoolConfig{.pool_prefix = "p"}); });   /// the probe error contract
}

TEST(CasStore, OpenCreatesPoolMetaAndReopens)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s1 = Store::open(b, PoolConfig{.pool_prefix = "p"});
    auto s2 = Store::open(b, PoolConfig{.pool_prefix = "p", .root_shards = 4});
    EXPECT_EQ(s2->poolMeta().root_shards, 8u);                      /// pool authoritative
    EXPECT_EQ(s1->poolMeta().pool_id, s2->poolMeta().pool_id);
}

TEST(CasStore, OpenWithExplicitConstantsCreatesThem)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .root_shards = 4, .blob_header_len = 512});
    EXPECT_EQ(s->poolMeta().root_shards, 4u);                       /// config applies at creation
    EXPECT_EQ(s->poolMeta().blob_header_len, 512u);
}

TEST(CasStore, VerbatimFilesLifecycle)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    RootNamespace ns{"srv1/tbl"};
    s->putNamespaceFile(ns, "format_version.txt", "1\n");
    s->putNamespaceFile(ns, "uuid.txt", "abc");
    EXPECT_EQ(s->getNamespaceFile(ns, "format_version.txt"), String("1\n"));
    EXPECT_FALSE(s->getNamespaceFile(ns, "absent").has_value());
    auto names = s->listNamespaceFiles(ns);
    EXPECT_EQ(names, (std::vector<String>{"format_version.txt", "uuid.txt"}));
    s->putNamespaceFile(ns, "uuid.txt", "def");                     /// overwrite allowed (head + putOverwrite)
    EXPECT_EQ(s->getNamespaceFile(ns, "uuid.txt"), String("def"));
}

TEST(CasStore, ListNamespaceFilesEmpty)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    RootNamespace ns{"srv1/tbl"};
    EXPECT_TRUE(s->listNamespaceFiles(ns).empty());
}

/// ---------- read side (spec §6): resolveRef / readTree / locate / listRefs ----------

TEST(CasStore, ResolveReadLocateRoundTrip)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const UInt128 dom = s->poolMeta().pool_id;
    Layout layout("p");

    /// blob "hello world" written with the pool's fixed header length.
    auto blob = writeBlobRaw(*b, layout, "hello world", s->poolMeta().blob_header_len, dom);

    /// tree: {"data.bin" → Blob hash("hello world"), 11} and {"small.txt" → Inline "tiny\n", 5}.
    std::vector<TreeEntry> entries;
    entries.push_back(TreeEntry{
        .name = "data.bin", .placement = Placement::Blob, .file_hash = u128Of("hello world"),
        .file_size = 11, .inline_bytes = ""});
    entries.push_back(TreeEntry{
        .name = "small.txt", .placement = Placement::Inline, .file_hash = {},
        .file_size = 5, .inline_bytes = "tiny\n"});
    auto tree = writeTreeRaw(*b, layout, entries, dom);

    RootShard root;
    root.shard_version = 1;
    root.refs["part_1"] = RefPayload{
        .tree_id = hexToU128(tree.string()), .tree_size = 0, .mutable_files = {{"txn_version.txt", "42"}}};
    /// Place the manifest in the shard the Store will look in for "part_1".
    publishRaw(*b, layout, RootNamespace{"srv1/tbl"},
        shardOfForTest("part_1", s->poolMeta().root_shards), root);

    auto r = s->resolveRef(RootNamespace{"srv1/tbl"}, "part_1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->tree_id, tree);
    EXPECT_EQ(r->mutable_files.at("txn_version.txt"), "42");

    auto read = s->readTree(r->tree_id);
    ASSERT_EQ(read.size(), 2u);

    auto loc = s->locate(read[0]);                  /// "data.bin" sorts before "small.txt"
    EXPECT_EQ(loc.offset, s->poolMeta().blob_header_len);
    EXPECT_EQ(loc.length, 11u);

    auto bytes = b->get(loc.key, Range{loc.offset, loc.length});
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes->bytes, "hello world");         /// ranged read, no header touch

    EXPECT_THROW(s->locate(read[1]), DB::Exception); /// Inline has no location
    (void)blob;
}

TEST(CasStore, ResolveDecodeCacheInvalidatesOnWrite)
{
    /// B113: resolveRef uses a token-validated shard-manifest decode cache. A write to the shard
    /// mints a new token, so a subsequent resolve must observe the change (cache must NOT serve a
    /// stale decoded manifest). Without token invalidation this would still see the dropped ref.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);

    RootShard root;
    root.shard_version = 1;
    root.refs["part_1"] = RefPayload{.tree_id = u128Of("tree-part_1"), .tree_size = 7, .mutable_files = {}};
    publishRaw(*b, layout, ns, shard, root);

    /// First resolve decodes + caches; second is a cache hit — both must see part_1.
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());

    /// Write through the Store (mutateShard => new shard token), removing part_1.
    s->dropRef(ns, "part_1");

    /// The cache must invalidate on the token change: resolve now reflects the drop.
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
    EXPECT_TRUE(s->listRefs(ns).empty());
}

TEST(CasStore, ResolveAbsentRefAndAbsentNamespace)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    RootNamespace ns{"srv1/tbl"};

    /// A freshly-opened pool has no shard manifests: an absent shard is an empty manifest, so resolve
    /// yields nullopt and listRefs is empty (NOT an error).
    EXPECT_FALSE(s->resolveRef(ns, "anything").has_value());
    EXPECT_TRUE(s->listRefs(ns).empty());
}

TEST(CasStore, ListRefsMergesAllShards)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const UInt128 dom = s->poolMeta().pool_id;
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const uint64_t shards = s->poolMeta().root_shards;

    /// Publish refs "a".."h", each in its own shard's manifest; refs colliding into one shard share it.
    std::map<uint64_t, RootShard> by_shard;
    for (char c = 'a'; c <= 'h'; ++c)
    {
        const String ref(1, c);
        const UInt128 tree_id = u128Of("tree-" + ref);
        by_shard[shardOfForTest(ref, shards)].refs[ref] =
            RefPayload{.tree_id = tree_id, .tree_size = 0, .mutable_files = {}};
    }
    for (auto & [shard, root] : by_shard)
    {
        root.shard_version = 1;
        publishRaw(*b, layout, ns, shard, root);
    }

    auto refs = s->listRefs(ns);
    ASSERT_EQ(refs.size(), 8u);
    for (char c = 'a'; c <= 'h'; ++c)
    {
        const String ref(1, c);
        ASSERT_TRUE(refs.count(ref));
        EXPECT_EQ(refs.at(ref).tree_id, TreeId(u128ToHex(u128Of("tree-" + ref))));
    }
    (void)dom;
}

TEST(CasStore, ReadTreeFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const UInt128 dom = s->poolMeta().pool_id;
    Layout layout("p");

    /// (1) A tree object stored at the WRONG key (key != hex(logical_hash)) ⇒ CORRUPTED_DATA.
    {
        std::vector<TreeEntry> entries;
        entries.push_back(TreeEntry{
            .name = "f", .placement = Placement::Inline, .file_hash = {},
            .file_size = 3, .inline_bytes = "abc"});
        const TreeId real_id = merkleTreeId(entries);
        const String encoded = encodeTree(entries);

        EnvelopeHeader header;
        header.kind = ObjectKind::Tree;
        header.hash_algo = 1;
        header.logical_size = encoded.size();
        header.logical_hash = hexToU128(real_id.string());   /// honest hash of THIS tree
        header.domain_id = dom;
        header.incarnation_tag = UInt128(0x1);
        header.build_id = UInt128(0x1);
        const String head = encodeEnvelopeHeader(header);

        /// ... but stored under a DIFFERENT id's key.
        const TreeId wrong_id{"00000000000000000000000000000001"};
        b->putIfAbsent(layout.treeKey(wrong_id), head + encoded);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { s->readTree(wrong_id); });
    }

    /// (2) A ref naming a tree id with NO object present ⇒ resolveRef SUCCEEDS (refs are manifest
    /// state) but readTree throws FILE_DOESNT_EXIST carrying the tree id (INV-NO-DANGLE surfaced).
    {
        RootNamespace ns{"srv1/dangle"};
        const UInt128 missing_tree = u128Of("missing");
        RootShard root;
        root.shard_version = 1;
        root.refs["part_x"] = RefPayload{.tree_id = missing_tree, .tree_size = 0, .mutable_files = {}};
        publishRaw(*b, layout, ns, shardOfForTest("part_x", s->poolMeta().root_shards), root);

        auto r = s->resolveRef(ns, "part_x");
        ASSERT_TRUE(r.has_value());
        expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { s->readTree(r->tree_id); });
    }

    /// (3) A Blob-kind envelope stored at a tree key ⇒ CORRUPTED_DATA (expected_kind=Tree mismatch).
    {
        /// Build a blob whose content id we then (ab)use as a tree key.
        const String payload = "not a tree";
        const BlobId blob_id = idOf(payload);
        const TreeId tree_key{blob_id.string()};   /// reuse the hex as a tree id

        EnvelopeHeader header;
        header.kind = ObjectKind::Blob;            /// WRONG kind for a tree key
        header.hash_algo = 1;
        header.logical_size = payload.size();
        header.logical_hash = u128Of(payload);
        header.domain_id = dom;
        header.incarnation_tag = UInt128(0x1);
        header.build_id = UInt128(0x1);
        const String head = encodeEnvelopeHeader(header);

        b->putIfAbsent(layout.treeKey(tree_key), head + payload);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { s->readTree(tree_key); });
    }
}

/// ---------- ref lifecycle: dropRef / updateRefPayload / dropNamespace ----------

TEST(CasStore, DropRefAppendsJournalAtomically)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const uint64_t shards = s->poolMeta().root_shards;
    const uint64_t shard = shardOfForTest("part_1", shards);
    const UInt128 tree_id = u128Of("tree-part_1");

    RootShard root;
    root.shard_version = 1;
    root.refs["part_1"] = RefPayload{.tree_id = tree_id, .tree_size = 7, .mutable_files = {}};
    publishRaw(*b, layout, ns, shard, root);

    s->dropRef(ns, "part_1");

    auto after = decodeRootShard(b->get(layout.rootShardKey(ns, shard))->bytes);
    EXPECT_TRUE(after.refs.empty());
    EXPECT_EQ(after.shard_version, 2u);                 /// +1 from the published value
    ASSERT_FALSE(after.journal.empty());
    const JournalRecord & last = after.journal.back();
    EXPECT_EQ(last.op, JournalRecord::Op::Remove);
    EXPECT_EQ(last.ref_name, "part_1");
    EXPECT_EQ(last.tree_id, tree_id);
    EXPECT_EQ(last.at_version, after.shard_version);    /// at_version == the committed shard_version

    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());

    /// Dropping a missing ref is fail-closed, never a silent no-op.
    expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { s->dropRef(ns, "no_such_ref"); });
}

TEST(CasStore, UpdateRefPayloadMutatesWithoutJournal)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    const UInt128 tree_id = u128Of("tree-part_1");

    RootShard root;
    root.shard_version = 3;
    root.refs["part_1"] = RefPayload{
        .tree_id = tree_id, .tree_size = 11, .mutable_files = {{"txn_version.txt", "1"}}};
    publishRaw(*b, layout, ns, shard, root);

    const size_t journal_before = decodeRootShard(b->get(layout.rootShardKey(ns, shard))->bytes).journal.size();

    s->updateRefPayload(ns, "part_1", [](RefPayload & p) { p.mutable_files["txn_version.txt"] = "7"; });

    auto after = decodeRootShard(b->get(layout.rootShardKey(ns, shard))->bytes);
    EXPECT_EQ(after.refs.at("part_1").mutable_files.at("txn_version.txt"), "7");
    EXPECT_EQ(after.refs.at("part_1").tree_id, tree_id);
    EXPECT_EQ(after.refs.at("part_1").tree_size, 11u);
    EXPECT_EQ(after.shard_version, 4u);                 /// +1 from the published value
    EXPECT_EQ(after.journal.size(), journal_before);    /// no reachability change ⇒ no journal record

    /// A mutator that changes tree_id is rejected, and the manifest is left UNTOUCHED.
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&]
    {
        s->updateRefPayload(ns, "part_1", [](RefPayload & p) { p.tree_id = u128Of("other"); });
    });
    auto unchanged = decodeRootShard(b->get(layout.rootShardKey(ns, shard))->bytes);
    EXPECT_EQ(unchanged.shard_version, 4u);             /// throw aborted before casPut
    EXPECT_EQ(unchanged.refs.at("part_1").tree_id, tree_id);
    EXPECT_EQ(unchanged.refs.at("part_1").mutable_files.at("txn_version.txt"), "7");

    /// Likewise, a mutator that changes tree_size is rejected.
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&]
    {
        s->updateRefPayload(ns, "part_1", [](RefPayload & p) { p.tree_size = 99; });
    });
}

TEST(CasStore, DropRefSurvivesCasConflict)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    const UInt128 tree_id = u128Of("tree-part_1");

    RootShard root;
    root.shard_version = 1;
    root.refs["part_1"] = RefPayload{.tree_id = tree_id, .tree_size = 7, .mutable_files = {}};
    publishRaw(*b, layout, ns, shard, root);

    /// Inject one artificial Conflict: the loop must re-read the (unchanged) manifest and re-apply.
    b->failNextCasPut(layout.rootShardKey(ns, shard));
    s->dropRef(ns, "part_1");

    auto after = decodeRootShard(b->get(layout.rootShardKey(ns, shard))->bytes);
    EXPECT_TRUE(after.refs.empty());
    EXPECT_GT(after.shard_version, 1u);                 /// advanced (exact delta depends on the fake)
    ASSERT_FALSE(after.journal.empty());
    /// Exactly one Remove for part_1 — the mutate must not double-append across the retry.
    size_t removes = 0;
    for (const JournalRecord & rec : after.journal)
        if (rec.op == JournalRecord::Op::Remove && rec.ref_name == "part_1")
            ++removes;
    EXPECT_EQ(removes, 1u);
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasStore, DropNamespaceTombstonesAndRemovesFiles)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const uint64_t shards = s->poolMeta().root_shards;

    /// Three refs, one per manifest (their shards may collide; each touched shard holds its refs).
    const std::vector<String> ref_names{"alpha", "bravo", "charlie"};
    std::map<uint64_t, RootShard> by_shard;
    for (const String & name : ref_names)
    {
        const uint64_t shard = shardOfForTest(name, shards);
        by_shard[shard].shard_version = 1;
        by_shard[shard].refs[name] = RefPayload{
            .tree_id = u128Of("tree-" + name), .tree_size = 3, .mutable_files = {}};
    }
    for (const auto & [shard, root] : by_shard)
        publishRaw(*b, layout, ns, shard, root);

    /// Two verbatim files.
    s->putNamespaceFile(ns, "format_version.txt", "1\n");
    s->putNamespaceFile(ns, "uuid.txt", "abc");

    s->dropNamespace(ns);

    /// Every TOUCHED shard manifest still EXISTS, with empty refs and a Remove journal record.
    for (const auto & [shard, root] : by_shard)
    {
        auto obj = b->get(layout.rootShardKey(ns, shard));
        ASSERT_TRUE(obj.has_value());
        auto after = decodeRootShard(obj->bytes);
        EXPECT_TRUE(after.refs.empty());
        bool has_remove = false;
        for (const JournalRecord & rec : after.journal)
            if (rec.op == JournalRecord::Op::Remove)
                has_remove = true;
        EXPECT_TRUE(has_remove);
    }

    /// UNTOUCHED shards (no manifest) remain absent — no tombstone manifest minted.
    for (uint64_t shard = 0; shard < shards; ++shard)
        if (!by_shard.count(shard))
            EXPECT_FALSE(b->get(layout.rootShardKey(ns, shard)).has_value());

    /// Verbatim files gone; listRefs empty.
    EXPECT_FALSE(s->getNamespaceFile(ns, "format_version.txt").has_value());
    EXPECT_FALSE(s->getNamespaceFile(ns, "uuid.txt").has_value());
    EXPECT_TRUE(s->listRefs(ns).empty());
}

TEST(CasStore, ListNamespacesFromRegistry)
{
    /// listNamespaces = one registry GET filtered by prefix (the W-REGISTER universe, never LIST).
    /// The wiring uses it for directory-style enumeration of opaque namespace strings (M-W).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});

    EXPECT_TRUE(s->listNamespaces("").empty());   /// fresh pool: no registry yet

    DB::Cas::tests::registerNamespaceRaw(*b, s->layout(), RootNamespace{"srv1/tbl"});
    DB::Cas::tests::registerNamespaceRaw(*b, s->layout(), RootNamespace{"shadow/bk1/tbl"});
    DB::Cas::tests::registerNamespaceRaw(*b, s->layout(), RootNamespace{"shadow/bk2/tbl"});

    const auto all = s->listNamespaces("");
    EXPECT_EQ(all.size(), 3u);
    const auto shadows = s->listNamespaces("shadow/");
    ASSERT_EQ(shadows.size(), 2u);                /// sorted (registry holds a std::set)
    EXPECT_EQ(shadows[0], "shadow/bk1/tbl");
    EXPECT_EQ(shadows[1], "shadow/bk2/tbl");
    EXPECT_TRUE(s->listNamespaces("nope/").empty());
}

namespace
{
/// Publish one ref through the real Build: tree {"f" -> blob of `payload`}.
/// Mirrors the `publishPart` helper in gtest_cas_gc_round.cpp (local copy to avoid cross-TU linkage).
void publishPart(const StorePtr & s, const String & ns, const String & ref, const String & payload)
{
    auto build = s->startBuild({});
    build->putBlob(DB::Cas::tests::idOf(payload), BlobSource::fromString(payload));
    TreeEntry entry;
    entry.name = "f";
    entry.placement = Placement::Blob;
    entry.file_hash = DB::Cas::tests::u128Of(payload);
    entry.file_size = payload.size();
    const TreeId tree = build->putTree({entry});
    build->publish(RootNamespace{ns}, ref, tree, {});
}

/// Blocks the FIRST head() call made after `arm()` on a latch so followers attach to the
/// single-flight entry while the leader is still in-flight. Deterministic: no sleeps.
/// Calls before `arm()` pass through immediately.
class GatedHeadBackend : public DB::Cas::InMemoryBackend
{
public:
    DB::Cas::HeadResult head(const String & key) override
    {
        {
            std::unique_lock lock(m);
            ++head_calls;
            if (armed && !leader_in_head)
            {
                leader_in_head = true;
                cv.notify_all();
                gate.wait(lock, [this] { return released; });
            }
        }
        return DB::Cas::InMemoryBackend::head(key);
    }

    /// Enable the gate: the NEXT head() call will be the "leader" and will block until release().
    void arm()
    {
        std::lock_guard lock(m);
        armed = true;
        leader_in_head = false;
        released = false;
    }

    void waitLeaderInHead()
    {
        std::unique_lock lock(m);
        cv.wait(lock, [this] { return leader_in_head; });
    }

    void release()
    {
        {
            std::lock_guard lock(m);
            released = true;
        }
        gate.notify_all();
    }

    uint64_t headCalls() const
    {
        std::lock_guard lock(m);
        return head_calls;
    }

private:
    mutable std::mutex m;
    std::condition_variable cv;
    std::condition_variable gate;
    uint64_t head_calls = 0;
    bool armed = false;
    bool leader_in_head = false;
    bool released = false;
};

/// Gates the FIRST get() made after `arm()`. It SNAPSHOTS the value as-of get-entry (modelling an
/// object-store read that began before a concurrent write committed), then blocks on a latch so the
/// test can land a write + cache-invalidation, then returns the SNAPSHOT (stale bytes) — not the
/// post-write value. Calls before arm(), and every call once the leader is gated, pass through
/// immediately. Deterministic: no sleeps. Used to reproduce the read-your-writes decode-cache
/// poisoning race (B157): an in-flight reader populating the TTL fast-path with a decode that a
/// committed write has already superseded + invalidated.
class GatedGetBackend : public DB::Cas::InMemoryBackend
{
public:
    std::optional<DB::Cas::GetResult> get(const String & key, DB::Cas::Range range = {}) override
    {
        auto snapshot = DB::Cas::InMemoryBackend::get(key, range);
        {
            std::unique_lock lock(m);
            if (armed && !leader_in_get)
            {
                leader_in_get = true;
                cv.notify_all();
                gate.wait(lock, [this] { return released; });
            }
        }
        return snapshot;
    }

    /// Enable the gate: the NEXT get() call will be the "leader" and will block until release().
    void arm()
    {
        std::lock_guard lock(m);
        armed = true;
        leader_in_get = false;
        released = false;
    }

    void waitLeaderInGet()
    {
        std::unique_lock lock(m);
        cv.wait(lock, [this] { return leader_in_get; });
    }

    void release()
    {
        {
            std::lock_guard lock(m);
            released = true;
        }
        gate.notify_all();
    }

private:
    std::mutex m;
    std::condition_variable cv;
    std::condition_variable gate;
    bool armed = false;
    bool leader_in_get = false;
    bool released = false;
};
}

TEST(CasStoreSingleFlight, ConcurrentResolvesCoalesceToOneHead)
{
    using namespace DB::Cas;

    /// Use GatedHeadBackend throughout. `open` + `publishPart` will call head() some times;
    /// snapshot the counter afterwards and assert that exactly ONE more head() happens during
    /// the concurrent burst (the single-flight leader's shard HEAD).
    auto b = std::make_shared<GatedHeadBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    /// Arm the gate: the NEXT head() call (the leader's shard HEAD) will block until release().
    b->arm();
    const uint64_t calls_before = b->headCalls();

    const RootNamespace ns{"srv1/tbl"};
    constexpr int followers = 8;
    std::vector<std::thread> threads;
    std::vector<std::optional<Resolved>> results(followers + 1);

    threads.emplace_back([&] { results[0] = s->resolveRef(ns, "part_1"); });
    b->waitLeaderInHead();
    for (int i = 0; i < followers; ++i)
        threads.emplace_back([&, i] { results[i + 1] = s->resolveRef(ns, "part_1"); });

    b->release();
    for (auto & t : threads)
        t.join();

    /// Single-flight: exactly ONE head() must have fired during the concurrent burst.
    EXPECT_EQ(b->headCalls() - calls_before, 1u);
    for (const auto & r : results)
    {
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->tree_size, results[0]->tree_size);
    }
}

/// ---------- Pillar B TTL decode cache (Task 3): opt-in bounded-TTL warm-hit path ----------

TEST(CasStoreDecodeTtl, WarmHitWithinTtlSkipsHead)
{
    using namespace DB::Cas;
    /// TTL of 60 s — easily satisfied in any test run.
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .shard_decode_cache_ttl_ms = std::chrono::milliseconds{60000}});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    const RootNamespace ns{"srv1/tbl"};
    /// Prime the cache: one force-fresh resolve (allow_stale=false) so the entry exists and
    /// validated_at is set.
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());
    b->resetCounts();

    /// Warm hit within TTL: MUST skip both HEAD and GET.
    ASSERT_TRUE(s->resolveRef(ns, "part_1", /*allow_stale=*/true).has_value());
    EXPECT_EQ(b->headTotal(), 0u);   /// no HEAD
    EXPECT_EQ(b->getTotal(), 0u);    /// no GET
}

TEST(CasStoreDecodeTtl, ForceFreshAlwaysHeads)
{
    using namespace DB::Cas;
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .shard_decode_cache_ttl_ms = std::chrono::milliseconds{60000}});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    const RootNamespace ns{"srv1/tbl"};
    /// Prime the cache.
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());
    b->resetCounts();

    /// Force-fresh resolve (allow_stale defaults false): MUST HEAD regardless of cached TTL.
    /// Single-flight guarantees exactly one HEAD per miss; resolving one ref touches one shard.
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());
    EXPECT_EQ(b->headTotal(), 1u);   /// exactly one HEAD
}

/// Read-your-writes coherence (B157): an in-flight reader whose GET snapshots a shard manifest that
/// PREDATES a concurrent publish must NOT poison the TTL decode cache with that stale decode after
/// the publish's invalidation erase has already run — otherwise a later allow_stale read serves the
/// stale decode within the TTL window and the just-published ref looks absent (→ "no ref" → the
/// adopted part is wrongly marked broken in the real server).
TEST(CasStore, MountpointObjectRoundTrip)
{
    auto b = std::make_shared<DB::Cas::InMemoryBackend>();
    auto store = DB::Cas::Store::open(b, DB::Cas::PoolConfig{.pool_prefix = "p"});
    const String key = "srv1/clickhouse_access_check_abc";
    EXPECT_FALSE(store->getMountpointObject(key).has_value());
    store->putMountpointObject(key, "probe-bytes");
    auto got = store->getMountpointObject(key);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "probe-bytes");
    store->removeMountpointObject(key);
    EXPECT_FALSE(store->getMountpointObject(key).has_value());
}

TEST(CasStoreDecodeTtl, ConcurrentWriteDuringGetDoesNotPoisonStaleEntry)
{
    using namespace DB::Cas;
    auto b = std::make_shared<GatedGetBackend>();
    /// root_shards = 1: every ref maps to shard 0, so publishing part_2 invalidates the SAME shard
    /// the gated reader is decoding for part_1. TTL of 60 s — comfortably covers the test.
    auto s = Store::open(b, PoolConfig{
        .pool_prefix = "p", .root_shards = 1, .shard_decode_cache_ttl_ms = std::chrono::milliseconds{60000}});

    const std::string ns_str = "srv1/tbl";
    const RootNamespace ns{ns_str};
    /// Publish part_1 (the shard now holds one ref; publish's casPut also erased the decode cache).
    publishPart(s, ns_str, "part_1", "payload-1");

    /// Arm the gate: the next get() (the reader's leader GET inside loadShardDecoded, after a cache
    /// miss) snapshots the gen-with-part_1 bytes, then blocks.
    b->arm();
    std::optional<Resolved> reader_result;
    std::thread reader([&] { reader_result = s->resolveRef(ns, "part_1", /*allow_stale=*/true); });
    b->waitLeaderInGet();

    /// While the reader's GET is parked on the stale snapshot, publish part_2 to the SAME shard.
    /// casPut commits the gen-with-{part_1,part_2} manifest and invalidates the decode cache.
    publishPart(s, ns_str, "part_2", "payload-2");

    /// Release the reader: its GET returns the stale gen-with-part_1 snapshot (no part_2). Without
    /// the coherence guard it caches that decode AFTER the invalidation erase, poisoning the cache.
    b->release();
    reader.join();
    ASSERT_TRUE(reader_result.has_value());   /// part_1 was always present; the reader still resolves it

    /// THE ASSERTION: a subsequent allow_stale resolve of the just-published part_2 MUST see it.
    /// With the poisoning bug it hits the stale cached decode (no part_2) and returns nullopt.
    EXPECT_TRUE(s->resolveRef(ns, "part_2", /*allow_stale=*/true).has_value())
        << "stale decode poisoned the TTL fast-path: just-published part_2 invisible (B157)";
}

TEST(CasStore, ListMirroredChildren)
{
    using namespace DB::Cas;
    auto b = std::make_shared<InMemoryBackend>();
    auto store = Store::open(b, PoolConfig{.pool_prefix = "p"});
    /// Seed two shadow archives by writing a verbatim file into each (creates the prefix in S3).
    store->putNamespaceFile(RootNamespace{"shadow/bk1/store/3f2/3f2a-uuid@cas@"}, "x", "1");
    store->putNamespaceFile(RootNamespace{"shadow/bk2/store/3f2/3f2a-uuid@cas@"}, "x", "1");
    auto children = store->listMirroredChildren("shadow/");
    std::sort(children.begin(), children.end());
    ASSERT_EQ(children.size(), 2u);
    EXPECT_EQ(children[0], "bk1");
    EXPECT_EQ(children[1], "bk2");
}
