#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Common/Exception.h>
#include <base/defines.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int ABORTED;
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

uint64_t retiredLogicalSize(ObjectKind kind, uint64_t object_size, uint64_t blob_header_len)
{
    if (kind != ObjectKind::Blob)
        return object_size;   /// trees/packs account whole-object (sizes are GC bookkeeping)
    if (object_size < blob_header_len)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS gc retire: blob object of {} bytes is smaller than the pool's fixed blob header ({} bytes)",
            object_size, blob_header_len);
    return object_size - blob_header_len;
}

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

    /// R1: fold the journals into a new durable snap generation (cursors advance only after).
    const FoldResult folded = fold(state, state_token);

    /// R2 consumes the EXACT (state, token) the fold's CAS committed - THREADED through, never
    /// re-read. A post-fold re-read would open a zombie window: a lease steal landing between the
    /// fold CAS and the re-read hands this (now stale) leader the thief's state with its bumped
    /// fence_seq, letting stale-snap retire sets land inside the thief's epoch paths - and the
    /// round CAS would ride the post-steal token. With the committed token threaded, any
    /// intervening steal makes the retire round CAS Conflict => ABORTED (fail closed).
    ///
    /// R2: HEAD-observe each candidate's current token, write the round's retire sets, advance
    /// .round. Candidates are derived STATELESSLY from the durable snap (the model GRetire guard
    /// `present ∧ everEdged ∧ InDeg = 0` over zeroInDegreeKnown) - never from the in-memory fold
    /// transitions, so a crash-replayed round re-derives the same set. `folded.transitioned` is
    /// only a health cross-check; the counts can differ legitimately (e.g. a node zeroed by an
    /// EARLIER round's fold is in zeroInDegreeKnown but did not transition in THIS fold).
    const std::map<uint64_t, RetiredSet> retired = retire(state, state_token, folded.snap);
    report.round = state.round;
    for (const auto & [snap_shard, set] : retired)
        report.candidates += set.entries.size();

    /// Tasks 8-12: fence / recheck / cascade / trim
    return report;
}

