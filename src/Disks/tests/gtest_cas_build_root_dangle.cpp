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
///   Build B starts and ADOPTS the same blob P via the dedup path (reuseBlob, body_recreatable=false —
///   the cross-node adopt case), assembles t2 -> { other.bin: P }, and `precommit(t2)` — which under the
///   current code is a STUB (no build-root edge published).
///
///   refA is dropped + watermark renewed; GC runs to fixpoint. Because P's only recorded in-degree was
///   refA -> t1, and the precommit recorded NO build-root edge, GC sees inDeg(P)=0 with a retired owner
///   and DELETES P. Build B then publishes refB -> t2 naming a blob that no longer exists — a dangle
///   (or an ABORTED at commit).
///
/// THE POSITIVE INVARIANT (RED under current code): the whole flow must succeed AND P must survive,
/// because B's precommit should have pinned P's closure across A's retire + GC. With the precommit a
/// stub this fails — that IS the bug. Goes GREEN once Task 2/3 land (precommit publishes a build-root
/// edge GC folds; fail-closed commit).
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

    /// Build B: adopt the SAME blob P (dedup / cross-node adopt — tokenless evidence), assemble t2,
    /// and precommit it. The precommit is meant to pin P's closure for the duration of the build.
    auto b = s->startBuild({});
    b->reuseBlob(idOf(P), /*body_recreatable=*/false);
    const TreeId t2 = b->putTree({blobEntry("other.bin", P)});
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
