#include <gtest/gtest.h>

#include <base/defines.h>
#include <base/scope_guard.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestBudget.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobIntentFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasMigratePool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasBoundedRun.h>
#include <Common/ThreadPool.h>
#include "cas_test_helpers.h"

#include <latch>

/// `clickhouse-disks cas-migrate-pool` over pools written with raw requests at floor 1 and floor 3: what each step
/// reads, writes and refuses, the order of the writes, and reruns after a stop at every write point.

namespace DB::ErrorCodes
{
extern const int ABORTED;
extern const int BAD_ARGUMENTS;
extern const int CANNOT_SCHEDULE_TASK;
extern const int CORRUPTED_DATA;
extern const int INVALID_STATE;
extern const int NOT_IMPLEMENTED;
extern const int READONLY;
extern const int UNKNOWN_FORMAT_VERSION;
}

namespace ProfileEvents
{
extern const Event CASGCIntentConfirmedExisting;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

/// The in-memory store with a switchable dialect, `HEAD`s that answer a weak token, injected write and delete
/// refusals, and hooks after a write lands and before a `HEAD`.
class MigrationBackend : public CountingBackend
{
public:
    Dialect dialect() const override { return current_dialect.load(); }
    void setDialect(Dialect d) { current_dialect.store(d); }

    void weakHeadsFor(const String & key_part)
    {
        std::lock_guard lock(mutex);
        weak_head_parts.push_back(key_part);
    }

    /// The next `times` writes to `key` are refused before they reach the store.
    void refuseWrites(const String & key, size_t times)
    {
        std::lock_guard lock(mutex);
        refused_writes[key] = times;
    }

    /// Every later write to a key under `prefix` is refused before it reaches the store.
    void refuseWritesUnder(const String & prefix)
    {
        std::lock_guard lock(mutex);
        refused_write_prefixes.push_back(prefix);
    }

    /// Called on every read and `HEAD`, from whichever thread sends it; set before the run.
    std::function<void()> on_read_or_head;

    void refuseDeletes(const String & key)
    {
        std::lock_guard lock(mutex);
        refused_deletes.insert(key);
    }

    void allowDeletes(const String & key)
    {
        std::lock_guard lock(mutex);
        refused_deletes.erase(key);
    }

    /// Runs `hook` once, after the first write to `key` lands.
    void afterWrite(const String & key, std::function<void()> hook)
    {
        std::lock_guard lock(mutex);
        after_write_key = key;
        after_write = std::move(hook);
    }

    /// Runs `hook` once, before the first `HEAD` of `key` is served.
    void beforeHead(const String & key, std::function<void()> hook)
    {
        std::lock_guard lock(mutex);
        before_head_key = key;
        before_head = std::move(hook);
    }

    /// Runs `hook` once, after the first `HEAD` of `key` was answered.
    void afterHead(const String & key, std::function<void()> hook)
    {
        std::lock_guard lock(mutex);
        after_head_key = key;
        after_head = std::move(hook);
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        std::function<void()> fire;
        std::function<void()> fire_after;
        bool weak = false;
        {
            std::lock_guard lock(mutex);
            if (before_head && key == before_head_key)
                fire = std::exchange(before_head, {});
            if (after_head && key == after_head_key)
                fire_after = std::exchange(after_head, {});
            for (const String & part : weak_head_parts)
                weak = weak || key.find(part) != String::npos;
        }
        if (fire)
            fire();
        if (on_read_or_head)
            on_read_or_head();
        auto meta = CountingBackend::head(key, access);
        if (fire_after)
            fire_after();
        if (meta && weak)
            meta->value = "W/\"weak\"";
        return meta;
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                            const std::optional<String> & expected_value, TransportAccess & access) override
    {
        {
            std::lock_guard lock(mutex);
            if (auto it = refused_writes.find(key); it != refused_writes.end() && it->second > 0)
            {
                --it->second;
                throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "injected refusal of the write to {}", key);
            }
            for (const String & prefix : refused_write_prefixes)
                if (key.starts_with(prefix))
                    throw DB::Exception(DB::ErrorCodes::READONLY, "injected 403 on the write to {}", key);
        }
        auto result = CountingBackend::write(key, bytes, expected_value, access);
        std::function<void()> fire;
        {
            std::lock_guard lock(mutex);
            if (result && after_write && key == after_write_key)
                fire = std::exchange(after_write, {});
        }
        if (fire)
            fire();
        return result;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (on_read_or_head)
            on_read_or_head();
        return CountingBackend::read(key, access);
    }

    /// `key` is left out of every listing; reads and `HEAD`s still find it.
    void hideFromListing(const String & key)
    {
        std::lock_guard lock(mutex);
        hidden_from_listing.insert(key);
    }

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        RawListPage page = CountingBackend::list(prefix, cursor, limit, access);
        std::lock_guard lock(mutex);
        std::erase_if(page.keys, [&](const RawListedKey & listed) { return hidden_from_listing.contains(listed.key); });
        return page;
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        {
            std::lock_guard lock(mutex);
            if (refused_deletes.contains(key))
                throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "injected refusal of the delete of {}", key);
        }
        return CountingBackend::remove(key, expected_value, access);
    }

private:
    std::atomic<Dialect> current_dialect{Dialect::Emulated};
    std::mutex mutex;
    std::vector<String> weak_head_parts;
    std::map<String, size_t> refused_writes;
    std::vector<String> refused_write_prefixes;
    std::set<String> refused_deletes;
    std::set<String> hidden_from_listing;
    String after_write_key;
    std::function<void()> after_write;
    String before_head_key;
    std::function<void()> before_head;
    String after_head_key;
    std::function<void()> after_head;
};

/// A live holder's slot body declaring a 30 s lease.
MountLease liveLease(uint64_t seq = 1)
{
    MountLease lease;
    lease.server_uuid = DB::UInt128(0xabc);
    lease.writer_epoch = 1;
    lease.hostname = "old-host";
    lease.pid = 7;
    lease.started_at_ms = 1'000;
    lease.expires_at_ms = 31'000;
    lease.seq = seq;
    lease.write_attempt_id = DB::UInt128(0x1000 + seq);
    return lease;
}

MountLease farewellLease()
{
    MountLease lease = liveLease();
    lease.min_active_build_sequence = std::numeric_limits<uint64_t>::max();
    return lease;
}

const DB::UInt128 kOldLeader = hexToU128("0000000000000000000000000000beef");

/// What the refusals over a lost GC baseline say about serving the pool until it is recovered.
constexpr std::string_view kKeepOfflineAdvice = "keep the pool offline or serve it read-only: a writable older server "
    "with GC enabled starts folding at generation zero without the lost holds";

/// One pool at `floor` written with raw requests, and the tool's runs over it.
struct Fixture
{
    explicit Fixture(uint64_t floor, uint64_t gc_shards = 1, String prefix_ = "p")
        : prefix(std::move(prefix_)), layout(prefix)
    {
        seedOlderPoolForTest(*backend, floor, prefix, gc_shards);
    }

    std::shared_ptr<MigrationBackend> backend = std::make_shared<MigrationBackend>();
    FakeClock clock;
    /// Runs after each pause of the tool, once the clock moved.
    std::function<void(uint64_t)> on_sleep;
    String prefix;
    Layout layout;
    bool skip_access_check = false;
    std::optional<uint64_t> request_window_ms;

    void body(uint64_t b) { writeBlobBody(*backend, layout, numberedBlobHash(b)); }
    void cleanMarker(uint64_t b) { writeMetaClean(*backend, layout, numberedBlobHash(b), 1); }
    void condemnedMarker(uint64_t b)
    {
        cleanMarker(b);
        condemnMeta(*backend, layout, numberedBlobHash(b), 1);
    }
    void rawMarker(uint64_t b, const String & bytes)
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(layout.blobMetaKey(blobRefOf(b)), bytes, Retry::standard())));
    }
    void removeBody(uint64_t b)
    {
        OperationForTest op(*backend);
        ASSERT_EQ((*op).removeCurrent(layout.blobKey(blobRefOf(b)), Retry::standard()), Removal::Removed);
    }

    std::optional<Etag> current(uint64_t b)
    {
        OperationForTest op(*backend);
        auto meta = (*op).head(layout.blobKey(blobRefOf(b)), Retry::standard());
        return meta ? std::optional<Etag>(meta->etag) : std::nullopt;
    }
    CanonicalIdentity identity(uint64_t b)
    {
        auto etag = current(b);
        if (!etag || !etag->canonical())
            throw std::runtime_error("blob has no canonical token");
        return *etag->canonical();
    }
    RetiredEntry row(uint64_t b, bool pending)
    {
        auto etag = current(b);
        if (!etag)
            throw std::runtime_error("blob has no body");
        return RetiredEntry{.kind = ObjectKind::Blob, .ref = blobRefOf(b), .token = PersistedEtag::capture(*etag), .size = 1,
                            .condemn_round = 3, .delete_pending = pending};
    }

    /// Condemned rows of GC shard `shard` under a `gc/state` at `header`.
    void rows(const std::vector<RetiredEntry> & entries, uint32_t header = G_REF_CHAIN, uint64_t shard = 0)
    {
        injectRetire(*backend, layout, /*round*/ 5, shard, entries);
        editGcState([&](GcState & state) { state.header_generation = header; });
    }
    void editGcState(const std::function<void(GcState &)> & edit)
    {
        OperationForTest op(*backend);
        const auto got = (*op).read(layout.gcStateKey(), Retry::standard());
        GcState state = got ? decodeGcState(got->bytes) : GcState{};
        edit(state);
        const String bytes = encodeGcState(state);
        const WriteResult written = got ? (*op).replace(layout.gcStateKey(), bytes, got->etag, Retry::standard())
                                        : (*op).create(layout.gcStateKey(), bytes, Retry::standard());
        if (!std::holds_alternative<Committed>(written))
            throw std::runtime_error("cannot write gc/state");
    }
    std::optional<GcState> gcState()
    {
        OperationForTest op(*backend);
        const auto got = (*op).read(layout.gcStateKey(), Retry::standard());
        return got ? std::optional<GcState>(decodeGcState(got->bytes)) : std::nullopt;
    }
    void heartbeat(uint64_t seq)
    {
        OperationForTest op(*backend);
        const String bytes = encodeGcHeartbeat(GcHeartbeat{.owner = kOldLeader, .hb_seq = seq});
        const auto got = (*op).read(layout.gcHbKey(), Retry::standard());
        const WriteResult written = got ? (*op).replace(layout.gcHbKey(), bytes, got->etag, Retry::standard())
                                        : (*op).create(layout.gcHbKey(), bytes, Retry::standard());
        if (!std::holds_alternative<Committed>(written))
            throw std::runtime_error("cannot write gc/hb");
    }

    void slot(const String & srid, const MountLease & lease)
    {
        OperationForTest op(*backend);
        const String key = layout.mountKey(srid);
        const auto got = (*op).read(key, Retry::standard());
        const WriteResult written = got ? (*op).replace(key, encodeMountLease(lease), got->etag, Retry::standard())
                                        : (*op).create(key, encodeMountLease(lease), Retry::standard());
        if (!std::holds_alternative<Committed>(written))
            throw std::runtime_error("cannot write the slot");
    }
    std::optional<MountLease> slotBody(const String & srid)
    {
        OperationForTest op(*backend);
        const auto got = (*op).read(layout.mountKey(srid), Retry::standard());
        return got ? std::optional<MountLease>(decodeMountLease(got->bytes)) : std::nullopt;
    }

    PoolPtr open()
    {
        PoolConfig config{.pool_prefix = prefix, .server_root_id = "migration-tool"};
        config.skip_access_check = skip_access_check;
        if (request_window_ms)
            setCasRequestWindow(config.cas_request_budget, *request_window_ms);
        config.boot_ms_fn = clock.nowFn();
        config.retry_sleep_fn = [this](uint64_t ms)
        {
            clock.sleepFn()(ms);
            if (on_sleep)
                on_sleep(ms);
        };
        return Pool::openForMigration(backend, std::move(config));
    }

    /// Each progress line also enters the request journal as a mark, so tests can order lines against requests.
    MigratePoolReport run(const MigratePoolOptions & options = {}, std::vector<String> * lines = nullptr)
    {
        auto pool = open();
        return migratePool(*pool, options, [this, lines](std::string_view line)
        {
            backend->mark(line);
            if (lines)
                lines->emplace_back(line);
        });
    }

    bool hasIntent(uint64_t b, const CanonicalIdentity & id)
    {
        OperationForTest op(*backend);
        const auto got = (*op).read(layout.blobIntentKey(blobRefOf(b), id), Retry::standard());
        return got && isBlobIntentRecord(got->bytes);
    }
    std::set<String> keysUnder(const String & key_prefix)
    {
        OperationForTest op(*backend);
        std::set<String> out;
        (*op).forEachListedKey(key_prefix, [&](const ListedKey & listed)
        {
            out.insert(listed.key);
            return true;
        }, Retry::standard());
        return out;
    }
    std::set<String> intents() { return keysUnder(layout.blobIntentsPrefix()); }
    std::set<String> markers()
    {
        std::set<String> out;
        for (const String & key : keysUnder(layout.blobsPrefix()))
            if (key.ends_with(".meta"))
                out.insert(key);
        return out;
    }
    PoolMeta poolMeta()
    {
        OperationForTest op(*backend);
        const auto got = (*op).read(layout.poolMetaKey(), Retry::standard());
        if (!got)
            throw std::runtime_error("no _pool_meta");
        return decodePoolMeta(got->bytes);
    }
};

bool contains(const std::vector<String> & lines, std::string_view text)
{
    return std::any_of(lines.begin(), lines.end(), [&](const String & line) { return line.find(text) != String::npos; });
}

