#include "cas_format_test_battery.h"
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCatalogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <fmt/format.h>

using namespace DB::Cas;

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int LIMIT_EXCEEDED;
}

namespace
{

/// Hand-builds one raw "ent" line, bypassing `encodeRefCatalog` entirely -- used by the decode-side
/// rejection tests, which must exercise bytes the encoder itself would refuse to produce.
String rawEntLine(const String & ns, const String & state, const String & inc_hex,
                   std::optional<std::tuple<String, uint64_t, uint64_t>> creator = std::nullopt)
{
    if (!creator)
        return fmt::format(R"({{"k":"ent","ns":"{}","st":"{}","inc":"{}"}})", ns, state, inc_hex);
    const auto & [srid, we, fg] = *creator;
    return fmt::format(R"({{"k":"ent","ns":"{}","st":"{}","inc":"{}","csr":"{}","cwe":"{}","cfg":"{}"}})",
                        ns, state, inc_hex, srid, we, fg);
}

/// Wraps `ent_lines` in the header/trailer a real `cas_ref_catalog` object carries. `v:1` always
/// passes the header gate (any version <= the build's `G_BUILD` does), matching the convention
/// `gtest_cas_fold_seal_format.cpp`'s `RejectsOutOfRangeNsCleanupState` uses for the same reason.
String rawCatalog(const std::vector<String> & ent_lines)
{
    String out = R"({"type":"cas_ref_catalog","v":1})" "\n";
    for (const String & l : ent_lines)
        out += l + "\n";
    out += fmt::format("{{\"n\":{}}}\n", ent_lines.size());
    return out;
}

CatalogEntry liveEntry(const String & ns, uint64_t inc)
{
    return CatalogEntry{.ns = RootNamespace{ns}, .state = NsState::Live, .incarnation = UInt128(inc)};
}

}

/// ---------- format-battery registration ----------

TEST(CasFormatBattery, RefCatalog)
{
    RefCatalog c;
    c.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Creating,
        .incarnation = UInt128(1),
        .creator = CreatorFence{.server_root_id = "srv1", .writer_epoch = 5, .fence_generation = 2}});
    c.entries.push_back(liveEntry("b", 2));
    runFormatBattery({FormatId::RefCatalog,
        [&] { return sealObject(FormatId::RefCatalog, encodeRefCatalog(c)); },
        [](std::string_view s) { decodeRefCatalog(std::string(openObject(FormatId::RefCatalog, s))); },
        "{\"type\":\"cas_ref_catalog\",\"v\":4}\n"
        "{\"k\":\"ent\",\"ns\":\"a\",\"st\":\"creating\",\"inc\":\"00000000000000000000000000000001\","
        "\"csr\":\"srv1\",\"cwe\":\"5\",\"cfg\":\"2\"}\n"
        "{\"k\":\"ent\",\"ns\":\"b\",\"st\":\"live\",\"inc\":\"00000000000000000000000000000002\"}\n"
        "{\"n\":2}\n"});
}

/// ---------- codec round-trip ----------

TEST(CasRefCatalogFormat, RoundTripsAllThreeStates)
{
    RefCatalog in;
    in.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Creating,
        .incarnation = UInt128(1),
        .creator = CreatorFence{.server_root_id = "srv1", .writer_epoch = 5, .fence_generation = 2}});
    in.entries.push_back(liveEntry("b", 2));
    in.entries.push_back(CatalogEntry{.ns = RootNamespace{"c"}, .state = NsState::Removing, .incarnation = UInt128(3)});

    const RefCatalog out = decodeRefCatalog(encodeRefCatalog(in));
    EXPECT_EQ(out, in);
    EXPECT_EQ(out.entries[0].state, NsState::Creating);
    EXPECT_EQ(out.entries[1].state, NsState::Live);
    EXPECT_EQ(out.entries[2].state, NsState::Removing);
}

TEST(CasRefCatalogFormat, EmptyCatalogRoundTrips)
{
    EXPECT_EQ(decodeRefCatalog(encodeRefCatalog(RefCatalog{})), RefCatalog{});
}

