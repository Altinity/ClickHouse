#include "cas_format_test_battery.h"
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCatalogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <fmt/format.h>
#include <limits>

using namespace DB::Cas;

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int LIMIT_EXCEEDED;
    extern const int NETWORK_ERROR;
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

/// Every `expectThrowsCode(LOGICAL_ERROR, ...)` in this block aborts the process in debug/sanitizer
/// builds instead of behaving like a catchable exception (`Common/Exception.cpp`'s
/// `handle_error_code`), so each test is split: the throw-and-catch form below runs only on a plain
/// release build, and its `...DeathTest` counterpart (grouped after this block) proves the abort
/// positively on debug/sanitizer builds instead.
#ifndef DEBUG_OR_SANITIZER_BUILD

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

TEST(CasRefCatalogFormat, EncodeRejectsEmptyNamespace)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("", 1));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)

TEST(CasRefCatalogFormatDeathTest, EncodeRejectsDuplicateNamespaceAborts)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("a", 1));
    c.entries.push_back(liveEntry("a", 2));
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "not canonically ordered");
}

TEST(CasRefCatalogFormatDeathTest, EncodeRejectsNonCanonicalOrderAborts)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("b", 1));
    c.entries.push_back(liveEntry("a", 2));
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "not canonically ordered");
}

TEST(CasRefCatalogFormatDeathTest, EncodeRejectsCreatorPresentOnLiveAborts)
{
    RefCatalog c;
    c.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128(1),
        .creator = CreatorFence{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1}});
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "carries a creator fence");
}

TEST(CasRefCatalogFormatDeathTest, EncodeRejectsCreatorAbsentOnCreatingAborts)
{
    RefCatalog c;
    c.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Creating, .incarnation = UInt128(1)});
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "lacks a creator fence");
}

TEST(CasRefCatalogFormatDeathTest, EncodeRejectsZeroIncarnationAborts)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("a", 0));
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "zero incarnation");
}

TEST(CasRefCatalogFormatDeathTest, EncodeRejectsNameOverByteBoundAborts)
{
    RefCatalog c;
    c.entries.push_back(liveEntry(String(kMaxNamespaceBytes + 1, 'a'), 1));
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "admission bound");
}

TEST(CasRefCatalogFormatDeathTest, EncodeRejectsEmptyNamespaceAborts)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("", 1));
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "namespace must not be empty");
}

#endif

/// A namespace + creator server_root_id that both max out at their respective byte bounds (512 +
/// 255), escaped worst-case, land one "ent" line over the 4 KiB line cap (~4.7 KiB) -- reachable
/// because neither this codec nor `validateServerRootId` restricts the charset, only the length.
/// The refusal must be `LIMIT_EXCEEDED` (a capacity refusal), not `LOGICAL_ERROR` (a bug report) --
/// `encodeFoldSeal`'s own `checkLineBytes` raises `LIMIT_EXCEEDED` for the identical shape of gate.
TEST(CasRefCatalogFormat, EncodeLineOverCapRaisesLimitExceeded)
{
    RefCatalog c;
    c.entries.push_back(CatalogEntry{
        .ns = RootNamespace{String(kMaxNamespaceBytes, '\x01')},
        .state = NsState::Creating,
        .incarnation = UInt128(1),
        .creator = CreatorFence{.server_root_id = String(255, '\x01'), .writer_epoch = 1, .fence_generation = 1}});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LIMIT_EXCEEDED, [&] { encodeRefCatalog(c); });
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