size_t writesTo(const std::vector<JournalEntry> & journal, const String & key)
{
    return std::count_if(journal.begin(), journal.end(),
        [&](const JournalEntry & e) { return e.verb == JournalVerb::Write && e.keys.front() == key; });
}

/// Indices of the writes whose key starts with `key_prefix`.
std::vector<size_t> writeIndices(const std::vector<JournalEntry> & journal, const String & key_prefix)
{
    std::vector<size_t> out;
    for (size_t i = 0; i < journal.size(); ++i)
        if (journal[i].verb == JournalVerb::Write && journal[i].keys.front().starts_with(key_prefix))
            out.push_back(i);
    return out;
}

/// Indices of the intent creates, without the capability probe's scratch object.
std::vector<size_t> intentWriteIndices(const std::vector<JournalEntry> & journal, const Layout & layout)
{
    std::vector<size_t> out;
    for (const size_t i : writeIndices(journal, layout.blobIntentsPrefix()))
        if (!layout.isBlobIntentProbeKey(journal[i].keys.front()))
            out.push_back(i);
    return out;
}

std::vector<size_t> markerDeleteIndices(const std::vector<JournalEntry> & journal)
{
    std::vector<size_t> out;
    for (size_t i = 0; i < journal.size(); ++i)
        if (journal[i].verb == JournalVerb::Remove && journal[i].keys.front().ends_with(".meta"))
            out.push_back(i);
    return out;
}

/// Every object of the pool with its bytes.
std::map<String, String> objectsOf(Fixture & f)
{
    std::map<String, String> out;
    OperationForTest op(*f.backend);
    for (const String & key : f.keysUnder(f.prefix + "/"))
        out[key] = (*op).read(key, Retry::standard())->bytes;
    return out;
}

/// The keys the run wrote or deleted, without the scratch objects of the capability and intent probes.
std::vector<String> mutationsOutsideTheProbes(const std::vector<JournalEntry> & journal, const Fixture & f)
{
    std::vector<String> out;
    for (const JournalEntry & e : journal)
    {
        if (e.verb != JournalVerb::Write && e.verb != JournalVerb::Remove && e.verb != JournalVerb::RemoveMany
            && e.verb != JournalVerb::Publish)
            continue;
        for (const String & key : e.keys)
            if (!key.starts_with(f.prefix + "/_probe/") && !f.layout.isBlobIntentProbeKey(key))
                out.push_back(key);
    }
    return out;
}

bool anyMutation(const std::vector<JournalEntry> & journal)
{
    return std::any_of(journal.begin(), journal.end(), [](const JournalEntry & e)
    {
        return e.verb == JournalVerb::Write || e.verb == JournalVerb::Remove || e.verb == JournalVerb::RemoveMany
            || e.verb == JournalVerb::Publish;
    });
}

/// The pool every crash test starts from: markers of each class, inherited rows of each kind, a silent slot and a
/// slot that said farewell.
void seedMixedPool(Fixture & f)
{
    for (uint64_t b = 1; b <= 6; ++b)
        f.body(b);
    f.condemnedMarker(1);
    f.cleanMarker(2);
    f.rawMarker(3, "not a marker");
    const RetiredEntry candidate = f.row(4, /*pending*/ false);
    const RetiredEntry pending = f.row(5, /*pending*/ true);
    const RetiredEntry replaced = f.row(6, /*pending*/ true);
    displaceBlobToken(*f.backend, f.layout, blobRefOf(6));
    f.rows({candidate, pending, replaced});
    f.heartbeat(4);
    f.slot("srv-silent", liveLease());
    f.slot("srv-gone", farewellLease());
}

/// Body 1 under a clean marker, body 2 with no marker, body 3 under a condemned marker.
void seedOneBodyOfEachMarking(Fixture & f)
{
    f.body(1);
    f.cleanMarker(1);
    f.body(2);
    f.body(3);
    f.condemnedMarker(3);
}

/// What a complete migration leaves, for comparing runs.
struct FinalState
{
    uint64_t floor = 0;
    std::optional<Dialect> dialect;
    uint32_t header = 0;
    uint64_t round = 0;
    uint64_t snap_generation = 0;
    std::set<String> intents;
    std::set<String> markers;
    bool silent_slot_fenced = false;

    bool operator==(const FinalState &) const = default;
};

FinalState finalStateOf(Fixture & f)
{
    const PoolMeta meta = f.poolMeta();
    const auto state = f.gcState();
    const auto slot = f.slotBody("srv-silent");
    return FinalState{
        .floor = meta.min_reader_generation,
        .dialect = meta.token_dialect,
        .header = state ? state->header_generation : 0,
        .round = state ? state->round : 0,
        .snap_generation = state ? state->snap_generation : 0,
        .intents = f.intents(),
        .markers = f.markers(),
        .silent_slot_fenced = slot && slot->gc_fenced};
}

}

TEST(CASMigratePool, RefusesASlotThatRenews)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    f.slot("srv-a", liveLease(1));
    bool renewed = false;
    f.on_sleep = [&](uint64_t)
    {
        if (!renewed)
        {
            renewed = true;
            f.slot("srv-a", liveLease(2));
        }
    };
    f.backend->clearJournal();

    const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run(); });
    EXPECT_NE(message.find("mount slot of server root 'srv-a' changed during the migration; stop that server and run the tool again"),
        String::npos) << message;
    EXPECT_TRUE(renewed);
    const auto journal = f.backend->journal();
    EXPECT_TRUE(intentWriteIndices(journal, f.layout).empty());
    EXPECT_EQ(writesTo(journal, f.layout.gcStateKey()), 0u);
    EXPECT_EQ(writesTo(journal, f.layout.poolMetaKey()), 0u);
    EXPECT_FALSE(f.slotBody("srv-a")->gc_fenced);
}

TEST(CASMigratePool, FencesASilentSlotAfterTheThreshold)
{
    Fixture f(3);
    f.slot("srv-a", liveLease());
    const MigratePoolReport report = f.run();
    ASSERT_TRUE(report.completed);

    const auto slot = f.slotBody("srv-a");
    ASSERT_TRUE(slot);
    EXPECT_TRUE(slot->gc_fenced);
    EXPECT_EQ(slot->seq, liveLease().seq + 1);
    const uint64_t threshold = mountObservationThresholdMs(30'000, declaredMountLeaseMs(liveLease()), 1'000);
    uint64_t slept = 0;
    for (const uint64_t ms : f.clock.sleeps)
        slept += ms;
    EXPECT_GE(slept, threshold + 120'000) << "fenced before the slot held for the threshold and the default grace";
}

TEST(CASMigratePool, RefusesAMovingGcLease)
{
    Fixture f(3);
    f.rows({});
    f.editGcState([](GcState & state) { state.lease = GcLease{.owner = kOldLeader, .seq = 1}; });
    bool moved = false;
    f.on_sleep = [&](uint64_t)
    {
        if (!moved)
        {
            moved = true;
            f.editGcState([](GcState & state) { state.lease.seq = 2; });
        }
    };

    const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run(); });
    EXPECT_NE(message.find("GC lease of owner " + u128ToHex(kOldLeader)
                           + " moved during the migration; stop that server and run the tool again"), String::npos) << message;
    EXPECT_EQ(f.poolMeta().min_reader_generation, 3u);
    EXPECT_EQ(f.gcState()->header_generation, G_REF_CHAIN);
}

TEST(CASMigratePool, RefusesAnotherDialectOnAMigratedPool)
{
    auto backend = std::make_shared<MigrationBackend>();
    const Layout layout("p");
    {
        PoolMeta pm;
        pm.pool_id = DB::UInt128(0x5eed);
        pm.blob_header_len = 256;
        pm.min_reader_generation = G_BLOB_INTENTS;
        pm.algos_used = {static_cast<uint8_t>(BlobHashAlgo::CityHash128)};
        pm.token_dialect = Dialect::ETag;
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(layout.poolMetaKey(), encodePoolMeta(pm), Retry::standard())));
    }
    writeBlobBody(*backend, layout, numberedBlobHash(1));
    writeMetaClean(*backend, layout, numberedBlobHash(1), 1);
    backend->clearJournal();

    auto pool = Pool::openForMigration(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "migration-tool"});
    const String message = messageOf(DB::ErrorCodes::BAD_ARGUMENTS, [&] { migratePool(*pool, {}, {}); });
    EXPECT_NE(message.find("CAS pool records token dialect 'etag', this disk uses 'emulated': identities are not comparable"),
        String::npos) << message;
    EXPECT_FALSE(anyMutation(backend->journal()));
}

TEST(CASMigratePool, RefusesAPoolPrefixOver668Bytes)
{
    const size_t limit = Layout::maxPoolPrefixBytesForBlobIntents();
    {
        Fixture f(3, 1, String(limit + 1, 'p'));
        f.body(1);
        f.condemnedMarker(1);
        f.backend->clearJournal();
        const String message = messageOf(DB::ErrorCodes::BAD_ARGUMENTS, [&] { f.run(); });
        EXPECT_NE(message.find(fmt::format("pool prefix of {} bytes leaves no room for blob intent keys; the limit is {}",
            limit + 1, limit)), String::npos) << message;
        EXPECT_FALSE(anyMutation(f.backend->journal()));
    }
    {
        /// Positive control: at the limit the same pool migrates.
        Fixture f(3, 1, String(limit, 'p'));
        f.body(1);
        f.condemnedMarker(1);
        EXPECT_TRUE(f.run().completed);
        EXPECT_TRUE(f.hasIntent(1, f.identity(1)));
    }
}

TEST(CASMigratePool, CondemnedMarkerGetsAnIntentForTheCurrentToken)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    f.body(2);
    f.cleanMarker(2);
    MigratePoolOptions trust;
    trust.trust_clean_markers = true;
    const MigratePoolReport report = f.run(trust);
    ASSERT_TRUE(report.completed);
    EXPECT_TRUE(f.hasIntent(1, f.identity(1)));
    EXPECT_EQ(f.intents(), (std::set<String>{f.layout.blobIntentKey(blobRefOf(1), f.identity(1))}));
    EXPECT_EQ(report.intents_confirmed, 1u);
}

TEST(CASMigratePool, UndecodableMarkerCountsAsCondemned)
{
    Fixture f(3);
    f.body(1);
    f.rawMarker(1, "not a marker");
    ASSERT_TRUE(f.run().completed);
    EXPECT_TRUE(f.hasIntent(1, f.identity(1)));
    EXPECT_TRUE(f.markers().empty());
}

TEST(CASMigratePool, EveryCondemnedRowGetsAnIntent)
{
    Fixture f(3);
    f.body(1);
    f.body(2);
    f.rows({f.row(1, /*pending*/ false), f.row(2, /*pending*/ true)});
    const MigratePoolReport report = f.run();
    ASSERT_TRUE(report.completed);
    EXPECT_EQ(report.condemned_rows, 2u);
    EXPECT_TRUE(f.hasIntent(1, f.identity(1))) << "candidate row";
    EXPECT_TRUE(f.hasIntent(2, f.identity(2))) << "pending row";
}

TEST(CASMigratePool, PendingRowWithAnAbsentOrReplacedBodyGetsAnIntentForItsOwnToken)
{
    Fixture f(3);
    f.body(1);
    f.body(2);
    const RetiredEntry absent = f.row(1, /*pending*/ true);
    const RetiredEntry replaced = f.row(2, /*pending*/ true);
    f.removeBody(1);
    displaceBlobToken(*f.backend, f.layout, blobRefOf(2));
    f.rows({absent, replaced});

    const MigratePoolReport report = f.run();
    ASSERT_TRUE(report.completed);
    EXPECT_TRUE(f.hasIntent(1, *absent.token.canonical())) << "absent body";
    EXPECT_TRUE(f.hasIntent(2, *replaced.token.canonical())) << "replaced body: the row's own token";
    EXPECT_TRUE(f.hasIntent(2, f.identity(2))) << "replaced body: the current token";
    EXPECT_EQ(f.intents().size(), 3u);
    EXPECT_EQ(report.intents_confirmed, 3u);
}

TEST(CASMigratePool, AbsentBodyAndUncanonicalTokenGetNoIntentForTheCurrentBody)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    f.removeBody(1);
    f.body(2);
    f.condemnedMarker(2);
    f.backend->weakHeadsFor(f.layout.blobKey(blobRefOf(2)));
    f.body(3);
    const RetiredEntry candidate = f.row(3, /*pending*/ false);
    f.removeBody(3);
    f.rows({candidate});
    /// Positive control: a body with a canonical token beside them gets its intent.
    f.body(4);
    f.condemnedMarker(4);

    const MigratePoolReport report = f.run();
    ASSERT_TRUE(report.completed);
    EXPECT_EQ(f.intents(), (std::set<String>{f.layout.blobIntentKey(blobRefOf(4), f.identity(4))}));
    EXPECT_EQ(report.markers_read, 3u);
}

TEST(CASMigratePool, UnconfirmedCreateFailsTheRun)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    const String intent_key = f.layout.blobIntentKey(blobRefOf(1), f.identity(1));
    {
        OperationForTest op(*f.backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(intent_key, "other bytes", Retry::standard())));
    }

    const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run(); });
    EXPECT_NE(message.find("could not be confirmed"), String::npos) << message;
    EXPECT_EQ(f.poolMeta().min_reader_generation, 3u);
    EXPECT_FALSE(f.gcState().has_value()) << "no gc/state may be written after an unconfirmed intent";
    EXPECT_EQ(f.markers().size(), 1u);

    /// The rerun after the occupant is gone creates the intent and completes.
    {
        OperationForTest op(*f.backend);
        ASSERT_EQ((*op).removeCurrent(intent_key, Retry::standard()), Removal::Removed);
    }
    ASSERT_TRUE(f.run().completed);
    EXPECT_TRUE(f.hasIntent(1, f.identity(1)));
}

