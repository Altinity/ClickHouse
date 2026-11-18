#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasBoundedRun.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobIntentFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>

#include <Common/ErrorCodes.h>
#include <Common/Exception.h>
#include <Common/HashTable/Hash.h>
#include <Common/scope_guard_safe.h>

#include <algorithm>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int TIMEOUT_EXCEEDED;
}
}

namespace DB::Cas
{

namespace
{
constexpr uint64_t PROGRESS_PAGES = 16;

using Deadline = std::optional<std::chrono::steady_clock::time_point>;

/// Enforce the optional overall scan deadline between backend operations. A timeout is propagated as
/// `TIMEOUT_EXCEEDED`; the public `runFsck` wrapper may convert that exception into a partial report when
/// explicitly requested.
void checkDeadline(const Deadline & deadline, std::string_view phase)
{
    if (deadline && std::chrono::steady_clock::now() > *deadline)
        throw Exception(ErrorCodes::TIMEOUT_EXCEEDED,
            "fsck: exceeded the deadline during '{}' — run against a QUIESCED pool or raise --timeout.", phase);
}

void listAll(CasOperation & op, const String & prefix, std::unordered_map<String, uint64_t> & out,
             const FsckProgress & on_progress, const Deadline & deadline, std::string_view phase)
{
    static constexpr size_t kPageLimit = CasOperation::kCasListPageKeys;
    uint64_t pages = 0;
    size_t count_in_page = 0;
    op.forEachListedKey(prefix, [&](const ListedKey & k)
    {
        out[k.key] = k.size;
        if (++count_in_page == kPageLimit)
        {
            count_in_page = 0;
            ++pages;
            checkDeadline(deadline, phase);
            if (on_progress && pages % PROGRESS_PAGES == 0)
                on_progress(phase, out.size(), pages);
        }
        return true;
    }, Retry::standard(), kPageLimit);
    /// The walk's own list lands at least once even for an empty/undersized final page --
    /// check it here, mirroring the original per-page loop (deadline checked after every physical page).
    if (count_in_page > 0 || pages == 0)
    {
        ++pages;
        checkDeadline(deadline, phase);
    }
    if (on_progress)
        on_progress(phase, out.size(), pages);
}

/// Parse (writer_epoch, build_sequence) from a manifest object key. Delegates to the one shared
/// `Layout::parseManifestKey` instead of hand-rolling a second parser; returns false on a
/// malformed or foreign key.
bool parseBuildPrefix(const Layout & layout, const String & key, BuildPrefix & out)
{
    const auto parsed = layout.parseManifestKey(key);
    if (!parsed)
        return false;
    out.writer_epoch = parsed->ref.writer_epoch;
    out.build_sequence = parsed->ref.build_sequence;
    return true;
}

/// The ref-walk (which builds `reachable_blobs`/`blob_labels`) and the HEAD-confirm below run minutes
/// apart with no snapshot between them. A ref that gets republished (now names a
/// different manifest) or DROPPED in that window, combined with a legitimate GC delete of the OLD
/// blob, makes the stale walk look like a genuine dangle (a "phantom dangling") — this made the fsck
/// oracle dishonest and falsely report a dangle during long-running validation.
///
/// Before counting a HEAD-absent blob as `Dangling`, re-resolve every `"ns/ref"` label under the same
/// immutable catalog row, using a fresh exact `_ckpt` from that original physical life. This admits a
/// same-life repoint/drop while refusing a competing rebirth. `label` is split on the LAST '/' —
/// mirroring exactly how the walk built it (`ns_str + "/" + ref_name`): `ref_name` never contains '/',
/// but `ns_str` may, so the join separator is always the rightmost one.
///
/// Fails CLOSED on any ambiguity (a malformed label, a recovery error, a corrupt manifest): treated as
/// "still referenced", i.e. the original conservative verdict.
/// The fix can only SHRINK false positives — it must never hide a real one.
struct FsckRecoveryAuthority
{
    NamespaceLifeId life;
    CatalogEntry catalog_entry;
};

using FsckRecoveryAuthorities = std::unordered_map<String, FsckRecoveryAuthority>;
using RecordRecoveryUnchecked = std::function<void(const RootNamespace &, const String &, const String &)>;

/// Recheck one ref table against a newer `_ckpt` from the SAME physical life selected by fsck's
/// original catalog cut. The catalog row and life id never move; only the monotone checkpoint may
/// advance, which is how a same-life drop/repoint that completed during a long scan becomes visible
/// without admitting a competing rebirth. A missing or unreadable checkpoint cannot prove that an
/// old owner went away, so the caller records lost coverage and keeps the conservative verdict.
std::optional<RefTableState> recoverLateRefTable(
    CasOperation & op, const Layout & layout, const FsckRecoveryAuthority & authority,
    const RecordRecoveryUnchecked & record_unchecked)
{
    try
    {
        for (size_t restarts = 0;; ++restarts)
        {
            const std::optional<CkptSample> sampled = readCkpt(op, layout, authority.life);
            if (!sampled)
            {
                record_unchecked(authority.life.ns, layout.refCkptKey(authority.life),
                    "late ref recheck: the original life checkpoint is absent");
                return std::nullopt;
            }
            if (std::optional<RecoveredRefTable> recovered
                    = recoverRefTableDetailedFromAuthority(op, layout, authority.catalog_entry, sampled))
                return std::move(recovered->state);
            if (restarts == kRefReadOnlyRecoveryMaxRestarts)
            {
                record_unchecked(authority.life.ns, layout.refCkptKey(authority.life),
                    "late ref recheck: the original life checkpoint kept moving during the walk");
                return std::nullopt;
            }
        }
    }
    catch (const Exception & e)
    {
        record_unchecked(authority.life.ns, layout.refCkptKey(authority.life),
            "late ref recheck: the original life checkpoint or replay is unreadable: " + e.message());
        return std::nullopt;
    }
    catch (...)
    {
        record_unchecked(authority.life.ns, layout.refCkptKey(authority.life),
            "late ref recheck: the original life checkpoint or replay could not be read");
        return std::nullopt;
    }
}

bool blobStillReferenced(CasOperation & op, Pool & store, const Layout & layout,
                          const FsckRecoveryAuthorities & authorities, const String & bkey,
                          const std::vector<String> & labels, const Deadline & deadline,
                          const RecordRecoveryUnchecked & record_unchecked)
{
    if (labels.empty())
        return true;
    for (const String & label : labels)
    {
        checkDeadline(deadline, "re-resolving refs at HEAD-absent");
        const size_t slash = label.rfind('/');
        if (slash == String::npos)
            return true;   /// malformed label — cannot re-resolve, fail closed
        const String ns_part = label.substr(0, slash);
        const String ref_name = label.substr(slash + 1);
        try
        {
            /// Never read a second catalog cut here. A later rebirth may name the same logical namespace
            /// but it is not the life whose original row made this blob reachable in this fsck pass.
            const auto authority_it = authorities.find(ns_part);
            if (authority_it == authorities.end())
            {
                record_unchecked(RootNamespace{ns_part}, layout.refCatalogKey(),
                    "late blob recheck: no original Live/Removing authority was retained");
                return true;   /// no original Live/Removing authority -- fail closed
            }
            const RootNamespace rns{ns_part};
            const std::optional<RefTableState> table = recoverLateRefTable(
                op, layout, authority_it->second, record_unchecked);
            if (!table)
                return true;
            const auto rit = table->getCommitted().find(ref_name);
            if (rit == table->getCommitted().end())
                continue;   /// the ref was DROPPED since the walk — this label no longer applies
            const PartManifest body = store.readManifest(ManifestId{rns, rit->second.manifest_ref});
            for (const ManifestEntry & e : body.entries)
            {
                if (e.placement != EntryPlacement::Blob)
                    continue;
                if (layout.blobKey(e.ref) == bkey)
                    return true;   /// an original-life ref still names this exact blob — a real dangle
            }
        }
        catch (...)
        {
            return true;   /// cannot confirm the ref moved away — keep the conservative verdict
        }
    }
    return false;   /// no original-life label names this blob — the stale-walk artifact is gone
}

/// The manifest sibling of the `blobStillReferenced` recheck above. The ref-walk captures each committed
/// `(ref_name -> manifest_ref)` from a FRESH per-namespace recovery, but the read of `mkey` that
/// confirms the manifest body runs LATER in the same (possibly long) namespace loop. A ref republished to
/// a DIFFERENT manifest — or DROPPED — in that window, combined with a legitimate GC delete of the OLD
/// manifest body, makes the stale captured row look like a committed ref over a missing manifest (a
/// "phantom dangling manifest"), the same dishonest-oracle failure `blobStillReferenced` kills for blobs.
///
/// Before counting a missing manifest body as `Dangling`, re-resolve the EXACT ref from the SAME frozen
/// catalog row with a fresh exact `_ckpt` from that original physical life, then check whether the
/// committed row still names THIS exact manifest key. A later catalog cut must not replace that row,
/// but a same-life checkpoint advance must be visible. Fails CLOSED on any ambiguity (a throw, a corrupt
/// table): treated as "still referenced", the original conservative verdict — the fix can only SHRINK
/// false positives, never hide a real loss.
bool manifestStillReferenced(CasOperation & op, const Layout & layout, const RootNamespace & ns,
                             const FsckRecoveryAuthorities & authorities, const String & ref_name,
                             const String & mkey, const Deadline & deadline,
                             const RecordRecoveryUnchecked & record_unchecked)
{
    checkDeadline(deadline, "re-resolving ref at missing-manifest");
    try
    {
        const auto authority_it = authorities.find(ns.string());
        if (authority_it == authorities.end())
        {
            record_unchecked(ns, layout.refCatalogKey(),
                "late manifest recheck: no original Live/Removing authority was retained");
            return true;   /// no original Live/Removing authority -- fail closed
        }
        const std::optional<RefTableState> table = recoverLateRefTable(
            op, layout, authority_it->second, record_unchecked);
        if (!table)
            return true;
        const auto rit = table->getCommitted().find(ref_name);
        if (rit == table->getCommitted().end())
            return false;   /// the ref was DROPPED since the walk — no longer a committed owner
        /// A republish moved the ref to a different manifest key: this old key is no longer owned.
        return layout.manifestKey(ManifestId{ns, rit->second.manifest_ref}) == mkey;
    }
    catch (...)
    {
        return true;   /// cannot confirm the ref moved away — keep the conservative verdict
    }
}

String renderId(const RefTxnId & id)
{
    return std::to_string(id.writer_epoch) + "-" + std::to_string(id.ref_sequence);
}

/// Per-NAMESPACE verdicts of the stream audit. Both counters count namespaces, not rows: a namespace
/// has exactly one answer about its stream even when several checks reach it.
///
/// A namespace PROVEN broken is never also counted `unchecked`. "Proved broken" and "could not prove"
/// are different answers, and letting the second overwrite or accompany the first would turn a fatal
/// into an ambiguity — the recovery path throws on a holed stream, so a chain-broken namespace reliably
/// produces a downstream failure too, and that failure must not dilute the verdict that explains it.
struct NsVerdicts
{
    std::set<String> chain_broken;
    std::set<String> unchecked;

