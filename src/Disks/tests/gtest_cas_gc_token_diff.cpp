#include "config.h"

#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcCursorKey.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

/// Phase 2 token-diff discovery: `supportsListTokens` capability probe.
///
/// Backends that can surface a per-key incarnation token through `list` return TRUE so that
/// GC `discover` may skip an unchanged root-shard body read when a listed token equals a
/// persisted folded token. Backends that cannot must return FALSE so discovery falls closed
/// to body reads.

namespace
{

/// Minimal stub backend that overrides every pure virtual with no-op implementations and
/// explicitly returns FALSE from `supportsListTokens`. Used to verify the default-false path.
class NoListTokenBackend final : public Backend
{
public:
    bool supportsListTokens() const override { return false; }

    std::optional<GetResult> get(const String & /*key*/, Range /*range*/ = {}) override
    {
        return std::nullopt;
    }

    HeadResult head(const String & /*key*/) override
    {
        return HeadResult{};
    }

    PutResult putIfAbsent(const String & /*key*/, const String & /*bytes*/, const ObjectMeta & /*meta*/ = {}) override
    {
        return PutResult{PutOutcome::Done, Token{}};
    }

    WriteSinkPtr putIfAbsentStream(const String & /*key*/, const ObjectMeta & /*meta*/ = {}) override
    {
        return nullptr;
    }

    PutResult putOverwrite(const String & /*key*/, const String & /*bytes*/, const Token & /*expected*/,
                           const ObjectMeta & /*meta*/ = {}) override
    {
        return PutResult{PutOutcome::Done, Token{}};
    }

    CasResult casPut(const String & /*key*/, const String & /*bytes*/, const std::optional<Token> & /*expected*/,
                     const ObjectMeta & /*meta*/ = {}) override
    {
        return CasResult{CasOutcome::Committed, Token{}};
    }

    DeleteOutcome deleteExact(const String & /*key*/, const Token & /*token*/) override
    {
        return DeleteOutcome{};
    }

    ListPage list(const String & /*prefix*/, const String & /*cursor*/, size_t /*limit*/) override
    {
        return ListPage{};
    }
};

} // namespace

/// The in-memory backend mints a monotonic token it surfaces through `list` — TRUE is correct.
TEST(CasBackendListTokens, InMemorySupportsListTokens)
{
    InMemoryBackend backend;
    EXPECT_TRUE(backend.supportsListTokens());
}

/// A backend explicitly overriding to FALSE must be observable as FALSE.
TEST(CasBackendListTokens, OverridableToFalse)
{
    NoListTokenBackend backend;
    EXPECT_FALSE(backend.supportsListTokens());
}

/// Characterization test: `CasFoldSeal::per_ns_shard` entries carry `folded_token` and
/// `folded_cursor` through the `encodeFoldSeal`/`decodeFoldSeal` codec byte-stably.
///
/// A later `discover` reads `ShardCoverage.folded_token`/`folded_cursor` back out of the
/// persisted `CasFoldSeal` to decide whether to skip a shard's body read. This test confirms
/// the round-trip was already correct in Phase 1d — no codec change was required.
TEST(CasShardCoverageRoundTrip, FoldedTokenAndCursorSurviveEncodeDecode)
{
    CasFoldSeal in;
    in.generation = 5;
    in.parent_generation = 4;

    /// One shard with a non-zero `folded_token`, non-zero `folded_cursor`, and a concrete
    /// `classification` value (2 = Folded per the spec enum).
    ShardCoverage cov;
    cov.classification = 2;
    cov.folded_token = Token{"etag-abc123", TokenType::ETag};
    cov.folded_cursor = 99;
    in.per_ns_shard["myns/7"] = cov;

    const CasFoldSeal out = decodeFoldSeal(encodeFoldSeal(in));

    ASSERT_EQ(out.per_ns_shard.size(), 1u);
    const ShardCoverage & decoded = out.per_ns_shard.at("myns/7");
    EXPECT_EQ(decoded.classification, cov.classification);
    EXPECT_EQ(decoded.folded_token, cov.folded_token);
    EXPECT_EQ(decoded.folded_cursor, cov.folded_cursor);
    /// The full struct equality confirms no other field was corrupted.
    EXPECT_EQ(out, in);
}

namespace
{

const UInt128 kGcDiscovery = hexToU128("00000000000000000000000000000001");

ManifestRef discoveryRef(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_instance_id = "srv-a:1", .build_sequence = seq, .manifest_instance_id = DB::UInt128(inst)};
}

} // namespace

