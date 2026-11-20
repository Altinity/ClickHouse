#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasMigratePool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasProbe.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRecordStreamFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobIntents.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasBoundedRun.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Common/Exception.h>
#include <Common/thread_local_rng.h>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <vector>

namespace DB
{
namespace ErrorCodes
{
    extern const int ABORTED;
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int INVALID_STATE;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

namespace
{

/// Three: a late commit of a stopped leader and a concurrent run of the tool can each move `gc/state` once, plus one
/// spare. The cap only keeps a GC that never stops from restarting the tool forever; a rerun counts again.
constexpr size_t kMaxRunsAfterGcStateConflict = 3;

/// The recovery of a lost `gc/state` by a build below generation 4, as an operator types it.
constexpr std::string_view kGcRebuildCommandLine
    = "clickhouse-disks --config-file READ_ONLY_CONFIG_FILE --disk DISK_NAME --query 'cas-gc-rebuild'";

constexpr uint64_t kDefaultGraceMs = 120'000;

/// Ten seconds on the tool's clock: a long step reports before it looks hung, and seldom enough to stay readable.
constexpr uint64_t kProgressEveryMs = 10'000;

/// One `LIST` page: what M4, M5 and M9 send and hold at once, so the tool's memory does not grow with the pool.
constexpr size_t kBatchKeys = CasOperation::kCasListPageKeys;

/// Where the run is when a member is seen moving: before the floor the run stops and a rerun redoes it; right after
/// it nothing can be undone, so the operator must stop the member if it is an older build.
enum class Window : uint8_t
{
    BeforeFloor,
    AfterFloor,
};

String whatToDo(Window window)
{
    switch (window)
    {
        case Window::BeforeFloor:
            return "stop that server and run the tool again";
        case Window::AfterFloor:
            return "the pool is already migrated and the tool cannot undo it: if that server runs an older build, stop it now";
    }
    UNREACHABLE();
}

/// The lease pair of `gc/state` and the pair of `gc/hb`. Either one moving between reads is a live GC. An absent
/// `gc/state` reads as the never-held lease, which is also what M6 creates.
struct GcPairs
{
    GcLease lease;
    std::optional<GcHeartbeat> heartbeat;
};

/// The token each present mount slot had when last read.
using SlotTokens = std::map<String, Etag>;

struct InheritedRow
{
    BlobRef ref;
    PersistedEtag token;
    bool pending = false;
};

/// What M4 found: only the markers that are not `Clean` are kept, the rest is counted.
struct MarkerScan
{
    std::set<BlobRef> not_clean;
    uint64_t markers = 0;
    uint64_t bodies = 0;
    uint64_t elapsed_ms = 0;
};

[[noreturn]] void throwSlotChanged(const String & srid, Window window = Window::BeforeFloor)
{
    throw Exception(ErrorCodes::INVALID_STATE, "mount slot of server root '{}' changed during the migration; {}", srid, whatToDo(window));
}

[[noreturn]] void throwGcMoved(UInt128 owner, Window window = Window::BeforeFloor)
{
    throw Exception(ErrorCodes::INVALID_STATE, "GC lease of owner {} moved during the migration; {}", u128ToHex(owner), whatToDo(window));
}

/// Whether a legacy marker says `Clean`. Only bytes that do not decode count as not `Clean`, since they may hide a
/// condemnation; a marker that needs a newer reader, or a failed read, stops the run before any intent.
bool markerIsClean(const String & key, const String & bytes)
{
    try
    {
        return decodeBlobMeta(bytes).state == MetaState::Clean;
    }
    catch (Exception & e)
    {
        if (e.code() == ErrorCodes::CORRUPTED_DATA)
            return false;
        e.addMessage("reading the legacy marker `{}` at M4; the run stops before it creates an intent", key);
        throw;
    }
}

class Migration
{
public:
    Migration(Pool & pool_, const MigratePoolOptions & options_, const std::function<void(std::string_view)> & progress_)
        : pool(pool_)
        , options(options_)
        , progress(progress_)
        , layout(pool.layout())
        , op(pool.openRequests().admit())
        , concurrency(std::max<size_t>(1, options.concurrency ? options.concurrency : pool.poolConfig().gc_io_concurrency))
        , grace_ms(options.grace_ms.value_or(std::max(kDefaultGraceMs, pool.poolConfig().cas_request_budget.request_window_ms)))
    {
    }

    MigratePoolReport run()
    {
        if (layout.poolPrefix().size() > Layout::maxPoolPrefixBytesForBlobIntents())
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "pool prefix of {} bytes leaves no room for blob intent keys; the limit is {}",
                layout.poolPrefix().size(), Layout::maxPoolPrefixBytesForBlobIntents());

        const PoolMeta meta = readPoolMeta();
        report.floor_read = meta.min_reader_generation;
        if (meta.min_reader_generation >= G_BLOB_INTENTS)
        {
            requireGeneration4Pool(meta, op.backendDialect());
            report.migrated = true;
            say(fmt::format("M1: the pool is at reader generation {} with token dialect {}: it is migrated; only markers are left "
                "to remove, and servers may run meanwhile", meta.min_reader_generation, dialectWord(op.backendDialect())));
            if (options.dry_run)
                return countMarkersOfAMigratedPool();
            if (options.keep_markers)
            {
                say("M9: skipped: --keep-markers");
                return report;
            }
            removeMarkers();
            report.completed = true;
            return report;
        }