    void recordChainBroken(FsckReport & report, const RootNamespace & ns, const String & key, String note)
    {
        chain_broken.insert(ns.string());
        unchecked.erase(ns.string());
        push(report, key, FsckClass::ChainBroken, std::move(note));
    }

    void recordUnchecked(FsckReport & report, const RootNamespace & ns, const String & key, String note)
    {
        if (chain_broken.contains(ns.string()))
            return;
        unchecked.insert(ns.string());
        push(report, key, FsckClass::Unchecked, std::move(note));
    }

    /// Both classes are emitted in EVERY mode, not just `detail`: they are namespace verdicts, bounded
    /// by the namespace count, and a summary run that hid them would report a number nobody could act on.
    void push(FsckReport & report, const String & key, FsckClass cls, String note) const
    {
        FsckObject o;
        o.key = key;
        o.kind = ObjectKind::Blob;   /// ref objects have no ObjectKind; reuse Blob as the generic kind
        o.size = 0;
        o.cls = cls;
        o.reachable_from = {std::move(note)};
        report.objects.push_back(std::move(o));
    }

    void publish(FsckReport & report) const
    {
        report.chain_broken = chain_broken.size();
        report.unchecked = unchecked.size();
    }

    /// Takes over the verdicts another instance recorded for namespaces this one has not judged.
    void absorb(const NsVerdicts & other)
    {
        for (const String & ns : other.chain_broken)
        {
            chain_broken.insert(ns);
            unchecked.erase(ns);
        }
        for (const String & ns : other.unchecked)
            if (!chain_broken.contains(ns))
                unchecked.insert(ns);
    }
};

/// Fetches logs inline and remembers what the walk read, so a failure names the key that caused it.
class RecordingKeyReader final : public KeyReader
{
public:
    explicit RecordingKeyReader(CasOperation & op_) : op(op_) {}
    void hint(const String &) override {}
    std::optional<Object> take(const String & key) override
    {
        last_key = key;
        std::optional<Object> got = op.read(key, Retry::standard());
        if (got)
            ++present;
        return got;
    }
    void discard(const String &) override {}
    size_t pending() const override { return 0; }
    size_t window() const override { return 0; }

