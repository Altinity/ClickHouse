#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCatalogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasByteBudget.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <limits>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LIMIT_EXCEEDED;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

std::string_view nsStateToWord(NsState s)
{
    switch (s)
    {
        case NsState::Creating: return "creating";
        case NsState::Live:     return "live";
        case NsState::Removing: return "removing";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref catalog: unknown ns state {}", static_cast<int>(s));
}

NsState nsStateFromWord(std::string_view w)
{
    if (w == "creating") return NsState::Creating;
    if (w == "live")     return NsState::Live;
    if (w == "removing") return NsState::Removing;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref catalog: unknown ns state '{}'", w);
}

namespace
{

/// `creator` is required iff `state == Creating`, forbidden otherwise -- one predicate, used by both
/// directions of the codec, so the writer's self-check and the reader's fail-close can never disagree.
bool creatorPairingOk(const CatalogEntry & e)
{
    return (e.state == NsState::Creating) == e.creator.has_value();
}

/// Whether `entries` is already in the catalog's canonical shape: strictly ascending by namespace
/// bytes, no duplicate namespace. A duplicate namespace fails the SAME check as an out-of-order pair
/// (equal keys never compare strictly less), which is exactly right -- both are "not canonical".
bool isCanonicalCatalogOrder(const std::vector<CatalogEntry> & entries)
{
    for (size_t i = 1; i < entries.size(); ++i)
        if (!(entries[i - 1].ns.string() < entries[i].ns.string()))
            return false;
    return true;
}

}

String encodeRefCatalog(const RefCatalog & catalog)
{
    const uint64_t line_cap = traitsFor(FormatId::RefCatalog).line_cap;
    CasJsonWriter out(256);

    /// EVERY line this encoder emits is measured against the LINE cap, on the bytes actually emitted
    /// -- escaping, framing and all -- mirroring `encodeFoldSeal`'s `checkLineBytes`. A line that does
    /// not fit is not a large line, it is an UNREADABLE one: `readLine` refuses it, so the whole
    /// object is lost. Refuse here, where nothing is durable yet.
    const auto checkLineBytes = [&](uint64_t bytes, std::string_view what)
    {
        if (!fitsLineCap(bytes, line_cap))
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS ref catalog: the {} line encodes to {} bytes, over the {}-byte line cap; a longer "
                "line cannot be read back, so the catalog is refused before it is written",
                what, bytes, line_cap);
    };

    writeHeaderLine(out, FormatId::RefCatalog);   /// emits its own terminator
    checkLineBytes(out.size() - 1, "header");

    /// This is our own state, about to become durable: an out-of-order or duplicate-keyed vector is a
    /// bug in the writer, not corruption arriving from a store.
    if (!isCanonicalCatalogOrder(catalog.entries))
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS ref catalog: entries are not canonically ordered -- strictly ascending by namespace "
            "bytes with no duplicate namespace is required before a catalog may be encoded");

    size_t line_start = out.size();
    const auto closeLine = [&](std::string_view what)
    {
        checkLineBytes(out.size() - line_start, what);
        writeChar('\n', out);
        line_start = out.size();
    };

    for (const CatalogEntry & e : catalog.entries)
    {
        if (e.ns.string().size() > kMaxNamespaceBytes)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS ref catalog: namespace '{}' is {} bytes, over the {}-byte admission bound",
                e.ns.string(), e.ns.string().size(), kMaxNamespaceBytes);
        if (e.incarnation == 0)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS ref catalog: namespace '{}' has a zero incarnation -- 0 never names a life",
                e.ns.string());
        if (!creatorPairingOk(e))
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS ref catalog: namespace '{}' is {} and {} a creator fence -- creator is required "
                "iff state == Creating and forbidden otherwise",
                e.ns.string(), nsStateToWord(e.state), e.creator ? "carries" : "lacks");

        bool first = true;
        writeKey(out, "k", first);   writeStringValue(out, "ent");
        writeKey(out, "ns", first);  writeStringValue(out, e.ns.string());
        writeKey(out, "st", first);  writeStringValue(out, nsStateToWord(e.state));
        writeKey(out, "inc", first); writeHex128Value(out, e.incarnation);
        if (e.creator)
        {
            writeKey(out, "csr", first); writeStringValue(out, e.creator->server_root_id);
            writeKey(out, "cwe", first); writeU64StringValue(out, e.creator->writer_epoch);
            writeKey(out, "cfg", first); writeU64StringValue(out, e.creator->fence_generation);
        }
        closeObject(out, first);
        closeLine("ent");
    }

    const size_t trailer_start = out.size();
    writeTrailerLine(out, catalog.entries.size());   /// emits its own terminator
    checkLineBytes(out.size() - trailer_start - 1, "trailer");

    return std::move(out).take();
}

