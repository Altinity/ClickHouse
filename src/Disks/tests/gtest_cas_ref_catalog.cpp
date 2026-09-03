#include "cas_format_test_battery.h"
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCatalogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>
/// Explicit rather than relying on a transitive path: `DEBUG_OR_SANITIZER_BUILD` (used below to gate
/// the `*DeathTest` split) must resolve in THIS translation unit.
#include <base/defines.h>
#include <Poco/AutoPtr.h>
#include <Poco/StreamChannel.h>
#include <fmt/format.h>
#include <algorithm>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <type_traits>
#include <utility>
#include <variant>

#include <magic_enum.hpp>

using namespace DB::Cas;

namespace ProfileEvents
{
    extern const Event CASGCUnmatchedAdoptedParentLives;
    extern const Event CASGCStuckRemovals;
}

namespace DB::Cas::tests
{

/// This friend-only compile pin is derived from the actual private production member pointers. It
/// fails if a raw round carrier becomes separately pairable with `fold`.
class GcRoundPlanSignatureAccess
{
public:
    using FoldSignature = decltype(&Gc::fold);
    using ExpectedFoldSignature = Gc::FoldResult (Gc::*)(
        GcState &, std::optional<Incarnation> &, RoundReport &, uint64_t, const RefPlan &, UniversePolicy,
        GcRoundWorkBudget &);
    using BuilderSignature = decltype(&buildRefWalkPlan);
    using ExpectedBuilderSignature = RefPlan (*)(RoundInput &&);

    static_assert(std::is_same_v<FoldSignature, ExpectedFoldSignature>);
    static_assert(std::is_same_v<BuilderSignature, ExpectedBuilderSignature>);
};

}

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int LIMIT_EXCEEDED;
    extern const int NETWORK_ERROR;
    extern const int BAD_ARGUMENTS;
}

namespace
{

/// Hand-builds one raw `entry` line, bypassing `encodeRefCatalog` entirely -- used by the decode-side
/// rejection tests, which must exercise bytes the encoder itself would refuse to produce.
String rawEntryLine(const String & ns, const String & state, const String & inc_hex,
                   std::optional<std::tuple<String, uint64_t, uint64_t>> creator = std::nullopt)
{
    if (!creator)
        return fmt::format(R"({{"kind":"entry","ns":"{}","state":"{}","life":"{}"}})", ns, state, inc_hex);
    const auto & [srid, we, fg] = *creator;
    return fmt::format(R"({{"kind":"entry","ns":"{}","state":"{}","life":"{}","creator":"{}","creator_epoch":"{}","creator_fence":"{}"}})",
                        ns, state, inc_hex, srid, we, fg);
}

/// Wraps `entry_lines` in the header/trailer a real `cas_ref_catalog` object carries. `v:1` always
/// passes the header gate because any version <= the build's `G_BUILD` does.
String rawCatalog(const std::vector<String> & entry_lines)
{
    String out = R"({"type":"cas_ref_catalog","v":1})" "\n";
    for (const String & l : entry_lines)
        out += l + "\n";
    out += fmt::format("{{\"n\":{}}}\n", entry_lines.size());
    return out;
}

String withRemovalStartedRound(String line, uint64_t round)
{
    const size_t close = line.rfind('}');
    EXPECT_NE(close, String::npos);
    line.insert(close, fmt::format(R"(,"remove_round":"{}")", round));
    return line;
}

CatalogEntry liveEntry(const String & ns, uint64_t inc)
{
    return CatalogEntry{.ns = RootNamespace{ns}, .state = NsState::Live, .incarnation = UInt128(inc)};
}

CatalogEntry entryInState(const String & ns, NsState state, uint64_t inc)
{
    CatalogEntry entry{.ns = RootNamespace{ns}, .state = state, .incarnation = UInt128(inc)};
    if (state == NsState::Creating)
        entry.creator = CreatorFence{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1};
    if (state == NsState::Removing)
        entry.removal_started_round = 1;
    return entry;
}

/// Per-key counts of the WRITE primitive. `CountingBackend` counts reads, heads and lists per key but
/// only totals for writes, and its legacy per-verb counters never see a caller that speaks the
/// primitives -- which every catalog writer below does.
class WriteCountingBackend : public DB::Cas::tests::CountingBackend
{
public:
    uint64_t writes(const String & key) const
    {
        std::lock_guard lock(write_count_mutex);
        const auto it = write_counts.find(key);
        return it == write_counts.end() ? 0 : it->second;
    }

    std::expected<String, DB::Cas::Backend::RawConflict> write(const String & key, const String & bytes,
                                                               const std::optional<String> & expected_value,
                                                               DB::Cas::TransportAccess & access) override
    {
        {
            std::lock_guard lock(write_count_mutex);
            ++write_counts[key];
        }
        return CountingBackend::write(key, bytes, expected_value, access);
    }

private:
    mutable std::mutex write_count_mutex;
    std::map<String, uint64_t> write_counts;
};

/// Lands a competing catalog body under the erase's own attempt and withdraws this actor's admission
/// with it -- the concurrent winner an erase has to be resolved against, driven deterministically and
/// without a second thread.
class EraseWinnerBackend final : public WriteCountingBackend
{
public:
    void replaceOnNextCatalogWrite(const String & key, std::optional<CatalogEntry> replacement_)
    {
        catalog_key = key;
        replacement = std::move(replacement_);
        armed = true;
    }

    bool admitted() const { return !fence_moved; }

    std::expected<String, DB::Cas::Backend::RawConflict> write(const String & key, const String & bytes,
                                                               const std::optional<String> & expected_value,
                                                               DB::Cas::TransportAccess & access) override
    {
        if (armed && key == catalog_key)
        {
            armed = false;
            RefCatalog winner_catalog;
            if (replacement)
                winner_catalog.entries.push_back(*replacement);
            /// Qualified, so the winner's own write is not counted as an attempt of the call under test.
            const auto current = WriteCountingBackend::read(key, access);
            if (!current)
                throw std::runtime_error("test fixture lost mandatory catalog");
            const auto winner = CountingBackend::write(
                key, encodeRefCatalog(winner_catalog), std::optional<String>{current->value}, access);
            if (!winner.has_value())
                throw std::runtime_error("test fixture winner failed to replace catalog");
            fence_moved = true;
        }
        return WriteCountingBackend::write(key, bytes, expected_value, access);
    }

private:
    String catalog_key;
    std::optional<CatalogEntry> replacement;
    bool armed = false;
    bool fence_moved = false;
};

/// Seeds one object, failing the current test rather than returning a value nobody checks.
void seedObject(CasOperation & op, const String & key, const String & bytes)
{
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key, bytes, Retry::standard())));
}

class ScopedCasGcLogCapture
{
public:
    ScopedCasGcLogCapture()
        : logger(getLogger("CasGc"))
        , channel(new Poco::StreamChannel(stream))
        , old_channel(logger->getChannel(), /*shared=*/true)
        , old_level(logger->getLevel())
    {
        logger->setChannel(channel.get());
        logger->setLevel("warning");
    }

    ~ScopedCasGcLogCapture()
    {
        logger->setChannel(old_channel);
        logger->setLevel(old_level);
    }

    String captured() const { return stream.str(); }

private:
    LoggerPtr logger;
    std::ostringstream stream; // STYLE_CHECK_ALLOW_STD_STRING_STREAM
    Poco::AutoPtr<Poco::StreamChannel> channel;
    /// A real reference (shared=true), so the parked previous channel cannot die while ours is installed.
    Poco::AutoPtr<Poco::Channel> old_channel;
    int old_level;
};

}

/// ---------- format-battery registration ----------

CAS_BATTERY_COVERS(RefCatalog);

TEST(CASFormatBattery, RefCatalog)
{
    RefCatalog c;
    c.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Creating,
        .incarnation = UInt128(1),
        .creator = CreatorFence{.server_root_id = "srv1", .writer_epoch = 5, .fence_generation = 2}});
    c.entries.push_back(liveEntry("b", 2));
    runFormatBattery({FormatId::RefCatalog,
        [&] { return sealObject(FormatId::RefCatalog, encodeRefCatalog(c)); },
        [](std::string_view s) { decodeRefCatalog(std::string(openObject(FormatId::RefCatalog, s))); },
        currentFormatHeader("cas_ref_catalog") +
        "{\"kind\":\"entry\",\"ns\":\"a\",\"state\":\"creating\",\"life\":\"00000000000000000000000000000001\","
        "\"creator\":\"srv1\",\"creator_epoch\":\"5\",\"creator_fence\":\"2\"}\n"
        "{\"kind\":\"entry\",\"ns\":\"b\",\"state\":\"live\",\"life\":\"00000000000000000000000000000002\"}\n"
        "{\"n\":2}\n"});
}

/// Closed-set pin: the three `NsState` words, walked through `magic_enum::enum_values`, which is what
/// proves the renderer and the parser consult the SAME table: a table entry missing altogether is
/// already a build error at the coverage assert, but two delegates drifting onto different tables is
/// not.
TEST(CASRefCatalogFormat, ClosedSetPinsNsStateWords)
{
    EXPECT_EQ(nsStateToWord(NsState::Creating), "creating");
    EXPECT_EQ(nsStateToWord(NsState::Live), "live");
    EXPECT_EQ(nsStateToWord(NsState::Removing), "removing");
    for (const auto s : magic_enum::enum_values<NsState>())
        EXPECT_EQ(nsStateFromWord(nsStateToWord(s)), s);
}