        /// A clean marker proves nothing about a pool whose history the tool cannot see, so by default every body
        /// gets an intent. Below generation 3 a marker proves nothing about the GC that ran: then always.
        bool all_bodies = !options.trust_clean_markers;
        if (meta.min_reader_generation < G_REF_CHAIN)
        {
            if (options.trust_clean_markers)
                say(fmt::format("M1: --trust-clean-markers is ignored: reader generation {} is below {}, so a marker proves "
                    "nothing; every body gets an intent", meta.min_reader_generation, G_REF_CHAIN));
            all_bodies = true;
        }
        say(all_bodies ? String("M1: every body gets an intent")
                       : String("M1: --trust-clean-markers: only a body a marker or a GC row condemns gets an intent"));

        if (options.dry_run)
            return dryRun(all_bodies, meta);

        runProbe();
        say(fmt::format("M1: pool at reader generation {}, token dialect {}", meta.min_reader_generation,
            dialectWord(op.backendDialect())));
        stepDone(1);

        for (size_t attempt = 1;; ++attempt)
        {
            const auto [slots, pairs] = waitForNoLiveMember();
            stepDone(2);

            const std::optional<Object> stored_state = op.read(layout.gcStateKey(), Retry::standard());
            const std::optional<GcState> state = stored_state ? std::optional<GcState>(decodeGcState(stored_state->bytes)) : std::nullopt;
            const std::vector<InheritedRow> rows = readCondemnedRows(state, meta);
            stepDone(3);

            const MarkerScan markers = readMarkers();
            stepDone(4);

            createIntents(rows, markers, all_bodies);
            stepDone(5);

            std::optional<Etag> state_token = stored_state ? std::optional<Etag>(stored_state->etag) : std::nullopt;
            /// The run that wrote header 4 had finished M5 first.
            if (state && state->header_generation >= G_BLOB_INTENTS)
            {
                say("M6: gc/state is already at generation 4; skipped");
            }
            else
            {
                /// After M5: the raise erases the floor that forced an intent for every body. The floor is read again
                /// because an earlier attempt of this run, or another run, may have raised it already.
                if (readPoolMeta().min_reader_generation < G_REF_CHAIN)
                {
                    say(fmt::format("M6: raising the reader floor to {}; after this write a 26.6 build can no longer open this pool",
                        G_REF_CHAIN));
                    pool.raiseReaderFloorToRefChain();
                    ++report.control_objects_written;
                    say(fmt::format("M6: reader floor raised to {}", G_REF_CHAIN));
                }
                say("M6: rewriting gc/state at generation 4; after this write a GC of an older build is refused");
                state_token = rewriteGcState(stored_state, state, meta);
                if (!state_token)
                {
                    if (attempt >= kMaxRunsAfterGcStateConflict)
                        throw Exception(ErrorCodes::INVALID_STATE,
                            "gc/state changed under the migration {} times: a GC of an older build still runs; stop it and run "
                            "the tool again", attempt);
                    say("M6: gc/state changed since M3 read it, a late commit of a stopped GC; restart from M2");
                    continue;
                }
                ++report.control_objects_written;
                say("M6: gc/state rewritten at generation 4: a GC of an older build is refused from now on");
            }
            stepDone(6);

            recheck(slots, pairs, state_token, Window::BeforeFloor);
            report.slots_reread = slots.size();
            say("M7: no mount slot, GC lease or gc/state moved");
            stepDone(7);

            say(fmt::format("M8: raising the reader floor to {} with token dialect {}; after this write an older build can no "
                "longer open this pool", G_BLOB_INTENTS, dialectWord(op.backendDialect())));
            report.floor_raised = PoolMeta::raiseToGeneration4(op, layout);
            report.migrated = true;
            say(fmt::format("M8: reader floor at {} with token dialect {}", G_BLOB_INTENTS, dialectWord(op.backendDialect())));
            /// A member that read `_pool_meta` before the raise and claimed its slot after M7 shows here.
            recheck(slots, pairs, state_token, Window::AfterFloor);
            say("M8: the pool is migrated: servers of this build may start now; marker removal needs no downtime");
            stepDone(8);

            if (options.keep_markers)
            {
                say("M9: skipped: --keep-markers; a rerun or `cas-fsck --fix legacy-markers` removes the markers");
                return report;
            }
            removeMarkers();
            report.completed = true;
            stepDone(9);
            noteMembersAfterMarkers(slots, pairs, state_token);
            return report;
        }
    }

private:
    /// Counts what a long step did and, on the tool's clock, reports every `kProgressEveryMs` with an estimate.
    class Meter
    {
    public:
        Meter(Migration & migration_, String step_, String what_, uint64_t expected_)
            : migration(migration_), step(std::move(step_)), what(std::move(what_)), expected(expected_)
            , started_ms(migration.pool.bootMsNow()), last_ms(started_ms)
        {
        }