    std::optional<String> last_key;
    uint64_t present = 0;

private:
    CasOperation & op;
};

/// THE STREAM AUDIT, one namespace: one walk from the `_ckpt` base to the first absent key, shared with
/// the owner replay. It reads no key below the base. A walk whose base moved is restarted from a fresh
/// `_ckpt`, up to `kRefReadOnlyRecoveryMaxRestarts` times; past that the life is unchecked. A corrupt
/// base or chain under an unchanged `_ckpt` is chain-broken, any other failure unchecked. A hole is
/// reported only below a generation 1 or 2 frontier (the walk refuses it) or against a listed log above
/// the walk's end. Returns the replayed table, or `nullopt` after recording a verdict.
std::optional<RefTableState> walkRefStream(
    CasOperation & op, const Layout & layout, const NamespaceLifeId & life, const CatalogEntry & catalog_entry,
    const std::optional<RefTxnId> & greatest_listed_log, const Deadline & deadline, FsckReport & report,
    NsVerdicts & verdicts)
{
    checkDeadline(deadline, "ref stream");
    const RootNamespace & ns = life.ns;
    std::optional<CkptSample> sample = readCkpt(op, layout, life);
    for (size_t restarts = 0;; ++restarts)
    {
        const RecoveryGrounding grounding = chooseRecoveryGrounding(
            catalog_entry, sample ? std::optional<RefCkpt>{sample->ckpt} : std::nullopt);
        RecordingKeyReader reader(op);
        /// Only a corrupt chain under an unchanged `_ckpt` is a finding; a failed read of the chain or of
        /// `_ckpt` itself proves nothing either way.
        const auto record_failure = [&](bool corrupted, const String & message)
        {
            report.ref_records_walked += reader.present;
            String key = layout.namespaceStreamPrefix(life);
            String note = corrupted
                ? "ref stream: " + message
                : "ref stream: the chain or its checkpoint authority could not be revalidated: " + message;
            if (reader.last_key)
                key = *reader.last_key;
            else if (grounding.base)
            {
                key = layout.refSnapshotKey(life, *grounding.base);
                note = "ref stream: checkpoint snapshot base " + renderId(*grounding.base) + " is invalid: " + message;
            }
            if (corrupted)
                verdicts.recordChainBroken(report, ns, key, note);
            else
                verdicts.recordUnchecked(report, ns, key, note);
        };
        std::optional<RecoveredRefTable> recovered;
        try
        {
            recovered = recoverRefTableDetailedFromAuthority(op, layout, catalog_entry, sample, &reader);
        }
        catch (const Exception & e)
        {
            if (e.code() == ErrorCodes::TIMEOUT_EXCEEDED)
                throw;
            record_failure(e.code() == ErrorCodes::CORRUPTED_DATA, e.message());
            return std::nullopt;
        }
        catch (...)
        {
            record_failure(false, getCurrentExceptionMessage(/*with_stacktrace=*/false));
            return std::nullopt;
        }

        if (recovered)
        {
            report.ref_records_walked += reader.present;
            if (recovered->end == RefChainEnd::AbsentKey && greatest_listed_log
                && recovered->next_key < *greatest_listed_log)
            {
                verdicts.recordChainBroken(report, ns, layout.refLogKey(life, *greatest_listed_log),
                    "ref stream: listed log " + renderId(*greatest_listed_log) + " lies above the end of the chain at "
                    + renderId(recovered->next_key));
                return std::nullopt;
            }
            return std::move(recovered->state);
        }

        const String key = reader.last_key.value_or(layout.refCkptKey(life));
        if (restarts == kRefReadOnlyRecoveryMaxRestarts)
        {
            verdicts.recordUnchecked(report, ns, key, "ref stream: checkpoint authority changed while walking the chain");
            return std::nullopt;
        }
        checkDeadline(deadline, "ref stream");
        sample = readCkpt(op, layout, life);
        if (!sample)
        {
            verdicts.recordUnchecked(report, ns, key, "ref stream: checkpoint authority changed: `_ckpt` is absent");
            return std::nullopt;
        }
    }
}

/// Counts one read or `HEAD` of an intent that gave no answer, and keeps the first reason, shortened.
void countIntentUnread(FsckReport & report, String reason)
{
    static constexpr size_t kMaxReasonBytes = 200;
    ++report.intent_unread;
    if (!report.intent_unread_reason.empty())
        return;
    if (reason.size() > kMaxReasonBytes)
        reason = reason.substr(0, kMaxReasonBytes) + "...";
    report.intent_unread_reason = std::move(reason);
}

void countIntentUnread(FsckReport & report, const Exception & e)
{
    countIntentUnread(report, String(ErrorCodes::getName(e.code())) + ": " + e.message());
}

constexpr uint64_t kProgressKeys = 1000;

/// Counts the listed deletion intents. A key that does not parse is `intent_malformed`; every other key is
/// `intent_listed` and, with `verify`, is also read and compared with its body: one `GET` and one `HEAD` per
/// intent, each landing in exactly one intent class. A listed key that vanishes before its read is not an
/// object any more and is not counted. Nothing here decides anything from an absent body: an intent
/// outlives its body until GC's later round.
void classifyIntents(CasOperation & op, const Layout & layout, bool verify, bool detail, const FsckProgress & on_progress,
                     const Deadline & deadline, FsckReport & report)
{
    std::unordered_map<String, uint64_t> listed;
    listAll(op, layout.blobIntentsPrefix(), listed, on_progress, deadline, "listing blob intents");
    std::vector<std::pair<String, uint64_t>> intents(listed.begin(), listed.end());
    std::sort(intents.begin(), intents.end());
    report.intents_verified = verify;

    uint64_t visited = 0;
    for (const auto & [key, size] : intents)
    {
        checkDeadline(deadline, "reading blob intents");
        if (on_progress && ++visited % kProgressKeys == 0)
            on_progress(verify ? "verifying blob intents" : "counting blob intents", visited, 0);

        if (layout.isBlobIntentProbeKey(key))
            continue;   /// the capability probe's scratch object, not an intent
        const auto parsed = layout.parseBlobIntentKey(key);
        if (parsed && !verify)
        {
            ++report.intent_listed;
            report.physical_bytes += size;
            continue;
        }

        FsckClass cls = FsckClass::IntentMalformed;
        String note = "the key is not an intent key";
        if (parsed)
        {
            bool counted = false;
            try
            {
                const auto got = op.read(key, Retry::standard());
                if (!got)
                    continue;
                counted = true;
                ++report.intent_listed;
                if (!isBlobIntentRecord(got->bytes))
                {
                    note = "the bytes are not the intent record";
                }
                else if (const std::optional<Meta> body = op.head(layout.blobKey(parsed->ref), Retry::standard()); !body)
                {
                    cls = FsckClass::IntentBodyGone;
                    note = "no body at the blob's key; GC removes the intent in a later round";
                }
                else if (const std::optional<CanonicalIdentity> seen = body->etag.canonical(); !seen)
                {
                    cls = FsckClass::IntentUnread;
                    note = "the body's token has no canonical identity";
                    countIntentUnread(report, "the token of the body at " + layout.blobKey(parsed->ref) + " has no canonical identity");
                }
                else if (*seen == parsed->identity)
                {
                    cls = FsckClass::IntentCurrent;
                    note = "the body is the incarnation the intent names";
                }
                else
                {
                    cls = FsckClass::IntentReplaced;
                    note = "the body is another incarnation; GC removes the intent later";
                }
            }
            catch (const Exception & e)
            {
                if (e.code() == ErrorCodes::TIMEOUT_EXCEEDED || e.code() == ErrorCodes::LOGICAL_ERROR)
                    throw;
                report.intent_listed += !counted;
                cls = FsckClass::IntentUnread;
                note = "a read failed: " + e.message();
                countIntentUnread(report, e);
            }
        }

        report.physical_bytes += size;
        switch (cls)
        {
            case FsckClass::IntentCurrent:   ++report.intent_current; break;
            case FsckClass::IntentReplaced:  ++report.intent_replaced; break;
            case FsckClass::IntentBodyGone:  ++report.intent_body_gone; break;
            case FsckClass::IntentUnread:    break;   /// counted where it was recorded
            default:                         ++report.intent_malformed; break;
        }
        if (detail)
            report.objects.push_back(FsckObject{.key = key, .kind = ObjectKind::Blob, .size = size, .cls = cls, .reachable_from = {std::move(note)}});
    }
}

/// What the adopted GC snapshot holds about the `wanted` blobs: whether GC knows them at all, the rows it
/// keeps for them, and with `detail` the source edges still counted on them.
struct GcSnapshotView
{
    bool have_state = false;
    /// `gc/state`, the seal it names and every run of it were read, and every run matched its checksum.
    bool complete = false;
    std::unordered_set<BlobRef, BlobRefHash> in_run_hashes;
    std::unordered_map<BlobRef, CondemnedRow, BlobRefHash> retired_by_hash;
    std::unordered_map<BlobRef, std::vector<UInt128>, BlobRefHash> edge_sources;
    /// Every `pending` row, `wanted` or not: GC deletes its body when its own cut shows no reference, so a reference
    /// committed after that cut does not stop the delete.
    std::unordered_map<BlobRef, CondemnedRow, BlobRefHash> pending_rows;
};

/// Reads `gc/state`, the adopted fold seal and every source-edge run of it, once each. A run whose checksum
/// disagrees with the seal is counted into `report.corrupted_runs`, and listed there with `detail`.
GcSnapshotView readGcSnapshot(CasOperation & op, const Layout & layout, const std::unordered_set<BlobRef, BlobRefHash> & wanted,
                              bool detail, const FsckProgress & on_progress, const Deadline & deadline, FsckReport & report)
{
    GcSnapshotView view;
    const auto state_got = op.read(layout.gcStateKey(), Retry::standard());
    if (!state_got)
        return view;
    view.have_state = true;
    const GcState gc_state = decodeGcState(state_got->bytes);
    /// The adopted fold seal names the snapshot runs; resolution is by ref, never by key
    /// construction. Every row whose hash is in our candidate set marks "known to GC" —
    /// edges still counted (drop unfolded), an explicit zero-marker mid-pipeline, or a
    /// `RunMarker::Condemned` sentinel row that carries the condemned state (retired-in-snapshot):
    /// the `RunMarker::Condemned` rows feed `retired_by_hash` in the same pass.
    ///
    /// These sets are keyed by the full `BlobRef`, not a narrowed digest. The run record's own
    /// `BlobRef` is compared directly with the full identity parsed from the listed blob key. This is required for mixed-algorithm
    /// pools: a 64-hex digest must not be truncated or compared as though it used the pool's
    /// local write algorithm, or its true GC state could be hidden as `Unaccounted`.
    if (gc_state.snap_generation == 0)
    {
        view.complete = true;   /// no fold has adopted a seal yet: there are no rows
        return view;
    }
    const auto seal_got = op.read(layout.foldSealKey(gc_state.snap_generation, gc_state.snap_attempt), Retry::standard());
    if (!seal_got)
        return view;
    bool checksums_match = true;
    uint64_t rows = 0;
    for (const RunRef & run : decodeFoldSeal(seal_got->bytes, gc_state.snap_generation).blob_target_runs)
    {
        checkDeadline(deadline, "reading gc snapshot runs");
        /// Typed open: the source-edge run reader goes through openSourceEdgeRun (the NDJSON
        /// header gates type == cas_run + kind == source_edge). Fsck keys off the row's hash
        /// (the record's own algo-prefixed key, never from pool meta).
        SourceEdgeRunView reader = openSourceEdgeRun(op, run.key);
        SourceEdgeRecord rec;
        while (reader.next(rec))
        {
            const BlobRef & ref = rec.ref;
            const bool is_wanted = wanted.contains(ref);
            if (is_wanted)
            {
                view.in_run_hashes.insert(ref);
                if (detail && rec.source_id != UInt128{0})
                    view.edge_sources[ref].push_back(rec.source_id);
            }
            if (rec.marker == RunMarker::Condemned)
            {
                CondemnedRow row = condemnedRowOf(rec);
                if (row.delete_pending && !row.standing)
                    view.pending_rows.emplace(ref, row);
                if (is_wanted)
                    view.retired_by_hash.emplace(ref, std::move(row));
            }
            if (on_progress && ++rows % 65536 == 0)
                on_progress("reading gc snapshot runs", view.in_run_hashes.size(), rows);
        }
        /// Whole-file seal checksum: compare the drained run's accumulated
        /// checksum to the seal's `RunRef::checksum`. Fsck is a read-only auditor — instead of
        /// throwing (which would abort the whole scan on the first corrupt run), catalogue the
        /// mismatch as a `CorruptedRun` finding (with the run key) and continue so the audit
        /// enumerates every problem in one pass. The deletion-deriving consumers
        /// (`fold`/`zeroInDegree`/`previewDeletes`) still fail closed on the same mismatch.
        if (reader.accumulatedChecksum() != run.checksum)
        {
            checksums_match = false;
            ++report.corrupted_runs;
            if (detail)
                report.objects.push_back(FsckObject{.key = run.key, .cls = FsckClass::CorruptedRun, .reachable_from = {}});
        }
    }
    view.complete = checksums_match;
    return view;
}

/// The note of a `candidate` or `standing` row over the body it names.
String retiredNote(const CondemnedRow & row)
{
    if (row.standing)
        return "standing: GC recorded this incarnation as deleted, yet it is present; the intent is present, so a writer "
               "publishes fresh and GC deletes nothing; investigate if it persists";
    return "candidate since round " + std::to_string(row.condemn_round)
        + ": the deletion intent is not confirmed yet; GC creates it in a later round (expected)";
}

/// `PendingWithoutIntent` is a suspect: a finding once a second read of `gc/state` agrees.
struct PendingVerdict
{
    FsckClass cls = FsckClass::PendingGc;
    String note;
};

/// A `pending` row is the one state GC deletes a body from without re-reading the intent, so the intent is read
/// here. A read that fails is `intent_unread`: not an absent intent, and no verdict.
PendingVerdict checkPendingRow(CasOperation & op, const Layout & layout, const BlobRef & ref, const CondemnedRow & row, FsckReport & report)
{
    const std::optional<CanonicalIdentity> identity = row.token.canonical();
    if (!identity)
        return {FsckClass::PendingTokenRejected, "delete_pending: the row's token has no canonical identity, so no intent key exists for it"};
    try
    {
        if (op.read(layout.blobIntentKey(ref, *identity), Retry::standard()))
            return {FsckClass::PendingGc, "delete_pending: the deletion intent is present; GC deletes the body in a later round (expected)"};
        const std::optional<Meta> body = op.head(layout.blobKey(ref), Retry::standard());
        if (body && sameIncarnation(row.token, body->etag))
            return {FsckClass::PendingWithoutIntent, {}};
        return {FsckClass::PendingIntentAbsent,
                "delete_pending: the deletion intent is absent and the body is gone or replaced; the row is about to become "
                "`standing` or to move, nothing is at risk"};
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::TIMEOUT_EXCEEDED || e.code() == ErrorCodes::LOGICAL_ERROR)
            throw;
        countIntentUnread(report, e);
        return {FsckClass::PendingGc, "delete_pending: the deletion intent could not be read: " + e.message()};
    }
}

/// Perform the scan and accumulate into `report`. This helper owns the read-only traversal: it first
/// recovers authoritative refs, then checks physical objects and GC labels, while preserving the
/// distinction between a missing live object and expected in-flight cleanup. Deadline exceptions are
/// intentionally left to `runFsck`, which decides whether partial results were requested.
void runFsckImpl(Pool & store, bool detail, const FsckProgress & on_progress, const Deadline & deadline,
                  const String & namespace_prefix, bool fix_legacy_markers, bool verify_intents, FsckReport & report)
{
    if (fix_legacy_markers && !namespace_prefix.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "fsck: a fix needs a scan of the whole pool, not of namespaces under '{}'", namespace_prefix);
    const Layout & layout = store.layout();
    CasOperation op = store.openRequests().admit();
    /// Path-derived per-object algorithm parsing: every listed blob-tree key -- across every
    /// admitted algo, not just the pool's node-local write algo -- is classified via
    /// `Layout::parseBlobKey`, which derives the `BlobRef` from the key's OWN `<algo>` path segment
    /// (and its `.meta` sibling). A foreign/malformed key (unknown algo segment, wrong-width hex, a
    /// non-`.meta`/non-blob shape) parses to `std::nullopt` and is classified as debris, never an
    /// exception.

    /// Reachability is recomputed from the authoritative refs (never from GC state):
    /// for each namespace, each committed ref resolves to a ManifestId; read its body; a committed ref
    /// naming a MISSING body is an ERROR (Dangling); a present body whose blobs are missing is an ERROR.
    std::set<String> reachable_blobs;        /// blob object keys named by a live owner
    std::set<String> owned_manifest_keys;    /// manifest object keys named by a committed owner
    /// blob key -> "ns/ref" labels of the refs that named it. Always populated (not just under
    /// `detail`) — the HEAD-absent re-resolve below needs it in every mode.
    std::unordered_map<String, std::vector<String>> blob_labels;

    uint64_t refs_walked = 0;
    NsVerdicts verdicts;
    SCOPE_EXIT({ verdicts.publish(report); });
    /// Where an unchecked verdict of a late recheck goes: a life's own scratch while its findings wait
    /// for the catalog confirmation, else the report.
    NsVerdicts * unchecked_sink = &verdicts;
    FsckReport * unchecked_report = &report;
    const RecordRecoveryUnchecked record_recovery_unchecked =
        [&](const RootNamespace & ns, const String & key, const String & detail_text)
        {
            unchecked_sink->recordUnchecked(*unchecked_report, ns, key, detail_text);
        };

    /// RECORD AND CONTINUE for a key that belongs to no namespace at all. fsck is the forensic tool an
    /// operator reaches for once something is already wrong, so a key it cannot attribute must become a
    /// FINDING and not an abort: an audit that died on the first bad key would report nothing about the
    /// healthy namespaces it never reached, which is the wrong failure order for a read-only diagnostic.
    ///
    /// `seen` is what makes the count a count of DEFECTS: each sweep below enumerates namespaces again
    /// and sees the same offending key, and only the first sighting is recorded.
    std::set<String> lifeless_seen;
    auto recordLifelessKeys = [&](const NamespaceListing & listing)
    {
        for (const UnattributableNamespaceKey & bad : listing.skipped)
        {
            if (!lifeless_seen.insert(bad.key).second)
                continue;
            ++report.lifeless_keys;
            FsckObject o;
            o.key = bad.key;
            o.kind = ObjectKind::Blob;   /// a lifeless key has no ObjectKind; reuse Blob as the generic kind
            o.cls = FsckClass::LifelessKey;
            o.size = 0;
            o.reachable_from = {bad.reason};
            report.objects.push_back(std::move(o));
        }
    };

    /// One immutable cut owns every physical-id join in this walk. `Creating` participates in that
    /// attribution (its physical keys may exist) but is never recovered: only Live/Removing rows have a
    /// recoverable chain. A diagnostic records duplicate ids and keeps walking unrelated
    /// unique lives.
    const CasRefCatalog::Snapshot catalog_cut = CasRefCatalog::read(op, layout);
    struct FsckWalkLife
    {
        NamespaceLifeId life;
        CatalogEntry catalog_entry;
    };
    std::vector<FsckWalkLife> walk_lives;
    walk_lives.reserve(catalog_cut.catalog.entries.size());
    for (const CatalogEntry & entry : catalog_cut.catalog.entries)
    {
        if (!entry.ns.string().starts_with(namespace_prefix))
            continue;
        if (entry.state == NsState::Creating)
            continue;
        try
        {
            if (const auto life = catalog_cut.life_index.resolve(entry.incarnation))
                walk_lives.push_back(FsckWalkLife{.life = *life, .catalog_entry = entry});
        }
        catch (const Exception & e)
        {
            if (e.code() != ErrorCodes::CORRUPTED_DATA)
                throw;
            recordLifelessKeys(NamespaceListing{{}, {{
                layout.refCatalogKey() + "#" + renderIncarnation(entry.incarnation), e.message()}}});
        }
    }

    /// Physical life-owned keys carry no logical name. Classify each COMPLETE, canonical key against a
    /// catalog cut taken AFTER this physical listing finishes (observe-then-cut), not the earlier
    /// `catalog_cut` above: `NamespaceJanitor::runOnePage` (the only real deleter of this debris) uses
    /// the identical ordering, and it is what makes "life absent from a LATER cut" sound -- creation
    /// always admits a `Creating` catalog row before writing any life-owned object (spec §2), so a life
    /// that is absent from a cut taken after the listing cannot be a concurrent birth this listing raced.
    /// A malformed shape (the parser refuses, or the reserved segment names no clean relative file) is
    /// classified immediately as it cannot become residue no matter which cut resolves it.
    /// Lives a catalog read after the observation no longer names: their findings are janitor-pending.
    std::unordered_set<UInt128> pending_lives;
    /// The greatest listed log id of each life, the witness that a walk ended above a hole.
    std::unordered_map<UInt128, RefTxnId> greatest_listed_log;
    if (namespace_prefix.empty())
    {
        struct CanonicalNamespaceKey
        {
            String key;
            uint64_t size;
            NamespaceLifePhysicalId life_id;
        };
        std::vector<CanonicalNamespaceKey> canonical_candidates;

        op.forEachListedKey(layout.namespaceRootPrefix(), [&](const ListedKey & listed)
        {
            std::optional<NamespaceLifePhysicalId> physical_id;
            try
            {
                if (const auto ref_object = layout.parseRefObjectKey(listed.key))
                {
                    physical_id = ref_object->life_id;
                    if (ref_object->kind == RefObjectKind::Log)
                    {
                        auto [it, inserted] = greatest_listed_log.emplace(ref_object->life_id, ref_object->txn_id);
                        if (!inserted && it->second < ref_object->txn_id)
                            it->second = ref_object->txn_id;
                    }
                }
                else if (const auto checkpoint = layout.parseRefCkptKey(listed.key))
                    physical_id = *checkpoint;
                else if (const auto namespace_file = layout.parseNamespaceFileKey(listed.key))
                    physical_id = namespace_file->life_id;
                else
                {
                    recordLifelessKeys(NamespaceListing{{}, {{listed.key, "unrecognized key under the namespace ownership tree"}}});
                    return true;
                }
            }
            catch (const Exception & e)
            {
                if (e.code() != ErrorCodes::CORRUPTED_DATA)
                    throw;
                recordLifelessKeys(NamespaceListing{{}, {{listed.key, e.message()}}});
                return true;
            }
            canonical_candidates.push_back(CanonicalNamespaceKey{listed.key, listed.size, *physical_id});
            return true;
        }, Retry::standard());

        /// The post-observation cut. All three catalog states -- `Creating`, `Live`, `Removing` --
        /// protect a life for this purpose; only a life absent from every one of them is residue.
        const CasRefCatalog::Snapshot post_listing_cut = CasRefCatalog::read(op, layout);
        for (const CanonicalNamespaceKey & candidate : canonical_candidates)
        {
            try
            {
                if (post_listing_cut.life_index.resolve(candidate.life_id))
                    continue;   /// protected by some catalog state as of the later cut -- not residue
            }
            catch (const Exception & e)
            {
                /// The reverse life index throws `CORRUPTED_DATA` when the post-listing cut carries a
                /// duplicated life id: a catalog defect, not evidence about THIS key. Record and keep
                /// walking, same as every other catalog-authority failure in this scan -- an audit that
                /// aborted on the first bad key would report nothing about the healthy candidates
                /// still queued behind it.
                if (e.code() != ErrorCodes::CORRUPTED_DATA)
                    throw;
                recordLifelessKeys(NamespaceListing{{}, {{candidate.key, e.message()}}});
                continue;
            }
            ++report.namespace_janitor_pending;
            report.namespace_janitor_pending_bytes += candidate.size;
            pending_lives.insert(candidate.life_id);
            FsckObject o;
            o.key = candidate.key;
            o.kind = ObjectKind::Blob;   /// no ObjectKind names namespace-life debris; reuse Blob as the generic kind
            o.cls = FsckClass::JanitorPending;
            o.size = candidate.size;
            o.reachable_from = {"physical life id is absent from a catalog cut taken after this listing; "
                                 "janitor-pending, not corruption"};
            report.objects.push_back(std::move(o));
        }
    }

    /// Every replay and late recheck below reuses the same exact catalog row and physical life. Each walk
    /// reads `_ckpt` at that SAME life, so a concurrent same-life drop/repoint is visible without ever
    /// accepting a rebirth.
    FsckRecoveryAuthorities recovery_authorities;
    recovery_authorities.reserve(walk_lives.size());

    /// A life's findings, held until a catalog read after the loop confirms the life: a walk that spans
    /// a janitor's deletes reads a deleted key as the end, which is no finding about a removed life.
    struct DeferredLifeFindings
    {
        NsVerdicts verdicts;
        FsckReport scratch;
    };
    std::vector<DeferredLifeFindings> deferred(walk_lives.size());

    /// A deadline leaves the held findings unconfirmed: they are reported unchecked, never as corruption.
    const auto report_held_as_unconfirmed = [&]
    {
        unchecked_sink = &verdicts;
        unchecked_report = &report;
        for (size_t life_index = 0; life_index < walk_lives.size(); ++life_index)
        {
            const DeferredLifeFindings & findings = deferred[life_index];
            report.ref_records_walked += findings.scratch.ref_records_walked;
            if (findings.verdicts.chain_broken.empty() && findings.verdicts.unchecked.empty()
                && findings.scratch.dangling == 0 && findings.scratch.objects.empty())
                continue;
            const NamespaceLifeId & life = walk_lives[life_index].life;
            verdicts.recordUnchecked(report, life.ns, layout.namespaceStreamPrefix(life),
                "fsck findings for this namespace were not confirmed before the deadline");
        }
    };
    try
    {
        for (size_t life_index = 0; life_index < walk_lives.size(); ++life_index)
        {
            const FsckWalkLife & walk_life = walk_lives[life_index];
            const NamespaceLifeId & life = walk_life.life;
            const RootNamespace & ns = life.ns;
            const String & ns_str = ns.string();
            DeferredLifeFindings & findings = deferred[life_index];
            unchecked_sink = &findings.verdicts;
            unchecked_report = &findings.scratch;
            /// RECORD AND CONTINUE, NEVER WEDGE. Everything below is per-namespace, and every one of these
            /// steps can raise `CORRUPTED_DATA` on a namespace whose stream is damaged — the replay refuses a
            /// non-contiguous tail, the codecs refuse an invalid body. For RECOVERY that throw is the correct
            /// fail-close; for a read-only diagnostic it is a bug, because the audit then reports NOTHING
            /// about the namespaces it never reached, including the healthy ones. So one namespace's failure
            /// becomes that namespace's verdict and the sweep goes on.
            ///
            /// `TIMEOUT_EXCEEDED` is deliberately NOT caught: the deadline is a property of the whole scan,
            /// and `runFsck`'s `partial` handling owns it.
            try
            {
                const auto [authority_it, inserted] = recovery_authorities.emplace(
                    ns.string(), FsckRecoveryAuthority{.life = life, .catalog_entry = walk_life.catalog_entry});
                chassert(inserted);

                /// The stream audit and the owner replay are one walk, so the two legs cannot see different
                /// histories after a concurrent `_ckpt` write.
                std::optional<RefTxnId> greatest_listed;
                if (const auto listed_it = greatest_listed_log.find(life.incarnation); listed_it != greatest_listed_log.end())
                    greatest_listed = listed_it->second;
                const std::optional<RefTableState> walked = walkRefStream(
                    op, layout, life, walk_life.catalog_entry, greatest_listed, deadline, findings.scratch, findings.verdicts);
                if (!walked)
                    continue;
                const RefTableState & table = *walked;
                for (const auto [ref_name, row] : table.getCommitted())
                {
                    const ManifestId id{ns, row.manifest_ref};
                    const String mkey = layout.manifestKey(id);
                    owned_manifest_keys.insert(mkey);
                    const String label = ns_str + "/" + ref_name;

                    const auto got = op.read(mkey, Retry::standard());
                    if (!got)
                    {
                        /// A committed ref naming a missing manifest body would be an INV-NO-DANGLE violation —
                        /// but the per-ref GET runs later than the namespace's ref recovery, so a stale captured
                        /// row plus a legitimate GC delete of a since-superseded manifest can masquerade as one,
                        /// and a bare GET can lag a present object. Revalidate exactly like the blob `Dangling`
                        /// recheck below: HEAD the exact object AND re-resolve under the original catalog row
                        /// plus a fresh checkpoint from its physical life. Count the dangle ONLY when the exact
                        /// object is HEAD-absent AND that life still names THIS exact manifest — otherwise it is
                        /// LIST/GET lag or a phantom stale-row, never a loss.
                        if (!op.head(mkey, Retry::standard())
                            && manifestStillReferenced(op, layout, ns, recovery_authorities, ref_name, mkey,
                                deadline, record_recovery_unchecked))
                        {
                            ++findings.scratch.dangling;
                            FsckObject o;
                            o.key = mkey;
                            o.kind = ObjectKind::Blob;   /// manifests have no ObjectKind; reuse Blob as the generic kind
                            o.size = 0;
                            o.cls = FsckClass::Dangling;
                            o.reachable_from = {label};
                            findings.scratch.objects.push_back(std::move(o));
                        }
                        /// A present object is GET lag. A row not named by the original-life authority is a
                        /// stale-walk artifact, not a dangle; its original owner cannot contribute blobs.
                        ++refs_walked;
                        continue;
                    }

                    PartManifest body = decodePartManifest(openObject(FormatId::PartManifest, got->bytes));
                    if (!refMatchesBody(id.ref, body) || !manifestNamespaceMatches(id.root_namespace, body))
                    {
                        ++findings.scratch.dangling;
                        FsckObject o;
                        o.key = mkey;
                        o.kind = ObjectKind::Blob;
                        o.size = got->bytes.size();
                        o.cls = FsckClass::Dangling;
                        o.reachable_from = {label};
                        findings.scratch.objects.push_back(std::move(o));
                        ++refs_walked;
                        continue;
                    }

                    for (const ManifestEntry & e : body.entries)
                    {
                        if (e.placement != EntryPlacement::Blob)
                            continue;
                        const String bkey = layout.blobKey(e.ref);
                        reachable_blobs.insert(bkey);
                        ++report.total_blob_refs;
                        report.referenced_logical_bytes += e.blob_size;
                        blob_labels[bkey].push_back(label);
                    }

                    ++refs_walked;
                    checkDeadline(deadline, "walking refs");
                    if (on_progress && refs_walked % 64 == 0)
                        on_progress("walking refs", reachable_blobs.size(), refs_walked);
                }
            }
            catch (const Exception & e)
            {
                if (e.code() == ErrorCodes::TIMEOUT_EXCEEDED)
                    throw;
                findings.verdicts.recordUnchecked(findings.scratch, ns,
                    layout.namespaceStreamPrefix(life),
                    "fsck could not examine this namespace: " + e.message());
            }
        }
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::TIMEOUT_EXCEEDED)
            report_held_as_unconfirmed();
        throw;
    }
    unchecked_sink = &verdicts;
    unchecked_report = &report;

