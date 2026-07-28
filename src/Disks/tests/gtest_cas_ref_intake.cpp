#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Common/Exception.h>

using namespace DB::Cas;

namespace
{

ManifestRef mr(uint64_t epoch, uint64_t seq, uint32_t ordinal = 1)
{
    return ManifestRef{epoch, seq, ordinal};
}

RefTxnId rid(uint64_t epoch, uint64_t seq)
{
    return RefTxnId{epoch, seq};
}

RefOp addOwner(RefOwnerKind kind, const String & ref, const ManifestRef & manifest)
{
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.new_binding = RefOwnerBinding{kind, ref, manifest};
    return op;
}

RefOp removeOwner(RefOwnerKind kind, const String & ref, const ManifestRef & manifest)
{
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.old_binding = RefOwnerBinding{kind, ref, manifest};
    return op;
}

RefOp promote(const String & ref, const ManifestRef & manifest)
{
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, ref, manifest};
    op.new_binding = RefOwnerBinding{RefOwnerKind::Committed, ref, manifest};
    return op;
}

/// A raw `owner_transition` op from explicit optional bindings, bypassing every shape-builder above --
/// used by the rejection tests to construct shapes `classifyOwnerTransitionShape` does not recognize.
RefOp rawOwnerTransition(std::optional<RefOwnerBinding> old_binding, std::optional<RefOwnerBinding> new_binding)
{
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.old_binding = std::move(old_binding);
    op.new_binding = std::move(new_binding);
    return op;
}

RefLogTxn txn(const String & ns, RefTxnId id, std::vector<RefOp> ops)
{
    RefLogTxn t;
    t.ns = ns;
    t.txn_id = id;
    t.ops = std::move(ops);
    return t;
}

}

/// spec §gc-step-produce-manifest-edge-delta: each explicit operation states its own edge change.
TEST(CasRefIntake, ManifestEdgesPerOperationShape)
{
    /// Add precommit => one +1.
    {
        const auto edges = manifestEdgesOfTxn(txn("db/t", rid(1, 1), {addOwner(RefOwnerKind::Precommit, "p", mr(1, 5))}));
        ASSERT_EQ(edges.size(), 1u);
        EXPECT_EQ(edges[0].change, 1);
        EXPECT_EQ(edges[0].manifest_id, (ManifestId{RootNamespace{"db/t"}, mr(1, 5)}));
        EXPECT_EQ(edges[0].op_ordinal, 0u);
        EXPECT_EQ(edges[0].edge_ordinal, 1u);
    }
    /// Remove committed => one -1.
    {
        const auto edges = manifestEdgesOfTxn(txn("db/t", rid(1, 2), {removeOwner(RefOwnerKind::Committed, "p", mr(1, 5))}));
        ASSERT_EQ(edges.size(), 1u);
        EXPECT_EQ(edges[0].change, -1);
        EXPECT_EQ(edges[0].manifest_id, (ManifestId{RootNamespace{"db/t"}, mr(1, 5)}));
    }
    /// Remove precommit => one -1 (the fourth classified shape, distinct from remove committed only by
    /// `old_binding.kind`).
    {
        const auto edges = manifestEdgesOfTxn(txn("db/t", rid(1, 25), {removeOwner(RefOwnerKind::Precommit, "p", mr(1, 5))}));
        ASSERT_EQ(edges.size(), 1u);
        EXPECT_EQ(edges[0].change, -1);
        EXPECT_EQ(edges[0].owner_kind, RefOwnerKind::Precommit);
        EXPECT_EQ(edges[0].manifest_id, (ManifestId{RootNamespace{"db/t"}, mr(1, 5)}));
    }
    /// Promote same manifest => no net edge (spec §Promote).
    {
        const auto edges = manifestEdgesOfTxn(txn("db/t", rid(1, 3), {promote("p", mr(1, 5))}));
        EXPECT_TRUE(edges.empty());
    }
    /// set_published_at / namespace_birth / remove_namespace => no edge.
    {
        RefOp set_published_at;
        set_published_at.kind = RefOpKind::SetPublishedAt;
        set_published_at.ref_name = "p";
        set_published_at.expected_manifest_ref = mr(1, 5);
        EXPECT_TRUE(manifestEdgesOfTxn(txn("db/t", rid(1, 4), {set_published_at})).empty());

        RefOp birth;
        birth.kind = RefOpKind::NamespaceBirth;
        EXPECT_TRUE(manifestEdgesOfTxn(txn("db/t", rid(1, 5), {birth})).empty());
    }
    /// Replace one manifest by a different one (two explicit ops) => -1 old, +1 new.
    {
        const auto edges = manifestEdgesOfTxn(txn("db/t", rid(1, 6),
            {removeOwner(RefOwnerKind::Committed, "p", mr(1, 5)), addOwner(RefOwnerKind::Precommit, "p", mr(1, 6))}));
        ASSERT_EQ(edges.size(), 2u);
        EXPECT_EQ(edges[0].change, -1);
        EXPECT_EQ(edges[0].manifest_id.ref, mr(1, 5));
        EXPECT_EQ(edges[1].change, 1);
        EXPECT_EQ(edges[1].manifest_id.ref, mr(1, 6));
    }
}