/// ---------- codec round-trip ----------

TEST(CASRefCatalogFormat, RoundTripsAllThreeStates)
{
    RefCatalog in;
    in.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Creating,
        .incarnation = UInt128(1),
        .creator = CreatorFence{.server_root_id = "srv1", .writer_epoch = 5, .fence_generation = 2}});
    in.entries.push_back(liveEntry("b", 2));
    in.entries.push_back(CatalogEntry{
        .ns = RootNamespace{"c"},
        .state = NsState::Removing,
        .incarnation = UInt128(3),
        .removal_started_round = 11});

    const RefCatalog out = decodeRefCatalog(encodeRefCatalog(in));
    EXPECT_EQ(out, in);
    EXPECT_EQ(out.entries[0].state, NsState::Creating);
    EXPECT_EQ(out.entries[1].state, NsState::Live);
    EXPECT_EQ(out.entries[2].state, NsState::Removing);
}

/// Mutation caught: making removal age caller-local or optional would let an adopted `Removing` row
/// lose the immutable round from which stuck-removal diagnostics measure.
TEST(CASRefCatalogFormat, RemovalStartedRoundIsRequiredExactlyForRemoving)
{
    CatalogEntry removing{
        .ns = RootNamespace{"removing"},
        .state = NsState::Removing,
        .incarnation = UInt128{7},
        .removal_started_round = 19};
    const RefCatalog catalog{.entries = {removing}};
    const String encoded = encodeRefCatalog(catalog);
    EXPECT_NE(encoded.find("\"remove_round\":\"19\""), String::npos);
    EXPECT_NE(encoded.find("\"state\":\"removing\""), String::npos);
    EXPECT_EQ(decodeRefCatalog(encoded), catalog);

    const String inc = "00000000000000000000000000000009";
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { (void)decodeRefCatalog(rawCatalog({rawEntryLine("missing", "removing", inc)})); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        (void)decodeRefCatalog(rawCatalog({withRemovalStartedRound(rawEntryLine("forbidden", "live", inc), 21)}));
    });
}

TEST(CASRefCatalogFormat, EmptyCatalogRoundTrips)
{
    EXPECT_EQ(decodeRefCatalog(encodeRefCatalog(RefCatalog{})), RefCatalog{});
}

/// Mutation caught: replacing the reverse index with `emplace`-and-ignore would make the first row
/// win. Every lifecycle state participates, both duplicate ids are unresolvable, and an unrelated
/// unique row remains usable by point resolution.
TEST(CASRefCatalogLifeIndex, DuplicatePhysicalIdsAreAmbiguousWithoutPoisoningUniquePointResolution)
{
    RefCatalog catalog;
    catalog.entries = {
        entryInState("a-creating", NsState::Creating, 7),
        entryInState("b-live", NsState::Live, 7),
        entryInState("c-removing", NsState::Removing, 8),
        entryInState("d-live", NsState::Live, 8),
        entryInState("e-unique", NsState::Live, 9),
    };

    const CatalogLifeIndex index(catalog);
    EXPECT_TRUE(index.isAmbiguous(UInt128{7}));
    EXPECT_TRUE(index.isAmbiguous(UInt128{8}));
    EXPECT_THROW(index.resolve(UInt128{7}), DB::Exception);
    EXPECT_THROW(index.resolve(UInt128{8}), DB::Exception);
    const auto unique = index.resolve(UInt128{9});
    ASSERT_TRUE(unique);
    EXPECT_EQ(unique->ns.string(), "e-unique");
}

/// Catalog mutation is destructive authority: any ambiguous current id stops the mutation before a
/// candidate can be written. An unrelated unique point lookup remains available from the same cut.
TEST(CASRefCatalogLifeIndex, AmbiguityStopsCatalogMutationButNotUnrelatedPointLookup)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout layout("p");
    RefCatalog catalog;
    catalog.entries = {
        entryInState("a", NsState::Live, 7),
        entryInState("b", NsState::Removing, 7),
        entryInState("c", NsState::Live, 9),
    };
    seedObject(op, layout.refCatalogKey(), encodeRefCatalog(catalog));
    const auto before = op.read(layout.refCatalogKey(), Retry::standard());
    ASSERT_TRUE(before);

    EXPECT_THROW(CasRefCatalog::casUpdate(op, layout, [](const RefCatalog & current) { return current; }), DB::Exception);
    const auto after = op.read(layout.refCatalogKey(), Retry::standard());
    ASSERT_TRUE(after);
    EXPECT_EQ(after->incarnation, before->incarnation);
    EXPECT_EQ(after->bytes, before->bytes);

    const auto unique = CasRefCatalog::lifeIfCataloged(op, layout, RootNamespace{"c"});
    ASSERT_TRUE(unique);
    EXPECT_EQ(unique->incarnation, UInt128{9});
}

TEST(CASRefCatalogFormat, NamespaceAtExactByteBoundRoundTrips)
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

TEST(CASRefCatalogFormat, EncodeRejectsDuplicateNamespace)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("a", 1));
    c.entries.push_back(liveEntry("a", 2));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

TEST(CASRefCatalogFormat, EncodeRejectsNonCanonicalOrder)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("b", 1));
    c.entries.push_back(liveEntry("a", 2));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

TEST(CASRefCatalogFormat, EncodeRejectsCreatorPresentOnLive)
{
    RefCatalog c;
    c.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128(1),
        .creator = CreatorFence{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1}});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

TEST(CASRefCatalogFormat, EncodeRejectsCreatorAbsentOnCreating)
{
    RefCatalog c;
    c.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Creating, .incarnation = UInt128(1)});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

TEST(CASRefCatalogFormat, EncodeRejectsZeroIncarnation)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("a", 0));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

TEST(CASRefCatalogFormat, EncodeRejectsNameOverByteBound)
{
    RefCatalog c;
    c.entries.push_back(liveEntry(String(kMaxNamespaceBytes + 1, 'a'), 1));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

TEST(CASRefCatalogFormat, EncodeRejectsEmptyNamespace)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("", 1));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRefCatalog(c); });
}

/// Mutation caught: making removal age caller-local or optional would let an adopted `Removing` row
/// lose the immutable round from which stuck-removal diagnostics measure.
TEST(CASRefCatalogFormat, EncodeRejectsLiveWithRemovalStartedRound)
{
    CatalogEntry live_with_round = liveEntry("live", 8);
    live_with_round.removal_started_round = 20;
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { (void)encodeRefCatalog(RefCatalog{.entries = {live_with_round}}); });
}

#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)

TEST(CASRefCatalogFormatDeathTest, EncodeRejectsDuplicateNamespaceAborts)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("a", 1));
    c.entries.push_back(liveEntry("a", 2));
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "not canonically ordered");
}

TEST(CASRefCatalogFormatDeathTest, EncodeRejectsNonCanonicalOrderAborts)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("b", 1));
    c.entries.push_back(liveEntry("a", 2));
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "not canonically ordered");
}

TEST(CASRefCatalogFormatDeathTest, EncodeRejectsCreatorPresentOnLiveAborts)
{
    RefCatalog c;
    c.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128(1),
        .creator = CreatorFence{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1}});
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "carries a creator fence");
}

TEST(CASRefCatalogFormatDeathTest, EncodeRejectsCreatorAbsentOnCreatingAborts)
{
    RefCatalog c;
    c.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Creating, .incarnation = UInt128(1)});
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "lacks a creator fence");
}

TEST(CASRefCatalogFormatDeathTest, EncodeRejectsZeroIncarnationAborts)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("a", 0));
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "zero incarnation");
}

TEST(CASRefCatalogFormatDeathTest, EncodeRejectsNameOverByteBoundAborts)
{
    RefCatalog c;
    c.entries.push_back(liveEntry(String(kMaxNamespaceBytes + 1, 'a'), 1));
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "admission bound");
}

TEST(CASRefCatalogFormatDeathTest, EncodeRejectsEmptyNamespaceAborts)
{
    RefCatalog c;
    c.entries.push_back(liveEntry("", 1));
    EXPECT_DEATH({ (void)encodeRefCatalog(c); }, "namespace must not be empty");
}

TEST(CASRefCatalogFormatDeathTest, EncodeRejectsLiveWithRemovalStartedRoundAborts)
{
    CatalogEntry live_with_round = liveEntry("live", 8);
    live_with_round.removal_started_round = 20;
    EXPECT_DEATH(
        { (void)encodeRefCatalog(RefCatalog{.entries = {live_with_round}}); }, "removal_started_round");
}

#endif

/// A namespace + creator server_root_id that both max out at their respective byte bounds (512 +
/// 255), escaped worst-case, land one `entry` line over the 4 KiB line cap (~4.7 KiB) -- reachable
/// because neither this codec nor `validateServerRootId` restricts the charset, only the length.
/// The refusal must be `LIMIT_EXCEEDED` (a capacity refusal), not `LOGICAL_ERROR` (a bug report) --
/// `encodeFoldSeal`'s own `checkLineBytes` raises `LIMIT_EXCEEDED` for the identical shape of gate.
TEST(CASRefCatalogFormat, EncodeLineOverCapRaisesLimitExceeded)
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