TEST(CasRefCatalogFormat, DecodeRejectsEmptyNamespace)
{
    const String bad = rawCatalog({rawEntLine("", "live", u128ToHex(UInt128(1)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CasRefCatalogFormat, DecodeRejectsMissingNamespaceKey)
{
    /// No "ns" key at all -- must be refused exactly like an explicit empty one, not read as "".
    const String bad = rawCatalog({R"({"k":"ent","st":"live","inc":")" + u128ToHex(UInt128(1)) + "\"}"});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

/// `nsStateToWord`'s only reachable input is either a live `NsState` or one `nsStateFromWord` already
/// validated on decode, so an unrecognized value is a bug in THIS process -- `LOGICAL_ERROR`, matching
/// this file's own stated taxonomy for the encode-side helper it (indirectly, via `creatorPairingOk`'s
/// error message) serves. Aborts under debug/sanitizer builds -- split like the block above;
/// `CasRefCatalogFormatDeathTest.NsStateToWordRaisesLogicalErrorOnImpossibleValueAborts` covers it there.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CasRefCatalogFormat, NsStateToWordRaisesLogicalErrorOnImpossibleValue)
{
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { nsStateToWord(static_cast<NsState>(99)); });
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CasRefCatalogFormatDeathTest, NsStateToWordRaisesLogicalErrorOnImpossibleValueAborts)
{
    EXPECT_DEATH({ (void)nsStateToWord(static_cast<NsState>(99)); }, "unknown ns state");
}
#endif

/// ---------- registry row / raw-storage tripwire ----------

/// The registry row is part of the contract, mirroring `gtest_cas_ref_ckpt.cpp`'s
/// `RegistryRowIsControlStrictWithTightCaps`: Control/Strict decides how the decoder treats unknown
/// keys, and the caps are the first thing that fires if a foreign object ever lands at the key.
TEST(CasRefCatalogFormat, RegistryRowIsControlStrictWithRawStorage)
{
    const FormatTraits & traits = traitsFor(FormatId::RefCatalog);
    EXPECT_EQ(traits.type, "cas_ref_catalog");
    EXPECT_EQ(traits.family, TextFamily::Control);
    EXPECT_EQ(traits.strictness, KeyStrictness::Strict);
    EXPECT_EQ(traits.object_cap, 256u * 1024u * 1024u);
    EXPECT_EQ(traits.line_cap, 4u * 1024u);
    EXPECT_EQ(traitsForType("cas_ref_catalog"), &traits);
    /// Raw, so the key has no suffix: `Pool/CasRefCatalog.cpp` hands bytes to/from the backend
    /// directly, bypassing `sealObject`/`openObject` because both are the identity under
    /// `CompressionPolicy::Never`. This line is the TRIPWIRE for that shortcut -- a policy flip to
    /// `Always` would silently write uncompressed bodies under a `.zst` key, which this assertion
    /// catches first (see `CasRefCatalogFormat.h`'s comment on `encodeRefCatalog`).
    EXPECT_EQ(storedSuffix(FormatId::RefCatalog), "");
    EXPECT_EQ(traits.compression, CompressionPolicy::Never);
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

/// `entry_count * worstCaseEntryFoldReservationBytes()` must saturate, not wrap: choosing
/// `entry_count` as the SMALLEST value whose true (unbounded) product with `reservation` crosses
/// 2^64, an unsaturated `uint64_t` multiplication wraps to a remainder SMALLER than `reservation`
/// itself (a few KiB) -- which reads as trivially "fits" a 256 MiB cap even though the real
/// reservation this many entries demands is astronomically larger. A saturating multiply refuses it
/// regardless of the wraparound arithmetic underneath.
TEST(CasRefCatalogAdmission, Predicate2SaturatesEntryCountReservationInsteadOfWrapping)
{
    const uint64_t reservation = worstCaseEntryFoldReservationBytes();
    const uint64_t entry_count = std::numeric_limits<uint64_t>::max() / reservation + 1;
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LIMIT_EXCEEDED,
        [&] { checkFoldSealReservation(entry_count, RootNamespace{"huge"}); });
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

/// A re-read that finds the catalog genuinely ABSENT after it was previously observed present is a
/// real concurrent delete, not a bootstrap -- `casUpdate` must refuse rather than silently create a
/// fresh catalog containing only this one mutation's entry (which would drop every other namespace).
/// Reproduced with a REAL delete (no fault injection needed): `mutate`'s first invocation deletes the
/// seeded object using the token `casUpdate`'s own initial read observed, so the loop's own `casPut`
/// against that now-stale token gets a genuine `Conflict`, and the follow-up re-read genuinely finds
/// the key absent.
/// The `LOGICAL_ERROR` this raises aborts the process in debug/sanitizer builds -- split like the
/// `CasRefCatalogFormat` block above. The DeathTest variant cannot also re-check the post-throw
/// backend state this test verifies (there is no post-abort state in a real debug/sanitizer build).
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CasRefCatalog, CasUpdateThrowsOnVanishMidRetryInsteadOfReplacingTheCatalog)
{
    InMemoryBackend backend;
    Layout layout("p");

    CasRefCatalog::casUpdate(backend, layout, [](const RefCatalog &)
    {
        RefCatalog next;
        next.entries.push_back(liveEntry("a", 1));
        return next;
    });
    const CasRefCatalog::Snapshot seeded = CasRefCatalog::read(backend, layout);
    ASSERT_TRUE(seeded.token.has_value());

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&]
    {
        CasRefCatalog::casUpdate(backend, layout, [&](const RefCatalog & cur)
        {
            backend.deleteExact(layout.refCatalogKey(), *seeded.token);
            RefCatalog next = cur;
            next.entries.push_back(liveEntry("b", 2));
            return next;
        });
    });

    /// Nothing was written by the failed attempt: the object is exactly as the delete left it
    /// (absent), never a fresh single-entry catalog.
    EXPECT_FALSE(CasRefCatalog::read(backend, layout).token.has_value());
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CasRefCatalogDeathTest, CasUpdateThrowsOnVanishMidRetryInsteadOfReplacingTheCatalogAborts)
{
    InMemoryBackend backend;
    Layout layout("p");

    CasRefCatalog::casUpdate(backend, layout, [](const RefCatalog &)
    {
        RefCatalog next;
        next.entries.push_back(liveEntry("a", 1));
        return next;
    });
    const CasRefCatalog::Snapshot seeded = CasRefCatalog::read(backend, layout);
    ASSERT_TRUE(seeded.token.has_value());

    EXPECT_DEATH(
        {
            CasRefCatalog::casUpdate(backend, layout, [&](const RefCatalog & cur)
            {
                backend.deleteExact(layout.refCatalogKey(), *seeded.token);
                RefCatalog next = cur;
                next.entries.push_back(liveEntry("b", 2));
                return next;
            });
        },
        "vanished mid-update");
}
#endif

/// The retry loop is bounded (the same live-lock brake `publishCkpt`/`allocateWriterEpoch` use on
/// their own contended token-CAS singletons) and ends in the typed retryable error, not an infinite
/// spin. `mutate` re-arms the one-shot conflict injection on every call, so every attempt fails.
TEST(CasRefCatalog, CasUpdateGivesUpAfterBoundedAttemptsWithRetryLaterError)
{
    InMemoryBackend backend;
    Layout layout("p");

    int mutate_calls = 0;
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&]
    {
        CasRefCatalog::casUpdate(backend, layout, [&](const RefCatalog & cur)
        {
            ++mutate_calls;
            backend.failNextCasPut(layout.refCatalogKey());
            RefCatalog next = cur;
            return next;
        });
    });
    EXPECT_GT(mutate_calls, 1);   /// genuinely retried, not a single-shot failure
}

