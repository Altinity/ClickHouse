#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int ABORTED;
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
}
}

namespace DB::Cas
{

Gc::Gc(StorePtr store_, UInt128 gc_id_)
    : store(std::move(store_))
    , gc_id(gc_id_)
{
    if (!store)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cas::Gc: store must not be null");
    /// owner 0 in GcLease means "never held" — a leader with id 0 would be indistinguishable from it.
    if (gc_id == UInt128(0))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cas::Gc: gc_id must not be 0 (reserved for 'lease never held')");
}

RoundReport Gc::runRegularRound()
{
    RoundReport report;
    GcState state;
    Token state_token;
    report.acquired_lease = acquireOrRenewLease(state, state_token);
    if (!report.acquired_lease)
        return report;

    report.round = state.round;

    /// R1: fold the journals into a new durable snap generation (cursors advance only after).
    const FoldResult folded = fold(state, state_token);

    /// Candidates for R2 are derived STATELESSLY from the durable snap (the model GRetire guard
    /// `present ∧ everEdged ∧ InDeg = 0` over zeroInDegreeKnown) - never from the in-memory fold
    /// transitions, so a crash-replayed round re-derives the same set. `folded.transitioned` is
    /// only a health cross-check; the counts can differ legitimately (e.g. a node zeroed by an
    /// EARLIER round's fold is in zeroInDegreeKnown but did not transition in THIS fold).
    uint64_t candidates = 0;
    for (const auto & [snap_shard, snap] : folded.snap)
        candidates += snap.zeroInDegreeKnown().size();
    report.candidates = candidates;

    /// Tasks 7-12: retire / fence / recheck / cascade / trim
    return report;
}

Gc::FoldResult Gc::fold(GcState & state, const Token & state_token)
{
    const Layout & layout = store->layout();
    Backend & backend = store->backend();
    FoldResult result;

    /// 1. Discover root-shard manifests: paginate roots/; the classifier skips verbatim files
    /// (`_files/...`) and any non-numeric tail.
    {
        String cursor;
        while (true)
        {
            const ListPage page = backend.list(layout.rootsPrefix(), cursor, /*limit*/ 1000);
            for (const ListedKey & listed : page.keys)
                if (auto parsed = layout.tryParseRootShardKey(listed.key))
                    result.root_shards.push_back(std::move(*parsed));
            if (page.next_cursor.empty())
                break;
            cursor = page.next_cursor;
        }
    }

    /// 2. Load the authoritative snap generation. An absent shard object is an EMPTY snap, not an
    /// error: generation 0 of a fresh pool has no objects at all (and the very first fold of a new
    /// pool legitimately starts from nothing).
    for (uint64_t snap_shard = 0; snap_shard < state.snap_shards; ++snap_shard)
    {
        if (const auto got = backend.get(layout.gcSnapKey(state.snap_generation, snap_shard)))
            result.snap.emplace(snap_shard, decodeGcSnap(got->bytes));
        else
        {
            GcSnap empty;
            empty.snap_shard = snap_shard;
            empty.generation = state.snap_generation;
            result.snap.emplace(snap_shard, std::move(empty));
        }
    }

    /// Edges live in the TARGET's snap shard (in-degree is intra-shard); the expansion marker
    /// lives in the TREE's own home shard. std::map node references are stable across inserts.
    const auto shard_for = [&](const UInt128 & hash) -> GcSnap &
    {
        return result.snap.at(hashPrefixShard(hash, state.snap_shards));
    };

    /// 3. Fold each discovered root shard's journal records in (folded_cursor, shard_version],
    /// in journal (= insertion) order.
    for (const auto & [ns, root_shard] : result.root_shards)
    {
        const auto [root, manifest_token] = store->readShard(ns, root_shard);
        const String cursor_key = ns.string() + "/" + std::to_string(root_shard);
        const auto cursor_it = state.folded_cursor.find(cursor_key);
        const uint64_t cursor = cursor_it != state.folded_cursor.end() ? cursor_it->second : 0;

        for (size_t record_idx = 0; record_idx < root.journal.size(); ++record_idx)
        {
            const JournalRecord & record = root.journal[record_idx];
            if (record.at_version <= cursor || record.at_version > root.shard_version)
                continue;

            if (record.op == JournalRecord::Op::Add)
            {
                /// Last-op-wins root edge (spec §7): a republish of an existing ref produces
                /// consecutive Adds for the same (root_shard, part_name) with DIFFERENT trees and
                /// no Remove between - addRootEdge re-points the edge and returns the displaced
                /// old target if it zeroed.
                auto displaced = shard_for(record.tree_id).addRootEdge(cursor_key, record.ref_name, record.tree_id);
                result.transitioned.insert(result.transitioned.end(), displaced.begin(), displaced.end());

                /// Once-per-tree expansion: the FIRST '+' to a tree with no marker reads the tree
                /// once and adds its child-edge set (each edge into the CHILD's shard).
                GcSnap & tree_home = shard_for(record.tree_id);
                if (!tree_home.isExpanded(record.tree_id))
                {
                    std::vector<TreeEntry> entries;
                    bool tree_present = true;
                    try
                    {
                        entries = store->readTree(TreeId(u128ToHex(record.tree_id)));
                    }
                    catch (const Exception & e)
                    {
                        if (e.code() != ErrorCodes::FILE_DOESNT_EXIST)
                            throw;
                        /// DECISION (Task 6): a journal Add implies the tree was published, but the
                        /// object may legitimately be gone NOW - a later Remove for the same ref
                        /// plus an already COMPLETED prior round can have deleted it (in-order
                        /// folding visits the Add anyway). Lookahead: a Remove for the same
                        /// ref_name at a later at_version in THIS shard's remaining journal =>
                        /// skip the expansion (the edges would be stripped anyway; the marker
                        /// stays unset; no edges were added for a gone tree, so over-count-only
                        /// is preserved). No later Remove => the manifest claims a LIVE ref to a
                        /// missing tree => INV-NO-DANGLE surfaced; fail closed (propagate).
                        bool later_remove = false;
                        for (size_t ahead = record_idx + 1; ahead < root.journal.size(); ++ahead)
                        {
                            const JournalRecord & next = root.journal[ahead];
                            if (next.op == JournalRecord::Op::Remove && next.ref_name == record.ref_name
                                && next.at_version > record.at_version)
                            {
                                later_remove = true;
                                break;
                            }
                        }
                        if (!later_remove)
                            throw;
                        tree_present = false;
                    }

                    if (tree_present)
                    {
                        for (const TreeEntry & entry : entries)
                        {
                            switch (entry.placement)
                            {
                                case Placement::Blob:
                                    shard_for(entry.file_hash).addTreeEdge(record.tree_id, ObjectKind::Blob, entry.file_hash);
                                    break;
                                case Placement::Subtree:
                                    shard_for(entry.file_hash).addTreeEdge(record.tree_id, ObjectKind::Tree, entry.file_hash);
                                    break;
                                case Placement::PackSlice:
                                    shard_for(entry.pack_hash).addPackEdge(record.tree_id, entry.pack_hash);
                                    break;
                                case Placement::Inline:
                                    break;   /// embedded bytes - no separate object, no edge
                            }
                        }
                        tree_home.markExpanded(record.tree_id);
                    }
                }
            }
            else
            {
                /// The model's GFold rem only removes the root edge - the cascade strip of the
                /// tree's child edges belongs to the recheck/delete pipeline (Task 10).
                auto cands = shard_for(record.tree_id).removeRootEdge(cursor_key, record.ref_name);
                result.transitioned.insert(result.transitioned.end(), cands.begin(), cands.end());
            }
        }

        state.folded_cursor[cursor_key] = root.shard_version;
    }

    /// 4. Persist, durable-before-cursor: the new-generation snap objects go durable FIRST; only
    /// then does the gc/state CAS advance snap_generation + folded_cursor. A crash between the two
    /// loses nothing - the old generation stays authoritative and the next round re-folds the same
    /// records into a byte-identical generation object (idempotent replay).
    const uint64_t new_generation = state.snap_generation + 1;
    for (auto & [snap_shard, snap] : result.snap)
    {
        snap.generation = new_generation;
        const String snap_key = layout.gcSnapKey(new_generation, snap_shard);
        const String body = encodeGcSnap(snap);
        if (backend.putIfAbsent(snap_key, body) == PutOutcome::PreconditionFailed)
        {
            /// Generation objects are write-once. An existing object is either OUR replay (a prior
            /// attempt wrote the snap, then crashed or lost the cursor CAS) or a competing leader's
            /// fold. Byte-equality proves the same fold => proceed. Different bytes => the
            /// competing fold saw different journals => abort THIS round gracefully (split-brain
            /// duplicate work must be benign; the next round re-derives from the
            /// then-authoritative gc/state).
            const auto existing = backend.get(snap_key);
            if (!existing || existing->bytes != body)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS gc fold: concurrent fold diverged at {}; retry next round", snap_key);
        }
    }

    state.snap_generation = new_generation;
    if (backend.casPut(layout.gcStateKey(), encodeGcState(state), state_token) != CasOutcome::Committed)
        /// Another leader advanced gc/state under us. The new-generation snap we just wrote is
        /// orphaned garbage - harmless (write-once, never referenced by any cursor); full-GC
        /// reclaims it in M-F.
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc fold: gc/state moved during the fold (another leader advanced it); retry next round");

    return result;
}

void Gc::rememberObservation(const GcLease & lease)
{
    has_observation = true;
    last_seen_owner = lease.owner;
    last_seen_seq = lease.seq;
}

bool Gc::acquireOrRenewLease(GcState & state, Token & state_token)
{
    const String key = store->layout().gcStateKey();

    /// Bounded: at most 2 CAS attempts per call. Iteration 2 is reached only after a create or
    /// renew Conflict (the lost-steal path returns directly — a contender that loses the steal
    /// race must re-enter the observation protocol on its NEXT round, never retry blindly).
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto got = store->backend().get(key);

        if (!got)
        {
            /// gc/state is NEVER legally deleted once created — absent AFTER we observed a lease on
            /// it proves an out-of-model deletion (operator/tooling wipe). Fail closed: recreating a
            /// default state would silently reset round/fence_seq/snap_generation/cursors and could
            /// let already-condemned incarnations be reused as live.
            if (has_observation)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc/state vanished after being observed (owner {}, seq {})",
                    u128ToHex(last_seen_owner), last_seen_seq);

            /// Step 1: fresh pool — create gc/state holding our lease (create-if-absent CAS).
            GcState fresh;
            fresh.lease = GcLease{gc_id, 1};
            Token committed_token;
            if (store->backend().casPut(key, encodeGcState(fresh), /*expected*/ std::nullopt, &committed_token)
                == CasOutcome::Committed)
            {
                rememberObservation(fresh.lease);
                state = std::move(fresh);
                state_token = committed_token;
                return true;
            }
            /// A racer created it first — re-read and fall through to renew/observe/steal.
            continue;
        }

