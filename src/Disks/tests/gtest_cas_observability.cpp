#include <gtest/gtest.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <algorithm>
#include <memory>
#include <vector>

using namespace DB::Cas;

namespace
{

StorePtr openStore(std::shared_ptr<InMemoryBackend> & b)
{
    b = std::make_shared<InMemoryBackend>();
    return Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

}

/// B170/Task 1 (Part A audit events): `Build::stageManifest` writes a part-manifest body but never
/// emitted an audit row for it — the log could not answer "when was this manifest written." Verifies
/// the emitted `ManifestPut` event (exactly once per successful stage).
TEST(CasObservability, StageManifestEmitsManifestPut)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openStore(b);
    std::vector<CasEvent> seen;
    s->setEventSink([&](const CasEvent & e){ seen.push_back(e); });

    const RootNamespace ns{"srv/tbl@cas@"};
    auto build = s->startBuild(BuildInfo{.intended_ref = ns.string() + "/all_0_0_0", .intended_namespace = ns});
    ManifestEntry e;
    e.path = "f";
    e.placement = EntryPlacement::Inline;
    e.inline_bytes = "AAA";
    const ManifestId id = build->stageManifest({e});
    s->setEventSink(nullptr);

    EXPECT_EQ(std::count_if(seen.begin(), seen.end(),
        [](const CasEvent & x){ return x.type == CasEventType::ManifestPut; }), 1);

    const auto it = std::find_if(seen.begin(), seen.end(),
        [](const CasEvent & x){ return x.type == CasEventType::ManifestPut; });
    ASSERT_NE(it, seen.end());
    EXPECT_EQ(it->object_kind, CasEventObjectKind::Manifest);
    EXPECT_EQ(it->object_hash, manifestRefDebugString(id.ref));
    EXPECT_FALSE(it->token.empty());
}

/// `Build::abandon` removes a live precommit's owner binding (the correctness-bearing step) but never
/// audited the removal — the log could not distinguish "never precommitted" from "precommitted then
/// abandoned." Verifies the emitted `PrecommitRemoved` event (exactly once, only when a precommit was
/// actually live).
TEST(CasObservability, AbandonEmitsPrecommitRemoved)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openStore(b);

    const RootNamespace ns{"srv/tbl@cas@"};
    auto build = s->startBuild(BuildInfo{.intended_ref = ns.string() + "/all_0_0_0", .intended_namespace = ns});
    ManifestEntry e;
    e.path = "f";
    e.placement = EntryPlacement::Inline;
    e.inline_bytes = "AAA";
    const ManifestId id = build->stageManifest({e});
    build->precommitAdd(ns, "all_0_0_0", id);

    std::vector<CasEvent> seen;
    s->setEventSink([&](const CasEvent & x){ seen.push_back(x); });
    build->abandon();
    s->setEventSink(nullptr);

    EXPECT_EQ(std::count_if(seen.begin(), seen.end(),
        [](const CasEvent & x){ return x.type == CasEventType::PrecommitRemoved; }), 1);

    const auto it = std::find_if(seen.begin(), seen.end(),
        [](const CasEvent & x){ return x.type == CasEventType::PrecommitRemoved; });
    ASSERT_NE(it, seen.end());
    EXPECT_EQ(it->namespace_, ns.string());
    EXPECT_EQ(it->ref_name, "all_0_0_0");
    EXPECT_EQ(it->object_kind, CasEventObjectKind::Root);
    EXPECT_EQ(it->object_hash, manifestRefDebugString(id.ref));
}

/// A build that never precommitted has nothing to remove: `abandon` must not fabricate a
/// `PrecommitRemoved` row for a binding that was never live.
TEST(CasObservability, AbandonWithoutPrecommitEmitsNoPrecommitRemoved)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openStore(b);

    const RootNamespace ns{"srv/tbl@cas@"};
    auto build = s->startBuild(BuildInfo{.intended_ref = ns.string() + "/all_0_0_0", .intended_namespace = ns});
    ManifestEntry e;
    e.path = "f";
    e.placement = EntryPlacement::Inline;
    e.inline_bytes = "AAA";
    build->stageManifest({e});   /// staged, never precommitted

    std::vector<CasEvent> seen;
    s->setEventSink([&](const CasEvent & x){ seen.push_back(x); });
    build->abandon();
    s->setEventSink(nullptr);

    EXPECT_EQ(std::count_if(seen.begin(), seen.end(),
        [](const CasEvent & x){ return x.type == CasEventType::PrecommitRemoved; }), 0);
}