/// Phase-2 token-diff discover skips a QUIESCED root shard.
///
/// A shard becomes skippable only AFTER a settling round. The reason is architectural: the GC round's
/// `fence` step calls `mutateShard` on EVERY shard EVERY round (a per-shard fence marker), and `trim`
/// also `mutateShard`s a shard that had trimmable events. Both bump the shard's backend token — the
/// very token `Backend::list` surfaces. `recheck` records `folded_token` from the POST-FENCE shard
/// token it reads via `readShard`. So:
///   - Round 1 folds AND trims the freshly-published events; recheck records the post-fence token, but
///     trim then bumps the token again => next discover sees an advanced token => Read.
///   - Round 2 has nothing to trim (the published events are gone); recheck records the post-fence
///     token, which is now the shard's FINAL token for the round => next discover sees an unchanged
///     token => Skip.
/// This 1-round settle is the intended, safe conservative behavior (the optimal one-round skip is
/// deferred to the backlog — it would require capturing the post-trim token, a model extension).
TEST(CasGcDiscovery, SkipsQuiescedShard)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"srv1/tbl"};
    const ManifestRef r = discoveryRef(1, 0xAA);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGcDiscovery);

    /// Round 1: folds + trims the published events. After it, trim's mutation advanced the token past
    /// the post-fence token recheck recorded — so the shard is NOT yet skippable.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    /// Round 2: a settling round — nothing left to trim. recheck records the post-fence token, which is
    /// the shard's final token for the round (no later mutation), so the NEXT discover sees it unchanged.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const auto decisions = gc.discoverDecisionsForTest();
    const String ck = cursorKey(ns, 0);
    ASSERT_TRUE(decisions.contains(ck));
    EXPECT_EQ(decisions.at(ck), Gc::DiscoverDecision::Skip)
        << "a quiesced shard must be skipped after a settling round (token unchanged since recheck)";
}

/// Phase-2 token-diff discover READS a shard whose token advanced (a new publish bumps it) — the safe
/// direction. After the shard has quiesced (skippable), a fresh committed publish advances the shard's
/// backend token; the listed token no longer equals the sealed `folded_token`, so the shard is Read.
TEST(CasGcDiscovery, ReadsShardWhenTokenAdvancedOrMissing)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"srv1/tbl"};
    const ManifestRef r1 = discoveryRef(1, 0xAA);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);

    Gc gc(store, kGcDiscovery);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);   /// round 1: fold + trim
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);   /// round 2: settle => shard now skippable

    const String ck = cursorKey(ns, 0);
    ASSERT_EQ(gc.discoverDecisionsForTest().at(ck), Gc::DiscoverDecision::Skip)
        << "precondition: the shard quiesced and is skippable";

    /// A NEW committed publish advances the shard's backend token past the sealed folded_token.
    const ManifestRef r2 = discoveryRef(2, 0xBB);
    writeBlobBody(*backend, store->layout(), DB::UInt128(2));
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl2", std::nullopt, r2);

    const auto decisions = gc.discoverDecisionsForTest();
    ASSERT_TRUE(decisions.contains(ck));
    EXPECT_EQ(decisions.at(ck), Gc::DiscoverDecision::Read)
        << "a shard whose token advanced since the fold must be read (fail closed in the safe direction)";
}

/// Phase-2 token-diff discover universe is the REGISTRY universe, never shrunk by LIST. Two namespaces
/// are registered but a publish lands in only ONE; both must still appear in the decisions (a namespace
/// with no LIST-visible shard token defaults to Read — fail closed — but is never DROPPED from the set).
TEST(CasGcDiscovery, RegistryUniverseNeverShrunkByList)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns_a{"srv1/tbl_a"};
    const RootNamespace ns_b{"srv1/tbl_b"};

    /// Register BOTH namespaces (a publish registers its own; register the other explicitly).
    registerNamespaceRaw(*backend, store->layout(), ns_b);

    const ManifestRef r = discoveryRef(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns_a, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns_a, "tbl", std::nullopt, r);

    Gc gc(store, kGcDiscovery);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const auto decisions = gc.discoverDecisionsForTest();
    EXPECT_TRUE(decisions.contains(cursorKey(ns_a, 0))) << "the published namespace must be present";
    EXPECT_TRUE(decisions.contains(cursorKey(ns_b, 0)))
        << "a registered-but-empty namespace must remain in the universe (registry authority, not LIST)";
}

/// ---- Task 5: fail-closed fallback to body reads on any ambiguity ----