/// `manifestEdgesOfTxn` rejects every `owner_transition` shape outside the four `classifyOwnerTransitionShape`
/// recognizes (Pool/CasRefProtocol.cpp) -- it must never silently assign edge meaning to a shape the
/// writer/replay state machine would refuse to apply. Each case throws `CORRUPTED_DATA`.
TEST(CasRefIntake, ManifestEdgesRejectsUnrecognizedShapes)
{
    /// Neither binding: a degenerate owner_transition that names no owner change at all.
    EXPECT_THROW(manifestEdgesOfTxn(txn("db/t", rid(1, 1), {rawOwnerTransition(std::nullopt, std::nullopt)})),
                 DB::Exception);

    /// old+new naming DIFFERENT manifests in ONE op (the never-legal "replace" shape; an atomic
    /// manifest replace is always two ops -- an explicit removal then a same-manifest promote).
    EXPECT_THROW(manifestEdgesOfTxn(txn("db/t", rid(1, 2),
        {rawOwnerTransition(RefOwnerBinding{RefOwnerKind::Committed, "p", mr(1, 5)},
                             RefOwnerBinding{RefOwnerKind::Precommit, "p", mr(1, 6)})})),
                 DB::Exception);

    /// Promote-shaped kinds (old=Precommit, new=Committed) but with MISMATCHED ref_names.
    EXPECT_THROW(manifestEdgesOfTxn(txn("db/t", rid(1, 3),
        {rawOwnerTransition(RefOwnerBinding{RefOwnerKind::Precommit, "p", mr(1, 5)},
                             RefOwnerBinding{RefOwnerKind::Committed, "q", mr(1, 5)})})),
                 DB::Exception);

    /// Add with new.kind == Committed (only Precommit is a legal add target).
    EXPECT_THROW(manifestEdgesOfTxn(txn("db/t", rid(1, 4),
        {rawOwnerTransition(std::nullopt, RefOwnerBinding{RefOwnerKind::Committed, "p", mr(1, 5)})})),
                 DB::Exception);

    /// old+new both Committed, same manifest: not a promote (promote requires old.kind == Precommit).
    EXPECT_THROW(manifestEdgesOfTxn(txn("db/t", rid(1, 5),
        {rawOwnerTransition(RefOwnerBinding{RefOwnerKind::Committed, "p", mr(1, 5)},
                             RefOwnerBinding{RefOwnerKind::Committed, "p", mr(1, 5)})})),
                 DB::Exception);
}

/// Namespaces are edge-distinct even with identical ManifestRef tuples (spec §gc-inputs-and-output).
TEST(CasRefIntake, EdgesAreNamespaceQualified)
{
    const auto a = manifestEdgesOfTxn(txn("db/a", rid(1, 1), {addOwner(RefOwnerKind::Precommit, "p", mr(1, 5))}));
    const auto b = manifestEdgesOfTxn(txn("db/b", rid(1, 1), {addOwner(RefOwnerKind::Precommit, "p", mr(1, 5))}));
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_NE(a[0].manifest_id, b[0].manifest_id);
}

