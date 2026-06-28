#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootsRegistry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcCursorKey.h>
#include <Common/Exception.h>
#include <set>
#include <limits>

namespace DB::Cas
{

namespace
{

/// The server hex of a writer_instance_id "<server_hex>:<process_epoch>" (the watermark slot key). An
/// id without a ':' has no derivable server => no eligibility (returns empty).
String serverHexOf(const String & writer_instance_id)
{
    const size_t colon = writer_instance_id.find(':');
    if (colon == String::npos)
        return {};
    return writer_instance_id.substr(0, colon);
}

/// The shard's sealed fold cursor (the latest CasFoldSeal's `folded_cursor` for ns/shard), or 0 when no
/// seal covers it yet. A precommit-removal event AT OR ABOVE this cursor has NOT had its `-1` blob
/// decrement folded+sealed, so the precommit's manifest body is still load-bearing (delete-after-
/// sealed-decrements). The seal is written at the FOLD generation (G+1) while gc/state points at the
/// COMPLETION generation (G+2), so scan downward from the current generation for the most recent seal.
uint64_t sealedFoldCursor(Store & store, const RootNamespace & ns, uint64_t shard)
{
    const Layout & layout = store.layout();
    const auto state_got = store.backend().get(layout.gcStateKey());
    if (!state_got)
        return 0;
    const GcState state = decodeGcState(state_got->bytes);
    const uint64_t gen = state.snap_generation;
    /// Task 3 placeholder: resolve the seal under the adopted attempt (Task 7 refines the back-scan).
    const uint64_t attempt = state.snap_attempt;
    const String key = cursorKey(ns, shard);
    for (uint64_t g = gen; ; --g)
    {
        if (const auto got = store.backend().get(layout.foldSealKey(g, attempt)))
        {
            const CasFoldSeal seal = decodeFoldSeal(got->bytes);
            const auto it = seal.per_ns_shard.find(key);
            return it != seal.per_ns_shard.end() ? it->second.folded_cursor : 0;
        }
        if (g == 0)
            return 0;
    }
}

/// The active manifest-object-KEY set for one namespace: every committed RootRef's manifest_ref plus
/// every live precommit binding (a precommit new_binding not later removed) AND every precommit body
/// whose REMOVAL is still PENDING (above the sealed fold cursor — its `-1` not yet sealed). Keys (not
/// ManifestIds) so a listed object key can be tested directly without parsing the key back.
std::set<String> activeManifestKeys(Store & store, const RootNamespace & ns)
{
    std::set<String> active;
    const Layout & layout = store.layout();
    const uint64_t shards = store.poolMeta().root_shards;
    for (uint64_t shard = 0; shard < shards; ++shard)
    {
        const auto got = store.backend().get(layout.rootShardKey(ns, shard));
        if (!got)
            continue;
        const RootShard root = decodeRootShard(got->bytes);

        /// Committed owners: the current ref payloads. `root.refs` IS the sealed committed view — a
        /// promote/publish sets refs[final_ref_name] AND appends the committed RootOwnerEvent in ONE
        /// mutateShard CAS (Build::promote / publishCommitted), so a committed manifest is ALWAYS in
        /// root.refs the instant it exists; there is no committed-in-journal-only window to miss.
        for (const auto & [name, ref] : root.refs)
            active.insert(layout.manifestKey(ManifestId{ns, ref.manifest_ref}));

        /// Live precommit owners: a precommit new_binding whose manifest_ref is not later removed. The
        /// journal is append-only (trimmed below the GC fold cursor only after sealing), so accumulate
        /// adds and subtract removals by manifest_ref. A removal whose `-1` is NOT YET SEALED (its
        /// transition_version is above the sealed fold cursor) is treated as STILL ACTIVE: the GC fold
        /// must read the body to emit that `-1` next round (delete-after-sealed-decrements), so the sweep
        /// must NOT delete it. This closes the B8 race where GC's own precommit reclaim appends a removal
        /// in a round whose end-of-round sweep would otherwise delete the body before the `-1` folds.
        const uint64_t cursor = sealedFoldCursor(store, ns, shard);
        std::set<ManifestRef> precommit_live;
        for (const RootOwnerEvent & e : root.journal)
        {
            const bool is_removal = e.old_binding
                && (!e.new_binding || e.old_binding->manifest_ref != e.new_binding->manifest_ref);
            if (is_removal && e.transition_version <= cursor)
                precommit_live.erase(e.old_binding->manifest_ref);
            if (e.new_binding && e.new_binding->owner_kind == OwnerKind::Precommit)
                precommit_live.insert(e.new_binding->manifest_ref);
            /// A PENDING precommit removal (its `-1` not yet sealed) keeps the body load-bearing even when
            /// the matching create-precommit was already folded and TRIMMED away — protect the body named
            /// by the removal's old_binding directly so the GC fold can still read it to emit the `-1`.
            if (is_removal && e.transition_version > cursor
                && e.old_binding->owner_kind == OwnerKind::Precommit)
                precommit_live.insert(e.old_binding->manifest_ref);
        }
        for (const ManifestRef & ref : precommit_live)
            active.insert(layout.manifestKey(ManifestId{ns, ref}));
    }
    return active;
}

}

bool prefixEligible(Store & store, const BuildPrefix & prefix)
{
    /// OQ6: durable watermark fact only. A missing watermark => NOT eligible (control #9: never a
    /// frozen-seq / judged-dead guess). The retired sentinel (min_active == UINT64_MAX) retires every
    /// seq; otherwise min_active > build_sequence means this build is below the live floor (retired).
    const String server_hex = serverHexOf(prefix.writer_instance_id);
    if (server_hex.size() != 32)
        return false;

    const auto got = store.backend().get(store.layout().serverWatermarkKey(server_hex));
    if (!got)
        return false;   /// no durable fact => not eligible

    const ServerWatermark w = decodeServerWatermark(got->bytes);
    if (w.min_active == std::numeric_limits<uint64_t>::max())
        return true;   /// farewell/retired sentinel: every seq is retired
    return w.min_active > prefix.build_sequence;
}

void sweepNamespace(Store & store, const RootNamespace & ns, const BuildPrefix & prefix)
{
    if (!prefixEligible(store, prefix))
        return;   /// not eligible by the durable watermark fact — delete nothing (controls #8/#9)

    const Layout & layout = store.layout();
    Backend & backend = store.backend();

    const std::set<String> active = activeManifestKeys(store, ns);

    /// Enumerate the ONE build prefix: roots/<ns>/_manifests/<writer_instance_id>/<build_sequence>/.
    const String prefix_key = layout.rootNamespacePrefix(ns) + "_manifests/"
        + prefix.writer_instance_id + "/" + std::to_string(prefix.build_sequence) + "/";

    String cursor;
    while (true)
    {
        const ListPage page = backend.list(prefix_key, cursor, /*limit*/1000);
        for (const ListedKey & listed : page.keys)
        {
            if (active.count(listed.key))
                continue;   /// owned by a committed/precommit owner — never sweep (control #8)

            /// Exact-token delete: HEAD for the current token, then deleteExact. A 404 between HEAD and
            /// delete (or a TokenMismatch — a fresh owner reclaimed it) is tolerated (record-and-continue).
            const HeadResult head = backend.head(listed.key);
            if (!head.exists)
                continue;
            backend.deleteExact(listed.key, head.token);   /// NotFound/TokenMismatch spared
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
}

std::optional<SweepTarget> pickOneSweepTarget(Store & store)
{
    /// Bounded backstop: scan the registry's namespaces, and for each enumerate its `_manifests/<writer>/`
    /// build prefixes, returning the FIRST eligible one. At most one namespace + one prefix per round.
    const Layout & layout = store.layout();
    const auto reg = store.backend().get(layout.rootsRegistryKey());
    if (!reg)
        return std::nullopt;
    const RootsRegistry registry = decodeRootsRegistry(reg->bytes);

    for (const String & ns_name : registry.namespaces)
    {
        const RootNamespace ns{ns_name};
        const String manifests_prefix = layout.rootNamespacePrefix(ns) + "_manifests/";

        /// LIST distinct writer/build prefixes, FOLLOWING next_cursor across ALL pages (mirroring
        /// sweepNamespace). Keys are `<manifests_prefix><writer_instance_id>/<build_sequence>/<aa>/<inst>.proto`;
        /// parse the first two path segments after the prefix to recover (writer_instance_id, build_sequence).
        /// A single page that is all live/ineligible prefixes must NOT hide an eligible older build prefix on
        /// a later page (else pre-precommit debris leaks forever — violates OrphanManifestDebrisDrains).
        std::set<std::pair<String, uint64_t>> seen;
        String cursor;
        while (true)
        {
            const ListPage page = store.backend().list(manifests_prefix, cursor, /*limit*/1000);
            for (const ListedKey & listed : page.keys)
            {
                if (!listed.key.starts_with(manifests_prefix))
                    continue;
                const String rest = listed.key.substr(manifests_prefix.size());
                const size_t s1 = rest.find('/');
                if (s1 == String::npos)
                    continue;
                const String writer = rest.substr(0, s1);
                const size_t s2 = rest.find('/', s1 + 1);
                if (s2 == String::npos)
                    continue;
                const String seq_str = rest.substr(s1 + 1, s2 - s1 - 1);
                uint64_t build_seq = 0;
                try
                {
                    size_t consumed = 0;
                    build_seq = std::stoull(seq_str, &consumed);
                    if (consumed != seq_str.size())
                        continue;
                }
                catch (...)
                {
                    continue;
                }
                if (!seen.emplace(writer, build_seq).second)
                    continue;
                const BuildPrefix prefix{.writer_instance_id = writer, .build_sequence = build_seq};
                if (prefixEligible(store, prefix))
                    return SweepTarget{.ns = ns, .prefix = prefix};
            }
            if (page.next_cursor.empty())
                break;
            cursor = page.next_cursor;
        }
    }
    return std::nullopt;
}

}
