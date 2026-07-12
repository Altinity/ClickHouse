#include <gtest/gtest.h>
#include <filesystem>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

/// RED/characterization tests for design doc 2026-07-08-cas-promote-over-committed-leak-fix-design.md:
///   BUG 1a (PROMOTE-OVER-COMMITTED-LEAK): `Build::promote` silently overwrites `refs[final_ref_name]`
///   when it already names a DIFFERENT committed manifest, orphaning the old manifest (leak). The fix
///   (Task 2) makes this throw `ABORTED` instead.
///   BUG 1c: `republishRef`'s only idempotency gate is "source absent" -- a re-drive after a crash
///   between `promote(dst)` and `dropRef(src)` finds dst ALREADY committed with the (same) content it is
///   about to re-publish, but re-stages+re-promotes anyway, minting a fresh manifest and orphaning the
///   first attempt's manifest. The fix (Task 3) makes the re-drive idempotent (content-keyed, not
///   ManifestId-keyed) when dst matches, and fail-closed (`ABORTED`) when dst holds different content.
///
/// These tests are EXPECTED TO FAIL pre-fix -- that failure IS the bug reproducing. They must not be
/// weakened to pass; Tasks 2/3 make them pass.

namespace DB::ErrorCodes
{
extern const int ABORTED;
}

using namespace DB::Cas;

namespace
{

StorePtr openStore(const std::shared_ptr<InMemoryBackend> & b)
{
    return Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

/// One inline-entry manifest naming `path` with content `bytes` (distinct bytes => distinct content).
/// EntryPlacement::Inline means `promote`'s blob-leaf revalidation skips it entirely -- no real blob
/// objects are needed for these tests.
std::vector<ManifestEntry> inlineEntries(const String & path, const String & bytes)
{
    ManifestEntry e;
    e.path = path;
    e.placement = EntryPlacement::Inline;
    e.inline_bytes = bytes;
    return {e};
}

/// The full write flow for an INLINE-only manifest: stageManifest -> precommitAdd -> promote. Returns
/// the committed ManifestId.
ManifestId publishCommitted(const StorePtr & s, const RootNamespace & ns, const String & ref,
                            const std::vector<ManifestEntry> & entries)
{
    auto build = s->startBuild(BuildInfo{.intended_ref = ns.string() + "/" + ref, .intended_namespace = ns});
    const ManifestId id = build->stageManifest(entries);
    build->precommitAdd(ns, ref, id);
    build->promote(ns, ref, build->buildId(), id);
    return id;
}

/// The ContentAddressedTransaction fixture (mirrors gtest_ca_transaction.cpp's openTxStorage /
/// writeFileTx): a real disk-layer storage + transaction, used to drive `republishRef` through its
/// ONLY caller (`ContentAddressedTransaction::moveDirectory`'s committed-source-ref-move branch),
/// since `republishRef` itself is private.
std::shared_ptr<DB::ContentAddressedMetadataStorage> openTxStorage()
{
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1", "test",
        std::filesystem::temp_directory_path() / "ca_tx_promote_republish_scratch", nullptr);
    storage->startup();
    return storage;
}

void writeFileTx(DB::IMetadataTransaction & tx, const std::string & path, const std::string & bytes)
{
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(tx);
    auto buf = ca_tx.writeFile(path, 65536, DB::WriteMode::Rewrite, {});
    buf->write(bytes.data(), bytes.size());
    buf->finalize();
}

}