TEST(CasRefIntake, RemovalTxnIdDetection)
{
    RefOp remove_ns;
    remove_ns.kind = RefOpKind::RemoveNamespace;
    const auto with_removal = txn("db/t", rid(3, 8), {removeOwner(RefOwnerKind::Committed, "p", mr(1, 5)), remove_ns});
    ASSERT_TRUE(removalTxnId(with_removal).has_value());
    EXPECT_EQ(*removalTxnId(with_removal), rid(3, 8));

    const auto ordinary = txn("db/t", rid(3, 9), {addOwner(RefOwnerKind::Precommit, "p", mr(1, 5))});
    EXPECT_FALSE(removalTxnId(ordinary).has_value());
}

/// spec §Step 1: one global LIST groups by table, split by kind, sorted; the reconstructed namespace is
/// re-validated (VERIFY-AT-T12) and a malformed ref key aborts ref folding (throws).
TEST(CasRefIntake, GroupRefKeys)
{
    const Layout layout{"p"};
    const RootNamespace ns{"db/t"};

    std::vector<String> keys{
        layout.refSnapshotKey(ns, rid(1, 4)),
        layout.refLogKey(ns, rid(1, 5)),
        layout.refLogKey(ns, rid(1, 3)),
        layout.refCleanupMarkerKey(ns, rid(1, 2)),
        layout.refCkptKey(ns),        /// the checkpoint (spec INV-4): a ref object with no txn id
        "p/cas/manifests/db/t/foo",   /// outside the ref prefix -> ignored
    };
    const auto grouped = groupRefKeys(layout, keys);
    ASSERT_EQ(grouped.size(), 1u);
    const RefTableListing & t = grouped.at("db/t");
    EXPECT_EQ(t.logs, (std::vector<RefTxnId>{rid(1, 3), rid(1, 5)}));
    EXPECT_EQ(t.snapshots, (std::vector<RefTxnId>{rid(1, 4)}));
    EXPECT_EQ(t.cleanup_markers, (std::vector<RefTxnId>{rid(1, 2)}));
    /// The checkpoint carries no transaction id, so it cannot join the three id vectors -- but it IS
    /// one of the table's ref objects, and a listing that dropped it would report a table with a
    /// checkpoint as indistinguishable from one without.
    EXPECT_TRUE(t.has_ckpt);

    /// And a table with no checkpoint says so, rather than defaulting to the same answer.
    const auto no_ckpt = groupRefKeys(layout, {layout.refLogKey(ns, rid(1, 1))});
    ASSERT_EQ(no_ckpt.size(), 1u);
    EXPECT_FALSE(no_ckpt.at("db/t").has_ckpt);

    /// A key under the ref prefix that is not a valid ref object aborts (a leftover old-format shard key).
    /// This is what makes the `_ckpt` arm above load-bearing rather than cosmetic: without it the
    /// checkpoint would land HERE, and the first namespace to get one would abort ref folding for the
    /// whole round, in every round, pool-wide.
    EXPECT_THROW(groupRefKeys(layout, {"p/cas/refs/db/t/0"}), DB::Exception);
    /// A malformed namespace (empty segment) under a valid kind directory aborts.
    EXPECT_THROW(groupRefKeys(layout, {"p/cas/refs/db//_log/" + renderRefTxnId(rid(1, 1))}), DB::Exception);
}