/// Creates run concurrently; one of many that cannot be confirmed stops the run before anything of M6 is written.
TEST(CASMigratePool, OneUnconfirmedCreateAmongManyStopsTheRunBeforeM6)
{
    Fixture f(3);
    for (uint64_t b = 1; b <= 40; ++b)
    {
        f.body(b);
        f.condemnedMarker(b);
    }
    {
        OperationForTest op(*f.backend);
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).create(f.layout.blobIntentKey(blobRefOf(20), f.identity(20)), "other bytes", Retry::standard())));
    }
    f.backend->clearJournal();
    MigratePoolOptions options;
    options.concurrency = 8;

    const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run(options); });
    EXPECT_NE(message.find("could not be confirmed"), String::npos) << message;
    const auto journal = f.backend->journal();
    EXPECT_EQ(writesTo(journal, f.layout.gcStateKey()), 0u);
    EXPECT_EQ(writesTo(journal, f.layout.poolMetaKey()), 0u);
    EXPECT_FALSE(f.gcState().has_value());
    EXPECT_EQ(f.poolMeta().min_reader_generation, 3u);
    EXPECT_EQ(f.markers().size(), 40u);
    size_t created = 0;
    for (uint64_t b = 1; b <= 40; ++b)
        created += b != 20 && f.hasIntent(b, f.identity(b));
    EXPECT_GT(created, 0u) << "positive control: other creates were sent and confirmed";
}

/// A marker that requires a newer reader is not understood: it is no corrupt marker, so the run stops before M5.
TEST(CASMigratePool, MarkerOfANewerFormatStopsTheRunBeforeAnyIntent)
{
    Fixture f(3);
    f.body(1);
    String bytes = encodeBlobMetaForTest(BlobMeta{.state = MetaState::Clean});
    const size_t version = bytes.find("\"v\":1");
    ASSERT_NE(version, String::npos) << bytes;
    bytes.replace(version, 5, "\"v\":99");
    f.rawMarker(1, bytes);
    f.backend->clearJournal();

    const String message = messageOf(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { f.run(); });
    EXPECT_NE(message.find("reading the legacy marker"), String::npos) << message;
    const auto journal = f.backend->journal();
    EXPECT_TRUE(intentWriteIndices(journal, f.layout).empty());
    EXPECT_EQ(writesTo(journal, f.layout.gcStateKey()), 0u);
    EXPECT_EQ(f.poolMeta().min_reader_generation, 3u);
    EXPECT_EQ(f.markers().size(), 1u);
}

/// The probe proves the rights on `blob-intents/` before any point of no return, and names the one that is missing.
TEST(CASMigratePool, MissingRightOnTheIntentPrefixRefusesAtM1)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    f.backend->refuseWritesUnder(f.layout.blobIntentsPrefix());
    f.backend->clearJournal();

    const String message = messageOf(DB::ErrorCodes::READONLY, [&] { f.run(); });
    EXPECT_NE(message.find("grant PUT with If-None-Match on 'p/blob-intents/'"), String::npos) << message;
    const auto journal = f.backend->journal();
    EXPECT_FALSE(writeIndices(journal, f.prefix + "/_probe/").empty()) << "positive control: the battery ran first";
    EXPECT_EQ(writesTo(journal, f.layout.gcStateKey()), 0u);
    EXPECT_EQ(writesTo(journal, f.layout.poolMetaKey()), 0u);
    EXPECT_TRUE(f.intents().empty());
}

/// `--keep-markers` ends the run once the floor is 4; a rerun removes the markers with no other write.
TEST(CASMigratePool, KeepMarkersStopsAtTheFloorAndARerunRemovesThem)
{
    Fixture f(3);
    seedMixedPool(f);
    MigratePoolOptions keep;
    keep.keep_markers = true;
    std::vector<String> lines;
    const MigratePoolReport kept = f.run(keep, &lines);
    EXPECT_TRUE(kept.migrated);
    EXPECT_FALSE(kept.completed);
    EXPECT_TRUE(contains(lines, "servers of this build may start now"));
    EXPECT_EQ(f.poolMeta().min_reader_generation, G_BLOB_INTENTS);
    EXPECT_EQ(f.markers().size(), 3u);

    const MigratePoolReport rerun = f.run();
    EXPECT_TRUE(rerun.completed);
    EXPECT_EQ(rerun.markers_removed, 3u);
    EXPECT_TRUE(f.markers().empty());
}

/// Long steps report on the tool's clock, a page at a time, with an estimate of the time left.
TEST(CASMigratePool, LongStepsReportProgressWithAnEstimate)
{
    constexpr uint64_t bodies = 2 * CasOperation::kCasListPageKeys + 10;
    Fixture f(3);
    for (uint64_t b = 1; b <= bodies; ++b)
        f.body(b);
    for (uint64_t b = 1; b <= 50; ++b)
        f.cleanMarker(b);
    f.backend->on_read_or_head = [&] { f.clock.now.fetch_add(1'000); };
    std::vector<String> lines;
    ASSERT_TRUE(f.run({}, &lines).completed);
    EXPECT_TRUE(contains(lines, "at the M4 rate about"));
    EXPECT_TRUE(std::any_of(lines.begin(), lines.end(), [&](const String & line)
    {
        return line.starts_with("M5: ") && line.find("blob(s) converted of about " + std::to_string(bodies) + ", about") != String::npos
            && line.find(" s left") != String::npos;
    }));
}

/// A member whose slot moves right after the floor is raised: the pool is migrated and cannot go back, so the run
/// names the member, says so, and removes no marker.
TEST(CASMigratePool, MemberThatAppearsAfterTheFloorIsNamed)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    f.rows({});
    f.backend->afterWrite(f.layout.poolMetaKey(), [&] { f.slot("srv-late", liveLease()); });

    const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run(); });
    EXPECT_NE(message.find("server root 'srv-late'"), String::npos) << message;
    EXPECT_NE(message.find("the pool is already migrated and the tool cannot undo it"), String::npos) << message;
    EXPECT_EQ(f.poolMeta().min_reader_generation, G_BLOB_INTENTS);
    EXPECT_EQ(f.markers().size(), 1u) << "no marker is removed after the refusal";
}

/// A member that appears while the markers are removed is a server of this build: the run lists it, completes and
/// removes every marker.
TEST(CASMigratePool, MemberThatAppearsDuringTheMarkerRemovalIsOnlyANotice)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    f.rows({});
    f.backend->afterHead(f.layout.blobMetaKey(blobRefOf(1)), [&] { f.slot("srv-new", liveLease()); });
    MigratePoolOptions options;
    options.concurrency = 1;

    std::vector<String> lines;
    const MigratePoolReport report = f.run(options, &lines);
    EXPECT_TRUE(report.completed);
    EXPECT_TRUE(contains(lines, "M9: seen since the pool was migrated: server root 'srv-new'"));
    EXPECT_TRUE(f.markers().empty()) << "positive control: the removal ran";
}

/// With nobody new, the run prints no notice.
TEST(CASMigratePool, NoNoticeWhenNoMemberAppearedDuringTheMarkerRemoval)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    f.rows({});

    std::vector<String> lines;
    const MigratePoolReport report = f.run({}, &lines);
    EXPECT_TRUE(report.completed);
    EXPECT_FALSE(contains(lines, "seen since the pool was migrated"));
    EXPECT_TRUE(f.markers().empty()) << "positive control: the removal ran";
}

/// A rerun on a migrated pool removes only keys that are a marker's exact spelling.
TEST(CASMigratePool, RerunOnAMigratedPoolLeavesForeignMetaKeys)
{
    Fixture f(3);
    seedMixedPool(f);
    ASSERT_TRUE(f.run().completed);
    f.cleanMarker(2);
    const std::vector<String> foreign{f.prefix + "/blobs/import.meta", f.prefix + "/blobs/ch128/AA/" + String(32, 'A') + ".meta"};
    {
        OperationForTest op(*f.backend);
        for (const String & key : foreign)
            ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, "foreign", Retry::standard()))) << key;
    }

    const MigratePoolReport report = f.run();
    ASSERT_TRUE(report.completed);
    EXPECT_EQ(report.markers_removed, 1u) << "positive control: the real marker was removed";
    EXPECT_EQ(f.markers(), (std::set<String>(foreign.begin(), foreign.end())));
}

TEST(CASMigratePool, RowsAreCarriedAsTheyAre)
{
    Fixture f(3);
    f.body(1);
    f.body(2);
    f.rows({f.row(1, /*pending*/ false), f.row(2, /*pending*/ true)});
    const auto before = currentRetiredSet(*f.backend, f.layout, 0);
    const GcState state_before = *f.gcState();
    ASSERT_EQ(before.size(), 2u);

    ASSERT_TRUE(f.run().completed);
    const auto after = currentRetiredSet(*f.backend, f.layout, 0);
    ASSERT_EQ(after.size(), before.size());
    for (size_t i = 0; i < before.size(); ++i)
    {
        EXPECT_EQ(after[i].ref, before[i].ref);
        EXPECT_EQ(after[i].token.value, before[i].token.value);
        EXPECT_EQ(after[i].delete_pending, before[i].delete_pending);
        EXPECT_EQ(after[i].condemn_round, before[i].condemn_round);
    }
    EXPECT_EQ(f.gcState()->snap_generation, state_before.snap_generation);
    EXPECT_EQ(f.gcState()->snap_attempt, state_before.snap_attempt);
}

TEST(CASMigratePool, GcStateIsRewrittenAtHeader4WithItsFieldsAsRead)
{
    Fixture f(3);
    f.body(1);
    f.rows({f.row(1, /*pending*/ false)});
    f.editGcState([](GcState & state)
    {
        state.round = 7;
        state.snap_pruned_through = 1;
        state.manifest_sweep_cursor = "cursor";
        state.lease = GcLease{.owner = kOldLeader, .seq = 9};
    });
    const GcState before = *f.gcState();
    ASSERT_EQ(before.header_generation, G_REF_CHAIN);

    ASSERT_TRUE(f.run().completed);
    const GcState after = *f.gcState();
    EXPECT_EQ(after.header_generation, G_BLOB_INTENTS);
    EXPECT_EQ(after.round, before.round);
    EXPECT_EQ(after.gc_shards, before.gc_shards);
    EXPECT_EQ(after.snap_generation, before.snap_generation);
    EXPECT_EQ(after.snap_pruned_through, before.snap_pruned_through);
    EXPECT_EQ(after.snap_attempt, before.snap_attempt);
    EXPECT_EQ(after.manifest_sweep_cursor, before.manifest_sweep_cursor);
    EXPECT_TRUE(after.intent_sweep_cursor.empty());
    EXPECT_EQ(after.lease.owner, before.lease.owner);
    EXPECT_EQ(after.lease.seq, before.lease.seq);
}

TEST(CASMigratePool, AbsentGcStateIsCreatedAtHeader4)
{
    Fixture f(3, /*gc_shards*/ 2);
    ASSERT_FALSE(f.gcState().has_value());
    ASSERT_TRUE(f.run().completed);
    /// Positive control: the run looked for the objects a GC leaves before it created the state.
    const auto journal = f.backend->journal();
    const auto sent = [&](JournalVerb verb, const String & key)
    {
        return std::any_of(journal.begin(), journal.end(), [&](const JournalEntry & e)
        {
            return e.verb == verb && !e.keys.empty() && e.keys.front() == key;
        });
    };
    EXPECT_TRUE(sent(JournalVerb::List, f.layout.gcGenPrefix(0).substr(0, f.layout.gcGenPrefix(0).size() - 2)));
    EXPECT_TRUE(sent(JournalVerb::Head, f.layout.gcHbKey()));
    EXPECT_TRUE(sent(JournalVerb::Head, f.layout.gcMaintenanceStateKey()));
    const auto state = f.gcState();
    ASSERT_TRUE(state);
    EXPECT_EQ(state->header_generation, G_BLOB_INTENTS);
    EXPECT_EQ(state->gc_shards, 2u);
    EXPECT_EQ(state->lease.owner, DB::UInt128(0));
    EXPECT_EQ(state->round, 0u);
}

namespace
{

/// Removes `gc/state` and returns its bytes, as a pool that lost the object while GC's other objects stayed.
String loseGcState(Fixture & f)
{
    OperationForTest op(*f.backend);
    const auto got = (*op).read(f.layout.gcStateKey(), Retry::standard());
    if (!got || (*op).removeCurrent(f.layout.gcStateKey(), Retry::standard()) != Removal::Removed)
        throw std::runtime_error("cannot remove gc/state");
    return got->bytes;
}

void restoreGcState(Fixture & f, const String & bytes)
{
    OperationForTest op(*f.backend);
    if (!std::holds_alternative<Committed>((*op).create(f.layout.gcStateKey(), bytes, Retry::standard())))
        throw std::runtime_error("cannot restore gc/state");
}

}

/// Each object GC leaves stays when `gc/state` is lost: a state created now would name none of GC's runs and holds, so
/// the run and its dry run stop before any intent. Once the state is recovered, a rerun converts the pool.
TEST(CASMigratePool, LostGcStateIsRefusedUntilItIsRecovered)
{
    for (const String artifact : {"run and fold seal", "heartbeat", "janitor cursor"})
    {
        SCOPED_TRACE(artifact);
        Fixture f(3);
        f.body(1);
        f.condemnedMarker(1);
        if (artifact == "run and fold seal")
            f.rows({f.row(1, /*pending*/ false)});
        else
        {
            f.editGcState([](GcState &) {});
            if (artifact == "heartbeat")
                f.heartbeat(1);
            else
            {
                OperationForTest op(*f.backend);
                ASSERT_TRUE(std::holds_alternative<Committed>(
                    (*op).create(f.layout.gcMaintenanceStateKey(), "a janitor cursor", Retry::standard())));
            }
        }
        const String state = loseGcState(f);

        for (const bool dry_run : {true, false})
        {
            MigratePoolOptions options;
            options.dry_run = dry_run;
            const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run(options); });
            EXPECT_NE(message.find("gc/state is absent, but GC has run on this pool"), String::npos) << message;
            EXPECT_NE(message.find("restore the pool from a consistent backup"), String::npos) << message;
            EXPECT_NE(message.find(kKeepOfflineAdvice), String::npos) << message;
        }
        EXPECT_TRUE(f.intents().empty());
        EXPECT_FALSE(f.gcState().has_value());
        EXPECT_EQ(f.poolMeta().min_reader_generation, 3u);

        restoreGcState(f, state);
        ASSERT_TRUE(f.run().completed);
        EXPECT_EQ(f.gcState()->header_generation, G_BLOB_INTENTS);
        EXPECT_TRUE(f.hasIntent(1, f.identity(1)));
    }
}