    /// One catalog read confirms every life walked before it: the janitor deletes only lives that a
    /// catalog read after its listing does not name, and an erased row never returns.
    if (!walk_lives.empty())
    {
        const CasRefCatalog::Snapshot confirmation_cut = CasRefCatalog::read(op, layout);
        for (size_t life_index = 0; life_index < walk_lives.size(); ++life_index)
        {
            DeferredLifeFindings & findings = deferred[life_index];
            report.ref_records_walked += findings.scratch.ref_records_walked;
            if (!catalogStillNamesLife(confirmation_cut.catalog, walk_lives[life_index].life))
            {
                pending_lives.insert(walk_lives[life_index].life.incarnation);
                continue;
            }
            verdicts.absorb(findings.verdicts);
            report.dangling += findings.scratch.dangling;
            for (FsckObject & object : findings.scratch.objects)
                report.objects.push_back(std::move(object));
        }
    }
    report.namespace_janitor_pending_lives = pending_lives.size();

    /// A late recheck walks a life again, after the confirmation above. Its `Dangling` verdicts and
    /// unchecked notes wait for one more batched catalog read that still names the life.
    struct LateDangling
    {
        FsckObject object;
        std::vector<String> labels;
    };
    std::vector<LateDangling> late_danglings;
    struct LateUnchecked
    {
        RootNamespace ns;
        String key;
        String note;
    };
    std::vector<LateUnchecked> late_unchecked;
    const RecordRecoveryUnchecked record_late_unchecked =
        [&](const RootNamespace & ns, const String & key, const String & detail_text)
        {
            late_unchecked.push_back(LateUnchecked{ns, key, detail_text});
        };
    const auto confirm_late_findings = [&]
    {
        if (late_danglings.empty() && late_unchecked.empty())
            return;
        const CasRefCatalog::Snapshot late_cut = CasRefCatalog::read(op, layout);
        /// A label or namespace without a retained authority cannot be shown removed: keep its verdict.
        const auto still_named = [&](const String & ns_str)
        {
            const auto it = recovery_authorities.find(ns_str);
            return it == recovery_authorities.end() || catalogStillNamesLife(late_cut.catalog, it->second.life);
        };
        for (LateUnchecked & unchecked : late_unchecked)
            if (still_named(unchecked.ns.string()))
                verdicts.recordUnchecked(report, unchecked.ns, unchecked.key, std::move(unchecked.note));
        for (LateDangling & dangling : late_danglings)
        {
            const bool any_named = dangling.labels.empty() || std::any_of(dangling.labels.begin(), dangling.labels.end(),
                [&](const String & label)
                {
                    const size_t slash = label.rfind('/');
                    return slash == String::npos || still_named(label.substr(0, slash));
                });
            if (!any_named)
                continue;
            ++report.dangling;
            report.objects.push_back(std::move(dangling.object));
        }
        late_danglings.clear();
        late_unchecked.clear();
    };
    const auto report_late_as_unconfirmed = [&]
    {
        static constexpr std::string_view note = "late recheck not confirmed before the deadline";
        for (const LateUnchecked & unchecked : late_unchecked)
            verdicts.recordUnchecked(report, unchecked.ns, unchecked.key, String(note));
        for (const LateDangling & dangling : late_danglings)
        {
            const String & label = dangling.labels.empty() ? String{} : dangling.labels.front();
            const size_t slash = label.rfind('/');
            verdicts.recordUnchecked(report, RootNamespace{slash == String::npos ? label : label.substr(0, slash)},
                dangling.object.key, String(note));
        }
        late_danglings.clear();
        late_unchecked.clear();
    };
    const auto confirm_late_findings_or_report_on_deadline = [&](const std::function<void()> & recheck)
    {
        try
        {
            recheck();
        }
        catch (const Exception & e)
        {
            if (e.code() == ErrorCodes::TIMEOUT_EXCEEDED)
                report_late_as_unconfirmed();
            throw;
        }
        confirm_late_findings();
    };
    report.distinct_blobs = reachable_blobs.size();