        void add(uint64_t n)
        {
            done += n;
            const uint64_t now = migration.pool.bootMsNow();
            if (now < last_ms + kProgressEveryMs)
                return;
            last_ms = now;
            migration.say(fmt::format("{}: {} {}{}", step, done, what, estimate(now)));
        }

        uint64_t elapsedMs() const { return migration.pool.bootMsNow() - started_ms; }
        uint64_t count() const { return done; }

    private:
        String estimate(uint64_t now) const
        {
            if (expected <= done || done == 0 || now <= started_ms)
                return "";
            const uint64_t left_ms = (expected - done) * (now - started_ms) / done;
            return fmt::format(" of about {}, about {} s left", expected, left_ms / 1000);
        }

        Migration & migration;
        const String step;
        const String what;
        const uint64_t expected;
        const uint64_t started_ms;
        uint64_t last_ms;
        uint64_t done = 0;
    };

    Pool & pool;
    const MigratePoolOptions & options;
    const std::function<void(std::string_view)> & progress;
    const Layout & layout;
    CasOperation op;
    const size_t concurrency;
    const uint64_t grace_ms;
    MigratePoolReport report;

    void say(const String & line)
    {
        if (progress)
            progress(line);
    }

    void stepDone(size_t step)
    {
        if (options.stop_after_step_for_test == step)
            throw Exception(ErrorCodes::ABORTED, "cas-migrate-pool stopped after step M{} for a test", step);
    }

    /// A request of one job of a bounded run: each job has an operation of its own.
    CasOperation jobOperation() { return pool.openRequests().resume(op.generation()); }

    PoolMeta readPoolMeta()
    {
        const std::optional<Object> stored = op.read(layout.poolMetaKey(), Retry::standard());
        if (!stored)
            Pool::throwNoPoolToMigrate(pool.poolConfig());
        return decodePoolMeta(stored->bytes);
    }

    /// The gates and the battery a writable `Pool::open` runs before it writes, then what the servers need after M8.
    void runProbe()
    {
        Backend & backend = pool.openRequests().backendForCapabilityPredicates();
        if (pool.poolConfig().skip_access_check)
        {
            backend.checkSkipAccessCheckSupport();
            backend.checkConditionalWriteSingleAttemptSupport();
        }
        else
        {
            backend.checkPoolPreconditions();
            backend.checkConditionalWriteSingleAttemptSupport();
            const UInt128 probe_uid = (static_cast<UInt128>(thread_local_rng()) << 64) | thread_local_rng();
            runCapabilityProbe(op, layout.poolPrefix() + "/_probe/" + u128ToHex(probe_uid));
            report.probes_run = 1;
        }
        /// Not skipped with `skip_access_check`: the pool cannot go back after M8, and its servers then need both.
        requireCanonicalTokens();
        runIntentPrefixProbe(op, layout, IntentPrefixRights::ReadWrite);
    }

    /// The token form the capability battery checks, read from `_pool_meta`: blob identities exist only in the
    /// canonical spelling, so a store whose tokens have none cannot run the protocol of generation 4.
    void requireCanonicalTokens()
    {
        const std::optional<Meta> meta = op.head(layout.poolMetaKey(), Retry::standard());
        if (!meta)
            Pool::throwNoPoolToMigrate(pool.poolConfig());
        if (!meta->etag.canonical())
            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                "M1: the store returns a token that cannot serve as a blob identity: {}; the pool cannot be migrated to this "
                "store", meta->etag.render());
    }

    MigratePoolReport dryRun(bool all_bodies, const PoolMeta & meta)
    {
        /// A dry run writes nothing, so it checks only the read side of the rights the real run proves.
        requireCanonicalTokens();
        runIntentPrefixProbe(op, layout, IntentPrefixRights::Read);
        say(fmt::format("dry run: the token form is canonical, and GET and LIST on {} are allowed; not checked: the "
            "conditional create, replace and delete rights, which the run checks at M1 before it writes",
            layout.blobIntentsPrefix()));
        const std::vector<NonTerminalMountSlot> held = probeNonTerminalMountSlots(op, layout);
        report.slots_read = listSlotKeys().size();
        for (const NonTerminalMountSlot & slot : held)
            say(fmt::format("M2 (dry run): server root '{}' {}", slot.server_root_id, slot.detail));
        const std::optional<Object> stored_state = op.read(layout.gcStateKey(), Retry::standard());
        const std::vector<InheritedRow> rows
            = readCondemnedRows(stored_state ? std::optional<GcState>(decodeGcState(stored_state->bytes)) : std::nullopt, meta);
        const MarkerScan markers = readMarkers();
        std::set<BlobRef> targets = markers.not_clean;
        for (const InheritedRow & row : rows)
            targets.insert(row.ref);
        say(fmt::format("dry run: {} mount slot(s) not terminal, {} bod(ies) listed, about {} would get an intent; nothing was "
            "written", held.size(), markers.bodies, all_bodies ? std::max<uint64_t>(markers.bodies, targets.size()) : targets.size()));
        return report;
    }