/// A `gc/state` at generation 0 beside a fold seal it does not name stops the run and its dry run at M3, before any
/// marker read or write of M4 onward. An interrupted `cas-gc-rebuild` leaves the state holding its lease, which an
/// older `cas-gc-rebuild` refuses, so the advice is a backup on every store.
TEST(CASMigratePool, InterruptedRebuildIsRefusedBeforeAnyIntent)
{
    for (const bool lease_held : {true, false})
    for (const Dialect dialect : {Dialect::Emulated, Dialect::ETag})
    {
        SCOPED_TRACE(fmt::format("{}, lease {}", dialectWord(dialect), lease_held ? "held" : "free"));
        Fixture f(3);
        f.backend->setDialect(dialect);
        f.body(1);
        f.condemnedMarker(1);
        f.rows({f.row(1, /*pending*/ false)});
        const GcState adopted = *f.gcState();
        ASSERT_NE(adopted.snap_generation, 0u);
        f.editGcState([&](GcState & state)
        {
            state.snap_generation = 0;
            state.snap_attempt = 0;
            state.lease = lease_held ? GcLease{.owner = kOldLeader, .seq = 1} : GcLease{};
        });
        f.backend->clearJournal();

        for (const bool dry_run : {true, false})
        {
            MigratePoolOptions options;
            options.dry_run = dry_run;
            const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run(options); });
            if (lease_held)
            {
                EXPECT_NE(message.find("M3: gc/state names no fold seal and GC owner " + u128ToHex(kOldLeader)
                    + " holds its lease, but `p/gc/gen/"), String::npos) << message;
                EXPECT_NE(message.find("refuses this state with `another GC leader holds the lease`"), String::npos) << message;
                EXPECT_NE(message.find("restore the pool from a consistent backup"), String::npos) << message;
                EXPECT_NE(message.find(kKeepOfflineAdvice), String::npos) << message;
                EXPECT_EQ(message.find("READ_ONLY_CONFIG_FILE"), String::npos) << message;
                continue;
            }
            EXPECT_NE(message.find("M3: gc/state names no fold seal, but `p/gc/gen/"), String::npos) << message;
            if (dialect == Dialect::Emulated)
            {
                EXPECT_NE(message.find("restore the pool from a consistent backup"), String::npos) << message;
                EXPECT_NE(message.find(kKeepOfflineAdvice), String::npos) << message;
            }
            else
            {
                EXPECT_NE(message.find("--config-file READ_ONLY_CONFIG_FILE"), String::npos) << message;
                EXPECT_NE(message.find("without --trust-clean-markers"), String::npos) << message;
            }
        }
        for (const JournalEntry & e : f.backend->journal())
        {
            if (e.verb == JournalVerb::Read)
                EXPECT_FALSE(e.keys.front().ends_with(".meta")) << "M4 read a marker: " << e.keys.front();
            if (e.verb == JournalVerb::Write || e.verb == JournalVerb::Remove)
                EXPECT_NE(e.keys.front().find("/_probe/"), String::npos) << "a write past the probe: " << e.keys.front();
        }
        EXPECT_TRUE(f.intents().empty());
        EXPECT_EQ(f.gcState()->snap_generation, 0u);
        EXPECT_EQ(f.gcState()->header_generation, G_REF_CHAIN);
        EXPECT_EQ(f.poolMeta().min_reader_generation, 3u);

        /// What the completed rebuild publishes: a state that names a fold seal.
        f.editGcState([&](GcState & state)
        {
            state.snap_generation = adopted.snap_generation;
            state.snap_attempt = adopted.snap_attempt;
        });
        ASSERT_TRUE(f.run().completed);
        EXPECT_TRUE(f.hasIntent(1, f.identity(1)));
    }
}

/// A GC that took its lease and never folded leaves `gc/state` at generation 0, `gc/hb` and possibly a janitor cursor.
/// None of them carries a hold, so the run converts the pool.
TEST(CASMigratePool, GcStateOfAGcThatNeverFoldedMigrates)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    f.editGcState([](GcState & state)
    {
        state.header_generation = G_REF_CHAIN;
        state.lease = GcLease{.owner = kOldLeader, .seq = 1};
    });
    f.heartbeat(1);
    {
        OperationForTest op(*f.backend);
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).create(f.layout.gcMaintenanceStateKey(), "a janitor cursor", Retry::standard())));
    }
    ASSERT_EQ(f.gcState()->snap_generation, 0u);
    f.backend->clearJournal();

    ASSERT_TRUE(f.run().completed);
    /// Positive control: the run looked under `gc/gen/` before it trusted the state.
    const String generations = f.layout.gcGenPrefix(0).substr(0, f.layout.gcGenPrefix(0).size() - 2);
    const auto journal = f.backend->journal();
    EXPECT_TRUE(std::any_of(journal.begin(), journal.end(), [&](const JournalEntry & e)
    {
        return e.verb == JournalVerb::List && e.keys.front() == generations;
    })) << "no LIST of " << generations;
    EXPECT_EQ(f.gcState()->header_generation, G_BLOB_INTENTS);
    EXPECT_EQ(f.gcState()->lease.owner, kOldLeader);
    EXPECT_TRUE(f.hasIntent(1, f.identity(1)));
}

TEST(CASMigratePool, LateOldGcCommitRestartsTheRun)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    f.rows({});
    f.editGcState([](GcState & state)
    {
        state.round = 7;
        state.lease = GcLease{.owner = kOldLeader, .seq = 3};
    });
    f.backend->beforeHead(f.layout.blobKey(blobRefOf(1)), [&] { f.editGcState([](GcState & state) { state.round = 8; }); });
    f.backend->clearJournal();

    std::vector<String> lines;
    const MigratePoolReport report = f.run({}, &lines);
    ASSERT_TRUE(report.completed);
    EXPECT_TRUE(contains(lines, "restart")) << "the run did not restart after the conflict";
    const GcState state = *f.gcState();
    EXPECT_EQ(state.header_generation, G_BLOB_INTENTS);
    EXPECT_EQ(state.round, 8u) << "the restarted run must rewrite the state it read after the late commit";
    /// The late commit, the refused rewrite, the rewrite after the restart.
    EXPECT_EQ(writesTo(f.backend->journal(), f.layout.gcStateKey()), 3u);
}

TEST(CASMigratePool, RecheckRefusesASlotThatAppearedAfterTheGcStateRewrite)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    f.rows({});
    f.backend->afterWrite(f.layout.gcStateKey(), [&] { f.slot("srv-late", liveLease()); });

    const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run(); });
    EXPECT_NE(message.find("mount slot of server root 'srv-late' changed during the migration"), String::npos) << message;
    EXPECT_EQ(f.gcState()->header_generation, G_BLOB_INTENTS) << "the refusal follows the gc/state rewrite";
    EXPECT_EQ(f.poolMeta().min_reader_generation, 3u);
    EXPECT_EQ(f.markers().size(), 1u);
}

TEST(CASMigratePool, RecheckRefusesAMovedGcPair)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    f.rows({});
    f.heartbeat(1);
    f.backend->afterWrite(f.layout.gcStateKey(), [&] { f.heartbeat(2); });

    const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run(); });
    EXPECT_NE(message.find("GC lease of owner " + u128ToHex(kOldLeader) + " moved during the migration"), String::npos) << message;
    EXPECT_EQ(f.poolMeta().min_reader_generation, 3u);
    EXPECT_EQ(f.markers().size(), 1u);
}

/// One successful run: the last intent write, the `gc/state` write, the floor, the line that lets servers start,
/// then every marker delete. Removing markers is not part of the downtime.
TEST(CASMigratePool, SuccessfulRunOrdersItsWritesAndMarkerDeletes)
{
    Fixture f(3);
    seedMixedPool(f);
    f.backend->clearJournal();
    ASSERT_TRUE(f.run().completed);

    const auto journal = f.backend->journal();
    const auto meta_writes = writeIndices(journal, f.layout.poolMetaKey());
    ASSERT_EQ(meta_writes.size(), 1u);
    const auto intent_writes = intentWriteIndices(journal, f.layout);
    const auto state_writes = writeIndices(journal, f.layout.gcStateKey());
    ASSERT_FALSE(intent_writes.empty());
    ASSERT_EQ(state_writes.size(), 1u);
    EXPECT_LT(intent_writes.back(), state_writes.front());
    EXPECT_LT(state_writes.front(), meta_writes.front());
    for (size_t i = meta_writes.front() + 1; i < journal.size(); ++i)
        EXPECT_NE(journal[i].verb, JournalVerb::Write) << "a write after the floor: " << journal[i].keys.front();

    size_t may_start = journal.size();
    for (size_t i = 0; i < journal.size() && may_start == journal.size(); ++i)
        if (journal[i].verb == JournalVerb::Mark && journal[i].detail.find("may start now") != String::npos)
            may_start = i;
    ASSERT_LT(may_start, journal.size());
    EXPECT_GT(may_start, meta_writes.front()) << "the line follows the floor";
    const auto deletes = markerDeleteIndices(journal);
    EXPECT_EQ(deletes.size(), 3u) << "positive control: every marker read at M4 is deleted";
    for (const size_t i : deletes)
        EXPECT_GT(i, may_start) << journal[i].keys.front();

    EXPECT_EQ(f.poolMeta().min_reader_generation, G_BLOB_INTENTS);
    EXPECT_EQ(f.poolMeta().token_dialect, Dialect::Emulated);
    EXPECT_TRUE(f.markers().empty());
}

class CASMigratePoolCrash : public ::testing::TestWithParam<size_t>
{
};

INSTANTIATE_TEST_SUITE_P(CASMigratePool, CASMigratePoolCrash, ::testing::Range(size_t{1}, size_t{10}),
    [](const ::testing::TestParamInfo<size_t> & param) { return "M" + std::to_string(param.param); });

TEST_P(CASMigratePoolCrash, CrashAfterEachStepThenRerunCompletes)
{
    FinalState reference;
    {
        Fixture f(3);
        seedMixedPool(f);
        ASSERT_TRUE(f.run().completed);
        reference = finalStateOf(f);
    }

    Fixture f(3);
    seedMixedPool(f);
    MigratePoolOptions stop;
    stop.stop_after_step_for_test = GetParam();
    const String message = messageOf(DB::ErrorCodes::ABORTED, [&] { f.run(stop); });
    EXPECT_NE(message.find("after step M" + std::to_string(GetParam())), String::npos) << message;

    ASSERT_TRUE(f.run().completed);
    EXPECT_EQ(finalStateOf(f), reference);
    EXPECT_TRUE(reference.markers.empty());
    EXPECT_TRUE(reference.silent_slot_fenced);
    EXPECT_EQ(reference.header, G_BLOB_INTENTS);
}

TEST(CASMigratePool, CrashInsideM9ThenRerunRemovesTheRest)
{
    Fixture f(3);
    seedMixedPool(f);
    const String refused = *f.markers().rbegin();
    f.backend->refuseDeletes(refused);
    MigratePoolOptions one_at_a_time;
    one_at_a_time.concurrency = 1;
    messageOf(DB::ErrorCodes::NOT_IMPLEMENTED, [&] { f.run(one_at_a_time); });
    EXPECT_EQ(f.poolMeta().min_reader_generation, G_BLOB_INTENTS);
    EXPECT_EQ(f.markers(), (std::set<String>{refused})) << "the markers before the refused one are deleted";

    f.backend->allowDeletes(refused);
    const MigratePoolReport report = f.run();
    ASSERT_TRUE(report.completed);
    EXPECT_EQ(report.markers_removed, 1u);
    EXPECT_TRUE(f.markers().empty());
}

TEST(CASMigratePool, CrashBetweenTwoFencesThenRerunCompletes)
{
    Fixture f(3);
    f.slot("srv-a", liveLease());
    f.slot("srv-b", liveLease());
    f.backend->refuseWrites(f.layout.mountKey("srv-b"), 1);
    messageOf(DB::ErrorCodes::NOT_IMPLEMENTED, [&] { f.run(); });
    EXPECT_TRUE(f.slotBody("srv-a")->gc_fenced);
    EXPECT_FALSE(f.slotBody("srv-b")->gc_fenced);
    EXPECT_EQ(f.poolMeta().min_reader_generation, 3u);

    ASSERT_TRUE(f.run().completed);
    EXPECT_TRUE(f.slotBody("srv-b")->gc_fenced);
    EXPECT_EQ(f.poolMeta().min_reader_generation, G_BLOB_INTENTS);
}

TEST(CASMigratePool, RerunOfAMigratedPoolOnlyRemovesMarkers)
{
    Fixture f(3);
    seedMixedPool(f);
    ASSERT_TRUE(f.run().completed);
    const auto intents = f.intents();
    const auto state = *f.gcState();
    f.cleanMarker(2);
    f.backend->clearJournal();

    const MigratePoolReport report = f.run();
    ASSERT_TRUE(report.completed);
    EXPECT_EQ(report.markers_removed, 1u);
    EXPECT_TRUE(f.markers().empty());
    const auto journal = f.backend->journal();
    for (const JournalEntry & e : journal)
    {
        EXPECT_NE(e.verb, JournalVerb::Write) << e.keys.front();
        if (e.verb == JournalVerb::Remove)
            EXPECT_EQ(e.keys.front(), f.layout.blobMetaKey(blobRefOf(2)));
    }
    EXPECT_EQ(f.intents(), intents);
    EXPECT_EQ(f.gcState()->round, state.round);
}