    /// Scoped mode skips the GLOBAL physical classification below: it is meaningless under a
    /// filter (blobs owned by other namespaces would read as unreachable) and would cost a
    /// pool-wide LIST for what should be O(scoped refs).
    if (namespace_prefix.empty())
    {
    /// Physical listing: blobs + manifest bodies. A legacy `.meta` marker lives under the SAME
    /// `blobsPrefix()` as its body, so partition the raw LIST into bodies vs markers up front: a marker
    /// must never be classified as a content body (it would be misread as an unreferenced blob), and a
    /// `.meta` key that does not parse as a marker's is foreign and left alone.
    std::unordered_map<String, uint64_t> present_all;
    listAll(op, layout.blobsPrefix(), present_all, on_progress, deadline, "listing blobs");
    std::unordered_map<String, uint64_t> present_blobs;
    std::vector<std::pair<String, uint64_t>> legacy_markers;
    present_blobs.reserve(present_all.size());
    for (const auto & [key, sz] : present_all)
    {
        if (!Layout::hasLegacyMarkerSuffix(key))
            present_blobs.emplace(key, sz);
        else if (layout.parseLegacyMarkerKey(key))
            legacy_markers.emplace_back(key, sz);
    }
    std::sort(legacy_markers.begin(), legacy_markers.end());
    for (const auto & [_, sz] : present_blobs)
        report.physical_bytes += sz;

    /// Reachable blobs must be present (HEAD-confirm against LIST lag before declaring loss).
    confirm_late_findings_or_report_on_deadline([&]
    {
        for (const String & bkey : reachable_blobs)
        {
            auto it = present_blobs.find(bkey);
            bool exists = it != present_blobs.end();
            uint64_t size = exists ? it->second : 0;
            if (!exists)
            {
                if (const std::optional<Meta> h = op.head(bkey, Retry::standard()))
                {
                    exists = true;
                    size = h->size;
                    report.physical_bytes += h->size;
                }
            }

            const auto lit = blob_labels.find(bkey);
            const std::vector<String> labels = lit != blob_labels.end() ? lit->second : std::vector<String>{};
            if (!exists)
            {
                /// Before declaring a loss, re-resolve the referencing refs from the original audit
                /// authority. A later rebirth must not replace the old owner while this verdict is being
                /// decided.
                const bool still_referenced = blobStillReferenced(op, store, layout, recovery_authorities, bkey,
                    labels, deadline, record_late_unchecked);
                if (!still_referenced)
                    continue;   /// stale-walk artifact: neither reachable nor dangling — skip entirely
            }

            FsckObject o;
            o.key = bkey;
            o.kind = ObjectKind::Blob;
            o.size = size;
            o.cls = exists ? FsckClass::Reachable : FsckClass::Dangling;
            if (detail)
                o.reachable_from = labels;
            if (!exists)
            {
                late_danglings.push_back(LateDangling{std::move(o), labels});
                continue;
            }
            ++report.reachable;
            if (detail)
                report.objects.push_back(std::move(o));
        }
    });

    /// Present-but-unreferenced blobs: classify through the GC pipeline view instead of one
    /// suspicious "unreachable" lump (the deletion pipeline keeps a nonzero churning
    /// set here on ANY active pool, and beta testers read "unreachable" as a leak). The GC state is
    /// read for LABELING ONLY — reachability above never consults it.
    std::unordered_set<BlobRef, BlobRefHash> unref_hashes;
    for (const auto & [bkey, sz] : present_blobs)
        if (!reachable_blobs.contains(bkey))
        {
            if (const std::optional<BlobRef> ref = layout.parseBlobKey(bkey))
                unref_hashes.insert(*ref);
        }

    /// The NON-SENTINEL source edges the snapshot still holds on each unreferenced blob are collected in
    /// `detail` mode only: the stale-edge cross-check below needs the edge IDENTITIES so it can ask
    /// whether their source manifests still exist.
    /// Read whenever a body is present: the `pending` rows over referenced bodies are checked too.
    GcSnapshotView snapshot;
    if (!present_blobs.empty())
        snapshot = readGcSnapshot(op, layout, unref_hashes, detail, on_progress, deadline, report);
    const bool have_gc_state = snapshot.have_state;
    const auto & retired_by_hash = snapshot.retired_by_hash;
    const auto & in_run_hashes = snapshot.in_run_hashes;
    const auto & unref_edge_sources = snapshot.edge_sources;

    /// STALE-EDGE cross-check. A residual `+1` whose matching `-1` never folded pins its blob at
    /// in-degree 1 forever: every GC round recomputes the same nonzero in-degree and never nominates
    /// the blob, so the `AwaitingGc` "expected, no action needed" label is a lie — nothing will ever
    /// reclaim it. The edge names its source, so the check is to ask whether that source still exists:
    /// build the set of source ids that every manifest body PRESENT in the pool would contribute, and
    /// treat an edge outside that set as one whose source manifest is gone.
    ///
    /// COST: one LIST per namespace plus one GET per manifest body. It is therefore gated on `detail`,
    /// which the summary path (the soak poll calls it in a loop) never sets, and additionally on some
    /// unreferenced blob actually carrying a real edge, so a pool with nothing to cross-check pays nothing.
    ///
    /// `stale_edge_check_available` is the fail-closed switch: a manifest body we cannot decode would
    /// silently withhold its edges from the live set and turn every blob it owns into a false hard
    /// finding, so one undecodable body disables the whole cross-check for this scan rather than
    /// manufacture an error. The check may only ever SHRINK to silence, never invent a finding.
    std::unordered_set<UInt128, UInt128Hash> live_source_ids;
    bool stale_edge_check_available = detail && !unref_edge_sources.empty();
    if (stale_edge_check_available)
    {
        const NamespaceListing stale_edge_listing = store.listNamespaces(namespace_prefix);
        recordLifelessKeys(stale_edge_listing);
        for (const String & ns_str : stale_edge_listing.namespaces)
        {
            const RootNamespace ns{ns_str};
            std::unordered_map<String, uint64_t> manifest_bodies;
            listAll(op, layout.manifestNamespacePrefix(ns), manifest_bodies, on_progress, deadline,
                    "listing manifests for the stale-edge check");
            for (const auto & [mkey, _] : manifest_bodies)
            {
                checkDeadline(deadline, "reading manifests for the stale-edge check");
                const std::optional<ManifestId> id = layout.parseManifestKey(mkey);
                if (!id)
                    continue;   /// foreign/malformed key under `manifests/` — contributes no source edge
                const auto got = op.read(mkey, Retry::standard());
                if (!got)
                    continue;   /// gone between the LIST and the read — genuinely not a live source
                try
                {
                    const PartManifest body = decodePartManifest(openObject(FormatId::PartManifest, got->bytes));
                    for (const ManifestEntry & e : body.entries)
                        if (e.placement == EntryPlacement::Blob)
                            live_source_ids.insert(sourceEdgeId(*id, e.path));
                }
                catch (...)
                {
                    stale_edge_check_available = false;   /// incomplete live set — do not accuse anyone
                    break;
                }
            }
            if (!stale_edge_check_available)
                break;
        }
    }

    /// A `pending` row over a present body of its token whose intent is absent. Each is confirmed against a
    /// second read of `gc/state` below: a round may have committed since the first.
    struct PendingSuspect
    {
        String key;
        uint64_t size = 0;
        BlobRef ref;
        PersistedEtag token;
        bool referenced = false;   /// a ref names the body: kept out of the backlog counts
    };
    std::vector<PendingSuspect> suspects;

    for (const auto & [bkey, sz] : present_blobs)
    {
        if (reachable_blobs.contains(bkey))
            continue;
        ++report.unreachable;

        /// A foreign/malformed key (`parseBlobKey` -> `nullopt`) falls back to the default `BlobRef{}`,
        /// which cannot match a real `retired_by_hash`/`in_run_hashes` entry — it lands in the generic
        /// `Unaccounted` bucket below, exactly the "debris, not ours" classification `parseBlobKey`
        /// documents: foreign algorithm segments are debris, not pool objects.
        const BlobRef hash = layout.parseBlobKey(bkey).value_or(BlobRef{});

        FsckClass cls = FsckClass::Unaccounted;
        String note;
        const auto rit = retired_by_hash.find(hash);
        /// HEAD only for a hash the snapshot actually retired, so an unretired blob still costs no request.
        const std::optional<Meta> retired_head
            = rit != retired_by_hash.end() ? op.head(bkey, Retry::standard()) : std::nullopt;
        /// GC's own comparison for a token that names an incarnation; the text for one that names none, so such a
        /// row still lands in its own class.
        const bool row_names_present = retired_head
            && (rit->second.token.canonical() ? sameIncarnation(rit->second.token, retired_head->etag)
                                              : rit->second.token.matches(retired_head->etag));
        if (row_names_present)
        {
            /// The PRESENT incarnation is the condemned one — deletion is scheduled. A mismatch
            /// means the listed entry belongs to a displaced older incarnation and says
            /// nothing about this object; fall through to the snapshot check.
            const CondemnedRow & row = rit->second;
            if (row.standing)
            {
                cls = FsckClass::StandingPresent;
                note = retiredNote(row);
            }
            else if (!row.delete_pending)
            {
                cls = FsckClass::PendingGc;
                note = retiredNote(row);
            }
            else
            {
                PendingVerdict verdict = checkPendingRow(op, layout, hash, row, report);
                if (verdict.cls == FsckClass::PendingWithoutIntent)
                {
                    suspects.push_back(PendingSuspect{bkey, sz, hash, row.token});
                    continue;
                }
                cls = verdict.cls;
                note = std::move(verdict.note);
            }
        }
        else if (in_run_hashes.contains(hash))
        {
            /// `in_run_hashes` only says the GC snapshot still holds SOMETHING for this blob. Split on
            /// whether any of it is still actionable. One edge whose source manifest is PRESENT keeps
            /// the ordinary `AwaitingGc` verdict — that manifest's removal still folds its `-1`, and an
            /// unowned-but-present manifest is reclaimed by the orphan sweep, so the blob is genuinely
            /// mid-pipeline. When EVERY edge names a manifest that no longer exists, no `-1` is left to
            /// fold: the in-degree is pinned above zero for good and only a rebuild can clear it.
            uint64_t stale_edges = 0;
            bool all_edges_stale = false;
            if (const auto eit = unref_edge_sources.find(hash);
                stale_edge_check_available && eit != unref_edge_sources.end() && !eit->second.empty())
            {
                for (const UInt128 & source_id : eit->second)
                    if (!live_source_ids.contains(source_id))
                        ++stale_edges;
                all_edges_stale = stale_edges == eit->second.size();
            }

            if (all_edges_stale)
            {
                cls = FsckClass::StaleEdge;
                note = "all " + std::to_string(stale_edges) + " source edges name manifests that no longer "
                       "exist — unreclaimable by the incremental GC (needs `cas-gc-rebuild`); NOT expected, investigate";
            }
            else
            {
                cls = FsckClass::AwaitingGc;
                note = "edges still in the GC snapshot; the drop has not folded yet (expected)";
            }
        }
        else if (!have_gc_state)
        {
            cls = FsckClass::AwaitingGc;
            note = "GC has not run on this pool yet";
        }
        else
        {
            note = "not in the current GC view — transient for a fast create+drop between rounds; "
                   "PERSISTENT occurrences violate INV-2 (reachability-before-content), investigate";
        }

        switch (cls)
        {
            case FsckClass::PendingGc:   ++report.pending_gc;   break;
            case FsckClass::PendingIntentAbsent:  ++report.pending_intent_absent; break;
            case FsckClass::PendingTokenRejected: ++report.pending_token_rejected; break;
            case FsckClass::StandingPresent:      ++report.standing_present; break;
            case FsckClass::AwaitingGc:  ++report.awaiting_gc;  break;
            case FsckClass::StaleEdge:   ++report.stale_edge;   break;
            default:                     ++report.unaccounted;  break;
        }
        if (detail)
        {
            FsckObject o;
            o.key = bkey;
            o.kind = ObjectKind::Blob;
            o.size = sz;
            o.cls = cls;
            o.reachable_from = {std::move(note)};
            report.objects.push_back(std::move(o));
        }
    }

    /// The same check for a `pending` row whose body a ref names again: a reference committed after GC's cut does not
    /// stop the delete.
    for (const auto & [ref, row] : snapshot.pending_rows)
    {
        const String bkey = layout.blobKey(ref);
        if (!reachable_blobs.contains(bkey))
            continue;
        checkDeadline(deadline, "checking pending rows over referenced bodies");
        if (checkPendingRow(op, layout, ref, row, report).cls == FsckClass::PendingWithoutIntent)
        {
            const auto it = present_blobs.find(bkey);
            suspects.push_back(PendingSuspect{bkey, it != present_blobs.end() ? it->second : 0, ref, row.token, /*referenced*/ true});
        }
    }

    if (!suspects.empty())
    {
        std::unordered_set<BlobRef, BlobRefHash> suspect_refs;
        for (const PendingSuspect & suspect : suspects)
            suspect_refs.insert(suspect.ref);
        FsckReport reread_report;   /// the run checksums were judged by the first read
        const GcSnapshotView reread = readGcSnapshot(op, layout, suspect_refs, false, on_progress, deadline, reread_report);
        for (const PendingSuspect & suspect : suspects)
        {
            FsckObject o{.key = suspect.key, .kind = ObjectKind::Blob, .size = suspect.size, .cls = FsckClass::PendingWithoutIntent, .reachable_from = {}};
            /// Only a complete snapshot can show the row moved on; anything less leaves the suspect open.
            if (!reread.have_state || !reread.complete)
            {
                countIntentUnread(report, "inconclusive: the pending row of " + suspect.key + " has no deletion intent, and the second read of "
                    "`gc/state` was not a complete snapshot, so whether the row moved on is unknown");
                o.cls = FsckClass::IntentUnread;
                o.reachable_from = {"delete_pending without its deletion intent; inconclusive: the second read of `gc/state` was incomplete"};
                report.objects.push_back(std::move(o));
                continue;
            }
            const auto row = reread.retired_by_hash.find(suspect.ref);
            const bool still_pending = row != reread.retired_by_hash.end() && row->second.delete_pending
                && !row->second.standing && row->second.token.canonical() == suspect.token.canonical();
            if (still_pending)
            {
                ++report.pending_without_intent;
                o.reachable_from = {"delete_pending: the deletion intent is absent while the body is present; GC would delete this body "
                                    "without warning any writer: stop GC on this pool and report it"};
                report.objects.push_back(std::move(o));
            }
            else if (!suspect.referenced)
            {
                ++report.pending_intent_absent;
                o.cls = FsckClass::PendingIntentAbsent;
                o.reachable_from = {"delete_pending: the row moved on between the scan and the second read of `gc/state`; nothing is at risk"};
                if (detail)
                    report.objects.push_back(std::move(o));
            }
        }
    }

    classifyIntents(op, layout, verify_intents, detail, on_progress, deadline, report);

    report.legacy_marker = legacy_markers.size();
    if (detail)
        for (const auto & [key, sz] : legacy_markers)
            report.objects.push_back(FsckObject{.key = key, .kind = ObjectKind::Blob, .size = sz, .cls = FsckClass::LegacyMarker,
                .reachable_from = {"debris of the marker protocol; nothing reads it"}});
    if (fix_legacy_markers)
    {
        std::vector<String> keys;
        keys.reserve(legacy_markers.size());
        for (const auto & [key, _] : legacy_markers)
            keys.push_back(key);
        /// One at a time: the fix runs beside live servers, so it adds no load burst, and it stops at the first refusal.
        const LegacyMarkerRemoval removal = removeLegacyMarkers(store, keys, /*concurrency*/ 1, deadline,
            [&](uint64_t deleted)
            {
                report.legacy_markers_deleted = deleted;
                if (on_progress)
                    on_progress("removing legacy markers", deleted, 0);
            });
        report.legacy_markers_deleted = removal.removed;
        report.legacy_markers_changed = removal.changed;
    }
    }
    else
    {
        /// Scoped mode: dangling-only for the selected namespaces. Each blob named by a scoped ref
        /// is HEAD-verified (O(scoped refs), no pool-wide LIST); the unreachable/pending pipeline
        /// classification needs the whole pool and is intentionally skipped.
        confirm_late_findings_or_report_on_deadline([&]
        {
            for (const String & bkey : reachable_blobs)
            {
                checkDeadline(deadline, "head-checking scoped blobs");
                const std::optional<Meta> h = op.head(bkey, Retry::standard());
                const auto lit = blob_labels.find(bkey);
                const std::vector<String> labels = lit != blob_labels.end() ? lit->second : std::vector<String>{};
                const bool exists = h.has_value();
                if (!exists)
                {
                    /// Use the same HEAD-absent re-resolve as the global-mode loop above.
                    const bool still_referenced = blobStillReferenced(op, store, layout, recovery_authorities, bkey,
                        labels, deadline, record_late_unchecked);
                    if (!still_referenced)
                        continue;   /// stale-walk artifact — neither reachable nor dangling
                }
                FsckObject o;
                o.key = bkey;
                o.kind = ObjectKind::Blob;
                o.size = exists ? h->size : 0;
                o.cls = exists ? FsckClass::Reachable : FsckClass::Dangling;
                if (detail)
                    o.reachable_from = labels;
                if (!exists)
                {
                    late_danglings.push_back(LateDangling{std::move(o), labels});
                    continue;
                }
                ++report.reachable;
                report.physical_bytes += h->size;
                if (detail)
                    report.objects.push_back(std::move(o));
            }
        });
    }