TEST(CASRefCatalogFormat, DecodeRejectsDuplicateNamespace)
{
    const String bad = rawCatalog({rawEntryLine("a", "live", u128ToHex(UInt128(1))),
                                    rawEntryLine("a", "live", u128ToHex(UInt128(2)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CASRefCatalogFormat, DecodeRejectsNonCanonicalOrder)
{
    const String bad = rawCatalog({rawEntryLine("b", "live", u128ToHex(UInt128(1))),
                                    rawEntryLine("a", "live", u128ToHex(UInt128(2)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CASRefCatalogFormat, DecodeRejectsCreatorPresentOnLive)
{
    const String bad = rawCatalog({rawEntryLine("a", "live", u128ToHex(UInt128(1)),
                                               std::make_tuple(String("srv"), uint64_t(1), uint64_t(1)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CASRefCatalogFormat, DecodeRejectsCreatorAbsentOnCreating)
{
    const String bad = rawCatalog({rawEntryLine("a", "creating", u128ToHex(UInt128(1)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CASRefCatalogFormat, DecodeRejectsZeroIncarnation)
{
    const String bad = rawCatalog({rawEntryLine("a", "live", u128ToHex(UInt128(0)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CASRefCatalogFormat, DecodeRejectsNameOverByteBound)
{
    const String too_long_ns(kMaxNamespaceBytes + 1, 'a');
    const String bad = rawCatalog({rawEntryLine(too_long_ns, "live", u128ToHex(UInt128(1)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CASRefCatalogFormat, DecodeRejectsUnknownState)
{
    const String bad = rawCatalog({rawEntryLine("a", "bogus", u128ToHex(UInt128(1)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CASRefCatalogFormat, DecodeRejectsUnknownEntryKey)
{
    const String bad = rawCatalog(
        {R"({"kind":"entry","ns":"a","state":"live","life":"00000000000000000000000000000001","unknown":"x"})"});
    try
    {
        (void)decodeRefCatalog(bad);
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
        EXPECT_NE(e.message().find("unknown entry key"), String::npos) << e.message();
    }
}

TEST(CASRefCatalogFormat, DecodeRejectsEmptyNamespace)
{
    const String bad = rawCatalog({rawEntryLine("", "live", u128ToHex(UInt128(1)))});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

TEST(CASRefCatalogFormat, DecodeRejectsMissingNamespaceKey)
{
    /// No "ns" key at all -- must be refused exactly like an explicit empty one, not read as "".
    const String bad = rawCatalog({R"({"kind":"entry","state":"live","life":")" + u128ToHex(UInt128(1)) + "\"}"});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefCatalog(bad); });
}

/// `nsStateToWord`'s only reachable input is either a live `NsState` or one `nsStateFromWord` already
/// validated on decode, so an unrecognized value is a bug in THIS process -- `LOGICAL_ERROR`, matching
/// this file's own stated taxonomy for the encode-side helper it (indirectly, via `creatorPairingOk`'s
/// error message) serves. Aborts under debug/sanitizer builds -- split like the block above;
/// `CASRefCatalogFormatDeathTest.NsStateToWordRaisesLogicalErrorOnImpossibleValueAborts` covers it there.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASRefCatalogFormat, NsStateToWordRaisesLogicalErrorOnImpossibleValue)
{
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { nsStateToWord(static_cast<NsState>(99)); });
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASRefCatalogFormatDeathTest, NsStateToWordRaisesLogicalErrorOnImpossibleValueAborts)
{
    EXPECT_DEATH({ (void)nsStateToWord(static_cast<NsState>(99)); }, "outside the wire vocabulary"); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange): the whole point of this test is an impossible enum value
}
#endif

/// ---------- registry row / raw-storage tripwire ----------

/// The registry row is part of the contract, mirroring `gtest_cas_ref_ckpt.cpp`'s
/// `RegistryRowIsControlStrictWithTightCaps`: Control/Strict decides how the decoder treats unknown
/// keys, and the caps are the first thing that fires if a foreign object ever lands at the key.
TEST(CASRefCatalogFormat, RegistryRowIsControlStrictWithRawStorage)
{
    const FormatTraits & traits = traitsFor(FormatId::RefCatalog);
    EXPECT_EQ(traits.type, "cas_ref_catalog");
    EXPECT_EQ(traits.family, TextFamily::Control);
    EXPECT_EQ(traits.strictness, KeyStrictness::Strict);
    EXPECT_EQ(traits.object_cap, 256u * 1024u * 1024u);
    EXPECT_EQ(traits.line_cap, 4u * 1024u);
    EXPECT_EQ(traitsForType("cas_ref_catalog"), &traits);
    /// Raw, so the key has no suffix: the catalog hands bytes directly to/from the backend,
    /// bypassing `sealObject`/`openObject` because both are the identity under
    /// `CompressionPolicy::Never`. This line is the TRIPWIRE for that shortcut -- a policy flip to
    /// `Always` would silently write uncompressed bodies under a `.zst` key, which this assertion
    /// catches first.
    EXPECT_EQ(storedSuffix(FormatId::RefCatalog), "");
    EXPECT_EQ(traits.compression, CompressionPolicy::Never);
}

/// ---------- capacity admission: per-predicate boundary tests ----------

TEST(CASRefCatalogAdmission, Predicate1AcceptsEqualityRefusesCapPlusOne)
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

TEST(CASRefCatalogAdmission, Predicate2AcceptsEqualityRefusesOneEntryOver)
{
    const Layout layout("p");
    constexpr uint64_t gc_shards = 1;
    /// The exact boundary is expressed in ENTRIES (predicate (2) is a sum over admitted entries), so
    /// the boundary count is derived from the real registry constants rather than assumed.
    const uint64_t cap = foldSealCaps().object_cap;
    const uint64_t fixed = foldSealFixedBytes();
    const uint64_t reservation = worstCaseEntryFoldReservationBytes();
    ASSERT_GT(reservation, 0u);
    const uint64_t nonentry = widestBlobTargetRunReservationBytes(layout, gc_shards)
        + widestCondemnedSummaryReservationBytes(gc_shards);
    const uint64_t max_entries = (cap - fixed - nonentry) / reservation;

    const RootNamespace ns{"admitted"};
    EXPECT_NO_THROW(checkFoldSealReservation(max_entries, gc_shards, layout, ns));
    try
    {
        checkFoldSealReservation(max_entries + 1, gc_shards, layout, ns);
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
TEST(CASRefCatalogAdmission, Predicate2SaturatesEntryCountReservationInsteadOfWrapping)
{
    const Layout layout("p");
    const uint64_t reservation = worstCaseEntryFoldReservationBytes();
    const uint64_t entry_count = std::numeric_limits<uint64_t>::max() / reservation + 1;
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LIMIT_EXCEEDED,
        [&] { checkFoldSealReservation(entry_count, 1, layout, RootNamespace{"huge"}); });
}

TEST(CASRefCatalogAdmission, CombinedAdmissionPropagatesCandidateEntryCount)
{
    /// `checkCatalogAdmission` runs predicate (1) then predicate (2) against the SAME candidate; for
    /// an ordinary small catalog both hold slack and it returns the exact bytes `encodeRefCatalog`
    /// would produce.
    RefCatalog candidate;
    candidate.entries.push_back(liveEntry("a", 1));
    candidate.entries.push_back(liveEntry("b", 2));
    const Layout layout("p");
    const String encoded = checkCatalogAdmission(candidate, 1, layout, RootNamespace{"b"});
    EXPECT_EQ(encoded, encodeRefCatalog(candidate));
}

TEST(CASRefCatalogAdmission, ReservationCoversActualWidestLegalRowsAcrossDecimalTransitions)
{
    const Layout layout("p/quoted-\"prefix");
    constexpr uint64_t gc_shards = 100;
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();

    for (const uint64_t entry_count : {9, 10, 99, 100})
    {
        CasFoldSeal seal;
        seal.generation = max;
        seal.parent_generation = max;
        for (uint64_t i = 0; i < entry_count; ++i)
        {
            seal.ref_lives.emplace(std::numeric_limits<UInt128>::max() - i, RefLifeFoldState{
                .coverage = RefCoverage{
                    .classification = CoverageClass::Clamped,
                    .last_folded_ref_id = RefTxnId{max, max},
                    .hold = RefHold{
                        .reason = HoldReason::UnconsumedSealCrossing,
                        .offending_position = RefTxnId{max, max},
                        .retry_count = std::numeric_limits<uint32_t>::max(),
                        .next_retry_round = max}},
                .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{max, max}}});
        }
        for (uint64_t shard = 0; shard < gc_shards; ++shard)
        {
            /// Predicate 2 charges exactly `gc_shards` widest `blob_run` rows. This fixture is the maximum
            /// legal cardinality, not an optimistic producer convention: authoritative fold-seal
            /// grammar permits at most one run per shard and requires its canonical key to use seq 0.
            seal.blob_target_runs.push_back(RunRef{
                .key = layout.blobTargetRunKey(max, max, shard, 0),
                .checksum = std::numeric_limits<UInt128>::max(),
                .shard = shard,
                .key_generation = max});
            seal.condemned_summary.emplace(shard, CondemnedSummary{
                .condemned_total = max,
                .pending_total = max,
                .oldest_nonpending_condemn_round = max});
        }

        ASSERT_EQ(seal.blob_target_runs.size(), gc_shards);
        EXPECT_NO_THROW(validateFoldSealForWrite(seal, layout, gc_shards));

        const uint64_t bound = foldSealFixedBytes()
            + entry_count * worstCaseEntryFoldReservationBytes()
            + gc_shards * widestBlobTargetRunReservationBytes(layout, gc_shards)
            + gc_shards * widestCondemnedSummaryReservationBytes(gc_shards);
        EXPECT_LE(encodeFoldSeal(seal).size(), bound) << "entry_count=" << entry_count;
    }
}

/// ---------- Constraint 13: removal is never refused, even at the admission boundary ----------

TEST(CASRefCatalogAdmission, RemovalNeverRefusedEvenAtCapacity)
{
    const Layout layout("p");
    constexpr uint64_t gc_shards = 1;
    const uint64_t cap = foldSealCaps().object_cap;
    const uint64_t fixed = foldSealFixedBytes();
    const uint64_t reservation = worstCaseEntryFoldReservationBytes();
    const uint64_t nonentry = widestBlobTargetRunReservationBytes(layout, gc_shards)
        + widestCondemnedSummaryReservationBytes(gc_shards);
    const uint64_t max_entries = (cap - fixed - nonentry) / reservation;

    /// Confirm the boundary is real: one entry beyond it is refused through admission.
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LIMIT_EXCEEDED,
        [&] { checkFoldSealReservation(max_entries + 1, gc_shards, layout, RootNamespace{"z"}); });

    /// Build a catalog carrying exactly `max_entries` Live entries -- as full as admission ever
    /// permits -- directly (a fixture, not itself an admission call).
    RefCatalog full;
    full.entries.reserve(max_entries);
    for (uint64_t i = 0; i < max_entries; ++i)
        full.entries.push_back(liveEntry(fmt::format("ns{:012}", i), i + 1));

    auto backend = std::make_shared<InMemoryBackend>();

    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);

    CasOperation op = requests.admit();
    seedObject(op, layout.refCatalogKey(), encodeRefCatalog(full));

    /// The removal transition (Live -> Removing) on one entry goes through the PLAIN update path
    /// (`casUpdate`, which runs no admission check at all) and succeeds even though the catalog is
    /// already at the point where ANY growth would be refused.
    const RefCatalog after = CasRefCatalog::casUpdate(op, layout, [](const RefCatalog & cur)
    {
        RefCatalog next = cur;
        next.entries[0].state = NsState::Removing;
        next.entries[0].removal_started_round = 1;
        return next;
    });
    EXPECT_EQ(after.entries.size(), max_entries);
    EXPECT_EQ(after.entries[0].state, NsState::Removing);
}

/// ---------- Pool/CasRefCatalog: read / create / update / conflict-retry ----------

TEST(CASRefCatalog, ReadAbsentFailsClosed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Layout layout("p");
    DB::Cas::tests::expectThrowsCode(
        DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)CasRefCatalog::read(op, layout); });
}

TEST(CASRefCatalog, CasUpdateRefusesWhenAbsent)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Layout layout("p");

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        CasRefCatalog::casUpdate(op, layout, [](const RefCatalog & cur) { return cur; });
    });
    EXPECT_FALSE(op.head(layout.refCatalogKey(), Retry::standard()).has_value());
}

TEST(CASRefCatalog, CasUpdateAppliesOnTopOfExistingState)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Layout layout("p");
    CasRefCatalog::initializeEmptyForNewPool(op, layout);
    CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("a", 1));

    const RefCatalog updated = CasRefCatalog::casUpdate(op, layout, [](const RefCatalog & cur)
    {
        RefCatalog next = cur;
        next.entries[0].state = NsState::Removing;
        next.entries[0].removal_started_round = 1;
        return next;
    });
    ASSERT_EQ(updated.entries.size(), 1u);
    EXPECT_EQ(updated.entries[0].ns.string(), "a");
    EXPECT_EQ(updated.entries[0].state, NsState::Removing);
}

/// `CasRefCatalog::casUpdate`'s identity-preserving refusal throws `LOGICAL_ERROR`, which aborts the
/// whole process in debug/sanitizer builds (`Common/Exception.cpp`'s `handle_error_code`) instead of
/// behaving like a catchable exception -- so the throw-and-catch form below runs only on a plain
/// release build, and `CASRefCatalogDeathTest.GenericCasUpdateCannotDeleteOrReplaceCatalogIdentityAborts`
/// proves the abort positively on debug/sanitizer builds instead.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASRefCatalog, GenericCasUpdateCannotDeleteOrReplaceCatalogIdentity)
{
    const Layout layout("p");
    {
        auto backend = std::make_shared<InMemoryBackend>();
        CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
        CasOperation op = requests.admit();
        CasRefCatalog::initializeEmptyForNewPool(op, layout);
        CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("a", 1));
        DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&]
        {
            (void)CasRefCatalog::casUpdate(op, layout, [](const RefCatalog &) { return RefCatalog{}; });
        });
    }

    {
        auto backend = std::make_shared<InMemoryBackend>();
        CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
        CasOperation op = requests.admit();
        CasRefCatalog::initializeEmptyForNewPool(op, layout);
        CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("a", 1));
        DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&]
        {
            (void)CasRefCatalog::casUpdate(op, layout, [](const RefCatalog & current)
            {
                RefCatalog next = current;
                next.entries[0] = liveEntry("b", 2);
                return next;
            });
        });
    }
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASRefCatalogDeathTest, GenericCasUpdateCannotDeleteOrReplaceCatalogIdentityAborts)
{
    const Layout layout("p");
    {
        auto backend = std::make_shared<InMemoryBackend>();
        CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
        CasOperation op = requests.admit();
        CasRefCatalog::initializeEmptyForNewPool(op, layout);
        CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("a", 1));
        EXPECT_DEATH(
            { (void)CasRefCatalog::casUpdate(op, layout, [](const RefCatalog &) { return RefCatalog{}; }); },
            "cannot add or delete catalog entries");
    }

    {
        auto backend = std::make_shared<InMemoryBackend>();
        CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
        CasOperation op = requests.admit();
        CasRefCatalog::initializeEmptyForNewPool(op, layout);
        CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("a", 1));
        EXPECT_DEATH(
            {
                (void)CasRefCatalog::casUpdate(op, layout, [](const RefCatalog & current)
                {
                    RefCatalog next = current;
                    next.entries[0] = liveEntry("b", 2);
                    return next;
                });
            },
            "cannot replace catalog identity");
    }
}
#endif

TEST(CASRefCatalog, CasUpdateRetriesOnConflictAgainstFreshState)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Layout layout("p");
    CasRefCatalog::initializeEmptyForNewPool(op, layout);
    CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("a", 1));

    backend->refuseNextWrite(layout.refCatalogKey());   /// one-shot artificial Conflict on the next write

    int mutate_calls = 0;
    const RefCatalog result = CasRefCatalog::casUpdate(op, layout, [&](const RefCatalog & cur)
    {
        ++mutate_calls;
        RefCatalog next = cur;
        next.entries[0].state = NsState::Removing;
        next.entries[0].removal_started_round = 1;
        return next;
    });

    EXPECT_EQ(mutate_calls, 2);   /// first attempt hit the injected conflict; the retry succeeded
    ASSERT_EQ(result.entries.size(), 1u);
    EXPECT_EQ(result.entries[0].state, NsState::Removing);

    const CasRefCatalog::Snapshot snap = CasRefCatalog::read(op, layout);
    EXPECT_EQ(snap.catalog, result);
}

TEST(CASRefCatalog, BeginRemovingRechecksAdmissionAfterACatalogConflict)
{
    auto backend = std::make_shared<WriteCountingBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation reader = requests.admit();
    const Layout layout("p");
    const CatalogEntry observed = liveEntry("a", 1);
    CasRefCatalog::initializeEmptyForNewPool(reader, layout);
    CasRefCatalog::casAdmitEntry(reader, layout, 1, observed);
    const uint64_t writes_before = backend->writes(layout.refCatalogKey());

    /// The transition's one attempt is refused, and this actor's admission is withdrawn as it is sent.
    /// The refusal must end the call rather than start another attempt, and it must be reported as an
    /// outcome rather than thrown.
    bool admitted = true;
    backend->refuseNextWrite(layout.refCatalogKey());
    backend->onBeforeWrite(layout.refCatalogKey(), [&admitted] { admitted = false; });
    CasOperation op = requests.admit([&admitted] { return admitted; });

    const auto outcome = CasRefCatalog::beginRemoving(op, layout, observed, /*removal_started_round*/ 13);

    EXPECT_EQ(outcome, CasRefCatalog::BeginRemovingOutcome::FencedOut);
    EXPECT_EQ(backend->writes(layout.refCatalogKey()), writes_before + 1)
        << "the refused attempt must not be followed by another";
    const CasRefCatalog::Snapshot after = CasRefCatalog::read(reader, layout);
    EXPECT_EQ(after.catalog.entries, std::vector<CatalogEntry>{observed})
        << "nothing may be written after the admission is gone";
}

/// A re-read that finds the catalog genuinely ABSENT after it was previously observed present is a
/// real concurrent delete, not a bootstrap -- `casUpdate` must refuse rather than silently create a
/// fresh catalog containing only this one mutation's entry (which would drop every other namespace).
/// Reproduced with a REAL delete (no fault injection needed): `mutate`'s first invocation deletes the
/// seeded object at the incarnation the update's own initial read observed, so its conditional write
/// is refused and the read that settles the refusal genuinely finds the key absent.
/// Missing mandatory authority raises `CORRUPTED_DATA`; the split remains only because the debug
/// variant historically lived in the death-test suite.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASRefCatalog, CasUpdateThrowsOnVanishMidRetryInsteadOfReplacingTheCatalog)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Layout layout("p");
    CasRefCatalog::initializeEmptyForNewPool(op, layout);
    CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("a", 1));
    const CasRefCatalog::Snapshot seeded = CasRefCatalog::read(op, layout);
    ASSERT_TRUE(seeded.incarnation.has_value());

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        CasRefCatalog::casUpdate(op, layout, [&](const RefCatalog & cur)
        {
            EXPECT_EQ(op.remove(layout.refCatalogKey(), *seeded.incarnation, Retry::standard()), Removal::Removed);
            RefCatalog next = cur;
            next.entries[0].state = NsState::Removing;
            next.entries[0].removal_started_round = 1;
            return next;
        });
    });

    /// Nothing was written by the failed attempt: the object is exactly as the delete left it
    /// (absent), never a fresh single-entry catalog.
    EXPECT_FALSE(op.head(layout.refCatalogKey(), Retry::standard()).has_value());
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASRefCatalogDeathTest, CasUpdateThrowsOnVanishMidRetryInsteadOfReplacingTheCatalogAborts)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Layout layout("p");
    CasRefCatalog::initializeEmptyForNewPool(op, layout);
    CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("a", 1));
    const CasRefCatalog::Snapshot seeded = CasRefCatalog::read(op, layout);
    ASSERT_TRUE(seeded.incarnation.has_value());

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
            CasRefCatalog::casUpdate(op, layout, [&](const RefCatalog & cur)
            {
                EXPECT_EQ(op.remove(layout.refCatalogKey(), *seeded.incarnation, Retry::standard()), Removal::Removed);
                RefCatalog next = cur;
                next.entries[0].state = NsState::Removing;
                next.entries[0].removal_started_round = 1;
                return next;
            });
    });
}
#endif