/// Characterization / lock test: when the backend's `supportsListTokens` returns FALSE, EVERY shard
/// decision must be Read (fail closed) regardless of sealed state. This locks the `supportsListTokens`
/// guard that was introduced in Task 4 and must never regress.
TEST(CasGcDiscovery, FailsClosedToReadWhenTokensUnobservable)
{
    /// Build a store over an `InMemoryBackend` (which supports list tokens) to run a real settling round,
    /// then verify: if we were using a backend that does NOT support list tokens, every shard would be Read.
    ///
    /// We can test this directly by using `NoListTokenBackend` for a store whose GC has gone through
    /// a round. However `NoListTokenBackend` is a no-op stub — it cannot run a real round. Instead we
    /// exercise the guard path: open a real store, run two settling rounds so a shard becomes Skip-eligible,
    /// confirm it IS Skip on the real backend, then open a SECOND store over a `NoListTokenBackend`-derived
    /// store that delegates all ops to the same in-memory storage but returns `supportsListTokens` = false.
    /// The second store's `discoverDecisionsForTest` must report all Read.
    auto real_backend = std::make_shared<InMemoryBackend>();
    auto real_store = openStoreForTest(real_backend);
    const RootNamespace ns{"srv1/no_list_tokens"};
    const ManifestRef r = discoveryRef(1, 0xCC);

    writeBlobBody(*real_backend, real_store->layout(), DB::UInt128(1));
    writeManifestRaw(*real_backend, real_store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*real_backend, real_store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc_real(real_store, kGcDiscovery);
    ASSERT_TRUE(gc_real.runRegularRound().acquired_lease);   /// round 1: fold + trim
    ASSERT_TRUE(gc_real.runRegularRound().acquired_lease);   /// round 2: settle

    /// Confirm the shard is Skip-eligible on the real (supportsListTokens=true) backend.
    const String ck = cursorKey(ns, 0);
    ASSERT_EQ(gc_real.discoverDecisionsForTest().at(ck), Gc::DiscoverDecision::Skip)
        << "precondition: shard settled and is skippable on a list-tokens-capable backend";

    /// Now build a no-list-tokens wrapper that delegates storage to the same in-memory backend but
    /// returns FALSE from `supportsListTokens`. Open a separate Store + Gc over it.
    class NoListWrapper final : public InMemoryBackend
    {
    public:
        explicit NoListWrapper(InMemoryBackend & base_) : base(base_) {}
        bool supportsListTokens() const override { return false; }

        std::optional<GetResult> get(const String & key, Range range = {}) override { return base.get(key, range); }
        HeadResult head(const String & key) override { return base.head(key); }
        PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta = {}) override
        {
            return base.putIfAbsent(key, bytes, meta);
        }
        WriteSinkPtr putIfAbsentStream(const String & key, const ObjectMeta & meta = {}) override
        {
            return base.putIfAbsentStream(key, meta);
        }
        PutResult putOverwrite(const String & key, const String & bytes, const Token & expected,
                               const ObjectMeta & meta = {}) override
        {
            return base.putOverwrite(key, bytes, expected, meta);
        }
        CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                         const ObjectMeta & meta = {}) override
        {
            return base.casPut(key, bytes, expected, meta);
        }
        DeleteOutcome deleteExact(const String & key, const Token & token) override
        {
            return base.deleteExact(key, token);
        }
        ListPage list(const String & prefix, const String & cursor, size_t limit) override
        {
            return base.list(prefix, cursor, limit);
        }

    private:
        InMemoryBackend & base;
    };

    auto no_list_backend = std::make_shared<NoListWrapper>(*real_backend);
    auto no_list_store = Store::open(
        no_list_backend, PoolConfig{.pool_prefix = "p", .root_shards = 1});
    Gc gc_no_list(no_list_store, kGcDiscovery);

    const auto decisions = gc_no_list.discoverDecisionsForTest();
    ASSERT_TRUE(decisions.contains(ck));
    EXPECT_EQ(decisions.at(ck), Gc::DiscoverDecision::Read)
        << "supportsListTokens=false must force every shard to Read (fail closed; the spec)";
}

/// Characterization / lock test: a shard registered but never covered by a prior completed round must
/// be Read. No `runRegularRound` is called here — there is no sealed folded_token — so the prior-coverage
/// guard must fire and keep the decision as Read.
TEST(CasGcDiscovery, FailsClosedToReadWhenNoPriorCoverage)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"srv1/no_coverage"};
    const ManifestRef r = discoveryRef(1, 0xDD);

    writeBlobBody(*backend, store->layout(), DB::UInt128(3));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(3))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    /// Deliberately do NOT run any GC round — there is no sealed coverage for this shard.
    Gc gc(store, kGcDiscovery);

    const auto decisions = gc.discoverDecisionsForTest();
    const String ck = cursorKey(ns, 0);
    ASSERT_TRUE(decisions.contains(ck));
    EXPECT_EQ(decisions.at(ck), Gc::DiscoverDecision::Read)
        << "a shard with no prior sealed coverage must be Read (fail closed; the spec)";
}