    /// Pre-precommit manifest debris: a `cas/manifests/` body with no committed owner. An ELIGIBLE prefix's
    /// orphan is reclaimable debris => INFO (Unreachable); a non-eligible (in-flight) one is also info,
    /// never an error. The owner-visible missing-body case is the error above.
    const NamespaceListing manifest_debris_listing = store.listNamespaces(namespace_prefix);
    recordLifelessKeys(manifest_debris_listing);
    for (const String & ns_str : manifest_debris_listing.namespaces)
    {
        const RootNamespace ns{ns_str};
        const String manifests_prefix = layout.manifestNamespacePrefix(ns);
        std::unordered_map<String, uint64_t> manifest_bodies;
        listAll(op, manifests_prefix, manifest_bodies, on_progress, deadline, "listing manifests");
        for (const auto & [mkey, sz] : manifest_bodies)
        {
            if (owned_manifest_keys.contains(mkey))
                continue;   /// owned by a committed ref — accounted above
            ++report.unreachable;
            if (detail)
            {
                BuildPrefix prefix;
                const bool parsed = parseBuildPrefix(layout, mkey, prefix);
                FsckObject o;
                o.key = mkey;
                o.kind = ObjectKind::Blob;
                o.size = sz;
                o.cls = FsckClass::Unreachable;
                if (parsed && buildPrefixEligibleUnderFloor(store, ns, prefix))
                    o.reachable_from = {"reclaimable-pre-precommit"};
                else
                    o.reachable_from = {"in-flight-pre-precommit"};
                report.objects.push_back(std::move(o));
            }
        }
    }

}

}

