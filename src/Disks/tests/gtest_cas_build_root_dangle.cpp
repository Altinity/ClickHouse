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
    return Store::open(out_backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1});
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

ManifestEntry blobEntry(const String & name, const String & payload)
{
    ManifestEntry e;
    e.path = name;
    e.placement = EntryPlacement::Blob;
    e.ref = DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(u128Of(payload))};

    e.blob_size = payload.size();
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
    const RootNamespace ns{"test/tbl"};
    const String P = "shared-blob-payload-P";

    /// Build A: upload P, publish refA -> manifest -> { data.bin: P }, then release A so its build_seq
    /// retires and min_active advances past it.
    {
        BuildInfo info;
        info.intended_ref = ns.string() + "/refA";
        auto a = s->startBuild(info);
        a->putBlob(idOf(P), BlobSource::fromString(P));
        const ManifestId id = a->stageManifest({blobEntry("data.bin", P)});
        a->precommitAdd(ns, "refA", id);
        a->promote(ns, "refA", a->buildId(), id);
    }
    s->renewWatermarkOnce();   /// A is gone; min_active now advances past A's build_seq

    /// Build B: adopt the SAME blob P (cross-node adopt — tokenless evidence via adoptEvidence), assemble
    /// its manifest, and precommitAdd it. The precommit pins P's closure (fold +1 edge) for the build.
    BuildInfo binfo;
    binfo.intended_ref = ns.string() + "/refB";
    auto b = s->startBuild(binfo);
    const ManifestEntry pe = blobEntry("other.bin", P);
    b->adoptEvidence(pe);
    const ManifestId t2 = b->stageManifest({pe});
    b->precommitAdd(ns, "refB", t2);

    /// The source ref disappears, and the watermark is renewed so the closure looks collectable.
    s->dropRef(ns, "refA");
    s->renewWatermarkOnce();

    /// GC to fixpoint. P must survive: the live precommit binding for refB activates a +1 blob edge on
    /// P during the fold, so P never reaches in-degree 0 (B171 two-phase commit).
    Gc gc(s, u128Of("gc-b171"));
    runGcToFixpoint(gc);

    /// Build B commits refB by promoting its precommit. Should succeed end-to-end; if it throws (e.g.
    /// ABORTED because the blob is gone) that is itself the RED outcome.
    ASSERT_NO_THROW(b->promote(ns, "refB", b->buildId(), t2))
        << "B171: Build B's promote must succeed — the precommit should have kept P alive";

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
    const RootNamespace ns{"test/tbl"};
    const String P = "shared-blob-payload-P-reclaim";

    /// Build A: upload P, publish refA -> manifest, retire A so min_active advances past it.
    {
        BuildInfo info;
        info.intended_ref = ns.string() + "/refA";
        auto a = s->startBuild(info);
        a->putBlob(idOf(P), BlobSource::fromString(P));
        const ManifestId id = a->stageManifest({blobEntry("data.bin", P)});
        a->precommitAdd(ns, "refA", id);
        a->promote(ns, "refA", a->buildId(), id);
    }
    s->renewWatermarkOnce();

    /// Build B: adopt P via tokenless evidence, assemble its manifest, precommitAdd it (the precommit
    /// owner binding for refB now protects P with a +1 fold edge).
    BuildInfo binfo;
    binfo.intended_ref = ns.string() + "/refB";
    auto b = s->startBuild(binfo);
    const ManifestEntry pe2 = blobEntry("other.bin", P);
    b->adoptEvidence(pe2);
    const ManifestId t2 = b->stageManifest({pe2});
    b->precommitAdd(ns, "refB", t2);

    /// SIMULATE a premature reclaim having already collected P: had the precommit binding been wrongly
    /// reclaimed with no other owner, GC would condemn+delete P's closure. Reproduce that END STATE
    /// directly by deleting P's blob object. (The durable ref-log stream is owned by the live writer, so a
    /// RAW removal append would collide with the writer's own `RefTxnId` sequence allocation on the next
    /// flush; the property under test is the COMMIT gate's fail-closed behavior against a missing
    /// dependency, not the reclaim mechanics -- so we go straight to the reclaimed state.)
    {
        const String pkey = s->layout().blobKey(idOf(P));
        const HeadResult h = backend->head(pkey);
        ASSERT_TRUE(h.exists) << "P must be present before the simulated reclaim";
        ASSERT_EQ(backend->deleteExact(pkey, h.token).kind, DeleteOutcome::Kind::Deleted);
    }
    /// Drop the source ref too (the state a real premature reclaim leaves: P unprotected and gone).
    s->dropRef(ns, "refA");
    s->renewWatermarkOnce();

    /// The shared blob must be GONE (the premature reclaim collected it).
    ASSERT_FALSE(backend->head(s->layout().blobKey(idOf(P))).exists)
        << "premature-reclaim setup invalid: P should have been collected after losing its precommit";

    /// FAIL-CLOSED: Build B's promote must THROW (ABORTED — the precommit binding was reclaimed and/or
    /// the dependency is gone), never silently publish a dangle. promote re-proves the precommit binding
    /// present and every blob leaf present/recreatable, and aborts when P is missing.
    ASSERT_ANY_THROW(b->promote(ns, "refB", b->buildId(), t2))
        << "B171 INV-COMMIT-FAILCLOSED: promote must abort over a missing dependency, not commit a dangle";

    /// And the dangle must NOT have been committed: P still absent, refB never resolved.
    ASSERT_FALSE(backend->head(s->layout().blobKey(idOf(P))).exists)
        << "B171: the missing blob must stay missing — commit must not fabricate it";
    ASSERT_FALSE(s->resolveRef(ns, "refB").has_value())
        << "B171: refB must NOT be committed when its closure is missing (fail-closed)";
}