TEST(CASMigratePool, TwoRunsConverge)
{
    Fixture f(3);
    seedMixedPool(f);
    bool other_completed = false;
    f.backend->beforeHead(f.layout.blobKey(blobRefOf(1)), [&] { other_completed = f.run().completed; });

    std::vector<String> lines;
    const MigratePoolReport report = f.run({}, &lines);
    ASSERT_TRUE(report.completed);
    ASSERT_TRUE(other_completed);
    EXPECT_TRUE(contains(lines, "restart"));
    EXPECT_FALSE(report.floor_raised) << "the other run raised the floor; this run's M8 sent no write";

    Fixture reference_pool(3);
    seedMixedPool(reference_pool);
    ASSERT_TRUE(reference_pool.run().completed);
    EXPECT_EQ(finalStateOf(f), finalStateOf(reference_pool));
}

TEST(CASMigratePool, ARunWithAnotherDialectLosesM8AndIsRefused)
{
    Fixture f(3);
    seedMixedPool(f);
    f.backend->setDialect(Dialect::ETag);
    bool other_completed = false;
    f.backend->beforeHead(f.layout.blobKey(blobRefOf(1)), [&]
    {
        f.backend->setDialect(Dialect::Emulated);
        other_completed = f.run().completed;
        f.backend->setDialect(Dialect::ETag);
    });

    const String message = messageOf(DB::ErrorCodes::BAD_ARGUMENTS, [&] { f.run(); });
    ASSERT_TRUE(other_completed);
    EXPECT_NE(message.find("CAS pool records token dialect 'emulated', this disk uses 'etag'"), String::npos) << message;
    EXPECT_EQ(f.poolMeta().token_dialect, Dialect::Emulated);
    size_t etag_intents = 0;
    size_t emulated_intents = 0;
    for (const String & key : f.intents())
    {
        const auto parsed = f.layout.parseBlobIntentKey(key);
        ASSERT_TRUE(parsed) << key;
        etag_intents += parsed->identity.dialect == Dialect::ETag;
        emulated_intents += parsed->identity.dialect == Dialect::Emulated;
    }
    EXPECT_GT(etag_intents, 0u) << "the losing run's intents stand, for the orphan sweep";
    EXPECT_GT(emulated_intents, 0u);
}

TEST(CASMigratePool, DryRunWritesNothingAndChecksOnlyReadRights)
{
    Fixture f(3);
    seedMixedPool(f);
    f.backend->clearJournal();
    MigratePoolOptions options;
    options.dry_run = true;
    const MigratePoolReport report = f.run(options);
    EXPECT_FALSE(report.completed);

    const auto journal = f.backend->journal();
    EXPECT_FALSE(anyMutation(journal));
    bool read_meta = false;
    bool listed_slots = false;
    bool read_intent_prefix = false;
    for (const JournalEntry & e : journal)
    {
        for (const String & key : e.keys)
        {
            EXPECT_FALSE(key.starts_with(f.prefix + "/_probe/")) << "the write battery ran: " << key;
            read_intent_prefix = read_intent_prefix || key.starts_with(f.layout.blobIntentsProbePrefix());
        }
        read_meta = read_meta || (e.verb == JournalVerb::Read && e.keys.front() == f.layout.poolMetaKey());
        listed_slots = listed_slots || (e.verb == JournalVerb::List && e.keys.front() == f.layout.serverRootsPrefix());
    }
    EXPECT_TRUE(read_meta) << "positive control: the dry run read _pool_meta";
    EXPECT_TRUE(listed_slots) << "positive control: the dry run listed the mount slots";
    EXPECT_TRUE(read_intent_prefix) << "the dry run checks the read rights on blob-intents/";
    EXPECT_EQ(report.slots_read, 2u);
    EXPECT_EQ(report.condemned_rows, 3u);
    EXPECT_EQ(report.markers_read, 3u);
}

TEST(CASMigratePool, ProbeFailureRefusesAtM1)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    f.backend->weakHeadsFor("/_probe/");
    f.backend->clearJournal();

    const String message = messageOf(DB::ErrorCodes::NOT_IMPLEMENTED, [&] { f.run(); });
    EXPECT_NE(message.find("the store returns a token that cannot serve as a blob identity"), String::npos) << message;
    const auto journal = f.backend->journal();
    EXPECT_TRUE(intentWriteIndices(journal, f.layout).empty());
    EXPECT_EQ(writesTo(journal, f.layout.gcStateKey()), 0u);
    EXPECT_EQ(writesTo(journal, f.layout.poolMetaKey()), 0u);
    EXPECT_FALSE(writeIndices(journal, f.prefix + "/_probe/").empty()) << "positive control: the probe ran";
}

TEST(CASMigratePool, EveryListedBodyGetsAnIntentUnlessCleanMarkersAreTrusted)
{
    const auto seed = [](Fixture & f)
    {
        f.body(1);
        f.cleanMarker(1);
        f.body(2);
        f.body(3);
        f.condemnedMarker(3);
    };
    Fixture f(3);
    seed(f);
    std::vector<String> lines;
    ASSERT_TRUE(f.run({}, &lines).completed);
    EXPECT_TRUE(contains(lines, "M1: every body gets an intent"));
    for (const uint64_t b : {1, 2, 3})
        EXPECT_TRUE(f.hasIntent(b, f.identity(b))) << "blob " << b;
    EXPECT_EQ(f.intents().size(), 3u);

    /// With the clean markers trusted only the condemned marker gets one.
    Fixture trusted(3);
    seed(trusted);
    MigratePoolOptions options;
    options.trust_clean_markers = true;
    ASSERT_TRUE(trusted.run(options).completed);
    EXPECT_EQ(trusted.intents(), (std::set<String>{trusted.layout.blobIntentKey(blobRefOf(3), trusted.identity(3))}));
}

TEST(CASMigratePool, MarkersAcrossListPagesAreAllRead)
{
    constexpr uint64_t blobs = CasOperation::kCasListPageKeys + 2;
    Fixture f(3);
    std::map<String, uint64_t> marker_of;
    for (uint64_t b = 1; b <= blobs; ++b)
    {
        f.body(b);
        f.cleanMarker(b);
        marker_of[f.layout.blobMetaKey(blobRefOf(b))] = b;
    }
    /// The marker listed last is the one that is not `Clean`.
    const uint64_t last = marker_of.rbegin()->second;
    condemnMeta(*f.backend, f.layout, numberedBlobHash(last), 1);

    MigratePoolOptions trust;
    trust.trust_clean_markers = true;
    const MigratePoolReport report = f.run(trust);
    ASSERT_TRUE(report.completed);
    EXPECT_EQ(report.markers_read, blobs);
    EXPECT_EQ(report.markers_removed, blobs);
    EXPECT_EQ(f.intents(), (std::set<String>{f.layout.blobIntentKey(blobRefOf(last), f.identity(last))}));
    EXPECT_TRUE(f.markers().empty());
}

TEST(CASMigratePool, DeferredStartupSendsNoRequest)
{
    const auto count_files = [](const DB::ObjectStoragePtr & storage)
    {
        size_t files = 0;
        for (const auto & entry : std::filesystem::recursive_directory_iterator(storage->getCommonKeyPrefix()))
            files += entry.is_regular_file();
        return files;
    };
    const auto settings = makeSettingsForTest("srv1", std::filesystem::temp_directory_path() / "cas_migrate_deferred_scratch");

    {
        DB::setDeferPoolOpenForMigration(true);
        SCOPE_EXIT({ DB::setDeferPoolOpenForMigration(false); });
        auto object_storage = makeLocalObjectStorageForTest();
        auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(object_storage, "pool", "srv1", "", nullptr, settings);
        storage->startup();
        EXPECT_EQ(storage->poolForTest(), nullptr);
        EXPECT_EQ(count_files(object_storage), 0u) << "a deferred startup wrote to the store";
    }
    {
        /// Positive control: without the flag the same startup creates the pool.
        auto object_storage = makeLocalObjectStorageForTest();
        auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(object_storage, "pool", "srv1", "", nullptr, settings);
        storage->startup();
        EXPECT_NE(storage->poolForTest(), nullptr);
        EXPECT_GT(count_files(object_storage), 0u);
    }
}

TEST(CASMigratePool, FloorOnePoolIsRaisedAfterM5AndCoversEveryBody)
{
    Fixture f(1);
    seedOneBodyOfEachMarking(f);
    f.rows({}, G_WRITE_FLOOR);
    f.backend->clearJournal();

    std::vector<String> lines;
    MigratePoolOptions trust;
    trust.trust_clean_markers = true;
    const MigratePoolReport report = f.run(trust, &lines);
    ASSERT_TRUE(report.completed);
    EXPECT_EQ(report.floor_read, 1u);
    EXPECT_TRUE(contains(lines, "--trust-clean-markers is ignored"));
    for (const uint64_t b : {1, 2, 3})
        EXPECT_TRUE(f.hasIntent(b, f.identity(b))) << "blob " << b;

    const auto journal = f.backend->journal();
    const auto meta_writes = writeIndices(journal, f.layout.poolMetaKey());
    const auto intent_writes = intentWriteIndices(journal, f.layout);
    const auto state_writes = writeIndices(journal, f.layout.gcStateKey());
    ASSERT_EQ(meta_writes.size(), 2u) << "the raise to 3, then to 4";
    ASSERT_EQ(intent_writes.size(), 3u);
    ASSERT_EQ(state_writes.size(), 1u);
    EXPECT_GT(meta_writes[0], intent_writes.back()) << "the floor 3 raise precedes an intent create";
    EXPECT_LT(meta_writes[0], state_writes.front()) << "the floor 3 raise follows the gc/state write";
    EXPECT_GT(meta_writes[1], state_writes.front());
    EXPECT_EQ(f.poolMeta().min_reader_generation, G_BLOB_INTENTS);
}

TEST(CASMigratePool, CrashAfterTheRaiseThenRerunCompletes)
{
    Fixture f(1);
    seedOneBodyOfEachMarking(f);
    f.rows({}, G_WRITE_FLOOR);
    f.backend->refuseWrites(f.layout.gcStateKey(), 1);
    MigratePoolOptions trust;
    trust.trust_clean_markers = true;
    messageOf(DB::ErrorCodes::NOT_IMPLEMENTED, [&] { f.run(trust); });
    EXPECT_EQ(f.poolMeta().min_reader_generation, G_REF_CHAIN) << "the run stopped after the raise";
    EXPECT_EQ(f.gcState()->header_generation, G_WRITE_FLOOR);
    const auto first_intents = f.intents();
    ASSERT_EQ(first_intents.size(), 3u);

    std::vector<String> lines;
    const MigratePoolReport report = f.run(trust, &lines);
    ASSERT_TRUE(report.completed);
    EXPECT_EQ(report.floor_read, G_REF_CHAIN);
    EXPECT_FALSE(contains(lines, "ignored")) << "at floor 3 the rerun takes the option";
    EXPECT_EQ(report.intents_confirmed, 1u) << "only the condemned marker is converted again";
    for (const String & key : first_intents)
        EXPECT_TRUE(f.intents().contains(key)) << key;
    EXPECT_EQ(f.gcState()->header_generation, G_BLOB_INTENTS);
    EXPECT_EQ(f.poolMeta().min_reader_generation, G_BLOB_INTENTS);
}

TEST(CASMigratePool, DryRunOnAFloorOnePoolWritesNothing)
{
    Fixture f(1);
    f.body(1);
    f.condemnedMarker(1);
    f.backend->clearJournal();
    MigratePoolOptions options;
    options.dry_run = true;
    const MigratePoolReport report = f.run(options);
    EXPECT_FALSE(report.completed);
    EXPECT_EQ(report.floor_read, 1u);
    EXPECT_FALSE(anyMutation(f.backend->journal()));
    EXPECT_EQ(f.poolMeta().min_reader_generation, 1u);
    EXPECT_EQ(report.markers_read, 1u) << "positive control: the dry run read the marker";
}

TEST(CASRemoveLegacyMarkers, DeletesEveryMarkerKeyAndNothingElse)
{
    Fixture f(3);
    std::vector<String> keys;
    for (uint64_t b = 1; b <= 3; ++b)
    {
        f.cleanMarker(b);
        keys.push_back(f.layout.blobMetaKey(blobRefOf(b)));
    }
    keys.push_back(f.layout.blobMetaKey(blobRefOf(4)));
    /// Keys under `blobs/` that end in `.meta` but are no marker's spelling.
    const std::vector<String> foreign{f.prefix + "/blobs/import.meta", f.prefix + "/blobs/ch128/AA/" + String(32, 'A') + ".meta",
                                      f.layout.blobMetaKey(blobRefOf(5)) + ".meta"};
    {
        OperationForTest op(*f.backend);
        for (const String & key : foreign)
            ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, "foreign", Retry::standard()))) << key;
    }
    keys.insert(keys.end(), foreign.begin(), foreign.end());

    auto pool = f.open();
    const LegacyMarkerRemoval removal = removeLegacyMarkers(*pool, keys);
    EXPECT_EQ(removal.removed, 3u) << "an absent key is not counted";
    OperationForTest op(*f.backend);
    for (size_t i = 0; i < 4; ++i)
        EXPECT_FALSE((*op).head(keys[i], Retry::standard()).has_value()) << keys[i];
    for (const String & key : foreign)
        EXPECT_TRUE((*op).head(key, Retry::standard()).has_value()) << "a key that is not a marker was deleted: " << key;
}