        GcState current = decodeGcState(got->bytes);

        if (current.lease.owner == gc_id)
        {
            /// Step 2: renew — seq advances, fence_seq does NOT (renewal is not a new epoch).
            GcState next = current;
            ++next.lease.seq;
            Token committed_token;
            if (store->backend().casPut(key, encodeGcState(next), got->token, &committed_token)
                == CasOutcome::Committed)
            {
                /// Remember the COMMITTED (owner, seq) so the next renew never self-triggers
                /// a steal-window anomaly.
                rememberObservation(next.lease);
                state = std::move(next);
                state_token = committed_token;
                return true;
            }
            /// Someone moved the lease under us (a steal happened) — re-read once; if the owner
            /// is still us, retry the renew once; else the foreign-owner branch records the
            /// observation and backs off (our remembered observation carries owner == gc_id, so
            /// it can never match a foreign owner and turn this into a steal).
            continue;
        }

        /// Foreign owner.
        const bool incumbent_renewed = !has_observation
            || current.lease.owner != last_seen_owner
            || current.lease.seq != last_seen_seq;
        if (incumbent_renewed)
        {
            /// Step 3: the incumbent is alive (or this is our first sight of this lease) —
            /// record the observation and back off. Eligibility to steal requires seeing the
            /// SAME (owner, seq) across one whole prior round attempt of OURS.
            rememberObservation(current.lease);
            return false;
        }