/// BUG 1a: promoting a DIFFERENT manifest onto an already-committed ref must fail closed (ABORTED),
/// not silently overwrite (which orphans the old manifest, PROMOTE-OVER-COMMITTED-LEAK).
/// PRE-FIX: promote() does not throw -- this test FAILS (RED), which IS the leak reproducing.
TEST(CasPromoteRepublish, PromoteOverDifferentCommittedRefFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv/tbl@cas@"};
    const String ref = "all_0_0_0";

    publishCommitted(s, ns, ref, inlineEntries("f", "AAA"));   // committed T_old

    auto build2 = s->startBuild(BuildInfo{.intended_ref = ns.string() + "/" + ref, .intended_namespace = ns});
    const ManifestId id2 = build2->stageManifest(inlineEntries("f", "BBB"));   // DIFFERENT content
    build2->precommitAdd(ns, ref, id2);

    try
    {
        build2->promote(ns, ref, build2->buildId(), id2);
        FAIL() << "PRE-FIX: promote silently overwrote a committed ref (PROMOTE-OVER-COMMITTED-LEAK); "
                  "POST-FIX must throw ABORTED";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::ABORTED);
    }
}

/// Re-promoting the SAME manifest_ref onto its own committed ref must NOT throw (idempotent
/// re-promote): the fix's guard keys on a DIFFERENT manifest_ref, not merely "ref already committed".
/// This is expected to pass BOTH pre- and post-fix (it is not part of the bug).
TEST(CasPromoteRepublish, PromoteSameManifestIsIdempotent)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv/tbl@cas@"};
    const String ref = "all_0_0_0";
    const ManifestId id = publishCommitted(s, ns, ref, inlineEntries("f", "AAA"));

    /// Re-precommit + re-promote the SAME id onto the same ref: allowed (same manifest_ref).
    auto build2 = s->startBuild(BuildInfo{.intended_ref = ns.string() + "/" + ref, .intended_namespace = ns});
    build2->precommitAdd(ns, ref, id);
    EXPECT_NO_THROW(build2->promote(ns, ref, build2->buildId(), id));
}

/// Sanity companion to BUG 1a: promote over an ABSENT ref (the normal insert path) must succeed
/// unconditionally -- the fail-close guard must only fire for an EXISTING different committed ref.
TEST(CasPromoteRepublish, PromoteOverAbsentRefSucceeds)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv/tbl@cas@"};
    const String ref = "all_0_0_0";

    auto build = s->startBuild(BuildInfo{.intended_ref = ns.string() + "/" + ref, .intended_namespace = ns});
    const ManifestId id = build->stageManifest(inlineEntries("f", "AAA"));
    build->precommitAdd(ns, ref, id);
    EXPECT_NO_THROW(build->promote(ns, ref, build->buildId(), id));
}

