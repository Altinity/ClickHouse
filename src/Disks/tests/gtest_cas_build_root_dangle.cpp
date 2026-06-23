#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/tests/cas_test_helpers.h>

#include <string>

using namespace DB::Cas;
using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;

namespace
{

/// root_shards=1 keeps both refs in one root shard / one snap shard, mirroring the B140 repro.
StorePtr openTestStore(std::shared_ptr<InMemoryBackend> & out_backend)
{
    out_backend = std::make_shared<InMemoryBackend>();
    return Store::open(out_backend, PoolConfig{.pool_prefix = "p", .root_shards = 1});
}

size_t runGcToFixpoint(Gc & gc, size_t max_rounds = 64)
{
    size_t rounds = 0;
    for (; rounds < max_rounds; ++rounds)
    {
        RoundReport rep;
        try
        {
            rep = gc.runRegularRound();
        }
        catch (const DB::Exception &)
        {
            break;
        }
        if (!rep.acquired_lease)
            continue;
        if (rep.candidates == 0 && rep.deleted == 0 && rep.absent == 0
            && rep.replaced == 0 && rep.spared == 0)
            break;
    }
    return rounds;
}

TreeEntry blobEntry(const String & name, const String & payload)
{
    TreeEntry e;
    e.name = name;
    e.placement = Placement::Blob;
    e.file_hash = u128Of(payload);
    e.file_size = payload.size();
    return e;
}

}

/// B171 build-root / precommit, RED repro of the B140-dangle at unit level driven entirely through the
/// public Build/Store/Gc API (no snap injection):
///
///   Build A uploads blob P and publishes refA -> t1 -> { data.bin: P }. A is then RELEASED (dtor),
///   retiring its build_seq so the GC watermark `min_active` advances PAST A. P now carries A's
///   `cas_owner` and is no longer protected by any in-flight build.
///
///   Build B starts and ADOPTS the same blob P via tokenless evidence (adoptEvidence — the cross-node
///   adopt case), assembles t2 -> { other.bin: P }, and `precommit(t2)` — which publishes a durable
///   build-root ref so GC's fold lifts the in-degree of P's closure.
///
///   refA is dropped + watermark renewed; GC runs to fixpoint. P is protected by B's precommit edge,
///   so GC must NOT delete it. Build B then publishes refB -> t2 successfully.
///
/// THE POSITIVE INVARIANT: the whole flow must succeed AND P must survive, because B's precommit pins
/// P's closure across A's retire + GC (B171 two-phase commit; `checkAndResolveDeps` proves closure
/// present at publish time).
/// Spec: docs/superpowers/specs/2026-06-18-ca-build-root-precommit-cpp-impl.md (§F.1)
TEST(CasBuildRootDangle, SharedBlobSurvivesSourceDropDuringBuild)
{
    std::shared_ptr<InMemoryBackend> backend;
    auto s = openTestStore(backend);
    const RootNamespace ns{"srv1/tbl"};
    const String P = "shared-blob-payload-P";

    /// Build A: upload P, publish refA -> t1 -> { data.bin: P }, then release A so its build_seq
    /// retires and min_active advances past it.
    {
        auto a = s->startBuild({});
        a->putBlob(idOf(P), BlobSource::fromString(P));
        const TreeId t1 = a->putTree({blobEntry("data.bin", P)});
        a->publish(ns, "refA", t1, {});
    }
    s->renewWatermarkOnce();   /// A is gone; min_active now advances past A's build_seq

    /// Build B: adopt the SAME blob P (cross-node adopt — tokenless evidence via adoptEvidence), assemble
    /// t2, and precommit it. The precommit is meant to pin P's closure for the duration of the build.
    auto b = s->startBuild({});
    const TreeEntry pe = blobEntry("other.bin", P);
    b->adoptEvidence(pe);
    const TreeId t2 = b->putTree({pe});
    b->precommit(t2);

    /// The source ref disappears, and the watermark is renewed so the closure looks collectable.
    s->dropRef(ns, "refA");
    s->renewWatermarkOnce();

    /// GC to fixpoint. Under current code this deletes P (inDeg 0 + retired owner, precommit a no-op).
    Gc gc(s, u128Of("gc-b171"));
    runGcToFixpoint(gc);

    /// Build B commits refB -> t2. Should succeed end-to-end; if it throws (e.g. ABORTED because the
    /// blob is gone) that is itself the RED outcome.
    ASSERT_NO_THROW(b->publish(ns, "refB", t2, {}))
        << "B171: Build B's commit must succeed — the precommit should have kept P alive";

    /// The blob B references must still be present (no dangle), and refB must resolve.
    ASSERT_TRUE(backend->head(s->layout().blobKey(idOf(P))).exists)
        << "B171-dangle: GC deleted the shared blob P that Build B adopted — its cas_owner was the "
        << "retired Build A and the stub precommit published no build-root edge, so inDeg(P) hit 0 "
        << "and the single content-delete site removed it. refB now dangles.";
    ASSERT_TRUE(s->resolveRef(ns, "refB").has_value())
        << "B171: refB must resolve to its committed manifest";
}