/// (The GC-reclaim test `CasBuildRoot.AbandonedPrecommitReclaimed` -- which asserted GC AUTOMATICALLY
/// reclaims an abandoned precommit of a judged-dead build and then collects its closure -- was removed
/// with the snapshot+log ref model. Per spec §Responsibility Boundary, reclaiming an abandoned precommit
/// is now the WRITER's job (it appends the exact `owner_transition` removal on recovery); GC never scans
/// for or removes precommit bindings, and there is no mutable shard journal to append a `PrecommitRemove`
/// into. The `precommitRemovalAppended` shard-journal probe it shared with `LivePrecommitNotReclaimed`
/// went with it.)

/// B8 CONSERVATISM (liveness-correctness guard): a live in-flight build's precommit binding (and its
/// pinned blobs) must survive a full GC run, and the build must still be able to promote it. In the
/// snapshot+log model GC never reclaims a precommit at all, so this is purely a liveness pin: the live
/// precommit's `+1` fold edge keeps its exclusively-owned blob alive across GC.
TEST(CasBuildRoot, LivePrecommitNotReclaimed)
{
    std::shared_ptr<InMemoryBackend> backend;
    auto s = openTestStore(backend);
    const RootNamespace ns{"test/tbl"};
    const String Q = "live-build-blob-payload-Q";

    /// Build B stays ALIVE: upload Q, assemble, precommitAdd — and we DO NOT retire its seq. So
    /// `min_active <= build_seq` (B is in-flight) and the watermark keeps a live, advancing seq.
    BuildInfo binfo;
    binfo.intended_ref = ns.string() + "/refLive";
    auto b = s->startBuild(binfo);
    b->putBlob(idOf(Q), BlobSource::fromString(Q));
    const ManifestId t = b->stageManifest({blobEntry("data.bin", Q)});
    b->precommitAdd(ns, "refLive", t);
    s->renewWatermarkOnce();
    ASSERT_LE(s->minActive(), b->buildSeq()) << "precondition: B must be in-flight (min_active <= seq)";

    /// GC to fixpoint while B is live.
    Gc gc(s, u128Of("gc-b8-live"));
    runGcToFixpoint(gc);

    /// Q must still be present (the live precommit's +1 edge pins it across GC).
    ASSERT_TRUE(backend->head(s->layout().blobKey(idOf(Q))).exists)
        << "B8 conservatism: the live precommit must keep its blob alive across GC";

    /// B can still commit (the precommit is intact).
    ASSERT_NO_THROW(b->promote(ns, "refLive", b->buildId(), t))
        << "B8 conservatism: a live build must still be able to promote its untouched precommit";
}