/// Persistent contention ends at the write policy's DEADLINE, with the typed retryable error -- not
/// after a fixed number of unslept iterations, and not in an infinite spin. `mutate` re-arms the
/// one-shot conflict injection on every call, so every attempt is refused; the injected clock reaches
/// the deadline without the test sleeping at all.
TEST(CASRefCatalog, CasUpdateEndsAtTheDeadlineNotAfterAHundredUnsleptIterations)
{
    auto backend = std::make_shared<InMemoryBackend>();
    DB::Cas::tests::FakeClock clock;
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    requests.setNowFnForTest(clock.nowFn());
    requests.setSleepFnForTest(clock.sleepFn());
    CasOperation op = requests.admit();
    Layout layout("p");
    CasRefCatalog::initializeEmptyForNewPool(op, layout);
    const uint64_t started_at = clock.now;

    int mutate_calls = 0;
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&]
    {
        CasRefCatalog::casUpdate(op, layout, [&](const RefCatalog & cur)
        {
            ++mutate_calls;
            backend->refuseNextWrite(layout.refCatalogKey());
            RefCatalog next = cur;
            return next;
        });
    });
    EXPECT_GT(mutate_calls, 1);   /// genuinely retried, not a single-shot failure
    EXPECT_FALSE(clock.sleeps.empty()) << "every retry must back off; an unslept loop would burn the "
                                          "deadline on requests instead of waiting out the contention";
    EXPECT_GE(clock.now - started_at, Retry::standard().window_ms - 5000)
        << "the loop ended at the policy's own deadline, not at an iteration count";
}