/// B171 INV-COMMIT-FAILCLOSED: even if the build-root precommit is PREMATURELY RECLAIMED mid-build
/// (e.g. a live build whose watermark renewer froze and was falsely judged dead), the real commit must
/// NEVER publish a table ref over a missing dependency. It must fail closed — abort — never dangle.
///
/// Setup mirrors the primary repro: Build A publishes refA -> t1 -> { data.bin: P } then retires; Build
/// B adopts P, assembles t2 -> { other.bin: P }, and precommits t2 (a real build-root edge now protects
/// P). We then SIMULATE the premature reclaim by manually dropping the build-root ref (as GC's reclaim
/// would) AND dropping refA, then renew the watermark and run GC to fixpoint. With P's only protection
/// (the precommit edge) gone and its owner retired, GC deletes P. Build B's publish must now ABORT
/// (`checkAndResolveDeps` finds the adopted blob absent and not re-creatable) instead of committing a dangle.
/// Spec: docs/superpowers/specs/2026-06-18-ca-build-root-precommit-design.md (§4.4, §4.6)
TEST(CasBuildRootDangle, PrematureReclaimCommitFailsClosed)
{
    std::shared_ptr<InMemoryBackend> backend;
    auto s = openTestStore(backend);
    const RootNamespace ns{"srv1/tbl"};
    const String P = "shared-blob-payload-P-reclaim";

    /// Build A: upload P, publish refA -> t1, retire A so min_active advances past it.
    {
        auto a = s->startBuild({});
        a->putBlob(idOf(P), BlobSource::fromString(P));
        const TreeId t1 = a->putTree({blobEntry("data.bin", P)});
        a->publish(ns, "refA", t1, {});
    }
    s->renewWatermarkOnce();

    /// Build B: adopt P via tokenless evidence, assemble t2, precommit it (build-root edge protects P).
    auto b = s->startBuild({});
    const TreeEntry pe2 = blobEntry("other.bin", P);
    b->adoptEvidence(pe2);
    const TreeId t2 = b->putTree({pe2});
    b->precommit(t2);

    /// SIMULATE premature reclaim: remove the precommit edge exactly as GC reclaim would —
    /// erase refs[build_seq] + append a Remove journal record on the precommit shard. The precommit
    /// namespace is `<server_hex>/_precommits`, and since B171's fix the precommit ref name is the build_seq
    /// and the shard is shardOf(build_seq) (sharded exactly like a table namespace). We reconstruct the
    /// RootNamespace from the public server_id and drive the shard manifest CAS directly through the
    /// backend codec.
    const RootNamespace precommit_ns{u128ToHex(s->poolConfig().server_id) + "/_precommits"};
    const String precommit_ref = std::to_string(b->buildSeq());
    {
        const String key = s->layout().rootShardKey(precommit_ns, s->shardOf(precommit_ref));
        const auto got = backend->get(key);
        ASSERT_TRUE(got.has_value()) << "precommit shard manifest must exist before reclaim";
        DB::Cas::RootShard root = DB::Cas::decodeRootShard(got->bytes);
        auto it = root.refs.find(precommit_ref);
        ASSERT_NE(it, root.refs.end()) << "precommit ref must exist before reclaim";
        const DB::UInt128 part_tree = it->second.tree_id;
        root.refs.erase(it);
        /// Bump shard_version like mutateShard does (it increments AFTER the lambda); the journal record
        /// pins at the NEW version, so the decode invariant at_version <= shard_version holds.
        ++root.shard_version;
        root.journal.push_back(JournalRecord{
            .op = JournalRecord::Op::Remove, .ref_name = precommit_ref, .tree_id = part_tree,
            .at_version = root.shard_version, .closure = {}});
        backend->casPut(key, DB::Cas::encodeRootShard(root), got->token);
    }

    /// Drop the source ref too, renew the watermark, and run GC: P is now unprotected (no precommit, no
    /// table ref, retired owner) and gets collected. The reclaim is a CASCADE — t2 (the manifest tree,
    /// now an orphan), then t1, then the leaf blob P are condemned over successive rounds, with empty
    /// rounds in between. The shared runGcToFixpoint helper stops at the first empty round, so it can
    /// halt mid-cascade; here we drive a fixed, generous number of rounds and let it settle so P is
    /// reliably collected before the commit attempt.
    s->dropRef(ns, "refA");
    s->renewWatermarkOnce();
    Gc gc(s, u128Of("gc-b171-reclaim"));
    for (int round = 0; round < 32; ++round)
    {
        try { gc.runRegularRound(); }
        catch (const DB::Exception &) { break; }
    }

    /// The shared blob must be GONE (the premature reclaim let GC collect it).
    ASSERT_FALSE(backend->head(s->layout().blobKey(idOf(P))).exists)
        << "premature-reclaim setup invalid: P should have been collected after losing its precommit";

    /// FAIL-CLOSED: Build B's commit must THROW (ABORTED — dependency lost), never silently publish a
    /// dangle. `checkAndResolveDeps` re-proves the closure present and aborts when P is missing.
    ASSERT_ANY_THROW(b->publish(ns, "refB", t2, {}))
        << "B171 INV-COMMIT-FAILCLOSED: publish must abort over a missing dependency, not commit a dangle";

    /// And the dangle must NOT have been committed: P still absent, refB never resolved.
    ASSERT_FALSE(backend->head(s->layout().blobKey(idOf(P))).exists)
        << "B171: the missing blob must stay missing — commit must not fabricate it";
    ASSERT_FALSE(s->resolveRef(ns, "refB").has_value())
        << "B171: refB must NOT be committed when its closure is missing (fail-closed)";
}