std::map<uint64_t, RetiredSet> Gc::retire(GcState & state, Token & state_token,
                                          const std::map<uint64_t, GcSnap> & snap)
{
    const Layout & layout = store->layout();
    Backend & backend = store->backend();

    /// Belt-and-braces for the no-re-read contract: the state retire runs under must be the one
    /// OUR lease committed (the caller threads the fold-committed state; a thief's state would
    /// carry a foreign owner).
    chassert(state.lease.owner == gc_id);

    /// state.round = "highest round whose retire sets are durable" => THIS round is the next one.
    const uint64_t round = state.round + 1;

    /// 1. Observe. One HEAD per candidate captures the CURRENT incarnation token - the only token
    /// the eventual delete may carry (exact-token delete, spec §7).
    ///
    /// Cross-round duplicates are possible and benign: a prior round may have retired the same
    /// (hash, token) and crashed before its recheck/outcomes - this round re-derives the candidate
    /// (still known + zero + present) and writes ANOTHER entry under its own unique path. The
    /// RetireView unions all present sets (the same condemned token twice), the delete is
    /// exact-token idempotent, and stale sets are cleaned by Task 9: a round's retired-set objects
    /// are deleted after its outcome log is durable -
    /// so no cross-round dedup is attempted here. Within one round a candidate appears once by
    /// construction (zeroInDegreeKnown yields unique nodes; a node lives in exactly one shard).
    std::map<uint64_t, RetiredSet> retired;
    for (const auto & [snap_shard, shard_snap] : snap)
    {
        for (const Candidate & candidate : shard_snap.zeroInDegreeKnown())
        {
            const HeadResult observed = backend.head(objectKey(layout, candidate.kind, candidate.hash));
            if (!observed.exists)
                /// No token to condemn - never fabricate one (fail closed). The object is already
                /// gone: a crashed prior round's landed delete, or debris; the recheck has nothing
                /// to do for it and the writer-facing barrier has nothing to bar.
                continue;

            RetiredEntry entry;
            entry.kind = candidate.kind;
            entry.hash = candidate.hash;
            entry.token = observed.token;
            entry.size = retiredLogicalSize(candidate.kind, observed.size, store->poolMeta().blob_header_len);
            retired[hashPrefixShard(candidate.hash, state.snap_shards)].entries.push_back(std::move(entry));
        }
    }

    /// 2. Write each shard's set - append-by-unique-path: the path is unique per
    /// (round, fence_seq, shard) and written ONCE (write-once, never rewritten). An EMPTY shard
    /// writes nothing (no empty objects; RetireView handles absent keys), and an all-empty round
    /// still advances .round below - the round RAN; the common idle case, with a trivially empty
    /// recheck.
    ///
    /// AN OCCUPIED PATH IS OUR OWN CRASHED PRIOR ATTEMPT. With the fold-committed state threaded
    /// (no re-read window) and fence_seq isolation (a lease steal bumps fence_seq, changing every
    /// path), nobody but THIS leader in THIS epoch ever writes this key. Byte-identical content is
    /// the exact replay - proceed. DIVERGENT content is the same round derived from an OLDER snap:
    /// the crash window admitted new journal activity or token displacement before this replay -
    /// ADOPT the occupant as this round's set (never rewrite). Safety: the occupant's entries were
    /// derived from an older durable snap of the same pool and are conservative - retired != dead,
    /// the recheck re-validates in-degree through the fence before any delete, and a stale
    /// observed token just makes the delete 412 => outcome replaced. Candidates the occupant lacks
    /// are NOT lost: zeroInDegreeKnown is stateless, the next round re-derives and retires them.
    /// Aborting here instead would WEDGE this leader forever - renewal never bumps fence_seq, so
    /// every later round would re-hit the same occupied path (the same problem class the fold's
    /// generation probe-upward solves). Fail closed (ABORTED) only when the occupant cannot be
    /// decoded - corrupt state must never be adopted as a delete input.
    for (auto & [snap_shard, set] : retired)
    {
        const String key = layout.retiredKey(round, state.fence_seq, snap_shard);
        const String body = encodeRetiredSet(set);
        if (backend.putIfAbsent(key, body) == PutOutcome::PreconditionFailed)
        {
            const auto existing = backend.get(key);
            if (!existing)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS gc retire: retired set at {} vanished between putIfAbsent and read", key);
            if (existing->bytes != body)
            {
                try
                {
                    set = decodeRetiredSet(existing->bytes);
                }
                catch (const Exception & e)
                {
                    throw Exception(ErrorCodes::ABORTED,
                        "CAS gc retire: undecodable occupant at {} cannot be adopted: {}", key, e.message());
                }
            }
        }
    }

    /// 3. The durable "retire phase complete" marker: ONE gc/state CAS advances .round, strictly
    /// AFTER the sets are durable (INV-MONOTONE-GC ordering: a writer whose RetireView refreshes
    /// at the new round is guaranteed to see the entries). A crash between step 2 and here leaves
    /// only re-derivable durable state - the replay adopts the byte-equal sets and re-runs this CAS.
    GcState next = state;
    next.round = round;
    Token committed_token;
    if (backend.casPut(layout.gcStateKey(), encodeGcState(next), state_token, &committed_token)
        != CasOutcome::Committed)
    {
        /// Another leader moved gc/state under us. Bounded and fail-closed: this leader's round
        /// attempt ends here (the lease makes contention rare); the re-read only sharpens the
        /// message - a round already at/past ours means a competitor COMPLETED this retire
        /// (duplicate work detected), anything else is a plain interleaving.
        const auto current = backend.get(layout.gcStateKey());
        if (current && decodeGcState(current->bytes).round >= round)
            throw Exception(ErrorCodes::ABORTED,
                "CAS gc retire: another leader already completed retire round {}; retry next round", round);
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc retire: gc/state moved during retire (another leader advanced it); retry next round");
    }
    state = std::move(next);
    state_token = committed_token;
    return retired;
}

