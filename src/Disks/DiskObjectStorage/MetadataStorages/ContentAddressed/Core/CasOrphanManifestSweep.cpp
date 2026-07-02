#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcCursorKey.h>
#include <Common/Exception.h>
#include <map>
#include <set>
#include <limits>

namespace DB::Cas
{

namespace
{

/// The build-watermark floor now rides the per-server mount lease (ack-floor merge, spec 2026-07-02).
/// A namespace is rooted by `server_root_id`, but that id is a clean relative path and can contain
/// slashes. Try namespace prefixes from longest to shortest and accept the first durable mount body.
/// No mount => no authority => fail open / not eligible. On the writable path the mount's
/// `writer_epoch` is the same durable value the old watermark's `epoch` carried (CasStore.cpp "THE
/// BRIDGE"), so `{writer_epoch, min_active}` are consumed exactly where `ServerWatermark::{epoch,
/// min_active}` were.
std::optional<MountLease> floorForNamespace(Store & store, const RootNamespace & ns)
{
    const String & value = ns.string();
    size_t pos = value.size();
    while (true)
    {
        pos = value.rfind('/', pos == 0 ? 0 : pos - 1);
        if (pos == String::npos)
            break;

        const String server_root_id = value.substr(0, pos);
        if (!server_root_id.empty())
        {
            if (const auto got = store.backend().get(store.layout().mountKey(server_root_id)))
                return decodeMountLease(got->bytes);
        }
        if (pos == 0)
            break;
    }
    return std::nullopt;
}

struct ListedManifestObject
{
    RootNamespace ns;
    BuildPrefix prefix;
    String key;
};

std::optional<ListedManifestObject> parseListedManifestObject(const Layout & layout, const String & key)
{
    const String base = layout.casManifestsPrefix();
    if (!key.starts_with(base))
        return std::nullopt;

    const String rest = key.substr(base.size());
    const size_t file_sep = rest.rfind('/');
    if (file_sep == String::npos)
        return std::nullopt;
    const size_t build_sep = rest.rfind('/', file_sep == 0 ? 0 : file_sep - 1);
    if (build_sep == String::npos)
        return std::nullopt;
    const size_t writer_sep = rest.rfind('/', build_sep == 0 ? 0 : build_sep - 1);
    if (writer_sep == String::npos)
        return std::nullopt;

    const String ns_str = rest.substr(0, writer_sep);
    const String writer_epoch_str = rest.substr(writer_sep + 1, build_sep - writer_sep - 1);
    const String seq_str = rest.substr(build_sep + 1, file_sep - build_sep - 1);
    const String file = rest.substr(file_sep + 1);
    if (ns_str.empty() || writer_epoch_str.empty() || seq_str.empty() || file.size() != 12 || !file.ends_with(".proto"))
        return std::nullopt;

    uint64_t writer_epoch = 0;
    uint64_t build_seq = 0;
    uint64_t ordinal = 0;
    try
    {
        size_t consumed = 0;
        writer_epoch = std::stoull(writer_epoch_str, &consumed);
        if (consumed != writer_epoch_str.size())
            return std::nullopt;
        consumed = 0;
        build_seq = std::stoull(seq_str, &consumed);
        if (consumed != seq_str.size())
            return std::nullopt;
        consumed = 0;
        ordinal = std::stoull(file.substr(0, 6), &consumed);
        if (consumed != 6 || ordinal == 0 || ordinal > kMaxManifestOrdinal)
            return std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }

    return ListedManifestObject{
        .ns = RootNamespace{ns_str},
        .prefix = BuildPrefix{.writer_epoch = writer_epoch, .build_sequence = build_seq},
        .key = key};
}

/// The shard's sealed fold cursor (the latest seal's `folded_cursor` for ns/shard), or 0 when no seal
/// covers it yet. A precommit-removal event AT OR ABOVE this cursor has NOT had its `-1` blob decrement
/// folded+sealed, so the precommit's manifest body is still load-bearing (delete-after-sealed-decrements).
///
/// B2 — resolve the seal DIRECTLY at the adopted `(snap_generation, snap_attempt)` (mirrors
/// `Gc::readSealedCursors`): the completion seal if the adopted round finished (it carries the cursors
/// forward into `folded_cursors`), else the fold seal at the same pair (mid-round: fold sealed, completion
/// not yet advanced, so `snap_generation == G_f`), else cursor 0 (fresh pool). The old `for g downto 1`
/// back-scan was UNSOUND with a single stored `snap_attempt`: a prior generation's adopted attempt was a
/// different `lease.seq`, recorded nowhere, so its key is unreachable here — the scan never legitimately
/// reached a prior round.
uint64_t sealedFoldCursor(Store & store, const RootNamespace & ns, uint64_t shard)
{
    const Layout & layout = store.layout();
    const auto state_got = store.backend().get(layout.gcStateKey());
    if (!state_got)
        return 0;
    const GcState state = decodeGcState(state_got->bytes);
    const uint64_t gen = state.snap_generation;
    const uint64_t attempt = state.snap_attempt;
    const String key = cursorKey(ns, shard);

    if (const auto got = store.backend().get(layout.completionSealKey(gen, attempt)))
    {
        const CasCompletionSeal seal = decodeCompletionSeal(got->bytes);
        const auto it = seal.folded_cursors.find(key);
        return it != seal.folded_cursors.end() ? it->second.folded_cursor : 0;
    }
    if (const auto got = store.backend().get(layout.foldSealKey(gen, attempt)))
    {
        const CasFoldSeal seal = decodeFoldSeal(got->bytes);
        const auto it = seal.per_ns_shard.find(key);
        return it != seal.per_ns_shard.end() ? it->second.folded_cursor : 0;
    }
    return 0;
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

bool prefixEligible(Store & store, const RootNamespace & ns, const BuildPrefix & prefix)
{
    /// OQ6: durable watermark fact only. A missing watermark => NOT eligible (control #9: never a
    /// frozen-seq / judged-dead guess). Compare writer_epoch first, then build_sequence, so old-epoch
    /// debris drains after a process restart even when its build_sequence is above the current min_active.
    const auto floor = floorForNamespace(store, ns);
    if (!floor)
        return false;

    const MountLease & w = *floor;
    if (prefix.writer_epoch < w.writer_epoch)
        return true;
    if (prefix.writer_epoch > w.writer_epoch)
        return false;
    if (w.min_active == std::numeric_limits<uint64_t>::max())
        return true;   /// farewell/retired sentinel: every seq is retired
    return w.min_active > prefix.build_sequence;
}

void sweepNamespace(Store & store, const RootNamespace & ns, const BuildPrefix & prefix)
{
    if (!prefixEligible(store, ns, prefix))
        return;   /// not eligible by the durable watermark fact — delete nothing (controls #8/#9)

    const Layout & layout = store.layout();
    Backend & backend = store.backend();

    const std::set<String> active = activeManifestKeys(store, ns);

    /// Enumerate the ONE build prefix: cas/manifests/<ns>/<writer_epoch>/<build_sequence>/.
    const String prefix_key = layout.manifestNamespacePrefix(ns)
        + std::to_string(prefix.writer_epoch) + "/" + std::to_string(prefix.build_sequence) + "/";

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

ManifestSweepResult sweepManifestCursorPage(
    Store & store,
    const String & cursor,
    uint64_t list_budget,
    uint64_t delete_budget)
{
    ManifestSweepResult result;
    result.next_cursor = cursor;
    if (list_budget == 0)
        return result;

    Backend & backend = store.backend();
    const Layout & layout = store.layout();
    const ListPage page = backend.list(layout.casManifestsPrefix(), cursor, list_budget);

    std::map<String, bool> eligible_by_prefix;
    std::map<String, std::set<String>> active_by_ns;
    for (const ListedKey & listed : page.keys)
    {
        ++result.listed;
        const auto parsed = parseListedManifestObject(layout, listed.key);
        if (!parsed)
        {
            ++result.skipped;
            continue;
        }

        if (result.deleted >= delete_budget)
        {
            ++result.skipped;
            continue;
        }

        const String eligibility_key = parsed->ns.string() + "\n"
            + std::to_string(parsed->prefix.writer_epoch) + "\n"
            + std::to_string(parsed->prefix.build_sequence);
        auto [eligible_it, eligible_inserted] = eligible_by_prefix.emplace(eligibility_key, false);
        if (eligible_inserted)
            eligible_it->second = prefixEligible(store, parsed->ns, parsed->prefix);
        if (!eligible_it->second)
        {
            ++result.skipped;
            continue;
        }

        auto [active_it, inserted] = active_by_ns.emplace(parsed->ns.string(), std::set<String>{});
        if (inserted)
            active_it->second = activeManifestKeys(store, parsed->ns);
        if (active_it->second.count(parsed->key))
        {
            ++result.skipped;
            continue;
        }

        Token token;
        if (listed.token)
            token = *listed.token;
        else
        {
            const HeadResult head = backend.head(parsed->key);
            if (!head.exists)
            {
                ++result.skipped;
                continue;
            }
            token = head.token;
        }

        const DeleteOutcome outcome = backend.deleteExact(parsed->key, token);
        if (outcome.kind == DeleteOutcome::Kind::Deleted)
            ++result.deleted;
        else
            ++result.skipped;
    }

    result.next_cursor = page.next_cursor;
    result.wrapped = page.next_cursor.empty();
    return result;
}

}