TEST(CASRefCatalog, CasAdmitEntryAcceptsAnOrdinaryCreation)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Layout layout("p");
    CasRefCatalog::initializeEmptyForNewPool(op, layout);

    const RefCatalog created = CasRefCatalog::casAdmitEntry(op, layout, 1,
        CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Creating, .incarnation = UInt128(1),
            .creator = CreatorFence{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1}});
    ASSERT_EQ(created.entries.size(), 1u);
    EXPECT_EQ(created.entries[0].state, NsState::Creating);
}

TEST(CASRefCatalog, CasAdmitEntryInsertsAtCanonicalPosition)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Layout layout("p");
    CasRefCatalog::initializeEmptyForNewPool(op, layout);

    CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("b", 1));
    const RefCatalog after = CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("a", 2));
    ASSERT_EQ(after.entries.size(), 2u);
    EXPECT_EQ(after.entries[0].ns.string(), "a");   /// inserted BEFORE "b", not appended
    EXPECT_EQ(after.entries[1].ns.string(), "b");
}

/// Caught by `encodeRefCatalog`'s own canonical-order/no-duplicate grammar check, inside
/// `checkCatalogAdmission` -- no separate duplicate check needed here. That `LOGICAL_ERROR` aborts
/// under debug/sanitizer builds -- split like the blocks above.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASRefCatalog, CasAdmitEntryRejectsADuplicateNamespace)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Layout layout("p");
    CasRefCatalog::initializeEmptyForNewPool(op, layout);
    CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("a", 1));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("a", 2)); });
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASRefCatalogDeathTest, CasAdmitEntryRejectsADuplicateNamespaceAborts)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Layout layout("p");
    CasRefCatalog::initializeEmptyForNewPool(op, layout);
    CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("a", 1));
    EXPECT_DEATH({ CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("a", 2)); }, "not canonically ordered");
}
#endif

TEST(CASRefCatalog, CasAdmitEntryRefusesOverCapacity)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Layout layout("p");

    const uint64_t cap = foldSealCaps().object_cap;
    const uint64_t fixed = foldSealFixedBytes();
    const uint64_t reservation = worstCaseEntryFoldReservationBytes();
    const uint64_t nonentry = widestBlobTargetRunReservationBytes(layout, 1)
        + widestCondemnedSummaryReservationBytes(1);
    const uint64_t max_entries = (cap - fixed - nonentry) / reservation;

    /// Seed the catalog directly at the admission boundary (a fixture -- not itself an admission call).
    RefCatalog full;
    full.entries.reserve(max_entries);
    for (uint64_t i = 0; i < max_entries; ++i)
        full.entries.push_back(liveEntry(fmt::format("ns{:012}", i), i + 1));
    seedObject(op, layout.refCatalogKey(), encodeRefCatalog(full));

    /// Admitting ONE more namespace is refused -- the additive predicate is checked BEFORE the write,
    /// so the backend object is untouched.
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LIMIT_EXCEEDED, [&]
    {
        CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry("zzz", 999999999));
    });

    const CasRefCatalog::Snapshot snap = CasRefCatalog::read(op, layout);
    EXPECT_EQ(snap.catalog.entries.size(), max_entries);
}