/// spec §Step 6: a log is deletable only under all three conditions; older snapshots may go too.
TEST(CasRefIntake, PlanRefCleanupThreeConditions)
{
    RefTableListing listing;
    listing.logs = {rid(1, 1), rid(1, 2), rid(1, 3)};
    listing.snapshots = {rid(1, 2)};   /// newest observed snapshot X = (1,2)

    /// Full coverage (cursor past everything): logs <= X and <= cursor are deletable; (1,3) > X stays.
    {
        const auto plan = planRefCleanup(listing, rid(1, 3), {});
        EXPECT_EQ(plan.deletable_logs, (std::vector<RefTxnId>{rid(1, 1), rid(1, 2)}));
        EXPECT_TRUE(plan.deletable_snapshots.empty());
    }
    /// Cursor lagging behind the snapshot: only logs <= cursor are deletable (condition 1).
    {
        const auto plan = planRefCleanup(listing, rid(1, 1), {});
        EXPECT_EQ(plan.deletable_logs, (std::vector<RefTxnId>{rid(1, 1)}));
    }
    /// A blocked removal log (its namespace-cleanup item not yet Completed) is retained (condition 3).
    {
        const auto plan = planRefCleanup(listing, rid(1, 3), {rid(1, 1)});
        EXPECT_EQ(plan.deletable_logs, (std::vector<RefTxnId>{rid(1, 2)}));
    }
    /// Older snapshots (< X) are deletable; X itself is retained.
    {
        RefTableListing two_snaps = listing;
        two_snaps.snapshots = {rid(1, 1), rid(1, 2)};
        const auto plan = planRefCleanup(two_snaps, rid(1, 3), {});
        EXPECT_EQ(plan.deletable_snapshots, (std::vector<RefTxnId>{rid(1, 1)}));
    }
    /// No snapshot => no coverage boundary => empty plan (condition 2).
    {
        RefTableListing no_snap;
        no_snap.logs = {rid(1, 1)};
        const auto plan = planRefCleanup(no_snap, rid(1, 5), {});
        EXPECT_TRUE(plan.deletable_logs.empty());
        EXPECT_TRUE(plan.deletable_snapshots.empty());
    }
}

/// T8 carry: a namespace literally named -- or ending in -- a kind directory (`_log`/`_snap`/`_cleanup`)
/// must not confuse `parseRefObjectKey` or the global-LIST grouping. The kind is always the SECOND-TO-LAST
/// path segment, so it is positionally unambiguous; a nested table whose keys physically sit under another
/// table's `_log/` directory is still attributed to its own namespace.
TEST(CasRefIntake, AdversarialNamespaceNamedLikeKindDirectory)
{
    const Layout layout{"p"};

    /// A table whose whole namespace IS exactly a kind directory name.
    for (const String & weird : {String("_log"), String("_snap"), String("_cleanup")})
    {
        const RootNamespace ns{weird};
        const std::vector<String> keys{
            layout.refLogKey(ns, rid(1, 1)),
            layout.refSnapshotKey(ns, rid(1, 2)),
            layout.refCleanupMarkerKey(ns, rid(1, 3))};
        const auto grouped = groupRefKeys(layout, keys);
        ASSERT_EQ(grouped.size(), 1u) << "namespace '" << weird << "'";
        ASSERT_TRUE(grouped.contains(weird)) << "namespace '" << weird << "'";
        const RefTableListing & t = grouped.at(weird);
        EXPECT_EQ(t.logs, (std::vector<RefTxnId>{rid(1, 1)})) << weird;
        EXPECT_EQ(t.snapshots, (std::vector<RefTxnId>{rid(1, 2)})) << weird;
        EXPECT_EQ(t.cleanup_markers, (std::vector<RefTxnId>{rid(1, 3)})) << weird;
    }

    /// A table "db/t" and a NESTED table "db/t/_log" coexist: the nested table's log objects live UNDER
    /// db/t's `_log/` directory prefix, but grouping keeps them distinct and never attributes the nested
    /// log to the outer table.
    const RootNamespace outer{"db/t"};
    const RootNamespace nested{"db/t/_log"};
    const std::vector<String> keys{
        layout.refLogKey(outer, rid(7, 1)),
        layout.refSnapshotKey(outer, rid(7, 2)),
        layout.refLogKey(nested, rid(9, 1))};
    const auto grouped = groupRefKeys(layout, keys);
    ASSERT_EQ(grouped.size(), 2u);
    EXPECT_EQ(grouped.at("db/t").logs, (std::vector<RefTxnId>{rid(7, 1)}));
    EXPECT_EQ(grouped.at("db/t").snapshots, (std::vector<RefTxnId>{rid(7, 2)}));
    EXPECT_EQ(grouped.at("db/t/_log").logs, (std::vector<RefTxnId>{rid(9, 1)}));
}