RefCatalog decodeRefCatalog(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::RefCatalog);
    const uint64_t line_cap = traitsFor(FormatId::RefCatalog).line_cap;

    RefCatalog catalog;
    uint64_t seen = 0;
    for (;;)
    {
        const String line = readLine(in, line_cap, "ref catalog");
        ReadBufferFromMemory l(line.data(), line.size());
        JsonObjectReader r(l, KeyStrictness::Strict, "ref catalog");
        String key;
        if (!r.nextKey(key))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref catalog: empty line");

        if (key == "n")
        {
            const uint64_t n = r.readU64Number();
            if (r.nextKey(key))
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref catalog: trailer has extra keys");
            if (!l.eof() || !in.eof())
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref catalog: bytes after trailer");
            if (n != seen)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS ref catalog: trailer count {} != {} records", n, seen);
            return catalog;
        }
        if (key != "k")
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref catalog: record must start with \"k\"");
        const String kind = r.readString();
        if (kind != "ent")
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref catalog: unknown record kind '{}'", kind);

        String ns_str;
        std::optional<String> st_word;
        std::optional<UInt128> inc;
        std::optional<String> csr;
        std::optional<uint64_t> cwe;
        std::optional<uint64_t> cfg;
        while (r.nextKey(key))
        {
            if (key == "ns") ns_str = r.readString();
            else if (key == "st") st_word = r.readString();
            else if (key == "inc") inc = r.readHex128();
            else if (key == "csr") csr = r.readString();
            else if (key == "cwe") cwe = r.readU64String();
            else if (key == "cfg") cfg = r.readU64String();
            else throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref catalog: unknown ent key '{}'", key);
        }
        if (!l.eof())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref catalog: junk after record");

        if (!st_word)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref catalog: entry '{}' missing st", ns_str);
        const NsState state = nsStateFromWord(*st_word);   /// throws CORRUPTED_DATA on an unknown word

        if (ns_str.size() > kMaxNamespaceBytes)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS ref catalog: namespace '{}' is {} bytes, over the {}-byte admission bound",
                ns_str, ns_str.size(), kMaxNamespaceBytes);

        if (!inc)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref catalog: entry '{}' missing inc", ns_str);
        if (*inc == 0)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS ref catalog: namespace '{}' has a zero incarnation -- 0 never names a life", ns_str);

        const bool any_creator_field = csr || cwe || cfg;
        const bool every_creator_field = csr && cwe && cfg;
        std::optional<CreatorFence> creator;
        if (state == NsState::Creating)
        {
            if (!every_creator_field)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS ref catalog: namespace '{}' is Creating but its creator fence is incomplete -- "
                    "server_root_id, writer_epoch and fence_generation are all required", ns_str);
            creator = CreatorFence{.server_root_id = *csr, .writer_epoch = *cwe, .fence_generation = *cfg};
        }
        else if (any_creator_field)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS ref catalog: namespace '{}' carries a creator fence at state '{}' -- creator is "
                "forbidden on anything but Creating", ns_str, *st_word);

        /// Canonical order, checked incrementally as records stream in: a namespace that does not
        /// compare strictly greater than the previous one is either a duplicate or out of order --
        /// both are "not canonical", and this one check rejects either shape.
        if (!catalog.entries.empty() && !(catalog.entries.back().ns.string() < ns_str))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS ref catalog: namespace '{}' does not sort strictly after the previous entry "
                "'{}' -- entries must be canonically ordered with no duplicate namespace",
                ns_str, catalog.entries.back().ns.string());

        catalog.entries.push_back(CatalogEntry{.ns = RootNamespace{ns_str}, .state = state,
                                                .incarnation = *inc, .creator = creator});
        ++seen;
    }
}