    GcPairs readPairs()
    {
        GcPairs pairs;
        if (const auto state = op.read(layout.gcStateKey(), Retry::standard()))
            pairs.lease = decodeGcState(state->bytes).lease;
        if (const auto heartbeat = op.read(layout.gcHbKey(), Retry::standard()))
            pairs.heartbeat = decodeGcHeartbeat(heartbeat->bytes);
        return pairs;
    }

    /// The owner of the GC that moved the lease or the heartbeat since `seen`, if one did. An older GC that created
    /// an absent `gc/state` is the new owner.
    std::optional<UInt128> gcOwnerThatMoved(const GcPairs & seen)
    {
        const GcPairs now = readPairs();
        if (now.lease.owner != seen.lease.owner)
            return now.lease.owner;
        if (now.lease.seq != seen.lease.seq)
            return seen.lease.owner;
        if (now.heartbeat.has_value() != seen.heartbeat.has_value()
            || (seen.heartbeat && (now.heartbeat->owner != seen.heartbeat->owner || now.heartbeat->hb_seq != seen.heartbeat->hb_seq)))
            return seen.heartbeat ? seen.heartbeat->owner : now.heartbeat->owner;
        return std::nullopt;
    }

    void checkPairs(const GcPairs & seen, Window window)
    {
        if (const auto owner = gcOwnerThatMoved(seen))
            throwGcMoved(*owner, window);
    }

    /// Every mount slot key: the listing, and the slot of every server root the catalog names. A namespace path
    /// does not say where its server root id ends, so each leading run of its segments is a candidate root.
    std::set<String> listSlotKeys()
    {
        std::set<String> keys;
        op.forEachListedKey(layout.serverRootsPrefix(), [&](const ListedKey & listed)
        {
            if (listed.key.ends_with("/mount"))
                keys.insert(listed.key);
            return true;
        }, Retry::standard());

        const CasRefCatalog::Snapshot catalog = CasRefCatalog::read(op, layout);
        std::set<String> roots;
        for (const CatalogEntry & entry : catalog.catalog.entries)
        {
            const String & ns = entry.ns.string();
            for (size_t slash = ns.find('/'); slash != String::npos; slash = ns.find('/', slash + 1))
                roots.insert(ns.substr(0, slash));
            roots.insert(ns);
            if (entry.creator)
                roots.insert(entry.creator->server_root_id);
        }
        for (const String & root : roots)
        {
            const String key = layout.mountKey(root);
            if (!keys.contains(key) && op.head(key, Retry::standard()))
                keys.insert(key);
        }
        return keys;
    }

    String sridOf(const String & key) const
    {
        const String prefix = layout.serverRootsPrefix();
        return key.substr(prefix.size(), key.size() - prefix.size() - std::string_view("/mount").size());
    }

    static MountLease decodeSlot(const String & srid, const Object & object)
    {
        try
        {
            return decodeMountLease(object.bytes);
        }
        catch (const Exception & e)
        {
            throw Exception(ErrorCodes::INVALID_STATE,
                "mount slot of server root '{}' cannot be decoded ({}); stop that server and run the tool again", srid, e.message());
        }
    }

    /// M2: no member renews. A slot that is not terminal is watched on the tool's clock for as long as a dead holder
    /// could still write, then fenced as GC fences a dead mount. Then the grace interval, then everything again.
    std::pair<SlotTokens, GcPairs> waitForNoLiveMember()
    {
        const GcPairs pairs = readPairs();

        struct Watched
        {
            String key;
            Etag etag;
            MountLease body;
            uint64_t first_seen_ms;
            uint64_t threshold_ms;
        };
        SlotTokens tokens;
        std::map<String, Watched> watched;
        const uint64_t observer_lease_ms = static_cast<uint64_t>(pool.poolConfig().mount_lease_ttl_ms.count());
        const std::set<String> slot_keys = listSlotKeys();
        for (const String & key : slot_keys)
        {
            const String srid = sridOf(key);
            const std::optional<Object> got = op.read(key, Retry::standard());
            if (!got)
                continue;
            const MountLease body = decodeSlot(srid, *got);
            if (body.gc_fenced || body.isCleanRelease())
            {
                tokens.emplace(srid, got->etag);
                continue;
            }
            const uint64_t threshold_ms = mountObservationThresholdMs(observer_lease_ms, declaredMountLeaseMs(body), options.poll_step_ms);
            say(fmt::format("M2: server root '{}' holds a mount lease; it is fenced if it does not renew for {} ms", srid, threshold_ms));
            watched.emplace(srid, Watched{key, got->etag, body, pool.bootMsNow(), threshold_ms});
        }
        report.slots_read = slot_keys.size();

        uint64_t fenced = 0;
        uint64_t last_progress_ms = pool.bootMsNow();
        while (!watched.empty())
        {
            op.pause(options.poll_step_ms);
            /// Taken before the reads: it can confirm that a token held, never date a sighting.
            const uint64_t sample_ms = pool.bootMsNow();
            if (sample_ms >= last_progress_ms + kProgressEveryMs)
            {
                last_progress_ms = sample_ms;
                say(fmt::format("M2: {} mount slot(s) still watched", watched.size()));
            }
            for (auto it = watched.begin(); it != watched.end();)
            {
                Watched & slot = it->second;
                const std::optional<Object> got = op.read(slot.key, Retry::standard());
                if (!got || got->etag != slot.etag)
                    throwSlotChanged(it->first);
                if (sample_ms < slot.first_seen_ms || sample_ms - slot.first_seen_ms < slot.threshold_ms)
                {
                    ++it;
                    continue;
                }
                WriteResult fence = op.replace(slot.key, encodeMountLease(slot.body.fencedCopy()), slot.etag, Retry::standard());
                if (std::holds_alternative<Conflict>(fence))
                    throwSlotChanged(it->first);
                const std::optional<Etag> fenced_token = orThrow(std::move(fence), fmt::format("CAS fence of mount slot '{}'", slot.key));
                if (!fenced_token)
                    throw Exception(ErrorCodes::INVALID_STATE, "CAS fence of mount slot '{}' reported no token", slot.key);
                tokens.emplace(it->first, *fenced_token);
                ++fenced;
                say(fmt::format("M2: server root '{}' fenced", it->first));
                it = watched.erase(it);
            }
        }

        say(fmt::format("M2: waiting {} ms in which no mount slot and no GC lease may move", grace_ms));
        op.pause(grace_ms);
        checkUnchanged(tokens, pairs, Window::BeforeFloor);
        say(fmt::format("M2: {} mount slot(s), {} fenced; no slot and no GC lease moved in {} ms", slot_keys.size(), fenced,
            grace_ms));
        return {tokens, pairs};
    }