TEST(CASRefCatalogRemoval, DeleteCompletedRemovingRequiresExactAdoptedProofAndAdmission)
{
    auto backend = std::make_shared<WriteCountingBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout layout("p");
    const CatalogEntry removing{
        .ns = RootNamespace{"a"},
        .state = NsState::Removing,
        .incarnation = UInt128{7},
        .removal_started_round = 13};
    seedObject(op, layout.refCatalogKey(), encodeRefCatalog(RefCatalog{.entries = {removing}}));

    CasFoldSeal held_parent;
    held_parent.ref_lives.emplace(UInt128{7}, RefLifeFoldState{
        .coverage = RefCoverage{
            .classification = CoverageClass::Clamped,
            .last_folded_ref_id = RefTxnId{1, 2},
            .hold = RefHold{.offending_position = RefTxnId{1, 3}}},
        .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 2}}});
    EXPECT_EQ(CasRefCatalog::deleteCompletedRemoving(op, layout, removing, held_parent),
        CasRefCatalog::CompletedRemovingDeleteOutcome::ProofRefused);

    CasFoldSeal mismatched_parent;
    mismatched_parent.ref_lives.emplace(UInt128{8}, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 2}},
        .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 2}}});
    EXPECT_EQ(CasRefCatalog::deleteCompletedRemoving(op, layout, removing, mismatched_parent),
        CasRefCatalog::CompletedRemovingDeleteOutcome::ProofRefused);

    CasFoldSeal ready_parent;
    ready_parent.ref_lives.emplace(UInt128{7}, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 2}},
        .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 2}}});

    CatalogEntry live = removing;
    live.state = NsState::Live;
    live.removal_started_round.reset();
    EXPECT_EQ(CasRefCatalog::deleteCompletedRemoving(op, layout, live, ready_parent),
        CasRefCatalog::CompletedRemovingDeleteOutcome::ProofRefused);
    CatalogEntry creating = live;
    creating.state = NsState::Creating;
    creating.creator = CreatorFence{.server_root_id = "server", .writer_epoch = 3, .fence_generation = 4};
    EXPECT_EQ(CasRefCatalog::deleteCompletedRemoving(op, layout, creating, ready_parent),
        CasRefCatalog::CompletedRemovingDeleteOutcome::ProofRefused);

    /// An operation whose admission is gone erases nothing and sends nothing. It is driven at a cut an
    /// ADMITTED operation took, because a withdrawn one cannot issue the mandatory read that would
    /// take one.
    CasOperation withdrawn = requests.admit([] { return false; });
    EXPECT_EQ(CasRefCatalog::deleteCompletedRemovingAtSnapshot(
                  withdrawn, layout, CasRefCatalog::read(op, layout), removing, ready_parent),
        CasRefCatalog::CompletedRemovingDeleteOutcome::FencedOut);
    const uint64_t writes_before_erase = backend->writes(layout.refCatalogKey());

    EXPECT_EQ(CasRefCatalog::deleteCompletedRemoving(op, layout, removing, ready_parent),
        CasRefCatalog::CompletedRemovingDeleteOutcome::Deleted);
    EXPECT_TRUE(CasRefCatalog::read(op, layout).catalog.entries.empty());
    EXPECT_EQ(backend->writes(layout.refCatalogKey()), writes_before_erase + 1);
    EXPECT_EQ(backend->listRequests(), 0u);
    EXPECT_EQ(backend->removeRequests(), 0u);
}

TEST(CASRefCatalogRemoval, ExactDeletionRefusesChangedEntryAndAdmissionCannotCarryRemoval)
{
    auto backend = std::make_shared<WriteCountingBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout layout("p");
    CasRefCatalog::initializeEmptyForNewPool(op, layout);
    const CatalogEntry removing{
        .ns = RootNamespace{"a"},
        .state = NsState::Removing,
        .incarnation = UInt128{7},
        .removal_started_round = 13};
#ifndef DEBUG_OR_SANITIZER_BUILD
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { (void)CasRefCatalog::casAdmitEntry(op, layout, 1, removing); });
#endif

    const CatalogEntry current{
        .ns = RootNamespace{"a"},
        .state = NsState::Removing,
        .incarnation = UInt128{7},
        .removal_started_round = 14};
    seedObject(op, "unrelated", "sentinel");
    ASSERT_TRUE(std::holds_alternative<Committed>(op.replace(
        layout.refCatalogKey(), encodeRefCatalog(RefCatalog{.entries = {current}}),
        *CasRefCatalog::read(op, layout).incarnation, Retry::standard())));

    CasFoldSeal ready_parent;
    ready_parent.ref_lives.emplace(UInt128{7}, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 2}},
        .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 2}}});
    EXPECT_EQ(CasRefCatalog::deleteCompletedRemoving(op, layout, removing, ready_parent),
        CasRefCatalog::CompletedRemovingDeleteOutcome::EntryChanged);
    EXPECT_EQ(CasRefCatalog::read(op, layout).catalog.entries, std::vector<CatalogEntry>{current});
}

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASRefCatalogRemovalDeathTest, AdmissionCannotCarryRemovalAborts)
{
    auto backend = std::make_shared<WriteCountingBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout layout("p");
    CasRefCatalog::initializeEmptyForNewPool(op, layout);
    const CatalogEntry removing{
        .ns = RootNamespace{"a"},
        .state = NsState::Removing,
        .incarnation = UInt128{7},
        .removal_started_round = 13};

    EXPECT_DEATH(
        { (void)CasRefCatalog::casAdmitEntry(op, layout, 1, removing); },
        "cannot admit namespace.*directly as Removing");
}
#endif

/// Mutation caught: deriving the control outcome from the resolution read would turn a stale leader's
/// `FencedOut` into `Deleted` or `EntryChanged`. A winner that replaced the catalog under this erase
/// cannot restore the caller's authority to continue the GC round, and the refusal stays a returned
/// outcome rather than an exception -- the caller distinguishes "I lost the round" from "I could not
/// talk to the store" by exactly that.
///
/// An operation whose admission is gone cannot issue the resolution read either, so the result carries
/// the cut this call was GIVEN rather than a fresh one: the erase's own effect is deliberately left
/// unreported, because there is no admitted request left with which to learn it.
TEST(CASRefCatalogRemoval, FenceLossRemainsControlOutcomeWhenWinnerRemovesOrReplacesLife)
{
    for (const bool replace : {false, true})
    {
        auto backend = std::make_shared<EraseWinnerBackend>();
        CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
        CasOperation reader = requests.admit();
        CasOperation op = requests.admit([&backend] { return backend->admitted(); });
        const Layout layout(replace ? "replacement" : "absence");
        const CatalogEntry removing{
            .ns = RootNamespace{"a"},
            .state = NsState::Removing,
            .incarnation = UInt128{7},
            .removal_started_round = 13};
        seedObject(reader, layout.refCatalogKey(), encodeRefCatalog(RefCatalog{.entries = {removing}}));

        CasFoldSeal ready_parent;
        ready_parent.ref_lives.emplace(UInt128{7}, RefLifeFoldState{
            .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 2}},
            .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 2}}});
        std::optional<CatalogEntry> replacement;
        if (replace)
            replacement = CatalogEntry{
                .ns = removing.ns,
                .state = NsState::Live,
                .incarnation = UInt128{8}};
        backend->replaceOnNextCatalogWrite(layout.refCatalogKey(), replacement);

        const CasRefCatalog::CompletedRemovingDeleteResult result
            = CasRefCatalog::deleteCompletedRemovingAtSnapshot(
                op, layout, CasRefCatalog::read(reader, layout), removing, ready_parent);

        EXPECT_EQ(result.outcome, CasRefCatalog::CompletedRemovingDeleteOutcome::FencedOut);
        const RefCatalog current = CasRefCatalog::read(reader, layout).catalog;
        if (replace)
            EXPECT_EQ(current.entries, std::vector<CatalogEntry>{*replacement});
        else
            EXPECT_TRUE(current.entries.empty());
    }
}

/// A transient failure of the erase attempt is settled by the mandatory resolution read and reissued,
/// never concluded from. Treating it as ordinary non-convergence would hide a real backend fault behind
/// `ProofRefused`/`EntryChanged`; treating it as a landed erase would report a deletion nobody proved.
TEST(CASRefCatalogRemoval, ATransientEraseFailureIsResolvedByAReadAndReissued)
{
    auto backend = std::make_shared<WriteCountingBackend>();
    DB::Cas::tests::FakeClock clock;
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    requests.setNowFnForTest(clock.nowFn());
    requests.setSleepFnForTest(clock.sleepFn());
    CasOperation op = requests.admit();
    const Layout layout("cas-put-throw");
    const CatalogEntry removing{
        .ns = RootNamespace{"a"},
        .state = NsState::Removing,
        .incarnation = UInt128{7},
        .removal_started_round = 13};
    seedObject(op, layout.refCatalogKey(), encodeRefCatalog(RefCatalog{.entries = {removing}}));
    CasFoldSeal ready_parent;
    ready_parent.ref_lives.emplace(UInt128{7}, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 2}},
        .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 2}}});

    const uint64_t reads_before = backend->readRequestCount(layout.refCatalogKey());
    backend->failNextWriteWith(layout.refCatalogKey(), std::make_exception_ptr(
        Poco::TimeoutException("injected erase failure whose outcome never reached the caller")));

    EXPECT_EQ(CasRefCatalog::deleteCompletedRemoving(op, layout, removing, ready_parent),
        CasRefCatalog::CompletedRemovingDeleteOutcome::Deleted);
    EXPECT_TRUE(CasRefCatalog::read(op, layout).catalog.entries.empty());
    EXPECT_EQ(backend->writes(layout.refCatalogKey()), 3u)
        << "the seed, the attempt whose outcome was lost, and the reissue that landed";
    EXPECT_GT(backend->readRequestCount(layout.refCatalogKey()), reads_before + 1)
        << "the lost attempt was settled by an exact read before anything was concluded from it";
}

