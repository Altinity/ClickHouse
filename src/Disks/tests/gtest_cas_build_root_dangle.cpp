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

ManifestEntry blobEntry(const String & name, const String & payload)
{
    ManifestEntry e;
    e.path = name;
    e.placement = EntryPlacement::Blob;
    e.blob_hash = u128Of(payload);
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
    const RootNamespace ns{"srv1/tbl"};
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
    const RootNamespace ns{"srv1/tbl"};
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

    /// SIMULATE premature reclaim: remove the precommit owner binding exactly as a GC reclaim /
    /// PrecommitRemove would — append a removal RootOwnerEvent {old = precommit(build_id, "refB", T),
    /// new = none} on the TABLE namespace shard (the converged model places the precommit binding in the
    /// future committed ref's own shard, keyed by final_ref_name, NOT a separate `_precommits` namespace).
    /// We drive the shard manifest CAS directly through the backend codec.
    {
        const String key = s->layout().rootShardKey(ns, s->shardOf("refB"));
        const auto got = backend->get(key);
        ASSERT_TRUE(got.has_value()) << "table shard manifest must exist before reclaim";
        DB::Cas::RootShard root = DB::Cas::decodeRootShard(got->bytes);
        ++root.shard_version;
        root.journal.push_back(DB::Cas::RootOwnerEvent{
            .transition_version = root.shard_version,
            .old_binding = DB::Cas::OwnerBinding{
                .owner_kind = DB::Cas::OwnerKind::Precommit,
                .ref_name = "refB",
                .build_id = b->buildId(),
                .manifest_ref = t2.ref},
            .new_binding = std::nullopt});
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

namespace
{

/// Whether a `PrecommitRemove` event (old = a `OwnerKind::Precommit` binding, new = none) for `ref_name`
/// appears in the shard's journal. This is the durable, trim-proof signal that a precommit was RECLAIMED
/// (the encoding GC / abandon / dropRef append). Unlike a "live set" replay, it survives the GC journal
/// trim: GC trims FOLDED events below the cursor, so a still-live precommit's create event may be gone
/// from the journal even though it is logically live — but a REMOVAL is what we actually want to detect.
bool precommitRemovalAppended(InMemoryBackend & backend, const Layout & layout,
                              const RootNamespace & ns, uint64_t shard, const String & ref_name)
{
    const auto got = backend.get(layout.rootShardKey(ns, shard));
    if (!got)
        return false;
    const RootShard root = decodeRootShard(got->bytes);
    for (const RootOwnerEvent & e : root.journal)
        if (!e.new_binding && e.old_binding && e.old_binding->owner_kind == OwnerKind::Precommit
            && e.old_binding->ref_name == ref_name)
            return true;
    return false;
}

}

/// B171 INV-BUILDROOT-RECLAIM (§C.3) / B8: GC AUTOMATICALLY reclaims an ABANDONED precommit of a
/// judged-dead build in the CONVERGED-SHARD model, then collects its exclusively-owned closure.
///
/// `precommitAdd` writes the precommit owner binding into the FUTURE COMMITTED REF's OWN table-namespace
/// shard (keyed by `final_ref_name`), keyed by `OwnerKind::Precommit`. There is no `_precommits`
/// namespace. The reclaim must therefore scan EVERY table shard the fold visits for live precommit
/// bindings, derive `(server, build_seq)` from the binding's `manifest_ref`
/// (`writer_instance_id = "<server_hex>:<epoch>"`, `build_sequence`), judge build-death via the per-server
/// watermark exactly as the orphan sweep does, and append a `PrecommitRemove` `RootOwnerEvent` for dead
/// builds. The next fold then folds the `-1` and the closure's blobs become zero-in-degree candidates.
///
/// Crash simulation: Build B uploads blob Q (exclusively its own), assembles a manifest, and
/// `precommitAdd`s it — then VANISHES without calling abandon/promote (we retire its build_seq directly,
/// as the dtor does on a crash). The watermark is renewed so `min_active` advances PAST B's build_seq.
/// GC must reclaim B's precommit and collect Q.
TEST(CasBuildRoot, AbandonedPrecommitReclaimed)
{
    std::shared_ptr<InMemoryBackend> backend;
    auto s = openTestStore(backend);
    const RootNamespace ns{"srv1/tbl"};
    const String Q = "exclusive-blob-payload-Q";

    /// Build B: upload Q (exclusively owned by this build), assemble a manifest, precommitAdd it. The
    /// precommit owner binding for refB now protects Q with a +1 fold edge.
    BuildInfo binfo;
    binfo.intended_ref = ns.string() + "/refB";
    auto b = s->startBuild(binfo);
    const uint64_t dead_seq = b->buildSeq();
    b->putBlob(idOf(Q), BlobSource::fromString(Q));
    const ManifestId t = b->stageManifest({blobEntry("data.bin", Q)});
    b->precommitAdd(ns, "refB", t);

    /// Sanity: Q is present before reclaim.
    ASSERT_TRUE(backend->head(s->layout().blobKey(idOf(Q))).exists);

    /// CRASH: the build vanishes — neither abandon (which would remove the binding) nor promote ran. Only
    /// the dtor's idempotent retireBuildSeq fires; `b.reset()` is that crash path — the precommit binding
    /// and the staged body survive, but the seq leaves the active set. Then renew the watermark so the
    /// durable `min_active` advances PAST `dead_seq` (B is now provably dead: build_seq < min_active).
    b.reset();
    s->renewWatermarkOnce();
    ASSERT_LT(dead_seq, s->minActive()) << "crash sim invalid: B's seq must be below the live floor";

    /// GC over a fixed, generous number of rounds. The reclaim fires while folding refB's shard in the
    /// first round: it judges B dead via the watermark and appends a PrecommitRemove. But that round still
    /// sees the precommit's +1 pinning Q (the removal is not folded until the NEXT round), so it reports no
    /// candidates — a fixpoint helper that stops on the first empty round would halt before the cascade.
    /// We therefore drive a fixed count and let the cascade settle: round 2 folds the -1 (Q -> in-degree 0),
    /// then Q is condemned and deleted (precommit edge removed -> orphaned manifest body -> leaf blob Q).
    Gc gc(s, u128Of("gc-b8-reclaim"));
    for (int round = 0; round < 32; ++round)
    {
        try { gc.runRegularRound(); }
        catch (const DB::Exception &) { break; }
    }

    /// Q's only protection was the precommit edge; with it reclaimed Q reaches in-degree 0 and is deleted.
    ASSERT_FALSE(backend->head(s->layout().blobKey(idOf(Q))).exists)
        << "B8: the dead build's exclusively-owned blob Q must be collected after the precommit reclaim";
}

/// B8 CONSERVATISM (liveness-correctness guard): a build with a LIVE precommit — its build_seq is at/above
/// `min_active` and the server's watermark is NOT frozen — must NEVER be reclaimed by GC. The precommit
/// binding (and its pinned blobs) must survive a full GC round. Safety against a WRONGFUL reclaim is
/// already covered by the promote-fail-closed guard (PrematureReclaimCommitFailsClosed); this guards the
/// dual liveness property: GC must not abort a still-live build by reclaiming its in-flight precommit.
TEST(CasBuildRoot, LivePrecommitNotReclaimed)
{
    std::shared_ptr<InMemoryBackend> backend;
    auto s = openTestStore(backend);
    const RootNamespace ns{"srv1/tbl"};
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

    /// No PrecommitRemove for refLive may have been appended — GC must not reclaim an in-flight build's
    /// precommit. (Checked directly: a reclaim's removal event is the unambiguous signal; in a live, all-
    /// folded round nothing trims it away before this check since no removal exists to fold.)
    ASSERT_FALSE(precommitRemovalAppended(*backend, s->layout(), ns, s->shardOf("refLive"), "refLive"))
        << "B8 conservatism: a live build's precommit must not be reclaimed (no PrecommitRemove appended)";

    /// And Q must still be present (the live precommit pins it across GC).
    ASSERT_TRUE(backend->head(s->layout().blobKey(idOf(Q))).exists)
        << "B8 conservatism: the live precommit must keep its blob alive across GC";

    /// B can still commit (the precommit is intact).
    ASSERT_NO_THROW(b->promote(ns, "refLive", b->buildId(), t))
        << "B8 conservatism: a live build must still be able to promote its untouched precommit";
}