TEST(CasRefCatalogFormat, NamespaceAtExactByteBoundRoundTrips)
{
    RefCatalog c;
    c.entries.push_back(liveEntry(String(kMaxNamespaceBytes, 'a'), 1));
    const RefCatalog out = decodeRefCatalog(encodeRefCatalog(c));
    EXPECT_EQ(out, c);
}

/// ---------- strict rejections: encode side (LOGICAL_ERROR -- our own state, not yet durable) ----------

TEST(CasRefCatalogFormat, EncodeRejectsDuplicateNamespace)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("a", 1));
    c.entries.push_back(liveEntry("a", 2));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

TEST(CasRefCatalogFormat, EncodeRejectsNonCanonicalOrder)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("b", 1));
    c.entries.push_back(liveEntry("a", 2));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

TEST(CasRefCatalogFormat, EncodeRejectsCreatorPresentOnLive)
{
    RefCatalog c;
    c.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128(1),
        .creator = CreatorFence{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1}});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

TEST(CasRefCatalogFormat, EncodeRejectsCreatorAbsentOnCreating)
{
    RefCatalog c;
    c.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Creating, .incarnation = UInt128(1)});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

TEST(CasRefCatalogFormat, EncodeRejectsZeroIncarnation)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("a", 0));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

TEST(CasRefCatalogFormat, EncodeRejectsNameOverByteBound)
{
    RefCatalog c;
    c.entries.push_back(liveEntry(String(kMaxNamespaceBytes + 1, 'a'), 1));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

/// ---------- strict rejections: decode side (CORRUPTED_DATA -- bytes may have come from anywhere) ----------

TEST(CasRefCatalogFormat, DecodeRejectsDuplicateNamespace)
{
    const String bad = rawCatalog({rawEntLine("a", "live", u128ToHex(UInt128(1))),
                                    rawEntLine("a", "live", u128ToHex(UInt128(2)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CasRefCatalogFormat, DecodeRejectsNonCanonicalOrder)
{
    const String bad = rawCatalog({rawEntLine("b", "live", u128ToHex(UInt128(1))),
                                    rawEntLine("a", "live", u128ToHex(UInt128(2)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CasRefCatalogFormat, DecodeRejectsCreatorPresentOnLive)
{
    const String bad = rawCatalog({rawEntLine("a", "live", u128ToHex(UInt128(1)),
                                               std::make_tuple(String("srv"), uint64_t(1), uint64_t(1)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CasRefCatalogFormat, DecodeRejectsCreatorAbsentOnCreating)
{
    const String bad = rawCatalog({rawEntLine("a", "creating", u128ToHex(UInt128(1)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CasRefCatalogFormat, DecodeRejectsZeroIncarnation)
{
    const String bad = rawCatalog({rawEntLine("a", "live", u128ToHex(UInt128(0)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CasRefCatalogFormat, DecodeRejectsNameOverByteBound)
{
    const String too_long_ns(kMaxNamespaceBytes + 1, 'a');
    const String bad = rawCatalog({rawEntLine(too_long_ns, "live", u128ToHex(UInt128(1)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CasRefCatalogFormat, DecodeRejectsUnknownState)
{
    const String bad = rawCatalog({rawEntLine("a", "bogus", u128ToHex(UInt128(1)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

/// ---------- capacity admission: per-predicate boundary tests [codex r2/r3 finding 9] ----------

TEST(CasRefCatalogAdmission, Predicate1AcceptsEqualityRefusesCapPlusOne)
{
    const uint64_t cap = traitsFor(FormatId::RefCatalog).object_cap;
    const RootNamespace ns{"admitted"};
    EXPECT_NO_THROW(checkCatalogObjectBytes(cap, ns));
    try
    {
        checkCatalogObjectBytes(cap + 1, ns);
        FAIL() << "expected LIMIT_EXCEEDED";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LIMIT_EXCEEDED);
        EXPECT_NE(e.message().find("predicate 1"), String::npos) << e.message();
        EXPECT_NE(e.message().find(ns.string()), String::npos) << e.message();
    }
}

TEST(CasRefCatalogAdmission, Predicate2AcceptsEqualityRefusesOneEntryOver)
{
    /// The exact boundary is expressed in ENTRIES (predicate (2) is a sum over admitted entries), so
    /// the boundary count is derived from the real registry constants rather than assumed.
    const uint64_t cap = foldSealCaps().object_cap;
    const uint64_t fixed = foldSealFixedBytes();
    const uint64_t reservation = worstCaseEntryFoldReservationBytes();
    ASSERT_GT(reservation, 0u);
    const uint64_t max_entries = (cap - fixed) / reservation;

    const RootNamespace ns{"admitted"};
    EXPECT_NO_THROW(checkFoldSealReservation(max_entries, ns));
    try
    {
        checkFoldSealReservation(max_entries + 1, ns);
        FAIL() << "expected LIMIT_EXCEEDED";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LIMIT_EXCEEDED);
        EXPECT_NE(e.message().find("predicate 2"), String::npos) << e.message();
        EXPECT_NE(e.message().find(ns.string()), String::npos) << e.message();
    }
}

TEST(CasRefCatalogAdmission, CombinedAdmissionPropagatesCandidateEntryCount)
{
    /// `checkCatalogAdmission` runs predicate (1) then predicate (2) against the SAME candidate; for
    /// an ordinary small catalog both hold slack and it returns the exact bytes `encodeRefCatalog`
    /// would produce.
    RefCatalog candidate;
    candidate.entries.push_back(liveEntry("a", 1));
    candidate.entries.push_back(liveEntry("b", 2));
    const String encoded = checkCatalogAdmission(candidate, RootNamespace{"b"});
    EXPECT_EQ(encoded, encodeRefCatalog(candidate));
}

/// ---------- Constraint 13: removal is never refused, even at the admission boundary ----------

TEST(CasRefCatalogAdmission, RemovalNeverRefusedEvenAtCapacity)
{
    const uint64_t cap = foldSealCaps().object_cap;
    const uint64_t fixed = foldSealFixedBytes();
    const uint64_t reservation = worstCaseEntryFoldReservationBytes();
    const uint64_t max_entries = (cap - fixed) / reservation;

    /// Confirm the boundary is real: one entry beyond it is refused through admission.
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LIMIT_EXCEEDED,
        [&] { checkFoldSealReservation(max_entries + 1, RootNamespace{"z"}); });

    /// Build a catalog carrying exactly `max_entries` Live entries -- as full as admission ever
    /// permits -- directly (a fixture, not itself an admission call).
    RefCatalog full;
    full.entries.reserve(max_entries);
    for (uint64_t i = 0; i < max_entries; ++i)
        full.entries.push_back(liveEntry(fmt::format("ns{:012}", i), i + 1));

    InMemoryBackend backend;
    Layout layout("p");
    backend.putIfAbsent(layout.refCatalogKey(), encodeRefCatalog(full));

    /// The removal transition (Live -> Removing) on one entry goes through the PLAIN update path
    /// (`casUpdate`, which runs no admission check at all) and succeeds even though the catalog is
    /// already at the point where ANY growth would be refused.
    const RefCatalog after = CasRefCatalog::casUpdate(backend, layout, [](const RefCatalog & cur)
    {
        RefCatalog next = cur;
        next.entries[0].state = NsState::Removing;
        return next;
    });
    EXPECT_EQ(after.entries.size(), max_entries);
    EXPECT_EQ(after.entries[0].state, NsState::Removing);
}

/// ---------- Pool/CasRefCatalog: token-CAS read / create / update / conflict-retry ----------

TEST(CasRefCatalog, ReadAbsentReturnsEmptyCatalogWithNoToken)
{
    InMemoryBackend backend;
    Layout layout("p");
    const CasRefCatalog::Snapshot snap = CasRefCatalog::read(backend, layout);
    EXPECT_TRUE(snap.catalog.entries.empty());
    EXPECT_FALSE(snap.token.has_value());
}

TEST(CasRefCatalog, CasUpdateCreatesWhenAbsent)
{
    InMemoryBackend backend;
    Layout layout("p");

    const RefCatalog created = CasRefCatalog::casUpdate(backend, layout, [](const RefCatalog & cur)
    {
        EXPECT_TRUE(cur.entries.empty());
        RefCatalog next;
        next.entries.push_back(liveEntry("a", 7));
        return next;
    });
    ASSERT_EQ(created.entries.size(), 1u);

    const CasRefCatalog::Snapshot snap = CasRefCatalog::read(backend, layout);
    EXPECT_EQ(snap.catalog, created);
    EXPECT_TRUE(snap.token.has_value());
}

TEST(CasRefCatalog, CasUpdateAppliesOnTopOfExistingState)
{
    InMemoryBackend backend;
    Layout layout("p");

    CasRefCatalog::casUpdate(backend, layout, [](const RefCatalog &)
    {
        RefCatalog next;
        next.entries.push_back(liveEntry("a", 1));
        return next;
    });

    const RefCatalog updated = CasRefCatalog::casUpdate(backend, layout, [](const RefCatalog & cur)
    {
        RefCatalog next = cur;
        next.entries.push_back(liveEntry("b", 2));
        return next;
    });
    ASSERT_EQ(updated.entries.size(), 2u);
    EXPECT_EQ(updated.entries[0].ns.string(), "a");
    EXPECT_EQ(updated.entries[1].ns.string(), "b");
}

TEST(CasRefCatalog, CasUpdateRetriesOnConflictAgainstFreshState)
{
    InMemoryBackend backend;
    Layout layout("p");

    CasRefCatalog::casUpdate(backend, layout, [](const RefCatalog &)
    {
        RefCatalog next;
        next.entries.push_back(liveEntry("a", 1));
        return next;
    });

    backend.failNextCasPut(layout.refCatalogKey());   /// one-shot artificial Conflict on the next write

    int mutate_calls = 0;
    const RefCatalog result = CasRefCatalog::casUpdate(backend, layout, [&](const RefCatalog & cur)
    {
        ++mutate_calls;
        RefCatalog next = cur;
        next.entries[0].state = NsState::Removing;
        return next;
    });

    EXPECT_EQ(mutate_calls, 2);   /// first attempt hit the injected conflict; the retry succeeded
    ASSERT_EQ(result.entries.size(), 1u);
    EXPECT_EQ(result.entries[0].state, NsState::Removing);

    const CasRefCatalog::Snapshot snap = CasRefCatalog::read(backend, layout);
    EXPECT_EQ(snap.catalog, result);
}

TEST(CasRefCatalog, CasUpdateAdmittingAcceptsAnOrdinaryCreation)
{
    InMemoryBackend backend;
    Layout layout("p");

    const RefCatalog created = CasRefCatalog::casUpdateAdmitting(backend, layout, RootNamespace{"a"},
        [](const RefCatalog & cur)
        {
            RefCatalog next = cur;
            next.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Creating,
                .incarnation = UInt128(1),
                .creator = CreatorFence{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1}});
            return next;
        });
    ASSERT_EQ(created.entries.size(), 1u);
    EXPECT_EQ(created.entries[0].state, NsState::Creating);
}

TEST(CasRefCatalog, CasUpdateAdmittingRefusesOverCapacity)
{
    InMemoryBackend backend;
    Layout layout("p");

    const uint64_t cap = foldSealCaps().object_cap;
    const uint64_t fixed = foldSealFixedBytes();
    const uint64_t reservation = worstCaseEntryFoldReservationBytes();
    const uint64_t max_entries = (cap - fixed) / reservation;

    /// Seed the catalog directly at the admission boundary (a fixture -- not itself an admission call).
    RefCatalog full;
    full.entries.reserve(max_entries);
    for (uint64_t i = 0; i < max_entries; ++i)
        full.entries.push_back(liveEntry(fmt::format("ns{:012}", i), i + 1));
    backend.putIfAbsent(layout.refCatalogKey(), encodeRefCatalog(full));

    /// Admitting ONE more namespace is refused -- the additive predicate is checked BEFORE the write,
    /// so the backend object is untouched.
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LIMIT_EXCEEDED, [&]
    {
        CasRefCatalog::casUpdateAdmitting(backend, layout, RootNamespace{"zzz"}, [](const RefCatalog & cur)
        {
            RefCatalog next = cur;
            next.entries.push_back(liveEntry("zzz", 999999999));
            return next;
        });
    });

    const CasRefCatalog::Snapshot snap = CasRefCatalog::read(backend, layout);
    EXPECT_EQ(snap.catalog.entries.size(), max_entries);
}