TEST(CasRefCatalog, CasAdmitEntryAcceptsAnOrdinaryCreation)
{
    InMemoryBackend backend;
    Layout layout("p");

    const RefCatalog created = CasRefCatalog::casAdmitEntry(backend, layout,
        CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Creating, .incarnation = UInt128(1),
            .creator = CreatorFence{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1}});
    ASSERT_EQ(created.entries.size(), 1u);
    EXPECT_EQ(created.entries[0].state, NsState::Creating);
}

TEST(CasRefCatalog, CasAdmitEntryInsertsAtCanonicalPosition)
{
    InMemoryBackend backend;
    Layout layout("p");

    CasRefCatalog::casAdmitEntry(backend, layout, liveEntry("b", 1));
    const RefCatalog after = CasRefCatalog::casAdmitEntry(backend, layout, liveEntry("a", 2));
    ASSERT_EQ(after.entries.size(), 2u);
    EXPECT_EQ(after.entries[0].ns.string(), "a");   /// inserted BEFORE "b", not appended
    EXPECT_EQ(after.entries[1].ns.string(), "b");
}

/// Caught by `encodeRefCatalog`'s own canonical-order/no-duplicate grammar check, inside
/// `checkCatalogAdmission` -- no separate duplicate check needed here. That `LOGICAL_ERROR` aborts
/// under debug/sanitizer builds -- split like the blocks above.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CasRefCatalog, CasAdmitEntryRejectsADuplicateNamespace)
{
    InMemoryBackend backend;
    Layout layout("p");
    CasRefCatalog::casAdmitEntry(backend, layout, liveEntry("a", 1));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { CasRefCatalog::casAdmitEntry(backend, layout, liveEntry("a", 2)); });
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CasRefCatalogDeathTest, CasAdmitEntryRejectsADuplicateNamespaceAborts)
{
    InMemoryBackend backend;
    Layout layout("p");
    CasRefCatalog::casAdmitEntry(backend, layout, liveEntry("a", 1));
    EXPECT_DEATH({ CasRefCatalog::casAdmitEntry(backend, layout, liveEntry("a", 2)); }, "not canonically ordered");
}
#endif

TEST(CasRefCatalog, CasAdmitEntryRefusesOverCapacity)
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
        CasRefCatalog::casAdmitEntry(backend, layout, liveEntry("zzz", 999999999));
    });

    const CasRefCatalog::Snapshot snap = CasRefCatalog::read(backend, layout);
    EXPECT_EQ(snap.catalog.entries.size(), max_entries);
}