TEST(CASRemoveLegacyMarkers, RefusedDeleteThrowsWithTheCount)
{
    Fixture f(3);
    std::vector<String> keys;
    for (uint64_t b = 1; b <= 3; ++b)
    {
        f.cleanMarker(b);
        keys.push_back(f.layout.blobMetaKey(blobRefOf(b)));
    }
    std::sort(keys.begin(), keys.end());
    f.backend->refuseDeletes(keys[1]);
    auto pool = f.open();
    const String message = messageOf(DB::ErrorCodes::NOT_IMPLEMENTED, [&] { removeLegacyMarkers(*pool, keys); });
    EXPECT_NE(message.find("1 legacy marker(s) deleted before"), String::npos) << message;
    OperationForTest op(*f.backend);
    EXPECT_FALSE((*op).head(keys[0], Retry::standard()).has_value());
    EXPECT_TRUE((*op).head(keys[1], Retry::standard()).has_value());
    EXPECT_TRUE((*op).head(keys[2], Retry::standard()).has_value()) << "the run stops at the refusal";
}

/// A marker replaced between its `HEAD` and the delete is left and counted, not deleted at its new token.
TEST(CASRemoveLegacyMarkers, ChangedMarkerIsLeftAndCounted)
{
    Fixture f(3);
    f.cleanMarker(1);
    f.cleanMarker(2);
    const String changing = f.layout.blobMetaKey(blobRefOf(1));
    f.backend->afterHead(changing, [&] { condemnMeta(*f.backend, f.layout, numberedBlobHash(1), 7); });
    auto pool = f.open();
    const LegacyMarkerRemoval removal = removeLegacyMarkers(*pool, {changing, f.layout.blobMetaKey(blobRefOf(2))});
    EXPECT_EQ(removal.changed, 1u);
    EXPECT_EQ(removal.removed, 1u) << "positive control: the other marker was removed";
    EXPECT_EQ(f.markers(), (std::set<String>{changing}));
}

TEST(CASMigratePool, FloorOneCrashAfterM5RerunStillCoversEveryBody)
{
    Fixture f(1);
    seedOneBodyOfEachMarking(f);
    MigratePoolOptions stop;
    stop.stop_after_step_for_test = 5;
    stop.trust_clean_markers = true;
    messageOf(DB::ErrorCodes::ABORTED, [&] { f.run(stop); });
    EXPECT_EQ(f.poolMeta().min_reader_generation, 1u) << "the stop precedes the raise";
    /// Lose the first run's intents but one, so the rerun must create them itself.
    {
        OperationForTest op(*f.backend);
        for (const uint64_t b : {1, 2})
            ASSERT_EQ((*op).removeCurrent(f.layout.blobIntentKey(blobRefOf(b), f.identity(b)), Retry::standard()), Removal::Removed);
    }

    std::vector<String> lines;
    MigratePoolOptions trust;
    trust.trust_clean_markers = true;
    const MigratePoolReport report = f.run(trust, &lines);
    ASSERT_TRUE(report.completed);
    EXPECT_TRUE(contains(lines, "--trust-clean-markers is ignored"));
    for (const uint64_t b : {1, 2, 3})
        EXPECT_TRUE(f.hasIntent(b, f.identity(b))) << "blob " << b;
}

TEST(CASMigratePool, DryRunOnAMigratedPoolWritesNothing)
{
    Fixture f(3);
    seedMixedPool(f);
    ASSERT_TRUE(f.run().completed);
    ASSERT_EQ(f.poolMeta().min_reader_generation, G_BLOB_INTENTS);
    f.cleanMarker(2);
    f.cleanMarker(4);
    const auto markers = f.markers();
    ASSERT_EQ(markers.size(), 2u);
    f.backend->clearJournal();

    MigratePoolOptions options;
    options.dry_run = true;
    std::vector<String> lines;
    const MigratePoolReport report = f.run(options, &lines);
    EXPECT_FALSE(report.completed);
    EXPECT_TRUE(contains(lines, "2 marker(s) would be removed; nothing was written"));
    EXPECT_FALSE(contains(lines, "marker(s) removed"));

    const auto journal = f.backend->journal();
    EXPECT_FALSE(anyMutation(journal));
    const bool listed_blobs = std::any_of(journal.begin(), journal.end(), [&](const JournalEntry & e)
    {
        return e.verb == JournalVerb::List && e.keys.front() == f.layout.blobsPrefix();
    });
    EXPECT_TRUE(listed_blobs) << "positive control: the dry run listed blobs/";
    EXPECT_EQ(report.markers_read, 2u);
    EXPECT_EQ(report.markers_removed, 0u);
    EXPECT_EQ(f.markers(), markers);
}

/// Every floor the tool accepts, with a live slot it would fence and markers it would delete.
TEST(CASMigratePool, DryRunJournalHoldsNoWriteAtFloors1To4)
{
    for (const uint64_t floor : {G_WRITE_FLOOR, G_REF_CHAIN, G_BLOB_INTENTS})
    {
        SCOPED_TRACE(floor);
        Fixture f(floor == G_BLOB_INTENTS ? G_REF_CHAIN : floor);
        seedMixedPool(f);
        if (floor == G_BLOB_INTENTS)
        {
            ASSERT_TRUE(f.run().completed);
            f.condemnedMarker(1);
            f.slot("srv-silent", liveLease(5));
        }
        f.backend->clearJournal();

        MigratePoolOptions options;
        options.dry_run = true;
        const MigratePoolReport report = f.run(options);
        EXPECT_FALSE(report.completed);
        EXPECT_EQ(report.floor_read, floor);
        const auto journal = f.backend->journal();
        for (const JournalEntry & e : journal)
            EXPECT_TRUE(e.verb != JournalVerb::Write && e.verb != JournalVerb::Remove && e.verb != JournalVerb::RemoveMany
                        && e.verb != JournalVerb::Publish) << (e.keys.empty() ? String{} : e.keys.front());
        EXPECT_GT(report.markers_read, 0u) << "positive control: the dry run found markers";
        EXPECT_FALSE(f.slotBody("srv-silent")->gc_fenced);
    }
}

/// A slot that the listing misses but the catalog names is watched: its renewal refuses the run.
TEST(CASMigratePool, CatalogNamedSlotMissingFromTheListingIsWatched)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    {
        OperationForTest op(*f.backend);
        CasRefCatalog::casAdmitEntry(*op, f.layout, 1,
            CatalogEntry{.ns = RootNamespace("srv-x/t"), .state = NsState::Live, .incarnation = DB::UInt128{901}});
    }
    f.slot("srv-x", liveLease(1));
    f.backend->hideFromListing(f.layout.mountKey("srv-x"));
    bool renewed = false;
    f.on_sleep = [&](uint64_t)
    {
        if (!renewed)
        {
            renewed = true;
            f.slot("srv-x", liveLease(2));
        }
    };
    f.backend->clearJournal();

    const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run(); });
    EXPECT_NE(message.find("mount slot of server root 'srv-x' changed during the migration"), String::npos) << message;
    EXPECT_TRUE(renewed);
    const auto journal = f.backend->journal();
    const bool headed = std::any_of(journal.begin(), journal.end(), [&](const JournalEntry & e)
    {
        return e.verb == JournalVerb::Head && e.keys.front() == f.layout.mountKey("srv-x");
    });
    EXPECT_TRUE(headed) << "the catalog-named slot was not read by its key";
    EXPECT_TRUE(intentWriteIndices(journal, f.layout).empty());
    EXPECT_EQ(f.poolMeta().min_reader_generation, 3u);
}

TEST(CASMigratePool, CatalogNamedSilentSlotMissingFromTheListingIsFenced)
{
    Fixture f(3);
    {
        OperationForTest op(*f.backend);
        CasRefCatalog::casAdmitEntry(*op, f.layout, 1,
            CatalogEntry{.ns = RootNamespace("srv-x/t"), .state = NsState::Live, .incarnation = DB::UInt128{902}});
    }
    f.slot("srv-x", liveLease());
    f.backend->hideFromListing(f.layout.mountKey("srv-x"));
    ASSERT_TRUE(f.run().completed);
    EXPECT_TRUE(f.slotBody("srv-x")->gc_fenced);
}

/// Blobs 1 and 3 belong to shard 1 of 2.
TEST(CASMigratePool, RowsOfAShardAboveZeroAreMigrated)
{
    Fixture f(3, /*gc_shards*/ 2);
    f.editGcState([](GcState & state) { state.gc_shards = 2; });
    f.body(1);
    f.body(3);
    const RetiredEntry candidate = f.row(1, /*pending*/ false);
    const RetiredEntry pending_absent = f.row(3, /*pending*/ true);
    f.removeBody(3);
    f.rows({candidate, pending_absent}, G_REF_CHAIN, /*shard*/ 1);
    ASSERT_EQ(currentRetiredSet(*f.backend, f.layout, 1).size(), 2u);
    ASSERT_TRUE(currentRetiredSet(*f.backend, f.layout, 0).empty());

    const MigratePoolReport report = f.run();
    ASSERT_TRUE(report.completed);
    EXPECT_EQ(report.condemned_rows, 2u);
    EXPECT_TRUE(f.hasIntent(1, f.identity(1))) << "candidate row of shard 1";
    EXPECT_TRUE(f.hasIntent(3, *pending_absent.token.canonical())) << "pending row of shard 1 over an absent body";
    EXPECT_EQ(f.intents().size(), 2u);
    EXPECT_EQ(currentRetiredSet(*f.backend, f.layout, 1).size(), 2u) << "the rows are carried";
}

/// GC of this build refuses a fold seal that misses a shard's condemned summary. After the floor is raised no older
/// build can repair it, so the run stops at M3 and leaves the pool as it was.
TEST(CASMigratePool, SealMissingAShardSummaryIsRefusedBeforeAnyWrite)
{
    Fixture f(3, /*gc_shards*/ 2);
    f.editGcState([](GcState & state) { state.gc_shards = 2; });
    f.body(1);
    f.rows({f.row(1, /*pending*/ true)}, G_REF_CHAIN, /*shard*/ 1);
    injectCondemnedSummarySeal(*f.backend, f.layout, /*generation*/ 1, /*attempt*/ 1, /*gc_shards*/ 2,
        {{1, CondemnedSummary{.condemned_total = 1, .pending_total = 1}}});
    const auto before = objectsOf(f);
    f.backend->clearJournal();

    std::vector<String> lines;
    const String message = messageOf(DB::ErrorCodes::CORRUPTED_DATA, [&] { f.run({}, &lines); });

    EXPECT_NE(message.find("condemned summary has 1 rows, but exactly 2 shards are required"), String::npos) << message;
    EXPECT_NE(message.find("M3: GC of this build refuses the fold seal"), String::npos) << message;
    EXPECT_TRUE(contains(lines, "M2:")) << "positive control: the run reached M3";
    EXPECT_EQ(mutationsOutsideTheProbes(f.backend->journal(), f), std::vector<String>{});
    EXPECT_EQ(objectsOf(f), before);
}

/// GC of this build refuses a run whose rows break the run's row rules, here two condemned rows for one blob. The run
/// passes its checksum, so only those rules catch it, and the migration stops at M3 with the pool as it was.
TEST(CASMigratePool, RunWithADuplicateCondemnedRowIsRefusedBeforeAnyWrite)
{
    Fixture f(3);
    f.body(1);
    const RetiredEntry row = f.row(1, /*pending*/ true);
    f.rows({row, row});
    const auto before = objectsOf(f);
    f.backend->clearJournal();

    std::vector<String> lines;
    const String message = messageOf(DB::ErrorCodes::CORRUPTED_DATA, [&] { f.run({}, &lines); });

    EXPECT_NE(message.find("duplicate sentinel row for one blob"), String::npos) << message;
    EXPECT_NE(message.find("M3: GC of this build refuses the source-edge run"), String::npos) << message;
    EXPECT_TRUE(contains(lines, "M2:")) << "positive control: the run reached M3";
    EXPECT_EQ(mutationsOutsideTheProbes(f.backend->journal(), f), std::vector<String>{});
    EXPECT_EQ(objectsOf(f), before);
}

namespace
{

SourceEdgeRecord condemnedRecord(const RetiredEntry & row)
{
    return SourceEdgeRecord{.ref = row.ref, .source_id = DB::UInt128{0}, .marker = RunMarker::Condemned,
                            .delete_pending = row.delete_pending, .token = row.token, .size = row.size,
                            .condemn_round = row.condemn_round};
}

/// The run stops at M3, before any intent and before the floor, with the pool as it was.
void expectInheritedRunRefused(Fixture & f, std::string_view what)
{
    const auto before = objectsOf(f);
    f.backend->clearJournal();

    std::vector<String> lines;
    const String message = messageOf(DB::ErrorCodes::CORRUPTED_DATA, [&] { f.run({}, &lines); });

    EXPECT_NE(message.find(what), String::npos) << message;
    EXPECT_NE(message.find("M3: GC of this build refuses the source-edge run"), String::npos) << message;
    EXPECT_TRUE(contains(lines, "M2:")) << "positive control: the run reached M3";
    EXPECT_EQ(mutationsOutsideTheProbes(f.backend->journal(), f), std::vector<String>{});
    EXPECT_EQ(objectsOf(f), before);
}

}

/// Blob 2 belongs to shard 0 of 2, so its live edge in the run of shard 1 is invisible to the shard that decides it.
TEST(CASMigratePool, MisplacedLiveEdgeIsRefusedBeforeAnyWrite)
{
    Fixture f(3, /*gc_shards*/ 2);
    f.editGcState([](GcState & state) { state.gc_shards = 2; });
    f.body(1);
    f.rows({});
    injectSourceEdgeRun(*f.backend, f.layout, /*shard*/ 1,
        {condemnedRecord(f.row(1, /*pending*/ true)),
         SourceEdgeRecord{.ref = blobRefOf(2), .source_id = DB::UInt128{7}, .marker = RunMarker::Edge}});
    expectInheritedRunRefused(f, "belongs to GC shard 0 of 2, not to shard 1");
}