/// B171 INV-BUILDROOT-RECLAIM (§C.3, design §4.3): GC AUTOMATICALLY reclaims an ABANDONED precommit and
/// then collects its exclusively-owned closure — no manual ref drop required (that drop is now GC's job).
///
/// Build B precommits a manifest naming a blob that is referenced by NO table ref. B is then RETIRED
/// (released; its build_seq leaves the active set) and the watermark renewed so `min_active` advances
/// past B. GC, while folding B's precommit shard, derives the server from the namespace and build_seq
/// from the ref NAME, judges B dead (build_seq < min_active), and RECLAIMS the precommit: it drops
/// refs[build_seq] + journal Remove. The next fold releases the edges; the exclusively-owned blob hits
/// in-degree 0 and is collected over the cascade rounds. We drive a multi-round loop (NOT the
/// first-empty-round helper, which would halt mid-cascade between the reclaim and the leaf delete).
/// Spec: docs/superpowers/specs/2026-06-18-ca-build-root-precommit-cpp-impl.md (§F.4)
TEST(CasBuildRoot, AbandonedPrecommitReclaimed)
{
    std::shared_ptr<InMemoryBackend> backend;
    auto s = openTestStore(backend);
    const String P = "abandoned-precommit-only-blob";
    const String blob_key = s->layout().blobKey(idOf(P));

    /// Build B precommits a manifest naming P; P has NO table ref — the precommit edge is its only
    /// in-degree. Then B is released so its build_seq retires.
    uint64_t build_seq = 0;
    {
        auto b = s->startBuild({});
        build_seq = b->buildSeq();
        b->putBlob(idOf(P), BlobSource::fromString(P));
        const TreeId t = b->putTree({blobEntry("data.bin", P)});
        b->precommit(t);
    }
    s->renewWatermarkOnce();   /// B is gone; min_active now advances past B's build_seq -> precommit abandoned

    /// Sanity: the precommit ref exists before GC reclaim. Since B171's fix the ref name IS the build_seq
    /// and the shard is shardOf(build_seq) (sharded like a table namespace).
    const RootNamespace precommit_ns{u128ToHex(s->poolConfig().server_id) + "/_precommits"};
    const String precommit_ref = std::to_string(build_seq);
    const String shard_key = s->layout().rootShardKey(precommit_ns, s->shardOf(precommit_ref));
    {
        const auto got = backend->get(shard_key);
        ASSERT_TRUE(got.has_value()) << "precommit shard manifest must exist before reclaim";
        ASSERT_TRUE(DB::Cas::decodeRootShard(got->bytes).refs.contains(precommit_ref))
            << "precommit ref must exist before reclaim";
    }
    ASSERT_TRUE(backend->head(blob_key).exists) << "P must exist before GC reclaim";

    /// Drive GC over many rounds: reclaim the precommit, then cascade the closure to deletion. Renew the
    /// watermark each round so B keeps being judged DEAD (build_seq < min_active) — the reclaim verdict.
    Gc gc(s, u128Of("gc-b171-reclaim-auto"));
    for (int round = 0; round < 32; ++round)
    {
        s->renewWatermarkOnce();
        try { gc.runRegularRound(); }
        catch (const DB::Exception &) { break; }
    }

    /// INV-PRECOMMIT-RECLAIM: the precommit ref for B is gone (the precommit was reclaimed).
    const auto after = backend->get(shard_key);
    if (after.has_value())
        ASSERT_FALSE(DB::Cas::decodeRootShard(after->bytes).refs.contains(precommit_ref))
            << "GC must have AUTOMATICALLY reclaimed the abandoned precommit ref";

    /// And the exclusively-owned blob is eventually deleted (no longer protected by anything).
    ASSERT_FALSE(backend->head(blob_key).exists)
        << "the exclusively-precommit-owned blob must be collected after the precommit is reclaimed";
}