        /// Step 4: the incumbent did not renew across our full observation window — steal.
        /// fence_seq++ opens a new leadership epoch: the new leader's retire/outcome paths
        /// (<round>.<fence_seq>) never collide with the old leader's (append-by-unique-path).
        GcState next = current;
        next.lease.owner = gc_id;
        ++next.lease.seq;
        ++next.fence_seq;
        Token committed_token;
        if (store->backend().casPut(key, encodeGcState(next), got->token, &committed_token)
            == CasOutcome::Committed)
        {
            rememberObservation(next.lease);
            state = std::move(next);
            state_token = committed_token;
            return true;
        }

        /// A racer stole (or the incumbent revived and renewed) first — our CAS carried the token
        /// we read BEFORE its write, so it lost. Re-read, record what is there now, back off.
        if (const auto reread = store->backend().get(key))
            rememberObservation(decodeGcState(reread->bytes).lease);
        /// else: gc/state vanished (never legal — it is never deleted). KEEP the stale observation,
        /// so the next round's absent-after-observed check above fails closed (CORRUPTED_DATA)
        /// instead of recreating a default state.
        return false;
    }

    /// Two CAS attempts exhausted (create/renew conflicted twice) — heavy contention on gc/state
    /// means another leader is moving it; back off, this round.
    return false;
}

}