/// `gc/state` is at round 5.
TEST(CASMigratePool, RowStampedAfterTheParentRoundIsRefusedBeforeAnyWrite)
{
    Fixture f(3);
    f.body(1);
    RetiredEntry row = f.row(1, /*pending*/ true);
    row.condemn_round = 6;
    f.rows({row});
    expectInheritedRunRefused(f, "stamped with round 6, after the parent round 5");
}

TEST(CASMigratePool, RowStampedWithTheParentRoundIsMigrated)
{
    Fixture f(3);
    f.body(1);
    RetiredEntry row = f.row(1, /*pending*/ true);
    row.condemn_round = 5;
    f.rows({row});
    const MigratePoolReport report = f.run();
    ASSERT_TRUE(report.completed);
    EXPECT_EQ(report.condemned_rows, 1u);
    EXPECT_TRUE(f.hasIntent(1, f.identity(1)));
}

TEST(CASMigratePool, RunInDescendingOrderIsRefusedBeforeAnyWrite)
{
    Fixture f(3);
    f.body(1);
    f.body(2);
    f.rows({});
    injectSourceEdgeRun(*f.backend, f.layout, /*shard*/ 0,
        {condemnedRecord(f.row(2, /*pending*/ true)), condemnedRecord(f.row(1, /*pending*/ true))});
    expectInheritedRunRefused(f, "sorts before the row above it");
}

/// A `gc/state` whose shard count disagrees with `_pool_meta` is refused by the GC lease, so the run stops at M3.
TEST(CASMigratePool, GcStateWithAnotherShardCountIsRefusedBeforeAnyWrite)
{
    Fixture f(3, /*gc_shards*/ 2);
    f.body(1);
    f.rows({f.row(1, /*pending*/ true)});
    ASSERT_EQ(f.gcState()->gc_shards, 1u);
    const auto before = objectsOf(f);
    f.backend->clearJournal();

    std::vector<String> lines;
    const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run({}, &lines); });

    EXPECT_NE(message.find("M3: gc/state names 1 GC shard(s), but _pool_meta names 2"), String::npos) << message;
    EXPECT_TRUE(contains(lines, "M2:")) << "positive control: the run reached M3";
    EXPECT_EQ(mutationsOutsideTheProbes(f.backend->journal(), f), std::vector<String>{});
    EXPECT_EQ(objectsOf(f), before);
}

/// `skip_access_check` skips the capability battery, not the rights on `blob-intents/` or the token form: the
/// servers need both once the floor is 4.
TEST(CASMigratePool, SkipAccessCheckStillProbesTheIntentPrefixAndTheTokenForm)
{
    {
        SCOPED_TRACE("no write right on blob-intents/");
        Fixture f(3);
        f.skip_access_check = true;
        f.body(1);
        f.condemnedMarker(1);
        f.backend->refuseWritesUnder(f.layout.blobIntentsPrefix());
        f.backend->clearJournal();

        const String message = messageOf(DB::ErrorCodes::READONLY, [&] { f.run(); });
        EXPECT_NE(message.find("grant PUT with If-None-Match on 'p/blob-intents/'"), String::npos) << message;
        const auto journal = f.backend->journal();
        EXPECT_TRUE(writeIndices(journal, f.prefix + "/_probe/").empty()) << "positive control: the battery was skipped";
        EXPECT_EQ(mutationsOutsideTheProbes(journal, f), std::vector<String>{});
    }
    {
        SCOPED_TRACE("a token with no canonical form");
        Fixture f(3);
        f.skip_access_check = true;
        f.body(1);
        f.condemnedMarker(1);
        f.backend->weakHeadsFor(f.layout.poolMetaKey());
        f.backend->clearJournal();

        const String message = messageOf(DB::ErrorCodes::NOT_IMPLEMENTED, [&] { f.run(); });
        EXPECT_NE(message.find("the store returns a token that cannot serve as a blob identity"), String::npos) << message;
        EXPECT_EQ(mutationsOutsideTheProbes(f.backend->journal(), f), std::vector<String>{});
    }
}

/// A dry run checks the token form and the read rights on `blob-intents/` whatever `skip_access_check` says, and writes
/// nothing.
TEST(CASMigratePool, DryRunChecksTheTokenFormAndReadRightsUnderSkipAccessCheck)
{
    MigratePoolOptions options;
    options.dry_run = true;
    {
        Fixture f(3);
        f.skip_access_check = true;
        f.body(1);
        f.backend->clearJournal();
        f.run(options);
        const auto journal = f.backend->journal();
        EXPECT_FALSE(anyMutation(journal));
        EXPECT_TRUE(std::any_of(journal.begin(), journal.end(), [&](const JournalEntry & e)
        {
            return !e.keys.empty() && e.keys.front().starts_with(f.layout.blobIntentsProbePrefix());
        })) << "the dry run checks the read rights on blob-intents/";
    }
    {
        Fixture f(3);
        f.skip_access_check = true;
        f.body(1);
        f.backend->weakHeadsFor(f.layout.poolMetaKey());
        f.backend->clearJournal();
        const String message = messageOf(DB::ErrorCodes::NOT_IMPLEMENTED, [&] { f.run(options); });
        EXPECT_NE(message.find("the store returns a token that cannot serve as a blob identity"), String::npos) << message;
        EXPECT_FALSE(anyMutation(f.backend->journal()));
    }
}

/// The default grace covers the disk's request window when that is longer than 120 s; an explicit grace is used as is.
TEST(CASMigratePool, DefaultGraceCoversALongerRequestWindow)
{
    const auto graceLine = [](std::optional<uint64_t> window_ms, std::optional<uint64_t> grace_ms)
    {
        Fixture f(3);
        f.request_window_ms = window_ms;
        MigratePoolOptions options;
        options.grace_ms = grace_ms;
        std::vector<String> lines;
        f.run(options, &lines);
        for (const String & line : lines)
            if (line.starts_with("M2: waiting "))
                return line;
        return String();
    };
    EXPECT_NE(graceLine(std::nullopt, std::nullopt).find("waiting 120000 ms"), String::npos);
    EXPECT_NE(graceLine(300'000, std::nullopt).find("waiting 300000 ms"), String::npos);
    EXPECT_NE(graceLine(300'000, 5'000).find("waiting 5000 ms"), String::npos);
}

TEST(CASMigratePool, UndecodableSlotIsRefused)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    {
        OperationForTest op(*f.backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(f.layout.mountKey("srv-bad"), "not a lease", Retry::standard())));
    }
    f.backend->clearJournal();

    const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run(); });
    EXPECT_NE(message.find("mount slot of server root 'srv-bad' cannot be decoded"), String::npos) << message;
    const auto journal = f.backend->journal();
    EXPECT_TRUE(intentWriteIndices(journal, f.layout).empty());
    EXPECT_EQ(writesTo(journal, f.layout.gcStateKey()), 0u);
    EXPECT_EQ(writesTo(journal, f.layout.poolMetaKey()), 0u);
}

TEST(CASMigratePool, RestartDoesNotRepeatTheRaiseOrItsCount)
{
    Fixture f(1);
    f.body(1);
    f.condemnedMarker(1);
    f.rows({}, G_WRITE_FLOOR);
    f.editGcState([](GcState & state) { state.lease = GcLease{.owner = kOldLeader, .seq = 3}; });
    f.backend->beforeHead(f.layout.blobKey(blobRefOf(1)), [&] { f.editGcState([](GcState & state) { state.round = 8; }); });
    f.backend->clearJournal();

    std::vector<String> lines;
    const MigratePoolReport report = f.run({}, &lines);
    ASSERT_TRUE(report.completed);
    EXPECT_TRUE(contains(lines, "restart")) << "positive control: the run restarted";
    EXPECT_EQ(std::count_if(lines.begin(), lines.end(), [](const String & l) { return l.find("reader floor raised to 3") != String::npos; }), 1);
    EXPECT_EQ(writeIndices(f.backend->journal(), f.layout.poolMetaKey()).size(), 2u) << "the raise to 3, then to 4";
    EXPECT_EQ(report.control_objects_written, 2u) << "one raise and one gc/state write were sent and landed";
    EXPECT_TRUE(report.floor_raised);
}

TEST(CASMigratePool, RestartCapStopsARunThatKeepsConflicting)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    f.rows({});
    size_t late_commits = 0;
    std::function<void()> late_commit = [&]
    {
        ++late_commits;
        f.editGcState([](GcState & state) { ++state.round; });
        f.backend->beforeHead(f.layout.blobKey(blobRefOf(1)), late_commit);
    };
    f.backend->beforeHead(f.layout.blobKey(blobRefOf(1)), late_commit);

    const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run(); });
    EXPECT_NE(message.find("gc/state changed under the migration 3 times"), String::npos) << message;
    EXPECT_EQ(late_commits, 3u);
    EXPECT_EQ(f.gcState()->header_generation, G_REF_CHAIN);
    EXPECT_EQ(f.poolMeta().min_reader_generation, 3u);
    EXPECT_EQ(f.markers().size(), 1u);
}