    /// Reads every listed slot in order and calls `on_change(srid, gone)` as soon as one differs from `tokens`: a
    /// present slot with another token or none in `tokens`, then, with `gone`, each slot of `tokens` that is absent.
    template <typename OnChange>
    void forEachChangedSlot(const SlotTokens & tokens, OnChange && on_change)
    {
        std::set<String> present;
        for (const String & key : listSlotKeys())
        {
            const String srid = sridOf(key);
            const std::optional<Object> got = op.read(key, Retry::standard());
            if (!got)
                continue;
            present.insert(srid);
            const auto it = tokens.find(srid);
            if (it == tokens.end() || !sameVersion(got->etag, it->second))
                on_change(srid, /*gone=*/ false);
        }
        for (const auto & [srid, token] : tokens)
            if (!present.contains(srid))
                on_change(srid, /*gone=*/ true);
    }

    /// Every slot and both GC pairs read again: each slot has its token, and no slot appeared or vanished.
    void checkUnchanged(const SlotTokens & tokens, const GcPairs & pairs, Window window)
    {
        checkPairs(pairs, window);
        forEachChangedSlot(tokens, [&](const String & srid, bool /*gone*/) { throwSlotChanged(srid, window); });
    }

    /// Holds live only in fold seals under `gc/gen/`, so a `gc/state` that names none must not hide one. An absent state
    /// beside any GC object was lost. A present state at generation 0 is GC's own record that it adopted no fold, so
    /// `gc/hb` and the janitor cursor, which carry no hold, are not checked.
    void requireNoGcHistory(const std::optional<GcState> & state)
    {
        const bool state_present = state.has_value();
        const String generation_prefix = layout.gcGenPrefix(0);
        const String generations = generation_prefix.substr(0, generation_prefix.size() - 2);
        std::vector<String> found;
        for (const ListedKey & listed : op.list(generations, /*cursor*/ "", /*limit*/ 1, Retry::standard()).keys)
            found.push_back(listed.key);
        if (!state_present)
            for (const String & key : {layout.gcHbKey(), layout.gcMaintenanceStateKey()})
                if (op.head(key, Retry::standard()))
                    found.push_back(key);
        if (found.empty())
            return;
        const bool lease_held = state_present && state->lease.owner != 0;
        String what;
        if (!state_present)
            what = fmt::format("gc/state is absent, but GC has run on this pool: `{}` exists", fmt::join(found, "`, `"));
        else if (lease_held)
            what = fmt::format("gc/state names no fold seal and GC owner {} holds its lease, but `{}` exists: an interrupted "
                "`cas-gc-rebuild` or GC round left a fold the state does not name", u128ToHex(state->lease.owner),
                fmt::join(found, "`, `"));
        else
            what = fmt::format("gc/state names no fold seal, but `{}` exists: an interrupted `cas-gc-rebuild` or GC round left "
                "a fold the state does not name", fmt::join(found, "`, `"));
        constexpr std::string_view keep_offline = "restore the pool from a consistent backup taken while its gc/state was "
            "intact. Until then keep the pool offline or serve it read-only: a writable older server with GC enabled starts "
            "folding at generation zero without the lost holds";
        /// `Dialect::Emulated` is local object storage, where the read-only disk the old rebuild needs refuses its writes.
        const String recovery = lease_held
            ? fmt::format("An older `cas-gc-rebuild` refuses this state with `another GC leader holds the lease`, because it "
                "takes a fresh identity and does not steal the lease: {}", keep_offline)
            : op.backendDialect() == Dialect::Emulated
            ? fmt::format("On local object storage `cas-gc-rebuild` cannot complete, because a read-only local store refuses "
                "its writes: {}", keep_offline)
            : fmt::format("Recover gc/state with the build that last ran GC on this pool: `{}`, with a configuration that "
                "opens the disk read-only, which `cas-gc-rebuild` requires. Then run this tool again without "
                "--trust-clean-markers", kGcRebuildCommandLine);
        throw Exception(ErrorCodes::INVALID_STATE,
            "M3: {}, and a gc/state written now would name none of the runs and holds GC left. {}", what, recovery);
    }