TEST(CASRefCatalogRemoval, CancelStalledCreatingRequiresExactRowAndTerminalCreatorFence)
{
    auto backend = std::make_shared<WriteCountingBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout layout("p");
    const CatalogEntry creating{
        .ns = RootNamespace{"a"},
        .state = NsState::Creating,
        .incarnation = UInt128{7},
        .creator = CreatorFence{.server_root_id = "server", .writer_epoch = 3, .fence_generation = 4}};
    seedObject(op, layout.refCatalogKey(), encodeRefCatalog(RefCatalog{.entries = {creating}}));
    const uint64_t writes_after_seed = backend->writes(layout.refCatalogKey());

    EXPECT_EQ(CasRefCatalog::cancelStalledCreating(
        op, layout, creating, [](const CreatorFence &) { return false; }),
        CasRefCatalog::StalledCreatingCancelOutcome::CreatorFenceStillLive);
    EXPECT_EQ(backend->writes(layout.refCatalogKey()), writes_after_seed);

    CatalogEntry stale = creating;
    stale.creator->writer_epoch = 2;
    EXPECT_EQ(CasRefCatalog::cancelStalledCreating(
        op, layout, stale, [](const CreatorFence &) { return true; }),
        CasRefCatalog::StalledCreatingCancelOutcome::EntryChanged);
    EXPECT_EQ(backend->writes(layout.refCatalogKey()), writes_after_seed);

    EXPECT_EQ(CasRefCatalog::cancelStalledCreating(
        op, layout, creating, [](const CreatorFence &) { return true; }),
        CasRefCatalog::StalledCreatingCancelOutcome::Cancelled);
    EXPECT_TRUE(CasRefCatalog::read(op, layout).catalog.entries.empty());
    EXPECT_EQ(backend->writes(layout.refCatalogKey()), writes_after_seed + 1);
    EXPECT_EQ(backend->listRequests(), 0u);
    EXPECT_EQ(backend->removeRequests(), 0u);
}

TEST(CASGCRefWalkPlan, CatalogIsSoleRowAdmissionAuthorityAcrossOrdinaryAndRebuildInputs)
{
    RefCatalog catalog;
    catalog.entries = {
        CatalogEntry{
            .ns = RootNamespace{"creating"},
            .state = NsState::Creating,
            .incarnation = UInt128{1},
            .creator = CreatorFence{.server_root_id = "server", .writer_epoch = 1, .fence_generation = 1}},
        liveEntry("live", 2),
        CatalogEntry{
            .ns = RootNamespace{"removing"},
            .state = NsState::Removing,
            .incarnation = UInt128{3},
            .removal_started_round = 8},
    };
    const CasRefCatalog::Snapshot cut{
        .catalog = catalog, .incarnation = std::nullopt, .life_index = CatalogLifeIndex(catalog)};

    RefScanSummary ordinary_scan;
    ordinary_scan.parent_ref_lives.emplace(UInt128{1}, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 1}}});
    ordinary_scan.parent_ref_lives.emplace(UInt128{3}, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{3, 3}},
        .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{3, 3}}});
    ordinary_scan.parent_ref_lives.emplace(UInt128{4}, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{4, 4}}});
    ordinary_scan.listed_lives = {UInt128{1}, UInt128{2}, UInt128{4}};
    ordinary_scan.holds.emplace(UInt128{1}, RefHold{.offending_position = RefTxnId{1, 2}});
    ordinary_scan.holds.emplace(UInt128{2}, RefHold{.offending_position = RefTxnId{2, 2}});
    ordinary_scan.checkpoint_observations.emplace(UInt128{1}, RefTxnId{1, 9});
    ordinary_scan.checkpoint_observations.emplace(UInt128{2}, RefTxnId{2, 9});
    ordinary_scan.max_log_by_life.emplace(UInt128{1}, RefTxnId{1, 10});
    ordinary_scan.max_log_by_life.emplace(UInt128{2}, RefTxnId{2, 10});

    RefScanSummary rebuild_scan;
    rebuild_scan.parent_ref_lives.emplace(UInt128{1}, ordinary_scan.parent_ref_lives.at(UInt128{1}));
    rebuild_scan.parent_ref_lives.emplace(UInt128{5}, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{5, 5}}});
    rebuild_scan.listed_lives = {UInt128{1}, UInt128{3}, UInt128{5}};
    rebuild_scan.holds.emplace(UInt128{1}, RefHold{.offending_position = RefTxnId{1, 3}});
    rebuild_scan.holds.emplace(UInt128{3}, RefHold{.offending_position = RefTxnId{3, 4}});
    rebuild_scan.checkpoint_observations.emplace(UInt128{1}, RefTxnId{1, 11});
    rebuild_scan.checkpoint_observations.emplace(UInt128{3}, RefTxnId{3, 11});
    rebuild_scan.max_log_by_life.emplace(UInt128{1}, RefTxnId{1, 12});
    rebuild_scan.max_log_by_life.emplace(UInt128{3}, RefTxnId{3, 12});

    const RefPlan ordinary = tests::buildRefWalkPlanForTest(ordinary_scan, cut);
    const RefPlan rebuild = tests::buildRefWalkPlanForTest(rebuild_scan, cut);
    const auto ordinary_parent_states = ordinary.parentFoldStates();
    const auto rebuild_parent_states = rebuild.parentFoldStates();
    const auto ordinary_successor_states = ordinary.successorFoldStates();
    const auto rebuild_successor_states = rebuild.successorFoldStates();
    EXPECT_EQ(ordinary_parent_states.size(), 1u);
    EXPECT_TRUE(ordinary_parent_states.contains(UInt128{3}));
    EXPECT_FALSE(ordinary_parent_states.contains(UInt128{2}));
    EXPECT_TRUE(ordinary_successor_states.contains(UInt128{2}));
    EXPECT_TRUE(ordinary_successor_states.contains(UInt128{3}));
    EXPECT_TRUE(rebuild_parent_states.empty());
    EXPECT_TRUE(rebuild_successor_states.contains(UInt128{2}));
    EXPECT_TRUE(rebuild_successor_states.contains(UInt128{3}));
    const std::set<UInt128> expected{UInt128{2}, UInt128{3}};
    EXPECT_EQ(ordinary.lifeIds(), expected);
    EXPECT_EQ(rebuild.lifeIds(), expected);

    EXPECT_TRUE(ordinary.row(UInt128{2}).listed_hint);
    ASSERT_TRUE(ordinary.row(UInt128{2}).fold_state.coverage.hold);
    EXPECT_EQ(ordinary.row(UInt128{2}).checkpoint_observation, (RefTxnId{2, 9}));
    EXPECT_EQ(ordinary.row(UInt128{2}).tail_observation, (RefTxnId{2, 10}));
    const std::optional<RefCleanupEvidence> cleanup_evidence{
        RefCleanupEvidence{.remove_txn_id = RefTxnId{3, 3}}};
    EXPECT_EQ(ordinary.row(UInt128{3}).fold_state.cleanup_evidence, cleanup_evidence);
    EXPECT_EQ(ordinary.row(UInt128{3}).removal_started_round, 8u);
    EXPECT_FALSE(ordinary.contains(UInt128{1}));
    EXPECT_FALSE(ordinary.contains(UInt128{4}));

    EXPECT_TRUE(rebuild.row(UInt128{3}).listed_hint);
    ASSERT_TRUE(rebuild.row(UInt128{3}).fold_state.coverage.hold);
    EXPECT_EQ(rebuild.row(UInt128{3}).checkpoint_observation, (RefTxnId{3, 11}));
    EXPECT_EQ(rebuild.row(UInt128{3}).tail_observation, (RefTxnId{3, 12}));
    EXPECT_FALSE(rebuild.contains(UInt128{1}));
    EXPECT_FALSE(rebuild.contains(UInt128{5}));
}

TEST(CASGCStuckRemoval, ThresholdAndRestartUseOnlyDurableRounds)
{
    const Layout layout("p");
    RefWalkPlanRow row{
        .life = NamespaceLifeId::fromCatalogEntry(RootNamespace{"removing"}, UInt128{7}),
        .fold_state = {},
        .removal_started_round = 10,
        .has_parent_fold_state = false,
        .listed_hint = false,
        .checkpoint_observation = std::nullopt,
        .tail_observation = std::nullopt};

    EXPECT_FALSE(stuckRemovalWarning(row, /*current_round=*/12, /*threshold_rounds=*/3, layout));
    const auto at_threshold = stuckRemovalWarning(row, /*current_round=*/13, /*threshold_rounds=*/3, layout);
    const auto next_round = stuckRemovalWarning(row, /*current_round=*/14, /*threshold_rounds=*/3, layout);
    ASSERT_TRUE(at_threshold);
    ASSERT_TRUE(next_round);
    EXPECT_NE(at_threshold->find("age_rounds=3"), String::npos);
    EXPECT_NE(next_round->find("age_rounds=4"), String::npos);

    /// A fresh process given the same durable catalog row and adopted round produces the same signal.
    EXPECT_EQ(stuckRemovalWarning(row, 13, 3, layout), at_threshold);

    row.fold_state.cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 1}};
    EXPECT_FALSE(stuckRemovalWarning(row, 100, 3, layout));
}