TEST(CASMigratePool, GraceAboveTheLimitIsRefused)
{
    EXPECT_EQ(migrateGraceMs(120), 120'000u);
    EXPECT_EQ(migrateGraceMs(kMaxMigrateGraceSeconds), kMaxMigrateGraceSeconds * 1000);
    messageOf(DB::ErrorCodes::BAD_ARGUMENTS, [] { migrateGraceMs(kMaxMigrateGraceSeconds + 1); });
    /// Times 1000 this wraps to 384 ms.
    messageOf(DB::ErrorCodes::BAD_ARGUMENTS, [] { migrateGraceMs(18'446'744'073'709'552ULL); });
}

TEST(CASMigratePool, OlderGcThatCreatesGcStateIsNamed)
{
    Fixture f(3);
    f.body(1);
    f.condemnedMarker(1);
    ASSERT_FALSE(f.gcState().has_value());
    bool created = false;
    f.on_sleep = [&](uint64_t)
    {
        if (!created)
        {
            created = true;
            f.editGcState([](GcState & state) { state.lease = GcLease{.owner = kOldLeader, .seq = 1}; });
        }
    };

    const String message = messageOf(DB::ErrorCodes::INVALID_STATE, [&] { f.run(); });
    EXPECT_NE(message.find("GC lease of owner " + u128ToHex(kOldLeader) + " moved"), String::npos) << message;
    EXPECT_EQ(f.poolMeta().min_reader_generation, 3u);
}

/// The watch reports while it waits, and a line precedes each write that cannot be undone.
TEST(CASMigratePool, WatchProgressAndPointsOfNoReturnAreAnnounced)
{
    Fixture f(1);
    f.body(1);
    f.condemnedMarker(1);
    f.rows({}, G_WRITE_FLOOR);
    f.slot("srv-silent", liveLease());
    f.backend->clearJournal();

    std::vector<String> lines;
    ASSERT_TRUE(f.run({}, &lines).completed);
    EXPECT_TRUE(contains(lines, "server root 'srv-silent' holds a mount lease"));
    EXPECT_TRUE(contains(lines, "still watched"));
    EXPECT_TRUE(contains(lines, "server root 'srv-silent' fenced"));
    EXPECT_TRUE(contains(lines, "waiting"));

    const auto journal = f.backend->journal();
    const auto mark_index = [&](std::string_view text)
    {
        for (size_t i = 0; i < journal.size(); ++i)
            if (journal[i].verb == JournalVerb::Mark && journal[i].detail.find(text) != String::npos)
                return i;
        return journal.size();
    };
    const auto meta_writes = writeIndices(journal, f.layout.poolMetaKey());
    const auto state_writes = writeIndices(journal, f.layout.gcStateKey());
    ASSERT_EQ(meta_writes.size(), 2u);
    ASSERT_EQ(state_writes.size(), 1u);
    const size_t before_raise_3 = mark_index("raising the reader floor to 3");
    const size_t before_state = mark_index("rewriting gc/state");
    const size_t before_raise_4 = mark_index("raising the reader floor to 4");
    ASSERT_LT(before_raise_3, journal.size());
    ASSERT_LT(before_state, journal.size());
    ASSERT_LT(before_raise_4, journal.size());
    EXPECT_LT(before_raise_3, meta_writes[0]);
    EXPECT_LT(before_state, state_writes[0]);
    EXPECT_GT(before_state, meta_writes[0]);
    EXPECT_LT(before_raise_4, meta_writes[1]);
    EXPECT_GT(before_raise_4, state_writes[0]);
}

TEST(CASMigratePool, FullRunUnderTheETagAndGenerationDialects)
{
    for (const Dialect dialect : {Dialect::ETag, Dialect::Generation})
    {
        SCOPED_TRACE(dialectWord(dialect));
        Fixture f(3);
        f.backend->setDialect(dialect);
        seedMixedPool(f);
        const MigratePoolReport report = f.run();
        ASSERT_TRUE(report.completed);
        EXPECT_EQ(f.poolMeta().min_reader_generation, G_BLOB_INTENTS);
        EXPECT_EQ(f.poolMeta().token_dialect, dialect);
        EXPECT_EQ(f.gcState()->header_generation, G_BLOB_INTENTS);
        EXPECT_TRUE(f.markers().empty());
        EXPECT_TRUE(f.hasIntent(1, f.identity(1))) << "condemned marker";
        EXPECT_TRUE(f.hasIntent(5, f.identity(5))) << "pending row";
        ASSERT_FALSE(f.intents().empty());
        for (const String & key : f.intents())
        {
            const auto parsed = f.layout.parseBlobIntentKey(key);
            ASSERT_TRUE(parsed) << key;
            EXPECT_EQ(parsed->identity.dialect, dialect) << key;
        }
    }
}

namespace
{

/// A server of this build over the migrated pool.
PoolPtr openServer(Fixture & f)
{
    return Pool::open(f.backend, PoolConfig{.pool_prefix = f.prefix, .server_root_id = "server"});
}

/// A GC of this build: rounds until it holds the lease and has run one round under it.
RoundReport roundAfterMigration(const PoolPtr & server, Gc & gc)
{
    RoundReport report;
    for (int i = 0; i < 6 && !report.acquired_lease; ++i)
        report = gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative);
    server->renewWatermarkOnce();
    return report;
}

/// Writes `payload` through a part of this build and reports what the writer did with the blob.
BlobUploadResult writeThroughServer(const PoolPtr & server, const String & payload, const String & part)
{
    const RootNamespace ns{"srv/migrated"};
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/" + part;
    auto build = server->beginPartWrite(info);
    const ManifestId id = build->stageManifest({blobEntryFor("col.bin", u128Of(payload), payload.size())});
    build->precommitAdd(ns, part, id);
    return build->uploadBlobDetached(BlobUploadRequest{idOf(payload), BlobSource::fromString(payload), payload.size()});
}

uint64_t confirmedExisting()
{
    return static_cast<uint64_t>(ProfileEvents::global_counters[ProfileEvents::CASGCIntentConfirmedExisting]);
}

}

TEST(CASMigratePool, OnlyThisCommandReachesOpenForMigration)
{
    Fixture f(G_REF_CHAIN);
    f.backend->clearJournal();
    const String refusal = "stop every server of the pool, then run `" + String(kMigratePoolCommandLine) + "`";
    EXPECT_NE(messageOf(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { openServer(f); }).find(refusal), String::npos);
    PoolConfig read_only{.pool_prefix = f.prefix, .server_root_id = "observer"};
    read_only.read_only = true;
    EXPECT_NE(messageOf(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { Pool::open(f.backend, read_only); }).find(refusal),
              String::npos);
    for (const JournalEntry & entry : f.backend->journal())
        for (const String & key : entry.keys)
            EXPECT_EQ(key.find("/blobs/"), String::npos) << "a refused open sent a request on " << key;

    /// The tool's own open reads the same pool.
    EXPECT_EQ(f.open()->poolMeta().min_reader_generation, G_REF_CHAIN);
}

TEST(CASGcIntents, InheritedCandidateConfirmsOnTheToolsIntent)
{
    Fixture f(G_REF_CHAIN);
    f.body(1);
    f.condemnedMarker(1);
    f.rows({f.row(1, /*pending*/ false)});
    ASSERT_TRUE(f.run().completed);
    ASSERT_TRUE(f.hasIntent(1, f.identity(1))) << "positive control: the tool created the intent";

    auto server = openServer(f);
    Gc gc(server, hexToU128("000000000000000000000000000000c1"));
    const uint64_t before = confirmedExisting();
    ASSERT_TRUE(roundAfterMigration(server, gc).acquired_lease);

    const auto rows = currentRetiredSet(*f.backend, f.layout, 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].delete_pending) << "the inherited candidate did not confirm on the tool's intent";
    EXPECT_EQ(confirmedExisting() - before, 1u);
    EXPECT_TRUE(f.current(1).has_value()) << "nothing is deleted in the confirming round";
}

/// The tool creates an absent `gc/state` with the never-held lease; a manual round takes it at once.
TEST(CASGcIntents, ManualRoundRightAfterAMigrationTakesTheNeverHeldLease)
{
    Fixture f(G_REF_CHAIN);
    f.body(1);
    f.condemnedMarker(1);
    ASSERT_FALSE(f.gcState().has_value());
    ASSERT_TRUE(f.run().completed);
    ASSERT_EQ(f.gcState()->lease.owner, DB::UInt128(0)) << "positive control: the tool left the never-held lease";

    auto server = openServer(f);
    const DB::UInt128 first = hexToU128("000000000000000000000000000000d1");
    Gc manual(server, first);
    EXPECT_TRUE(manual.runRegularRound({}, /*allow_steal*/ false, UniversePolicy::Authoritative).acquired_lease);
    EXPECT_EQ(f.gcState()->lease.owner, first);

    /// A held lease is not taken by another manual round, nor by a loop's first tick.
    Gc other(server, hexToU128("000000000000000000000000000000d2"));
    EXPECT_FALSE(other.runRegularRound({}, /*allow_steal*/ false, UniversePolicy::Authoritative).acquired_lease);
    EXPECT_FALSE(other.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative).acquired_lease);
    EXPECT_EQ(f.gcState()->lease.owner, first);
}

TEST(CASGcIntents, InheritedPendingRowIsDeletedByTheFirstRoundAtZero)
{
    Fixture f(G_REF_CHAIN);
    f.body(1);
    f.condemnedMarker(1);
    f.rows({f.row(1, /*pending*/ true)});
    ASSERT_TRUE(f.run().completed);
    ASSERT_TRUE(f.hasIntent(1, f.identity(1)));

    auto server = openServer(f);
    Gc gc(server, hexToU128("000000000000000000000000000000c2"));
    const RoundReport report = roundAfterMigration(server, gc);
    ASSERT_TRUE(report.acquired_lease);
    EXPECT_EQ(report.bodies_deleted, 1u);
    EXPECT_FALSE(f.current(1).has_value()) << "the inherited pending row's body was not deleted";
}

/// With the clean markers trusted a clean-marked body is adopted with no upload; by default it carries an intent, so
/// its first reuse publishes it once more.
TEST(CASGcIntents, CleanMarkedBodyIsAdoptedOnlyWhenCleanMarkersAreTrusted)
{
    for (const bool trust : {true, false})
    {
        SCOPED_TRACE(trust);
        Fixture f(G_REF_CHAIN);
        const String payload = "clean-marked-before-migration";
        writeBlobRaw(*f.backend, f.layout, payload, 256, DB::UInt128{});
        writeMetaClean(*f.backend, f.layout, u128Of(payload), payload.size());
        f.rows({});
        MigratePoolOptions options;
        options.trust_clean_markers = trust;
        ASSERT_TRUE(f.run(options).completed);

        auto server = openServer(f);
        f.backend->resetCounts();
        const BlobUploadResult result = writeThroughServer(server, payload, "adopt");
        if (trust)
        {
            EXPECT_EQ(result.diagnostics.action, BlobMaterializationAction::Observed);
            EXPECT_EQ(f.backend->publishTotal(), 0u);
        }
        else
        {
            EXPECT_EQ(result.diagnostics.reason, BlobPublicationReason::IntentPresent);
            EXPECT_EQ(f.backend->publishTotal(), 1u);
        }
    }
}

TEST(CASGcIntents, LateOldDeleteMissesAfterARepublish)
{
    Fixture f(G_REF_CHAIN);
    const String payload = "condemned-before-migration";
    const BlobRef ref = writeBlobRaw(*f.backend, f.layout, payload, 256, DB::UInt128{});
    writeMetaClean(*f.backend, f.layout, u128Of(payload), payload.size());
    condemnMeta(*f.backend, f.layout, u128Of(payload), 3);
    f.rows({});
    Etag condemned_token = [&]
    {
        OperationForTest op(*f.backend);
        return (*op).head(f.layout.blobKey(ref), Retry::standard())->etag;
    }();
    ASSERT_TRUE(f.run().completed);

    auto server = openServer(f);
    const BlobUploadResult result = writeThroughServer(server, payload, "republish");
    EXPECT_EQ(result.diagnostics.reason, BlobPublicationReason::IntentPresent);

    /// The older build's delete of the condemned incarnation lands after the migration.
    OperationForTest op(*f.backend);
    EXPECT_EQ((*op).remove(f.layout.blobKey(ref), condemned_token, Retry::standard()), Removal::Mismatch);
    EXPECT_TRUE((*op).head(f.layout.blobKey(ref), Retry::standard()).has_value());
}

TEST(CASGcIntents, FloorOnePoolMigratesThenServes)
{
    Fixture f(G_WRITE_FLOOR);
    const String payload = "floor-one-body";
    writeBlobRaw(*f.backend, f.layout, payload, 256, DB::UInt128{});
    writeMetaClean(*f.backend, f.layout, u128Of(payload), payload.size());
    f.rows({}, G_WRITE_FLOOR);
    ASSERT_TRUE(f.run().completed);

    auto server = openServer(f);
    EXPECT_EQ(server->poolMeta().min_reader_generation, G_BLOB_INTENTS);
    EXPECT_EQ(server->poolMeta().token_dialect, Dialect::Emulated);
    /// The forced all-bodies option put every listed body under an intent: the first reuse uploads once.
    EXPECT_EQ(writeThroughServer(server, payload, "first").diagnostics.reason, BlobPublicationReason::IntentPresent);
    EXPECT_EQ(writeThroughServer(server, payload, "second").diagnostics.action, BlobMaterializationAction::Observed);

    Gc gc(server, hexToU128("000000000000000000000000000000c3"));
    EXPECT_TRUE(roundAfterMigration(server, gc).acquired_lease);
}

TEST(CASGcIntents, RebuildOnAMigratedPoolKeepsHeader4AndAdoptsEveryIntent)
{
    Fixture f(G_REF_CHAIN);
    f.body(1);
    f.condemnedMarker(1);
    f.body(2);
    f.rows({f.row(2, /*pending*/ true)});
    ASSERT_TRUE(f.run().completed);
    const std::set<String> intents = f.intents();
    ASSERT_EQ(intents.size(), 2u) << "positive control: the tool created both intents";

    PoolConfig config{.pool_prefix = f.prefix, .server_root_id = "server"};
    config.gc_fold_max_defer_rounds = 0;
    auto server = Pool::open(f.backend, std::move(config));
    Gc gc(server, hexToU128("000000000000000000000000000000c4"));
    /// The rebuild needs the lease; the round that takes it also settles the inherited rows.
    ASSERT_TRUE(roundAfterMigration(server, gc).acquired_lease);
    const RebuildReport rebuilt = gc.rebuildBaseline(/*force*/ true);
    ASSERT_TRUE(rebuilt.refusal.empty()) << rebuilt.refusal;
    ASSERT_EQ(f.gcState()->header_generation, G_BLOB_INTENTS);
    EXPECT_TRUE(f.gcState()->intent_sweep_cursor.empty());
    EXPECT_TRUE(currentRetiredSet(*f.backend, f.layout, 0).empty()) << "the rebuild keeps no row";

    for (int i = 0; i < 6; ++i)
    {
        gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative);
        server->renewWatermarkOnce();
    }
    EXPECT_TRUE(f.intents().empty()) << "an intent was never adopted and removed";
    EXPECT_FALSE(f.current(1).has_value());
    EXPECT_FALSE(f.current(2).has_value());
}

TEST(CASGcIntents, UncanonicalRowTokenIsMovedByACanonicalBody)
{
    Fixture f(G_REF_CHAIN);
    f.body(1);
    /// An inherited pending row whose own token names no incarnation.
    RetiredEntry inherited = f.row(1, /*pending*/ true);
    inherited.token = PersistedEtag{"emulated", "W/\"x\""};
    ASSERT_FALSE(inherited.token.canonical().has_value());
    f.rows({inherited});
    ASSERT_TRUE(f.run().completed);
    ASSERT_TRUE(f.hasIntent(1, f.identity(1))) << "positive control: the tool gave the current body an intent";

    auto server = openServer(f);
    Gc gc(server, hexToU128("000000000000000000000000000000c5"));
    f.backend->clearJournal();
    ASSERT_TRUE(roundAfterMigration(server, gc).acquired_lease);

    const auto rows = currentRetiredSet(*f.backend, f.layout, 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].delete_pending && !rows[0].standing);
    EXPECT_TRUE(sameIncarnation(rows[0].token, *f.current(1))) << "the row did not move to the canonical body";
    EXPECT_TRUE(f.current(1).has_value()) << "the moving round deletes no body";
    for (const JournalEntry & e : f.backend->journal())
        for (const String & key : e.keys)
            EXPECT_FALSE((e.verb == JournalVerb::Remove || e.verb == JournalVerb::RemoveMany)
                         && key.find("/blob-intents/") != String::npos) << "the old token names no intent";
}

namespace
{

/// Jobs 0 to 2 hold every thread of the run, or wait queued behind it, until the submission of job 3 is made to fail.
struct SubmissionFailsWhileJobsAreQueued
{
    std::latch & first_started;
    std::latch & release;
    std::atomic<size_t> & finished;

    void operator()(size_t i) const
    {
        if (i == 0)
            first_started.count_down();
        release.wait();
        finished.fetch_add(1);
    }

    void beforeSubmit(size_t i) const
    {
        if (i != 3)
            return;
        first_started.wait();
        CannotAllocateThreadFaultInjector::setFaultProbability(1.0);
        release.count_down();
    }
};

}

/// A submission that throws leaves job 2 queued: the run still joins it before the failure leaves, and under ASan the
/// job's read of the run's state shows whether that state outlived the throw.
TEST(CASBoundedRun, FailedSubmissionJoinsTheQueuedJobs)
{
    SCOPE_EXIT({ CannotAllocateThreadFaultInjector::setFaultProbability(0); });
    for (size_t attempt = 0; attempt < 200; ++attempt)
    {
        std::latch first_started(1);
        std::latch release(1);
        std::atomic<size_t> finished{0};
        int code = 0;
        try
        {
            runBounded(2, 4, SubmissionFailsWhileJobsAreQueued{first_started, release, finished});
        }
        catch (const DB::Exception & e)
        {
            code = e.code();
        }
        CannotAllocateThreadFaultInjector::setFaultProbability(0);
        ASSERT_EQ(code, DB::ErrorCodes::CANNOT_SCHEDULE_TASK) << attempt;
        ASSERT_EQ(finished.load(), 3u) << "a submitted job was not joined before the failure left the run";
    }
}