    /// M3: every condemned row of the runs the adopted fold seal names. The state, the seal and the runs pass the checks
    /// GC of this build applies when it adopts them, because after the floor is raised no older build can repair them.
    std::vector<InheritedRow> readCondemnedRows(const std::optional<GcState> & state, const PoolMeta & meta)
    {
        if (state && state->gc_shards != meta.gc_shards)
            throw Exception(ErrorCodes::INVALID_STATE,
                "M3: gc/state names {} GC shard(s), but _pool_meta names {}; GC of this build refuses such a state. The run "
                "stopped before any intent and before the floor", state->gc_shards, meta.gc_shards);
        if (!state || state->snap_generation == 0)
            requireNoGcHistory(state);
        std::vector<InheritedRow> rows;
        if (state && state->snap_generation != 0)
        {
            const String seal_key = layout.foldSealKey(state->snap_generation, state->snap_attempt);
            const std::optional<Object> seal_bytes = op.read(seal_key, Retry::standard());
            if (!seal_bytes)
                throw Exception(ErrorCodes::INVALID_STATE, "gc/state names the fold seal `{}`, which is absent", seal_key);
            CasFoldSeal seal;
            try
            {
                seal = decodeFoldSeal(seal_bytes->bytes, layout, meta.gc_shards, state->snap_generation);
            }
            catch (Exception & e)
            {
                e.addMessage("M3: GC of this build refuses the fold seal `{}` that gc/state adopts. The run stopped before any "
                    "intent and before the floor", seal_key);
                throw;
            }
            std::vector<std::vector<RunRef>> runs_of_shard(meta.gc_shards);
            for (const RunRef & run : seal.blob_target_runs)
                runs_of_shard[run.shard].push_back(run);
            for (uint64_t shard = 0; shard < meta.gc_shards; ++shard)
            {
                const AdoptedRunContext parent{.gc_shards = meta.gc_shards, .parent_round = state->round,
                                               .summary = seal.condemned_summary.at(shard)};
                AdoptedRunReader reader(op, runs_of_shard[shard], shard, parent);
                try
                {
                    SourceEdgeRecord rec;
                    while (reader.next(rec))
                        if (rec.marker == RunMarker::Condemned)
                            rows.push_back(InheritedRow{.ref = rec.ref, .token = rec.token, .pending = rec.delete_pending});
                }
                catch (Exception & e)
                {
                    if (e.code() != ErrorCodes::CORRUPTED_DATA)
                        throw;
                    e.addMessage("M3: GC of this build refuses a source-edge run of the fold seal that gc/state adopts. The run "
                        "stopped before any intent and before the floor");
                    throw;
                }
            }
        }
        size_t pending = 0;
        for (const InheritedRow & row : rows)
            pending += row.pending;
        report.condemned_rows = rows.size();
        say(fmt::format("M3: {} condemned row(s), {} pending", rows.size(), pending));
        return rows;
    }

    /// M6: `gc/state` at header 4 with every field as read, or created at header 4 when absent. Nothing on a conflict.
    std::optional<Etag> rewriteGcState(const std::optional<Object> & stored, const std::optional<GcState> & state, const PoolMeta & meta)
    {
        GcState next;
        if (state)
            next = *state;
        else
            next.gc_shards = meta.gc_shards;
        next.header_generation = G_BLOB_INTENTS;
        const String bytes = encodeGcState(next);
        WriteResult written = stored ? op.replace(layout.gcStateKey(), bytes, stored->etag, Retry::standard())
                                     : op.create(layout.gcStateKey(), bytes, Retry::standard());
        if (std::holds_alternative<Conflict>(written))
            return std::nullopt;
        const std::optional<Etag> token = orThrow(std::move(written), "CAS gc/state rewrite at generation 4");
        if (!token)
            throw Exception(ErrorCodes::INVALID_STATE, "CAS gc/state rewrite at generation 4 reported no token");
        return token;
    }