TEST(CASGCStuckRemoval, BoundaryAndAbsentVersusUnreadableMessagesAreExact)
{
    const Layout layout("p");
    RefWalkPlanRow row{
        .life = NamespaceLifeId::fromCatalogEntry(RootNamespace{"removing"}, UInt128{7}),
        .fold_state = {},
        .removal_started_round = std::numeric_limits<uint64_t>::max(),
        .has_parent_fold_state = false,
        .listed_hint = false,
        .checkpoint_observation = std::nullopt,
        .tail_observation = std::nullopt};
    EXPECT_FALSE(stuckRemovalWarning(row, 0, 1, layout));

    row.removal_started_round = 1;
    const auto absent = stuckRemovalWarning(row, 2, 1, layout);
    ASSERT_TRUE(absent);
    EXPECT_NE(absent->find("terminal has not folded"), String::npos);
    EXPECT_EQ(absent->find("/_log/"), String::npos) << "an absent terminal has no exact id to name";

    row.fold_state.coverage.classification = CoverageClass::Clamped;
    row.fold_state.coverage.hold = RefHold{
        .reason = HoldReason::BodyUndecodable,
        .offending_position = RefTxnId{5, 6}};
    const auto unreadable = stuckRemovalWarning(row, 2, 1, layout);
    ASSERT_TRUE(unreadable);
    EXPECT_NE(unreadable->find(layout.refLogKey(row.life, RefTxnId{5, 6})), String::npos);
    EXPECT_NE(unreadable->find("is unreadable"), String::npos);
    EXPECT_NE(unreadable->find("restore the exact object"), String::npos);
    EXPECT_NE(unreadable->find("recreate the pool"), String::npos);
    EXPECT_EQ(unreadable->find("REBUILD"), String::npos)
        << "the diagnostic must not promise a command that cannot recover this exact object";
}

TEST(CASGCStuckRemoval, DiagnosticDoesNotAppendOrMutateBackend)
{
    auto backend = std::make_shared<WriteCountingBackend>();
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout layout("p");
    const RefWalkPlanRow row{
        .life = NamespaceLifeId::fromCatalogEntry(RootNamespace{"removing"}, UInt128{7}),
        .fold_state = {},
        .removal_started_round = 1,
        .has_parent_fold_state = false,
        .listed_hint = false,
        .checkpoint_observation = std::nullopt,
        .tail_observation = std::nullopt};
    const uint64_t writes_before = backend->writeRequests();
    EXPECT_TRUE(stuckRemovalWarning(row, 11, 10, layout));
    EXPECT_EQ(backend->writeRequests(), writes_before);
    EXPECT_EQ(backend->removeRequests(), 0u);
}

TEST(CASGCStuckRemoval, AdoptedRoundWarnsEveryRestartWithoutAppending)
{
    auto backend = std::make_shared<WriteCountingBackend>();
    auto store = Pool::open(backend, PoolConfig{
        .pool_prefix = "p",
        .server_root_id = "test",
        .gc_stuck_removal_rounds = 10});
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const UInt128 gc_id{99};
    const UInt128 life_id{7};

    const CatalogEntry removing{
        .ns = RootNamespace{"removing"},
        .state = NsState::Removing,
        .incarnation = life_id,
        .removal_started_round = 1};
    const auto catalog = op.read(layout.refCatalogKey(), Retry::standard());
    ASSERT_TRUE(catalog);
    ASSERT_TRUE(std::holds_alternative<Committed>(op.replace(
        layout.refCatalogKey(), encodeRefCatalog(RefCatalog{.entries = {removing}}),
        catalog->incarnation, Retry::standard())));

    CasFoldSeal seal;
    seal.generation = 1;
    seal.ref_lives.emplace(life_id, RefLifeFoldState{
        .coverage = RefCoverage{
            .classification = CoverageClass::Clamped,
            .hold = RefHold{
                .reason = HoldReason::BodyUndecodable,
                .offending_position = RefTxnId{5, 6},
                .retry_count = 0,
                .next_retry_round = 12}}});
    seal.condemned_summary[0] = CondemnedSummary{};
    seedObject(op, layout.foldSealKey(1, 1), encodeFoldSeal(seal));

    GcState state;
    state.lease = GcLease{.owner = gc_id, .seq = 1};
    state.round = 11;
    state.gc_shards = 1;
    state.snap_generation = 1;
    state.snap_attempt = 1;
    seedObject(op, layout.gcStateKey(), encodeGcState(state));

    const uint64_t signals_before
        = ProfileEvents::global_counters[ProfileEvents::CASGCStuckRemovals].load();
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(removing.ns, life_id);
    const String unreadable_ref_log_key = layout.refLogKey(life, RefTxnId{5, 6});
    const uint64_t append_writes_before = backend->writes(unreadable_ref_log_key);
    ScopedCasGcLogCapture log_capture;
    Gc first_process(store, gc_id);
    EXPECT_TRUE(first_process.runRegularRound().acquired_lease);
    Gc restarted_process(store, gc_id);
    EXPECT_TRUE(restarted_process.runRegularRound().acquired_lease);

    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASGCStuckRemovals].load() - signals_before, 2u);
    EXPECT_EQ(backend->writes(unreadable_ref_log_key), append_writes_before)
        << "the diagnostic cannot append the unreadable ref log";
    const String captured = log_capture.captured();
    EXPECT_EQ(std::count(captured.begin(), captured.end(), '\n'), 2u);
    EXPECT_NE(captured.find(unreadable_ref_log_key), String::npos);
    EXPECT_NE(captured.find("is unreadable"), String::npos);
}

TEST(CASGCStuckRemoval, ZeroThresholdIsRefusedAtGcConstruction)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{
        .pool_prefix = "p",
        .server_root_id = "test",
        .gc_stuck_removal_rounds = 0});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { Gc gc(store, UInt128{1}); });
}

TEST(CASGCRefWalkPlan, UnmatchedAdoptedParentLifeIsObservedWithoutEnteringThePlan)
{
    const NamespaceLifePhysicalId current_life{2};
    const NamespaceLifePhysicalId unmatched_life =
        hexToU128("fedcba98765432100123456789abcdef");
    RefCatalog catalog{.entries = {liveEntry("live", 2)}};
    const CasRefCatalog::Snapshot cut{
        .catalog = catalog, .incarnation = std::nullopt, .life_index = CatalogLifeIndex(catalog)};
    RefScanSummary scan;
    scan.parent_ref_lives.emplace(current_life, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{2, 3}}});
    scan.parent_ref_lives.emplace(unmatched_life, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{9, 9}}});

    const uint64_t events_before =
        ProfileEvents::global_counters[ProfileEvents::CASGCUnmatchedAdoptedParentLives].load();
    const RefPlan plan = tests::buildRefWalkPlanForTest(scan, cut);

    EXPECT_EQ(
        ProfileEvents::global_counters[ProfileEvents::CASGCUnmatchedAdoptedParentLives].load() - events_before,
        1u);
    EXPECT_EQ(plan.droppedParentRows(), 1u);
    EXPECT_EQ(plan.size(), 1u);
    EXPECT_TRUE(plan.contains(current_life));
    EXPECT_FALSE(plan.contains(unmatched_life));
    EXPECT_FALSE(plan.parentFoldStates().contains(unmatched_life));
    EXPECT_FALSE(plan.successorFoldStates().contains(unmatched_life));
}

TEST(CASGCRefPlan, RoundInputOwnsObservationsAndSuccessorStateCannotChangePlan)
{
    /// This catches a plan that borrows the post-LIST observations or lets its successor state alias a
    /// row. Replacing the owning `RoundInput`/`RefPlan` boundary with the former loose inputs, or
    /// returning plan storage for the successor, must make this fail.
    static_assert(!std::is_constructible_v<RoundInput, RefScanSummary, CasRefCatalog::Snapshot>);
    static_assert(!std::is_default_constructible_v<RoundInput>);
    static_assert(!std::is_default_constructible_v<RefPlan>);
    static_assert(!std::is_assignable_v<RefPlan &, RefPlan>);
    static_assert(!std::is_assignable_v<RoundInput &, RoundInput>);

    RefCatalog catalog;
    catalog.entries = {liveEntry("live", 2)};
    CasRefCatalog::Snapshot cut{
        .catalog = catalog, .incarnation = std::nullopt, .life_index = CatalogLifeIndex(catalog)};

    RefScanSummary observations;
    observations.max_log_by_life.emplace(UInt128{2}, RefTxnId{2, 7});
    observations.parent_ref_lives.emplace(UInt128{2}, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{2, 3}}});

    const RefPlan plan = tests::buildRefWalkPlanForTest(observations, cut);

    /// The caller may reuse and mutate the sources after its one post-LIST/catalog observation and
    /// plan construction. Those mutations cannot retarget the plan DEFER, fold, and publication use.
    observations.max_log_by_life.at(UInt128{2}) = RefTxnId{2, 99};
    observations.parent_ref_lives.at(UInt128{2}).coverage.last_folded_ref_id = RefTxnId{2, 88};
    cut.catalog.entries.clear();

    ASSERT_TRUE(plan.contains(UInt128{2}));
    EXPECT_EQ(plan.row(UInt128{2}).tail_observation, (RefTxnId{2, 7}));
    EXPECT_EQ(plan.row(UInt128{2}).fold_state.coverage.last_folded_ref_id, (RefTxnId{2, 3}));

    /// A fold/rebuild successor starts as a copy. It can earn a new cleanup state without changing the
    /// immutable input that DEFER, the fold, and publication all consume.
    auto successor_lives = plan.successorFoldStates();
    successor_lives.at(UInt128{2}).coverage.last_folded_ref_id = RefTxnId{2, 9};
    successor_lives.emplace(UInt128{9}, RefLifeFoldState{});
    EXPECT_EQ(plan.row(UInt128{2}).fold_state.coverage.last_folded_ref_id, (RefTxnId{2, 3}));
    EXPECT_FALSE(plan.contains(UInt128{9}));
}