Gc::FoldResult Gc::fold(GcState & state, Token & state_token)
{
    /// M-C3 limitation, fail closed: last-op-wins displacement is INTRA-shard only (addRootEdge
    /// re-points the edge inside ONE GcSnap). With the displaced and displacing trees in DIFFERENT
    /// snap shards, a republish would leave the old root edge behind forever - a permanent leak
    /// (over-count-safe, but silently broken sharding). Raising the constant needs a design first:
    /// per-ref displacement records, or root edges sharded by REF identity rather than target hash.
    if (state.snap_shards != 1)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "CAS gc fold: snap_shards = {} - cross-shard last-op-wins displacement is undesigned; "
            "snap_shards must be 1 in M-C3", state.snap_shards);

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
                        /// DECISION (Task 6, refined by review): a journal Add implies the tree was
                        /// published, but the object may legitimately be gone NOW - this record's
                        /// edge was DISPLACED later in the journal and an already COMPLETED
                        /// competing round deleted the displaced tree (in-order folding still
                        /// visits the Add). Displacement proof is a later record for the SAME
                        /// ref_name: a Remove drops the edge, and a later Add re-points it
                        /// (last-op-wins republish - the COMMON mutation path). Either => skip the
                        /// expansion (the edges would be stripped/displaced anyway; the marker
                        /// stays unset; no edges were added for a gone tree, so over-count-only is
                        /// preserved). No later record for the ref => the manifest claims a LIVE
                        /// ref to a missing tree => INV-NO-DANGLE surfaced; fail closed
                        /// (propagate). The op check stays EXPLICIT: a future journal op (e.g. the
                        /// model's fence records) must NOT count as displacement.
                        bool displaced_later = false;
                        for (size_t ahead = record_idx + 1; ahead < root.journal.size(); ++ahead)
                        {
                            const JournalRecord & next = root.journal[ahead];
                            if ((next.op == JournalRecord::Op::Remove || next.op == JournalRecord::Op::Add)
                                && next.ref_name == record.ref_name && next.at_version > record.at_version)
                            {
                                displaced_later = true;
                                break;
                            }
                        }
                        if (!displaced_later)
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
                /// LOAD-BEARING routing: the Remove routes via the journal record's DROP-TIME
                /// tree_id, which by in-order folding is exactly the shard where the live
                /// (root_shard, part_name) edge resides - any earlier displacing Add already
                /// re-pointed the edge when ITS record folded (intra-shard while snap_shards == 1).
                auto cands = shard_for(record.tree_id).removeRootEdge(cursor_key, record.ref_name);
                result.transitioned.insert(result.transitioned.end(), cands.begin(), cands.end());
            }
        }

        state.folded_cursor[cursor_key] = root.shard_version;
    }

    /// 4. Persist, durable-before-cursor: the new-generation snap objects go durable FIRST; only
    /// then does the gc/state CAS advance snap_generation + folded_cursor. A crash between the two
    /// loses nothing - the old generation stays authoritative.
    ///
    /// THE WRITE GENERATION PROBES UPWARD. Generation objects are write-once, and a generation key
    /// occupied by DIFFERENT bytes can never be reused: a fold that wrote its snaps but lost the
    /// gc/state CAS leaves an orphan at snap_generation+1, and once ANY new journal record arrives
    /// every later fold covers a larger range - its bytes can never match the orphan again, so a
    /// FIXED write generation would abort forever (a permanent GC wedge). Per probed generation:
    /// every shard Done or byte-equal => adopt it (byte-equality keeps crash-replay adoption: the
    /// orphan of OUR identical earlier attempt is reused, not abandoned); ANY shard divergent =>
    /// abandon the generation (partially written objects there are orphans - harmless, write-once,
    /// never referenced by any cursor; full-GC reclaims them in M-F) and retry one higher. The
    /// adopted generation is > the snap_generation we read and the gc/state CAS below guards the
    /// advance, so INV-MONOTONE-GC holds; generations need NOT be dense - the gc/state pointer is
    /// authoritative, a reader never enumerates generations.
    constexpr uint64_t max_generation_probes = 1000;   /// runaway brake, never a legitimate bound
    uint64_t adopted_generation = 0;
    for (uint64_t generation = state.snap_generation + 1; ; ++generation)
    {
        if (generation > state.snap_generation + max_generation_probes)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS gc fold: no adoptable snap generation within {} probes above {} (runaway divergence)",
                max_generation_probes, state.snap_generation);

        bool diverged = false;
        for (auto & [snap_shard, snap] : result.snap)
        {
            snap.generation = generation;
            const String snap_key = layout.gcSnapKey(generation, snap_shard);
            const String body = encodeGcSnap(snap);
            if (backend.putIfAbsent(snap_key, body) == PutOutcome::PreconditionFailed)
            {
                const auto existing = backend.get(snap_key);
                if (!existing || existing->bytes != body)
                {
                    diverged = true;
                    break;
                }
                /// byte-equal: OUR replay (a prior attempt crashed or lost the cursor CAS) - adopt.
            }
        }
        if (!diverged)
        {
            adopted_generation = generation;
            break;
        }
    }

    state.snap_generation = adopted_generation;
    Token committed_token;
    if (backend.casPut(layout.gcStateKey(), encodeGcState(state), state_token, &committed_token)
        != CasOutcome::Committed)
        /// Another leader advanced gc/state under us. The new-generation snap we just wrote is
        /// orphaned garbage - harmless (write-once, never referenced by any cursor); full-GC
        /// reclaims it in M-F.
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc fold: gc/state moved during the fold (another leader advanced it); retry next round");

    /// The committed token is threaded to retire - the round CAS rides exactly this incarnation,
    /// so any intervening lease steal Conflicts there (no re-read window, see runRegularRound).
    state_token = committed_token;
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