    /// M4: every listed marker read by its exact key, a page at a time, `concurrency` reads in flight.
    MarkerScan readMarkers()
    {
        MarkerScan scan;
        Meter meter(*this, "M4", "marker(s) read", 0);
        std::vector<std::pair<String, BlobRef>> batch;
        const auto flush = [&]
        {
            std::vector<std::optional<bool>> clean(batch.size());
            runBounded(concurrency, batch.size(), [&](size_t i)
            {
                CasOperation job = jobOperation();
                if (const std::optional<Object> got = job.read(batch[i].first, Retry::standard()))
                    clean[i] = markerIsClean(batch[i].first, got->bytes);
            });
            for (size_t i = 0; i < batch.size(); ++i)
            {
                if (!clean[i])
                    continue;
                ++scan.markers;
                if (!*clean[i])
                    scan.not_clean.insert(batch[i].second);
            }
            meter.add(batch.size());
            batch.clear();
        };
        op.forEachListedKey(layout.blobsPrefix(), [&](const ListedKey & listed)
        {
            if (Layout::hasLegacyMarkerSuffix(listed.key))
            {
                if (const auto ref = layout.parseLegacyMarkerKey(listed.key))
                    batch.emplace_back(listed.key, *ref);
                if (batch.size() >= kBatchKeys)
                    flush();
            }
            else if (layout.parseBlobKey(listed.key))
            {
                ++scan.bodies;
            }
            return true;
        }, Retry::standard());
        flush();
        scan.elapsed_ms = meter.elapsedMs();
        report.markers_read = scan.markers;
        report.bodies_listed = scan.bodies;
        say(fmt::format("M4: {} marker(s), {} not Clean; {} bod(ies) listed", scan.markers, scan.not_clean.size(), scan.bodies));
        return scan;
    }

    /// Only a confirmed intent counts. One more create follows an unconfirmed one, since an unanswered create is often
    /// answered by the next; an occupant with other bytes is not, and fails the run before M6.
    void createIntent(CasOperation & job, const BlobRef & ref, const CanonicalIdentity & identity, std::atomic<uint64_t> & confirmed)
    {
        IntentCreateResult result;
        try
        {
            for (size_t attempt = 0; attempt < 2 && !result.confirmed(); ++attempt)
                result = createBlobIntent(job, layout, ref, identity, Retry::standard());
        }
        catch (Exception & e)
        {
            e.addMessage("M5: an intent create failed; nothing was written after the intents, and a rerun passes only once the "
                "store accepts the create");
            throw;
        }
        if (!result.confirmed())
            throw Exception(ErrorCodes::INVALID_STATE,
                "the intent of blob `{}` for {} tag {} could not be confirmed ({}); nothing was written after the intents, run the "
                "tool again", layout.blobKey(ref), dialectWord(identity.dialect), identity.tag_hex, result.detail);
        ++confirmed;
    }

    /// The `HEAD` of each blob and the create of the intent of its current token, `concurrency` blobs at a time.
    /// `current`, when given, receives each blob's identity.
    void convert(const std::vector<BlobRef> & blobs, std::atomic<uint64_t> & confirmed, Meter & meter,
                 std::vector<std::optional<CanonicalIdentity>> * current = nullptr)
    {
        for (size_t begin = 0; begin < blobs.size(); begin += kBatchKeys)
        {
            const size_t n = std::min(kBatchKeys, blobs.size() - begin);
            runBounded(concurrency, n, [&](size_t i)
            {
                CasOperation job = jobOperation();
                const BlobRef & ref = blobs[begin + i];
                const std::optional<Meta> head = job.head(layout.blobKey(ref), Retry::standard());
                std::optional<CanonicalIdentity> identity = head ? head->etag.canonical() : std::nullopt;
                if (identity)
                    createIntent(job, ref, *identity, confirmed);
                if (current)
                    (*current)[begin + i] = std::move(identity);
            });
            meter.add(n);
        }
    }

    /// M5: an intent for the current token of every blob a marker or a row condemns, and with `all_bodies` of every
    /// listed body; and for the own token of every pending row whose body is absent or another incarnation. Every
    /// create is confirmed before the step returns, and the first one that is not stops the run.
    void createIntents(const std::vector<InheritedRow> & rows, const MarkerScan & markers, bool all_bodies)
    {
        std::set<BlobRef> targets = markers.not_clean;
        for (const InheritedRow & row : rows)
            targets.insert(row.ref);
        const std::vector<BlobRef> condemned(targets.begin(), targets.end());
        /// The condemned bodies are mostly among the listed ones; the larger count is the estimate.
        const uint64_t expected = all_bodies ? std::max<uint64_t>(markers.bodies, condemned.size()) : condemned.size();
        String estimate;
        if (markers.elapsed_ms > 0 && markers.markers > 0)
            estimate = fmt::format("; at the M4 rate about {} s", 2 * expected * markers.elapsed_ms / markers.markers / 1000);
        say(fmt::format("M5: about {} blob(s), one HEAD and one create each, {} in flight{}", expected, concurrency, estimate));

        std::atomic<uint64_t> confirmed{0};
        Meter meter(*this, "M5", "blob(s) converted", expected);
        std::vector<std::optional<CanonicalIdentity>> current(condemned.size());
        convert(condemned, confirmed, meter, &current);

        if (all_bodies)
        {
            /// Listed again rather than kept from M4, so the tool holds one page of the pool, not all of it.
            std::vector<BlobRef> page;
            op.forEachListedKey(layout.blobsPrefix(), [&](const ListedKey & listed)
            {
                if (!Layout::hasLegacyMarkerSuffix(listed.key))
                    if (const auto ref = layout.parseBlobKey(listed.key); ref && !targets.contains(*ref))
                        page.push_back(*ref);
                if (page.size() >= kBatchKeys)
                {
                    convert(page, confirmed, meter);
                    page.clear();
                }
                return true;
            }, Retry::standard());
            convert(page, confirmed, meter);
        }

        /// `runBounded` queues every job it is given, so these creates go in batches, as in `convert`.
        std::vector<std::pair<BlobRef, CanonicalIdentity>> own;
        const auto create_own = [&]
        {
            runBounded(concurrency, own.size(), [&](size_t i)
            {
                CasOperation job = jobOperation();
                createIntent(job, own[i].first, own[i].second, confirmed);
            });
            own.clear();
        };
        for (const InheritedRow & row : rows)
        {
            if (!row.pending)
                continue;
            const std::optional<CanonicalIdentity> identity = row.token.canonical();
            const size_t at = std::lower_bound(condemned.begin(), condemned.end(), row.ref) - condemned.begin();
            if (identity && current[at] != identity)
                own.emplace_back(row.ref, *identity);
            if (own.size() >= kBatchKeys)
                create_own();
        }
        create_own();

        report.intents_confirmed = confirmed.load();
        say(fmt::format("M5: {} intent(s) confirmed for {} blob(s); until M6 an older build can still open and use this pool",
            report.intents_confirmed, meter.count()));
    }