FsckReport runFsck(Pool & store, bool detail, FsckProgress on_progress,
                   std::optional<std::chrono::steady_clock::time_point> deadline,
                   bool partial_on_deadline, const String & namespace_prefix, bool fix_legacy_markers, bool verify_intents)
{
    FsckReport report;
    try
    {
        runFsckImpl(store, detail, on_progress, deadline, namespace_prefix, fix_legacy_markers, verify_intents, report);
    }
    catch (const Exception & e)
    {
        if (!partial_on_deadline || e.code() != ErrorCodes::TIMEOUT_EXCEEDED)
            throw;
        report.partial = true;
        report.partial_reason = e.message();
    }
    return report;
}

LegacyMarkerRemoval removeLegacyMarkers(Pool & pool, const std::vector<String> & keys, size_t concurrency,
                                        const std::optional<std::chrono::steady_clock::time_point> & deadline,
                                        const std::function<void(uint64_t)> & on_progress)
{
    std::vector<const String *> markers;
    for (const String & key : keys)
        if (pool.layout().parseLegacyMarkerKey(key))
            markers.push_back(&key);

    std::atomic<uint64_t> removed{0};
    std::atomic<uint64_t> changed{0};
    const uint64_t generation = pool.openRequests().admit().generation();
    try
    {
        for (size_t begin = 0; begin < markers.size(); begin += kProgressKeys)
        {
            checkDeadline(deadline, "removing legacy markers");
            runBounded(concurrency, std::min<size_t>(kProgressKeys, markers.size() - begin), [&](size_t i)
            {
                const String & key = *markers[begin + i];
                CasOperation job = pool.openRequests().resume(generation);
                const std::optional<Meta> seen = job.head(key, Retry::standard());
                if (!seen)
                    return;
                const Removal removal = job.remove(key, seen->etag, Retry::standard());
                removed += removal == Removal::Removed;
                changed += removal == Removal::Mismatch;
            });
            if (on_progress)
                on_progress(removed.load());
        }
    }
    catch (Exception & e)
    {
        e.addMessage(fmt::format("{} legacy marker(s) deleted before the stop; run again to continue", removed.load()));
        throw;
    }
    return {removed.load(), changed.load()};
}