void checkCatalogObjectBytes(uint64_t encoded_bytes, const RootNamespace & ns)
{
    const uint64_t cap = traitsFor(FormatId::RefCatalog).object_cap;
    if (!fitsObjectCap(encoded_bytes, /*entries_reservation*/0, cap))
        throw Exception(ErrorCodes::LIMIT_EXCEEDED,
            "CAS ref catalog: admitting '{}' would grow the catalog to {} bytes, over the {}-byte "
            "object cap (predicate 1: encoded_catalog_bytes <= catalog_object_cap) -- refused before "
            "the write", ns.string(), encoded_bytes, cap);
}

uint64_t foldSealFixedBytes()
{
    static const uint64_t bytes = encodeFoldSeal(CasFoldSeal{}).size();
    return bytes;
}

uint64_t worstCaseEntryFoldReservationBytes()
{
    /// Measured through the REAL fold-seal encoder, never a hand-kept formula, so a later change to
    /// the fold seal's wire shape is felt here automatically instead of silently drifting.
    static const uint64_t bytes = []() -> uint64_t
    {
        /// Worst-case namespace content: `kMaxNamespaceBytes` control bytes, each expanding under
        /// `CasJsonWriter::stringValue` to its widest escape (`\u00XX`, 6 bytes -- wider than any
        /// named escape like `\n`/`\t`). This is the byte content admission reserves for EVERY entry,
        /// independent of any real admitted name's actual bytes.
        const String worst_ns(kMaxNamespaceBytes, '\x01');
        constexpr uint64_t kU64Max = std::numeric_limits<uint64_t>::max();
        constexpr uint32_t kU32Max = std::numeric_limits<uint32_t>::max();

        CasFoldSeal seal;
        /// The widest `cov` row: classification 4 (held), every numeric hold field at its widest
        /// decimal rendering, and the longest registered `HoldReason` word.
        seal.per_ns_shard[worst_ns + "/0"] = ShardCoverage{
            .classification = 4,
            .last_folded_ref_id = RefTxnId{kU64Max, kU64Max},
            .hold = RefHold{.reason = HoldReason::UnconsumedSealCrossing,
                             .offending_position = RefTxnId{kU64Max, kU64Max},
                             .retry_count = kU32Max,
                             .next_retry_round = kU64Max}};
        /// The widest `nsc` row: the longer of the two registered `RefNsCleanupState` words.
        const String nsc_key = worst_ns + "\n" + renderRefTxnId(RefTxnId{kU64Max, kU64Max});
        seal.ns_cleanup_items[nsc_key] = RefNsCleanupItem{
            .ns = RootNamespace{worst_ns}, .remove_txn_id = RefTxnId{kU64Max, kU64Max},
            .state = RefNsCleanupState::Completed};

        return encodeFoldSeal(seal).size() - foldSealFixedBytes();
    }();
    return bytes;
}

void checkFoldSealReservation(uint64_t entry_count, const RootNamespace & ns)
{
    const uint64_t cap = foldSealCaps().object_cap;
    const uint64_t fixed = foldSealFixedBytes();
    const uint64_t reservation = entry_count * worstCaseEntryFoldReservationBytes();
    if (!fitsObjectCap(fixed, reservation, cap))
        throw Exception(ErrorCodes::LIMIT_EXCEEDED,
            "CAS ref catalog: admitting '{}' would need a fold seal reserving {} bytes for {} entries "
            "plus the {}-byte fixed frame, over the {}-byte fold-seal object cap (predicate 2: "
            "fold_fixed_bytes + entry_count * worst_case_entry_reservation <= fold_seal_object_cap) -- "
            "refused before the write", ns.string(), reservation, entry_count, fixed, cap);
}

String checkCatalogAdmission(const RefCatalog & candidate, const RootNamespace & admitting_ns)
{
    const String encoded = encodeRefCatalog(candidate);   /// grammar-checked; LOGICAL_ERROR on our own bug
    checkCatalogObjectBytes(encoded.size(), admitting_ns);           /// predicate (1)
    checkFoldSealReservation(candidate.entries.size(), admitting_ns); /// predicate (2)
    return encoded;
}

}