/// BUG 1c: a `republishRef` re-drive where the destination is ALREADY committed with the SAME content
/// (the crash-before-`dropRef(src)` state) must be idempotent: skip the re-stage/re-promote, drop src,
/// and leave dst's manifest UNCHANGED (no fresh manifest minted for identical content).
///
/// PRE-FIX: republishRef's only idempotency gate is "source absent" -- it re-stages+re-promotes
/// unconditionally, minting a FRESH manifest id at dst even though the content is identical, orphaning
/// the first attempt's manifest. This test asserts dst's ManifestId is UNCHANGED across the re-drive --
/// PRE-FIX this FAILS (RED: the id changes, proving the orphaning leak).
TEST(CasPromoteRepublish, RepublishReDriveOverCommittedDstIsIdempotent)
{
    auto storage = openTxStorage();
    const auto ns = storage->liveNamespace("uuid-promote-republish");
    const String src_ref = "all_1_1_0";
    const String dst_ref = "detached_all_1_1_0";
    const String src_path = "uui/uuid-promote-republish/" + src_ref;
    const String dst_path = "uui/uuid-promote-republish/" + dst_ref;

    /// 1. Publish a committed src part via the normal write flow (tmp -> final rename, B151
    ///    publish-at-rename), exactly as gtest_ca_transaction.cpp's fixtures do.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-promote-republish/tmp_insert_" + src_ref + "/data.bin", "payload-A");
        tx->moveDirectory("uui/uuid-promote-republish/tmp_insert_" + src_ref, src_path);
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->store()->resolveRef(ns, src_ref).has_value());

    /// 2. Construct the "crash-before-dropRef(src)" state of a PRIOR republishRef drive by replaying
    ///    its exact body (resolve src -> adoptEvidence every entry -> stageManifest(same entries) ->
    ///    precommitAdd -> promote) WITHOUT the trailing dropRef(src). This leaves BOTH src and dst
    ///    committed, with dst holding the SAME content as src -- precisely the state a re-driven
    ///    republishRef must handle idempotently (ContentAddressedTransaction.cpp:143-169).
    const auto resolved_src = storage->store()->resolveRef(ns, src_ref);
    ASSERT_TRUE(resolved_src.has_value());
    const PartManifest src_manifest = storage->store()->readManifest(resolved_src->manifest_id);
    {
        auto build = storage->store()->startBuild(
            BuildInfo{.intended_ref = ns.string() + "/" + dst_ref, .intended_namespace = ns});
        for (const auto & entry : src_manifest.entries)
            build->adoptEvidence(entry);
        const ManifestId id = build->stageManifest(src_manifest.entries);
        build->precommitAdd(ns, dst_ref, id);
        build->setPendingMutableFiles(resolved_src->mutable_files);
        build->promote(ns, dst_ref, build->buildId(), id);
        /// Deliberately NO dropRef(ns, src_ref) here -- this is the simulated crash.
    }
    ASSERT_TRUE(storage->store()->resolveRef(ns, src_ref).has_value())
        << "src must still be committed (the simulated crash happened before dropRef)";
    const auto resolved_dst_before = storage->store()->resolveRef(ns, dst_ref);
    ASSERT_TRUE(resolved_dst_before.has_value());
    const ManifestId dst_id_before = resolved_dst_before->manifest_id;

    /// 3. RE-DRIVE the same rename through the real transaction path: both endpoints are already
    ///    committed-ref part paths (not a table-level rename, no staged source in this fresh
    ///    transaction) -- moveDirectory's "move any COMMITTED source ref" branch calls
    ///    republishRef(src, dst) for real (the only way to reach the private method).
    {
        auto tx = storage->createTransaction();
        tx->moveDirectory(src_path, dst_path);
        tx->commit(DB::NoCommitOptions{});
    }

    /// 4. Idempotency: src dropped, dst unchanged (SAME ManifestId -- no second manifest minted for
    ///    identical content, so nothing orphaned).
    EXPECT_FALSE(storage->store()->resolveRef(ns, src_ref).has_value())
        << "src ref must be dropped by the re-drive";
    const auto resolved_dst_after = storage->store()->resolveRef(ns, dst_ref);
    ASSERT_TRUE(resolved_dst_after.has_value());
    EXPECT_EQ(resolved_dst_after->manifest_id, dst_id_before)
        << "PRE-FIX: republishRef re-drive mints a FRESH manifest for identical content, orphaning the "
           "first attempt's manifest (BUG 1c leak). POST-FIX: idempotent no-op, same manifest.";
    EXPECT_EQ(storage->getFileSize(dst_path + "/data.bin"), 9u);
}