String formatFsckSummary(const FsckReport & report)
{
    /// Field order is load-bearing for humans only; every consumer parses `key=value` tokens. `partial`
    /// and its free-text reason go LAST because the reason can contain spaces and quotes, so a parser
    /// splitting on whitespace has to trim from the tail (see the harness's `parse_fsck_summary`).
    /// `std::ostringstream`, not a ClickHouse write buffer: this reproduces the exact `std::cout`
    /// formatting the line has always had, `dedup_ratio`'s default double precision included, so
    /// extracting the line from the command changes nothing a parser can observe.
    std::ostringstream out;   // STYLE_CHECK_ALLOW_STD_STRING_STREAM
    out << "reachable=" << report.reachable
        << " dangling=" << report.dangling
        << " unreachable=" << report.unreachable
        << " pending_gc=" << report.pending_gc
        << " awaiting_gc=" << report.awaiting_gc
        << " unaccounted=" << report.unaccounted
        << " stale_edge=" << report.stale_edge
        << " corrupted_runs=" << report.corrupted_runs
        << " chain_broken=" << report.chain_broken
        << " lifeless_keys=" << report.lifeless_keys
        << " intent_listed=" << report.intent_listed
        << " intent_current=" << report.intent_current
        << " intent_replaced=" << report.intent_replaced
        << " intent_body_gone=" << report.intent_body_gone
        << " intent_malformed=" << report.intent_malformed
        << " intent_unread=" << report.intent_unread
        << " pending_without_intent=" << report.pending_without_intent
        << " pending_intent_absent=" << report.pending_intent_absent
        << " pending_token_rejected=" << report.pending_token_rejected
        << " standing_present=" << report.standing_present
        << " intents_verified=" << (report.intents_verified ? 1 : 0)
        << " legacy_marker=" << report.legacy_marker
        << " legacy_markers_deleted=" << report.legacy_markers_deleted
        << " legacy_markers_changed=" << report.legacy_markers_changed
        << " janitor_pending=" << report.namespace_janitor_pending
        << " janitor_pending_bytes=" << report.namespace_janitor_pending_bytes
        << " janitor_pending_lives=" << report.namespace_janitor_pending_lives
        << " unchecked=" << report.unchecked
        << " ref_records_walked=" << report.ref_records_walked
        << " physical_bytes=" << report.physical_bytes
        << " referenced_logical_bytes=" << report.referenced_logical_bytes
        << " distinct_blobs=" << report.distinct_blobs
        << " total_blob_refs=" << report.total_blob_refs
        << " dedup_ratio=" << report.dedupRatio();
    if (report.partial)
        out << " partial=1 reason='" << report.partial_reason << "'";
    return out.str();
}

}