/// New guard test: a shard whose key appears MORE THAN ONCE in the LIST sweep (ambiguous) must be
/// Read, even when the token would otherwise match the sealed folded_token. This is tested via a
/// focused unit on a duplicate-key-yielding backend: the backend's `list` returns the same shard key
/// twice in the same page; `listRootShardTokens` must mark that key as ambiguous, and
/// `computeDiscoverDecisions` must return Read for it.
TEST(CasGcDiscovery, FailsClosedToReadWhenListKeyAmbiguous)
{
    /// First run a real settling round to get a valid sealed completion seal with a folded_token.
    auto real_backend = std::make_shared<InMemoryBackend>();
    auto real_store = openStoreForTest(real_backend);
    const RootNamespace ns{"srv1/ambiguous"};
    const ManifestRef r = discoveryRef(1, 0xEE);

    writeBlobBody(*real_backend, real_store->layout(), DB::UInt128(4));
    writeManifestRaw(*real_backend, real_store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(4))});
    publishCommittedTransition(*real_backend, real_store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc_real(real_store, kGcDiscovery);
    ASSERT_TRUE(gc_real.runRegularRound().acquired_lease);   /// round 1: fold + trim
    ASSERT_TRUE(gc_real.runRegularRound().acquired_lease);   /// round 2: settle

    /// Confirm the shard is Skip-eligible on the real backend (precondition for the ambiguity test:
    /// the token-match path would normally yield Skip, which we need to override with Read).
    const String ck = cursorKey(ns, 0);
    ASSERT_EQ(gc_real.discoverDecisionsForTest().at(ck), Gc::DiscoverDecision::Skip)
        << "precondition: shard settled and is skippable";

    /// Build a backend that injects a duplicate `ListedKey` for the shard's full key on every `list`
    /// call, while delegating all other ops to the real backend. This simulates an ambiguous LIST
    /// sweep (the same key returned twice in one page or across pages).
    const String roots_prefix = real_store->layout().rootsPrefix();
    const String shard_full_key = roots_prefix + ck;   /// e.g. "p/roots/srv1/ambiguous/0"

    class DuplicateListBackend final : public InMemoryBackend
    {
    public:
        DuplicateListBackend(InMemoryBackend & base_, String dup_key_)
            : base(base_), dup_key(std::move(dup_key_))
        {}

        bool supportsListTokens() const override { return true; }

        std::optional<GetResult> get(const String & key, Range range = {}) override { return base.get(key, range); }
        HeadResult head(const String & key) override { return base.head(key); }
        PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta = {}) override
        {
            return base.putIfAbsent(key, bytes, meta);
        }
        WriteSinkPtr putIfAbsentStream(const String & key, const ObjectMeta & meta = {}) override
        {
            return base.putIfAbsentStream(key, meta);
        }
        PutResult putOverwrite(const String & key, const String & bytes, const Token & expected,
                               const ObjectMeta & meta = {}) override
        {
            return base.putOverwrite(key, bytes, expected, meta);
        }
        CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                         const ObjectMeta & meta = {}) override
        {
            return base.casPut(key, bytes, expected, meta);
        }
        DeleteOutcome deleteExact(const String & key, const Token & token) override
        {
            return base.deleteExact(key, token);
        }

        /// Inject the duplicate: for any page that would contain `dup_key`, also add a second entry
        /// for that key with the same token (so the token-match would succeed if ambiguity were not detected).
        ListPage list(const String & prefix, const String & cursor, size_t limit) override
        {
            ListPage page = base.list(prefix, cursor, limit);
            /// Find whether dup_key appears in this page and, if so, append a duplicate entry.
            for (size_t i = 0; i < page.keys.size(); ++i)
            {
                if (page.keys[i].key == dup_key)
                {
                    /// Inject a second occurrence of the same key with the same token.
                    page.keys.push_back(page.keys[i]);
                    break;
                }
            }
            return page;
        }

    private:
        InMemoryBackend & base;
        String dup_key;
    };

    auto dup_backend = std::make_shared<DuplicateListBackend>(*real_backend, shard_full_key);
    auto dup_store = Store::open(
        dup_backend, PoolConfig{.pool_prefix = "p", .root_shards = 1});
    Gc gc_dup(dup_store, kGcDiscovery);

    /// The GC on the duplicate-yielding backend should see the shard key as ambiguous and fall back
    /// to Read (even though the token value would otherwise match the sealed folded_token).
    const auto decisions = gc_dup.discoverDecisionsForTest();
    ASSERT_TRUE(decisions.contains(ck));
    EXPECT_EQ(decisions.at(ck), Gc::DiscoverDecision::Read)
        << "an ambiguous (duplicate) listed key must be Read (fail closed; never Skip on ambiguity)";
}