/// Final-review hardening: on the idempotent-skip path, dst's `mutable_files` must be RE-SYNCED from
/// src's CURRENT resolve, not left at whatever snapshot was captured by the crashed attempt. Simulates
/// src's `mutable_files` drifting (e.g. a `metadata_version.txt` rewrite) AFTER the crashed
/// `promote(dst)` but BEFORE the re-drive: the re-drive must carry the drifted value onto dst, not the
/// stale one dst already has.
TEST(CasPromoteRepublish, RepublishReDriveResyncsDriftedMutableFiles)
{
    auto storage = openTxStorage();
    const auto ns = storage->liveNamespace("uuid-promote-republish-mutable");
    const String src_ref = "all_3_3_0";
    const String dst_ref = "detached_all_3_3_0";
    const String src_path = "uui/uuid-promote-republish-mutable/" + src_ref;
    const String dst_path = "uui/uuid-promote-republish-mutable/" + dst_ref;

    /// 1. Publish a committed src part via the normal write flow.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-promote-republish-mutable/tmp_insert_" + src_ref + "/data.bin", "payload-A");
        tx->moveDirectory("uui/uuid-promote-republish-mutable/tmp_insert_" + src_ref, src_path);
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->store()->resolveRef(ns, src_ref).has_value());

    /// 2. Construct the "crash-before-dropRef(src)" state exactly as in
    ///    RepublishReDriveOverCommittedDstIsIdempotent: dst gets committed with a SNAPSHOT of src's
    ///    (currently empty) mutable_files, src is left committed (the simulated crash).
    const auto resolved_src = storage->store()->resolveRef(ns, src_ref);
    ASSERT_TRUE(resolved_src.has_value());
    ASSERT_TRUE(resolved_src->mutable_files.empty()) << "sanity: no mutable files yet before the drift";
    const PartManifest src_manifest = storage->store()->readManifest(resolved_src->manifest_id);
    {
        auto build = storage->store()->startBuild(
            BuildInfo{.intended_ref = ns.string() + "/" + dst_ref, .intended_namespace = ns});
        for (const auto & entry : src_manifest.entries)
            build->adoptEvidence(entry);
        const ManifestId id = build->stageManifest(src_manifest.entries);
        build->precommitAdd(ns, dst_ref, id);
        build->setPendingMutableFiles(resolved_src->mutable_files);
        build->promote(ns, dst_ref, build->buildId(), id);
        /// Deliberately NO dropRef(ns, src_ref) here -- this is the simulated crash.
    }
    const auto resolved_dst_before = storage->store()->resolveRef(ns, dst_ref);
    ASSERT_TRUE(resolved_dst_before.has_value());
    const ManifestId dst_id_before = resolved_dst_before->manifest_id;
    ASSERT_TRUE(resolved_dst_before->mutable_files.empty());

    /// 3. DRIFT: src's mutable_files change AFTER the crashed promote(dst) but BEFORE the re-drive
    ///    (e.g. a metadata_version.txt bump that landed on the still-committed src). Mutable-fields-only
    ///    write -- the same primitive the autocommit one-shot path uses, no reachability change.
    storage->store()->updateRefPayload(ns, src_ref, [](RefMutableFilesUpdate & payload)
    {
        payload.mutable_files["metadata_version.txt"] = "7";
    });
    ASSERT_EQ(storage->store()->resolveRef(ns, src_ref)->mutable_files.at("metadata_version.txt"), "7");
    ASSERT_TRUE(storage->store()->resolveRef(ns, dst_ref)->mutable_files.empty())
        << "sanity: the direct src write must not have touched dst";

    /// 4. RE-DRIVE: both endpoints already committed-ref part paths -- moveDirectory's "move any
    ///    COMMITTED source ref" branch calls republishRef(src, dst) for real, hitting the idempotent-skip
    ///    path (same `entries`, drifted `mutable_files`).
    {
        auto tx = storage->createTransaction();
        tx->moveDirectory(src_path, dst_path);
        tx->commit(DB::NoCommitOptions{});
    }

    /// 5. src dropped, dst's ManifestId UNCHANGED (still idempotent -- no fresh manifest minted), but
    ///    dst's mutable_files now carry the DRIFTED value from src's current resolve, not the stale
    ///    snapshot dst already had.
    EXPECT_FALSE(storage->store()->resolveRef(ns, src_ref).has_value())
        << "src ref must be dropped by the re-drive";
    const auto resolved_dst_after = storage->store()->resolveRef(ns, dst_ref);
    ASSERT_TRUE(resolved_dst_after.has_value());
    EXPECT_EQ(resolved_dst_after->manifest_id, dst_id_before)
        << "the re-sync must only touch mutable fields -- the manifest identity must stay idempotent";
    EXPECT_EQ(resolved_dst_after->mutable_files.at("metadata_version.txt"), "7")
        << "dst's mutable_files must be re-synced from src's CURRENT resolve on the idempotent-skip path, "
           "not left at the stale pre-drift snapshot (final-review Minor (c))";
}