    /// Right before the floor (M7) and right after it: every slot and both GC pairs as M2 left them, `gc/state` as M6
    /// left it (on a rerun that skipped M6, as M3 read it).
    void recheck(const SlotTokens & slots, const GcPairs & pairs, const std::optional<Etag> & state_token, Window window)
    {
        const std::optional<Object> stored_state = op.read(layout.gcStateKey(), Retry::standard());
        if (stored_state.has_value() != state_token.has_value() || (stored_state && !sameVersion(stored_state->etag, *state_token)))
            throwGcMoved(stored_state ? decodeGcState(stored_state->bytes).lease.owner : pairs.lease.owner, window);
        checkUnchanged(slots, pairs, window);
    }

    /// After the marker removal the pool is at generation 4 and an older build cannot open it, so a member that
    /// appeared since M8 is a server of this build that the operator started: listed, never an error.
    void noteMembersAfterMarkers(const SlotTokens & slots, const GcPairs & pairs, const std::optional<Etag> & state_token)
    {
        std::vector<String> seen;
        forEachChangedSlot(slots, [&](const String & srid, bool gone)
        {
            seen.push_back(gone ? fmt::format("server root '{}' (its slot is gone)", srid) : fmt::format("server root '{}'", srid));
        });
        const std::optional<Object> stored_state = op.read(layout.gcStateKey(), Retry::standard());
        const bool state_moved = stored_state.has_value() != state_token.has_value()
            || (stored_state && !sameVersion(stored_state->etag, *state_token));
        if (const auto owner = gcOwnerThatMoved(pairs); owner || state_moved)
            seen.push_back(owner ? fmt::format("GC lease of owner {}", u128ToHex(*owner)) : String("a change of gc/state"));
        if (seen.empty())
            return;
        say(fmt::format("M9: seen since the pool was migrated: {}; a server of this build may start after the M8 line, so this is "
            "expected; if one of them runs an older build, stop it now", fmt::join(seen, ", ")));
    }

    /// M9: every listed marker, a page at a time, through the one removal `cas-fsck --fix legacy-markers` uses too.
    void removeMarkers()
    {
        Meter meter(*this, "M9", "marker(s) removed", report.markers_read);
        std::vector<String> page;
        const auto flush = [&]
        {
            try
            {
                const LegacyMarkerRemoval removal = removeLegacyMarkers(pool, page, concurrency);
                report.markers_removed += removal.removed;
                report.markers_changed += removal.changed;
            }
            catch (Exception & e)
            {
                e.addMessage("M9: {} marker(s) removed in earlier pages; the pool is migrated, and a rerun or `cas-fsck --fix "
                    "legacy-markers` removes the rest", report.markers_removed);
                throw;
            }
            meter.add(page.size());
            page.clear();
        };
        op.forEachListedKey(layout.blobsPrefix(), [&](const ListedKey & listed)
        {
            if (Layout::hasLegacyMarkerSuffix(listed.key))
                page.push_back(listed.key);
            if (page.size() >= kBatchKeys)
                flush();
            return true;
        }, Retry::standard());
        flush();
        say(fmt::format("M9: {} marker(s) removed, {} left because they changed during the removal; a rerun or `cas-fsck "
            "--fix legacy-markers` removes those", report.markers_removed, report.markers_changed));
    }

    MigratePoolReport countMarkersOfAMigratedPool()
    {
        op.forEachListedKey(layout.blobsPrefix(), [&](const ListedKey & listed)
        {
            report.markers_read += layout.parseLegacyMarkerKey(listed.key).has_value();
            return true;
        }, Retry::standard());
        say(fmt::format("dry run: {} marker(s) would be removed; nothing was written", report.markers_read));
        return report;
    }
};

}

uint64_t migrateGraceMs(uint64_t seconds)
{
    if (seconds > kMaxMigrateGraceSeconds)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "--grace-seconds {} is above the limit of {}", seconds, kMaxMigrateGraceSeconds);
    return seconds * 1000;
}

MigratePoolReport migratePool(Pool & pool, const MigratePoolOptions & options, const std::function<void(std::string_view)> & progress)
{
    return Migration(pool, options, progress).run();
}

}