/// Companion conflict case: a re-drive where dst is committed to DIFFERENT content than src is a
/// genuine conflict (an ATTACH-onto-existing-name collision), not a re-drive -- it must fail closed
/// (ABORTED), never silently drop src (which would lose src's content) nor silently overwrite dst.
/// This scenario reaches the SAME `promote`-over-different-committed-ref guard as BUG 1a, so pre-fix it
/// behaves the same way BUG 1a does: no throw (silent overwrite), which is also a leak/data-loss risk.
TEST(CasPromoteRepublish, RepublishReDriveOverDifferentContentDstFailsClosed)
{
    auto storage = openTxStorage();
    const auto ns = storage->liveNamespace("uuid-promote-republish-conflict");
    const String src_ref = "all_2_2_0";
    const String dst_ref = "detached_all_2_2_0";
    const String src_path = "uui/uuid-promote-republish-conflict/" + src_ref;
    const String dst_path = "uui/uuid-promote-republish-conflict/" + dst_ref;

    /// src committed with content "payload-SRC".
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-promote-republish-conflict/tmp_insert_" + src_ref + "/data.bin", "payload-SRC");
        tx->moveDirectory("uui/uuid-promote-republish-conflict/tmp_insert_" + src_ref, src_path);
        tx->commit(DB::NoCommitOptions{});
    }
    /// dst ALREADY committed with genuinely DIFFERENT content (not a re-drive artifact -- a real
    /// name collision), via a completely independent build.
    publishCommitted(storage->store(), ns, dst_ref, inlineEntries("data.bin", "different-content"));
    ASSERT_TRUE(storage->store()->resolveRef(ns, src_ref).has_value());
    ASSERT_TRUE(storage->store()->resolveRef(ns, dst_ref).has_value());

    try
    {
        auto tx = storage->createTransaction();
        tx->moveDirectory(src_path, dst_path);
        tx->commit(DB::NoCommitOptions{});
        FAIL() << "PRE-FIX: republishRef silently overwrote dst's different content "
                  "(promote-over-committed leak); POST-FIX must throw ABORTED and leave src intact";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::ABORTED);
    }
}

/// BUG 2: `abandon` must emit its precommit removal (an exact `owner_transition`) BEFORE retiring the
/// build_seq, so the build stays active until that removal is durable and no freshness-window consumer
/// judges the manifest build-dead while an un-removed precommit still names it (GC no longer reclaims
/// abandoned precommits — the writer removes them itself).
/// Assert the removal is present in the shard journal immediately after `abandon` returns.
/// (This test passes both before AND after the reorder -- the removal is emitted either way. Its value
/// is as a regression guard that `abandon` still emits the removal after the reorder. The reorder's
/// *ordering* correctness -- that GC cannot observe the pre-reorder window -- is not deterministically
/// reproducible as a timing race in a unit test; it is covered structurally by this reorder plus the
/// TLA+ `WAbandonPrecommit` model.)
TEST(CasPromoteRepublish, AbandonEmitsRemovalBeforeRetire)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv/tbl@cas@"};
    const String ref = "all_0_0_0";
    auto build = s->startBuild(BuildInfo{.intended_ref = ns.string() + "/" + ref, .intended_namespace = ns});
    const ManifestId id = build->stageManifest(inlineEntries("f", "AAA"));
    build->precommitAdd(ns, ref, id);
    build->abandon();

    /// Task 10: the exact precommit-removal transaction is proven black-box (spec §Remove Precommit):
    /// a FRESH precommitAdd for the SAME (ref_name, manifest_ref) must succeed -- if abandon had left
    /// the exact binding live, this would instead throw CORRUPTED_DATA ("add precommit ... already
    /// exists").
    auto rebuild = s->startBuild(BuildInfo{.intended_ref = ns.string() + "/" + ref, .intended_namespace = ns});
    EXPECT_NO_THROW(rebuild->precommitAdd(ns, ref, id))
        << "abandon() must append the exact precommit-removal transaction before returning";
}
