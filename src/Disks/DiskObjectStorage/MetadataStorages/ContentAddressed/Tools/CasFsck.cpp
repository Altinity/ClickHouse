#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBoundedRun.h>
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
#include <Common/Stopwatch.h>
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
    extern const int UNKNOWN_FORMAT_VERSION;
}
}

namespace DB::Cas
{

namespace
{
/// One request-bound phase of the scan as the progress sink sees it. Every sink call goes through `emit`, which
/// drops a sink exception, so the sink cannot change what the checker reports. After the first sink failure the
/// phase calls the sink only for its `End`. A phase left by an exception emits no `End`: the last `Start` or
/// `Tick` names where the scan stopped.
class FsckPhase
{
public:
    FsckPhase(const FsckProgress & sink_, std::string_view name_, std::string_view unit_, uint64_t interval_)
        : sink(sink_), name(name_), unit(unit_), interval(interval_)
    {
        emit(FsckProgressStep::Start);
    }

    void advance()
    {
        if (++done % interval == 0)
            emit(FsckProgressStep::Tick);
    }

    void tickAt(uint64_t n)
    {
        done = n;
        emit(FsckProgressStep::Tick);
    }

    void end() { emit(FsckProgressStep::End); }

    /// The name its progress lines carry; a deadline error names the phase by it.
    std::string_view phaseName() const { return name; }

private:
    void emit(FsckProgressStep step)
    {
        if (!sink || (sink_failed && step != FsckProgressStep::End))
            return;
        const bool started = step == FsckProgressStep::Start;
        try
        {
            sink(step, name, unit, started ? 0 : done, started ? 0.0 : watch.elapsedSeconds());
        }
        catch (Exception & e)
        {
            /// An exception destroyed unmarked is logged again, with a stack trace.
            if (sink_failed)
                e.markAsLogged();
            else
                tryLogCurrentException("CasFsck");
            sink_failed = true;
        }
        catch (...)
        {
            if (!sink_failed)
                tryLogCurrentException("CasFsck");
            sink_failed = true;
        }
    }

    const FsckProgress & sink;
    const std::string_view name;
    const std::string_view unit;
    const uint64_t interval;
    uint64_t done = 0;
    bool sink_failed = false;
    Stopwatch watch;
};

/// The phase the ref walk and its re-resolve helpers run in.
constexpr std::string_view kWalkingRefs = "walking refs";

/// Keys are counted per listed key; one tick per 16 list pages.
constexpr uint64_t kListTickKeys = 16 * CasOperation::kCasListPageKeys;
constexpr uint64_t kSnapshotTickRows = 65536;
constexpr uint64_t kBodyTickItems = 250;
constexpr uint64_t kRefTickItems = 64;
constexpr uint64_t kDefaultTickItems = 100;

/// Every key under `prefix`, each page requested under `ToolDeadline::bound`.
void forEachListedKeyBounded(CasOperation & op, const String & prefix, const ToolDeadline & deadline, std::string_view phase,
                             const ListedKeyFn & fn)
{
    String cursor;
    for (;;)
    {
        ListPage page = op.list(prefix, cursor, CasOperation::kCasListPageKeys, deadline.bound(op, phase));
        for (const ListedKey & entry : page.keys)
            if (!fn(entry))
                return;
        if (page.next_cursor.empty())
            return;
        cursor = std::move(page.next_cursor);
    }
}

void listAll(CasOperation & op, const String & prefix, std::unordered_map<String, uint64_t> & out,
             FsckPhase * progress, const ToolDeadline & deadline, std::string_view phase)
{
    static constexpr size_t kPageLimit = CasOperation::kCasListPageKeys;
    uint64_t pages = 0;
    size_t count_in_page = 0;
    forEachListedKeyBounded(op, prefix, deadline, phase, [&](const ListedKey & k)
    {
        out[k.key] = k.size;
        if (progress)
            progress->advance();
        if (++count_in_page == kPageLimit)
        {
            count_in_page = 0;
            ++pages;
            deadline.check(phase);
        }
        return true;
    });
    /// The walk's own list lands at least once even for an empty/undersized final page --
    /// check it here, mirroring the original per-page loop (deadline checked after every physical page).
    if (count_in_page > 0 || pages == 0)
    {
        ++pages;
        deadline.check(phase);
    }
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
                          const std::vector<String> & labels, const ToolDeadline & deadline, std::string_view phase,
                          const RecordRecoveryUnchecked & record_unchecked)
{
    if (labels.empty())
        return true;
    for (const String & label : labels)
    {
        deadline.check(phase);
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
                             const String & mkey, const ToolDeadline & deadline,
                             const RecordRecoveryUnchecked & record_unchecked)
{
    deadline.check(kWalkingRefs);
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
    RecordingKeyReader(CasOperation & op_, const ToolDeadline & deadline_) : op(op_), deadline(deadline_) {}
    void hint(const String &) override {}
    std::optional<Object> take(const String & key) override
    {
        last_key = key;
        std::optional<Object> got = op.read(key, deadline.bound(op, kWalkingRefs));
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
    const ToolDeadline deadline;
};

/// THE STREAM AUDIT, one namespace: one walk from the `_ckpt` base to the first absent key, shared with
/// the owner replay. It reads no key below the base. A walk whose base moved is restarted from a fresh
/// `_ckpt`, up to `kRefReadOnlyRecoveryMaxRestarts` times; past that the life is unchecked. A corrupt
/// base or chain under an unchanged `_ckpt` is chain-broken, any other failure unchecked. A hole is
/// reported only below a generation 1 or 2 frontier (the walk refuses it) or against a listed log above
/// the walk's end. Returns the replayed table, or `nullopt` after recording a verdict.
std::optional<RefTableState> walkRefStream(
    CasOperation & op, const Layout & layout, const NamespaceLifeId & life, const CatalogEntry & catalog_entry,
    const std::optional<RefTxnId> & greatest_listed_log, const ToolDeadline & deadline, FsckReport & report,
    NsVerdicts & verdicts)
{
    deadline.check(kWalkingRefs);
    const RootNamespace & ns = life.ns;
    std::optional<CkptSample> sample = readCkpt(op, layout, life);
    if (!sample)
    {
        /// Deferred like every finding of a walked life: a janitor that deleted this `_ckpt` after the catalog cut
        /// leaves a life the confirmation cut no longer names. No exception is built for it, because the destructor
        /// of a `CORRUPTED_DATA` logs an error whether or not anyone catches it.
        verdicts.recordUnchecked(report, ns, layout.namespaceStreamPrefix(life),
            "fsck could not examine this namespace: " + String(kRecoveryGroundingNoCkptMessage));
        return std::nullopt;
    }
    for (size_t restarts = 0;; ++restarts)
    {
        const RecoveryGrounding grounding = chooseRecoveryGrounding(catalog_entry, std::optional<RefCkpt>{sample->ckpt});
        RecordingKeyReader reader(op, deadline);
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
            deadline.rethrowIfOutOfTime(op, e, kWalkingRefs);
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
        deadline.check(kWalkingRefs);
        sample = readCkpt(op, layout, life);
        if (!sample)
        {
            verdicts.recordUnchecked(report, ns, key, "ref stream: checkpoint authority changed: `_ckpt` is absent");
            return std::nullopt;
        }
    }
}

String staleEdgeNote(uint64_t edges)
{
    return "all " + std::to_string(edges) + " source edges name manifests that no longer exist — unreclaimable by the "
           "incremental GC (needs `cas-gc-rebuild`); NOT expected, investigate";
}

/// Counts one check the scan could not make, of an intent or of the GC snapshot, and keeps the first reason, shortened.
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
/// `intents_listed` and, with `verify`, is also read and compared with its body: one `GET` and one `HEAD` per
/// intent, each landing in exactly one intent class. A listed key that vanishes before its read is not an
/// object any more and is not counted. Nothing here decides anything from an absent body: an intent
/// outlives its body until GC's later round.
void classifyIntents(CasOperation & op, const Layout & layout, bool verify, bool detail, const FsckProgress & on_progress,
                     const ToolDeadline & deadline, FsckReport & report)
{
    report.intents_verified = verify;
    /// A summary needs only counts, so it counts each key as the listing returns it.
    if (!verify && !detail)
    {
        FsckPhase counting(on_progress, "counting blob intents", "keys", kProgressKeys);
        forEachListedKeyBounded(op, layout.blobIntentsPrefix(), deadline, counting.phaseName(), [&](const ListedKey & k)
        {
            if (layout.isBlobIntentProbeKey(k.key))
                return true;
            counting.advance();
            ++(layout.parseBlobIntentKey(k.key) ? report.intents_listed : report.intent_malformed);
            report.physical_bytes += k.size;
            return true;
        });
        counting.end();
        return;
    }

    std::unordered_map<String, uint64_t> listed;
    FsckPhase listing(on_progress, "listing blob intents", "keys", kListTickKeys);
    listAll(op, layout.blobIntentsPrefix(), listed, &listing, deadline, "listing blob intents");
    listing.end();
    std::vector<std::pair<String, uint64_t>> intents(listed.begin(), listed.end());
    /// The detail rows are listed in key order.
    if (detail)
        std::sort(intents.begin(), intents.end());
    FsckPhase reading(on_progress, verify ? "verifying blob intents" : "counting blob intents", "keys", kProgressKeys);
    for (const auto & [key, size] : intents)
    {
        deadline.check(reading.phaseName());
        if (layout.isBlobIntentProbeKey(key))
            continue;   /// the capability probe's scratch object, not an intent
        reading.advance();
        const auto parsed = layout.parseBlobIntentKey(key);
        if (parsed && !verify)
        {
            ++report.intents_listed;
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
                const auto got = op.read(key, deadline.bound(op, reading.phaseName()));
                if (!got)
                    continue;
                counted = true;
                ++report.intents_listed;
                if (!isBlobIntentRecord(got->bytes))
                {
                    note = "the bytes are not the intent record";
                }
                else if (const std::optional<Meta> body = op.head(layout.blobKey(parsed->ref), deadline.bound(op, reading.phaseName()));
                         !body)
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
                deadline.rethrowIfOutOfTime(op, e, reading.phaseName());
                report.intents_listed += !counted;
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
    reading.end();
}

/// What the adopted GC snapshot holds about the `wanted` blobs: whether GC knows them at all, the rows it
/// keeps for them, and with `detail` the source edges still counted on them.
struct GcSnapshotView
{
    bool have_state = false;
    /// The snapshot generation `gc/state` adopts; a round never lowers it.
    uint64_t snap_generation = 0;
    /// `gc/state`, the seal it names and every run of it were read, and every run passed the checks a round applies.
    bool complete = false;
    uint64_t gc_shards = 1;
    /// No row was read, or none of these shards' rows were: the view cannot speak for their blobs.
    bool unread_all = false;
    std::set<uint64_t> unread_shards;
    std::unordered_set<BlobRef, BlobRefHash> in_run_hashes;
    std::unordered_map<BlobRef, CondemnedRow, BlobRefHash> retired_by_hash;
    std::unordered_map<BlobRef, std::vector<UInt128>, BlobRefHash> edge_sources;
    /// Every `pending` row, `wanted` or not: GC deletes its body when its own cut shows no reference, so a reference
    /// committed after that cut does not stop the delete.
    std::unordered_map<BlobRef, CondemnedRow, BlobRefHash> pending_rows;
    /// The seals and runs refused under a `gc/state` that had not moved since, each with its reason.
    std::vector<std::pair<String, String>> confirmed_refusals;

    bool speaksFor(const BlobRef & ref) const
    {
        return !unread_all && !unread_shards.contains(blobShard(ref, gc_shards));
    }
};

/// One read of `gc/state` and the snapshot it adopts. Refusals are held apart: they are findings only if `gc/state`
/// has not moved since.
struct GcSnapshotAttempt
{
    GcSnapshotView view;
    std::optional<Etag> state_token;
    /// The adopted `(generation, attempt)` when its fold seal is absent.
    std::optional<std::pair<uint64_t, uint64_t>> seal_absent;
    /// The refused seal or run and the reason.
    std::vector<std::pair<String, String>> refusals;
    /// Why every GC round refuses `gc/state` itself; then nothing else was read.
    std::optional<String> refused_state;
};

GcSnapshotAttempt readGcSnapshotOnce(CasOperation & op, const Layout & layout, const std::unordered_set<BlobRef, BlobRefHash> & wanted,
                                     bool detail, FsckPhase & progress, const ToolDeadline & deadline)
{
    GcSnapshotAttempt attempt;
    GcSnapshotView & view = attempt.view;
    const auto state_got = op.read(layout.gcStateKey(), deadline.bound(op, progress.phaseName()));
    if (!state_got)
        return attempt;
    attempt.state_token = state_got->etag;
    view.have_state = true;
    const GcState gc_state = decodeGcState(state_got->bytes);
    view.snap_generation = gc_state.snap_generation;
    try
    {
        requireGenerationFourGcState(gc_state);
    }
    catch (const Exception & e)
    {
        if (e.code() != ErrorCodes::UNKNOWN_FORMAT_VERSION)
            throw;
        attempt.refused_state = e.message();
        view.unread_all = true;
        return attempt;
    }
    view.gc_shards = gc_state.gc_shards;
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
        return attempt;
    }
    view.unread_all = true;
    const String seal_key = layout.foldSealKey(gc_state.snap_generation, gc_state.snap_attempt);
    const auto seal_got = op.read(seal_key, deadline.bound(op, progress.phaseName()));
    if (!seal_got)
    {
        attempt.seal_absent.emplace(gc_state.snap_generation, gc_state.snap_attempt);
        return attempt;
    }
    CasFoldSeal seal;
    try
    {
        seal = decodeFoldSeal(seal_got->bytes, layout, gc_state.gc_shards, gc_state.snap_generation);
    }
    catch (const Exception & e)
    {
        if (e.code() != ErrorCodes::CORRUPTED_DATA)
            throw;
        attempt.refusals.emplace_back(seal_key, e.message());
        return attempt;
    }
    view.unread_all = false;
    std::map<uint64_t, std::vector<RunRef>> runs_by_shard;
    for (const RunRef & run : seal.blob_target_runs)
        runs_by_shard[run.shard].push_back(run);
    /// Each shard is read the way the GC round reads it, so a run the round refuses is a finding here, and the scan
    /// goes on with the next shard to list every one in a pass.
    for (uint64_t shard = 0; shard < gc_state.gc_shards; ++shard)
    {
        const std::vector<RunRef> & runs = runs_by_shard[shard];
        AdoptedRunReader reader(op, runs, shard,
            AdoptedRunContext{.gc_shards = gc_state.gc_shards, .parent_round = gc_state.round, .summary = seal.condemned_summary.at(shard)},
            AdoptedRunReader::OnRefusal::Report, [&] { return deadline.bound(op, progress.phaseName()); });
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
            progress.advance();
        }
        if (const auto & refusal = reader.refusal())
        {
            view.unread_shards.insert(shard);
            /// A shard with no run has no run key; the shard itself identifies the finding.
            attempt.refusals.emplace_back(
                refusal->run_key.empty() ? fmt::format("{}#shard-{}", seal_key, shard) : refusal->run_key, refusal->message);
        }
    }
    view.complete = attempt.refusals.empty();
    return attempt;
}

void recordCorruptedRun(FsckReport & report, const String & key, const String & reason, bool detail)
{
    ++report.corrupted_runs;
    if (detail)
        report.objects.push_back(FsckObject{.key = key, .cls = FsckClass::CorruptedRun, .reachable_from = {reason}});
}

/// Reads `gc/state`, the adopted fold seal and every source-edge run of it, once each. A seal or run a GC round refuses
/// is counted into `report.corrupted_runs`, and listed there with `detail`, when `gc/state` has not moved since; under a
/// moved `gc/state` a round may have pruned it, and the newer snapshot is read instead.
GcSnapshotView readGcSnapshot(CasOperation & op, const Layout & layout, const std::unordered_set<BlobRef, BlobRefHash> & wanted,
                              bool detail, FsckPhase & progress, const ToolDeadline & deadline, FsckReport & report)
{
    std::optional<std::pair<uint64_t, uint64_t>> seal_absent_before;
    for (size_t attempt_no = 1;; ++attempt_no)
    {
        GcSnapshotAttempt attempt = readGcSnapshotOnce(op, layout, wanted, detail, progress, deadline);
        if (attempt.refused_state)
        {
            countIntentUnread(report, fmt::format("{}: every GC round refuses it, so the GC snapshot and its pending rows "
                "were not read", *attempt.refused_state));
            return std::move(attempt.view);
        }
        bool moved = false;
        if (attempt.seal_absent)
        {
            if (attempt.seal_absent == seal_absent_before)
            {
                countIntentUnread(report, fmt::format("the fold seal {} that gc/state adopts is absent, so the GC snapshot and "
                    "its pending rows were not read", layout.foldSealKey(attempt.seal_absent->first, attempt.seal_absent->second)));
                return std::move(attempt.view);
            }
            seal_absent_before = attempt.seal_absent;
            moved = true;
        }
        else if (attempt.refusals.empty())
            return std::move(attempt.view);
        else
        {
            const auto state_now = op.head(layout.gcStateKey(), deadline.bound(op, progress.phaseName()));
            moved = !state_now || !sameVersion(state_now->etag, *attempt.state_token);
        }
        if (!moved)
        {
            for (const auto & [key, reason] : attempt.refusals)
                recordCorruptedRun(report, key, reason, detail);
            attempt.view.confirmed_refusals = std::move(attempt.refusals);
            return std::move(attempt.view);
        }
        if (attempt_no == kAdoptedSnapshotReadAttempts)
        {
            countIntentUnread(report, fmt::format("gc/state moved during each of {} reads of the GC snapshot, so its rows "
                "were not read", attempt_no));
            attempt.view.complete = false;
            attempt.view.unread_all = true;
            return std::move(attempt.view);
        }
    }
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

/// `PendingWithoutIntent` is a suspect: a finding once a second snapshot read agrees.
struct PendingVerdict
{
    FsckClass cls = FsckClass::PendingGc;
    String note;
};

/// A `pending` row is the one state GC deletes a body from without re-reading the intent, so the intent is read
/// here. A read that fails is `intent_unread`: not an absent intent, and no verdict.
PendingVerdict checkPendingRow(CasOperation & op, const Layout & layout, const BlobRef & ref, const CondemnedRow & row,
                               const ToolDeadline & deadline, std::string_view phase, FsckReport & report)
{
    const std::optional<CanonicalIdentity> identity = row.token.canonical();
    if (!identity)
        return {FsckClass::PendingTokenRejected, "`pending` row: the row's token has no canonical identity, so no intent key exists for it"};
    try
    {
        if (op.read(layout.blobIntentKey(ref, *identity), deadline.bound(op, phase)))
            return {FsckClass::PendingGc, "`pending` row: the deletion intent is present; GC deletes the body in a later round (expected)"};
        const std::optional<Meta> body = op.head(layout.blobKey(ref), deadline.bound(op, phase));
        if (body && sameIncarnation(row.token, body->etag))
            return {FsckClass::PendingWithoutIntent, {}};
        return {FsckClass::PendingIntentAbsent,
                "`pending` row: the deletion intent is absent and the body is gone or replaced; the row is about to become "
                "`standing` or to move, nothing is at risk"};
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::TIMEOUT_EXCEEDED || e.code() == ErrorCodes::LOGICAL_ERROR)
            throw;
        deadline.rethrowIfOutOfTime(op, e, phase);
        countIntentUnread(report, e);
        return {FsckClass::PendingGc, "`pending` row: the deletion intent could not be read: " + e.message()};
    }
}

/// Perform the scan and accumulate into `report`. This helper owns the read-only traversal: it first
/// recovers authoritative refs, then checks physical objects and GC labels, while preserving the
/// distinction between a missing live object and expected in-flight cleanup. Deadline exceptions are
/// intentionally left to `runFsck`, which decides whether partial results were requested.
void runFsckImpl(Pool & store, bool detail, const FsckProgress & on_progress, const ToolDeadline & deadline,
                  const String & namespace_prefix, bool fix_legacy_markers, bool verify_intents, size_t fix_concurrency,
                  bool read_creator_slots, FsckReport & report)
{
    if (fix_legacy_markers && !namespace_prefix.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "fsck: a fix needs a scan of the whole pool, not of namespaces under '{}'", namespace_prefix);
    if (verify_intents && !namespace_prefix.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "fsck: verifying intents needs a scan of the whole pool, not of namespaces under '{}'", namespace_prefix);
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
    /// One mount-slot read per creator root and writer epoch, kept with its error: rows can share a creator,
    /// and each read of a slot that keeps failing costs a whole request window.
    struct CreatorSlotRead
    {
        std::optional<CreatorSlotStatus> slot;
        String error;
    };
    std::map<std::pair<String, uint64_t>, CreatorSlotRead> creator_slot_reads;
    report.catalog_has_no_walkable_row = !catalog_cut.catalog.entries.empty()
        && std::all_of(catalog_cut.catalog.entries.begin(), catalog_cut.catalog.entries.end(),
                       [](const CatalogEntry & entry) { return entry.state == NsState::Creating; });
    for (const CatalogEntry & entry : catalog_cut.catalog.entries)
    {
        if (!entry.ns.string().starts_with(namespace_prefix))
            continue;
        if (entry.state == NsState::Creating && !read_creator_slots)
            continue;
        if (entry.state == NsState::Creating)
        {
            const auto [slot_read, first] = creator_slot_reads.try_emplace(
                std::pair{entry.creator->server_root_id, entry.creator->writer_epoch});
            if (first)
            {
                try
                {
                    slot_read->second.slot = probeCreatorSlot(op, layout, entry.creator->server_root_id,
                        entry.creator->writer_epoch, deadline.bound(op, "reading creator mount slots"));
                }
                catch (const Exception & e)
                {
                    if (e.code() == ErrorCodes::TIMEOUT_EXCEEDED || e.code() == ErrorCodes::LOGICAL_ERROR)
                        throw;
                    deadline.rethrowIfOutOfTime(op, e, "reading creator mount slots");
                    slot_read->second.error = e.message();
                }
            }
            FsckCreatingRow row{.ns = entry.ns.string(), .creator_root = entry.creator->server_root_id,
                .writer_epoch = entry.creator->writer_epoch, .slot = slot_read->second.slot,
                .slot_read_error = slot_read->second.error};
            ++(row.slot == CreatorSlotStatus::Terminal ? report.stalled_creating : report.creating_unproven);
            report.creating_rows.push_back(std::move(row));
            continue;
        }
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

        FsckPhase namespaces_phase(on_progress, "listing namespaces", "keys", kListTickKeys);
        forEachListedKeyBounded(op, layout.namespaceRootPrefix(), deadline, "listing namespaces", [&](const ListedKey & listed)
        {
            namespaces_phase.advance();
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
        });
        namespaces_phase.end();

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
    FsckPhase refs_phase(on_progress, kWalkingRefs, "refs", kRefTickItems);
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

                    const auto got = op.read(mkey, deadline.bound(op, kWalkingRefs));
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
                        if (!op.head(mkey, deadline.bound(op, kWalkingRefs))
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
                        refs_phase.advance();
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
                        refs_phase.advance();
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

                    refs_phase.advance();
                    deadline.check(kWalkingRefs);
                }
            }
            catch (const Exception & e)
            {
                if (e.code() == ErrorCodes::TIMEOUT_EXCEEDED)
                    throw;
                deadline.rethrowIfOutOfTime(op, e, kWalkingRefs);
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
    refs_phase.end();
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
    FsckPhase blobs_phase(on_progress, "listing blobs", "keys", kListTickKeys);
    listAll(op, layout.blobsPrefix(), present_all, &blobs_phase, deadline, "listing blobs");
    blobs_phase.end();
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
        FsckPhase heads_phase(on_progress, "head-checking reachable blobs", "blobs", kDefaultTickItems);
        for (const String & bkey : reachable_blobs)
        {
            auto it = present_blobs.find(bkey);
            bool exists = it != present_blobs.end();
            uint64_t size = exists ? it->second : 0;
            if (!exists)
            {
                heads_phase.advance();
                if (const std::optional<Meta> h = op.head(bkey, deadline.bound(op, "head-checking reachable blobs")))
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
                    labels, deadline, heads_phase.phaseName(), record_late_unchecked);
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
        heads_phase.end();
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
    /// Read even with no body present: a `pending` row is classified whether its body is present or not.
    FsckPhase snapshot_phase(on_progress, "reading gc snapshot runs", "rows", kSnapshotTickRows);
    const GcSnapshotView snapshot = readGcSnapshot(op, layout, unref_hashes, detail, snapshot_phase, deadline, report);
    snapshot_phase.end();
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
            {
                FsckPhase listing(on_progress, "listing manifests for the stale-edge check", "keys", kListTickKeys);
                listAll(op, layout.manifestNamespacePrefix(ns), manifest_bodies, &listing, deadline,
                        "listing manifests for the stale-edge check");
                listing.end();
            }
            FsckPhase reading(on_progress, "reading manifests for the stale-edge check", "manifests", kDefaultTickItems);
            for (const auto & [mkey, _] : manifest_bodies)
            {
                deadline.check("reading manifests for the stale-edge check");
                const std::optional<ManifestId> id = layout.parseManifestKey(mkey);
                if (!id)
                    continue;   /// foreign/malformed key under `manifests/` — contributes no source edge
                reading.advance();
                const auto got = op.read(mkey, deadline.bound(op, "reading manifests for the stale-edge check"));
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
            reading.end();
            if (!stale_edge_check_available)
                break;
        }
    }

    /// A `pending` row over a present body of its token whose intent is absent. Each is confirmed against a
    /// second snapshot read below (`gc/state`, its seal and the runs): a round may have committed since the first.
    struct PendingSuspect
    {
        String key;
        uint64_t size = 0;
        BlobRef ref;
        PersistedEtag token;
        bool referenced = false;   /// a ref names the body: kept out of the backlog counts
    };
    std::vector<PendingSuspect> suspects;
    /// A stale edge is provisional until the second snapshot read below still holds one of its edges: a round that
    /// committed after the first read folds them and deletes the manifest bodies they name. It is counted and
    /// listed only once confirmed.
    struct StaleCandidate
    {
        FsckObject row;
        BlobRef ref;
        std::vector<UInt128> sources;
    };
    std::vector<StaleCandidate> stale_candidates;
    /// The `pending` rows the scan of present bodies classified.
    std::unordered_set<BlobRef, BlobRefHash> pending_rows_checked;

    /// Counts retired bodies, each of which costs a `HEAD`; a pending row's check sends more requests that are not counted.
    /// A body the snapshot has not retired costs no request and no tick.
    FsckPhase bodies_phase(on_progress, "head-checking retired bodies", "bodies", kBodyTickItems);
    for (const auto & [bkey, sz] : present_blobs)
    {
        if (reachable_blobs.contains(bkey))
            continue;
        ++report.unreachable;

        /// A key that is no blob key has no GC identity: it is classified before any snapshot lookup or request.
        const std::optional<BlobRef> parsed = layout.parseBlobKey(bkey);
        if (!parsed)
        {
            ++report.unaccounted;
            if (detail)
                report.objects.push_back(FsckObject{.key = bkey, .kind = ObjectKind::Blob, .size = sz, .cls = FsckClass::Unaccounted,
                    .reachable_from = {"Malformed blob key under `blobs/`. No GC identity."}});
            continue;
        }
        const BlobRef & hash = *parsed;

        FsckClass cls = FsckClass::Unaccounted;
        String note;
        if (have_gc_state && !snapshot.speaksFor(hash))
        {
            /// The refusal or the unread snapshot is the one finding; without rows nothing is known about this body.
            if (detail)
                report.objects.push_back(FsckObject{.key = bkey, .kind = ObjectKind::Blob, .size = sz, .cls = FsckClass::Unchecked,
                    .reachable_from = {fmt::format("not examined: the GC snapshot rows of shard {} were not read",
                                                   blobShard(hash, snapshot.gc_shards))}});
            continue;
        }
        const auto rit = retired_by_hash.find(hash);
        if (rit != retired_by_hash.end())
        {
            bodies_phase.advance();
            deadline.check(bodies_phase.phaseName());
        }
        /// HEAD only for a hash the snapshot actually retired, so an unretired blob still costs no request.
        const std::optional<Meta> retired_head
            = rit != retired_by_hash.end() ? op.head(bkey, deadline.bound(op, bodies_phase.phaseName())) : std::nullopt;
        /// GC's identity comparison. A row whose token names no incarnation is classed by the row: GC deletes no body for it.
        const bool row_names_present = retired_head
            && (!rit->second.token.canonical() || sameIncarnation(rit->second.token, retired_head->etag));
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
                pending_rows_checked.insert(hash);
                PendingVerdict verdict = checkPendingRow(op, layout, hash, row, deadline, bodies_phase.phaseName(), report);
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
                stale_candidates.push_back(StaleCandidate{
                    FsckObject{.key = bkey, .kind = ObjectKind::Blob, .size = sz, .cls = FsckClass::StaleEdge,
                               .reachable_from = {staleEdgeNote(stale_edges)}},
                    hash, unref_edge_sources.at(hash)});
                continue;
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

    bodies_phase.end();

    /// The same check for every other `pending` row: one whose body a ref names again, since a reference committed after
    /// GC's cut does not stop the delete, and one whose body was gone or replaced before the scan listed it.
    FsckPhase pending_phase(on_progress, "checking pending rows", "rows", kDefaultTickItems);
    for (const auto & [ref, row] : snapshot.pending_rows)
    {
        if (pending_rows_checked.contains(ref) || !snapshot.speaksFor(ref))
            continue;
        const String bkey = layout.blobKey(ref);
        const bool referenced = reachable_blobs.contains(bkey);
        pending_phase.advance();
        PendingVerdict verdict = checkPendingRow(op, layout, ref, row, deadline, pending_phase.phaseName(), report);
        const auto it = present_blobs.find(bkey);
        const uint64_t size = it != present_blobs.end() ? it->second : 0;
        if (verdict.cls == FsckClass::PendingWithoutIntent)
        {
            suspects.push_back(PendingSuspect{bkey, size, ref, row.token, referenced});
            continue;
        }
        /// A referenced body is not backlog: only a finding about it is reported.
        if (referenced)
            continue;
        if (verdict.cls == FsckClass::PendingIntentAbsent)
            ++report.pending_intent_absent;
        else if (verdict.cls == FsckClass::PendingTokenRejected)
            ++report.pending_token_rejected;
        else
            continue;
        if (detail)
            report.objects.push_back(FsckObject{.key = bkey, .kind = ObjectKind::Blob, .size = size, .cls = verdict.cls,
                .reachable_from = {std::move(verdict.note)}});
    }
    pending_phase.end();

    /// One second snapshot read (`gc/state`, its seal and the runs) confirms both the `pending` suspects and the
    /// stale-edge candidates.
    GcSnapshotView reread;
    if (!suspects.empty() || !stale_candidates.empty())
    {
        std::unordered_set<BlobRef, BlobRefHash> reread_refs;
        for (const PendingSuspect & suspect : suspects)
            reread_refs.insert(suspect.ref);
        for (const StaleCandidate & candidate : stale_candidates)
            reread_refs.insert(candidate.ref);
        FsckReport reread_report;   /// only its confirmed refusals are findings, merged below
        FsckPhase reread_phase(on_progress, "re-reading gc snapshot runs", "rows", kSnapshotTickRows);
        reread = readGcSnapshot(op, layout, reread_refs, /*detail=*/!stale_candidates.empty(), reread_phase, deadline, reread_report);
        reread_phase.end();
        std::unordered_set<String> refused_keys;
        for (const auto & [key, _] : snapshot.confirmed_refusals)
            refused_keys.insert(key);
        for (const auto & [key, reason] : reread.confirmed_refusals)
            if (refused_keys.insert(key).second)
                recordCorruptedRun(report, key, reason, detail);
    }
    /// A recreated `gc/state` holds no edge although no round folded one.
    const bool reread_regressed = reread.have_state && reread.snap_generation < snapshot.snap_generation;

    for (StaleCandidate & candidate : stale_candidates)
    {
        /// Only a snapshot that speaks for the candidate's shard can show its edges folded.
        if (!reread.have_state || !reread.speaksFor(candidate.ref) || reread_regressed)
        {
            const String why = reread_regressed
                ? fmt::format("went from snapshot generation {} back to {}", snapshot.snap_generation, reread.snap_generation)
                : String("did not cover this blob's shard");
            countIntentUnread(report, "inconclusive: the blob of " + candidate.row.key + " carries only stale edges, and the second read of "
                "the GC snapshot " + why + ", so whether a GC round folded them is unknown");
            candidate.row.cls = FsckClass::IntentUnread;
            candidate.row.reachable_from = {"every source edge names an absent manifest; inconclusive: the second read of the GC snapshot "
                                            + why};
            report.objects.push_back(std::move(candidate.row));
            continue;
        }
        /// A real stale edge is never folded, so any source still held in the re-read is one.
        uint64_t still_held = 0;
        if (const auto held = reread.edge_sources.find(candidate.ref); held != reread.edge_sources.end())
        {
            const std::unordered_set<UInt128, UInt128Hash> held_ids(held->second.begin(), held->second.end());
            for (const UInt128 & source : candidate.sources)
                still_held += held_ids.contains(source);
        }
        if (!still_held)
        {
            ++report.stale_edge_superseded;
            continue;
        }
        ++report.stale_edge;
        candidate.row.reachable_from = {staleEdgeNote(still_held)};
        report.objects.push_back(std::move(candidate.row));
    }

    if (!suspects.empty())
    {
        for (const PendingSuspect & suspect : suspects)
        {
            FsckObject o{.key = suspect.key, .kind = ObjectKind::Blob, .size = suspect.size, .cls = FsckClass::PendingWithoutIntent, .reachable_from = {}};
            /// Only a complete snapshot can show the row moved on; anything less leaves the suspect open.
            if (!reread.have_state || !reread.complete)
            {
                countIntentUnread(report, "inconclusive: the pending row of " + suspect.key + " has no deletion intent, and the second read of "
                    "`gc/state` was not a complete snapshot, so whether the row moved on is unknown");
                o.cls = FsckClass::IntentUnread;
                o.reachable_from = {"`pending` row without its deletion intent; inconclusive: the second read of `gc/state` was incomplete"};
                report.objects.push_back(std::move(o));
                continue;
            }
            const auto row = reread.retired_by_hash.find(suspect.ref);
            const bool still_pending = row != reread.retired_by_hash.end() && row->second.delete_pending
                && !row->second.standing && row->second.token.canonical() == suspect.token.canonical();
            if (still_pending)
            {
                ++report.pending_without_intent;
                o.reachable_from = {"`pending` row: the deletion intent is absent while the body is present; GC would delete this body "
                                    "without warning any writer: stop GC on this pool and report it"};
                report.objects.push_back(std::move(o));
            }
            else if (!suspect.referenced)
            {
                ++report.pending_intent_absent;
                o.cls = FsckClass::PendingIntentAbsent;
                o.reachable_from = {"`pending` row: the row moved on between the scan and the second read of `gc/state`; nothing is at risk"};
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
        const std::unique_ptr<ThreadPool> io_pool = makeBoundedPool(fix_concurrency);
        FsckPhase removal_phase(on_progress, "removing legacy markers", "markers", kProgressKeys);
        const LegacyMarkerRemoval removal = removeLegacyMarkers(store, keys, *io_pool, deadline,
            [&](uint64_t deleted)
            {
                report.legacy_markers_deleted = deleted;
                removal_phase.tickAt(deleted);
            });
        report.legacy_markers_deleted = removal.deleted;
        removal_phase.end();
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
            FsckPhase scoped_phase(on_progress, "head-checking scoped blobs", "blobs", kDefaultTickItems);
            for (const String & bkey : reachable_blobs)
            {
                scoped_phase.advance();
                const std::optional<Meta> h = op.head(bkey, deadline.bound(op, "head-checking scoped blobs"));
                const auto lit = blob_labels.find(bkey);
                const std::vector<String> labels = lit != blob_labels.end() ? lit->second : std::vector<String>{};
                const bool exists = h.has_value();
                if (!exists)
                {
                    /// Use the same HEAD-absent re-resolve as the global-mode loop above.
                    const bool still_referenced = blobStillReferenced(op, store, layout, recovery_authorities, bkey,
                        labels, deadline, scoped_phase.phaseName(), record_late_unchecked);
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
            scoped_phase.end();
        });
    }

    /// Pre-precommit manifest debris: a `cas/manifests/` body with no committed owner. An ELIGIBLE prefix's
    /// orphan is reclaimable debris => INFO (Unreachable); a non-eligible (in-flight) one is also info,
    /// never an error. The owner-visible missing-body case is the error above.
    const NamespaceListing manifest_debris_listing = store.listNamespaces(namespace_prefix);
    recordLifelessKeys(manifest_debris_listing);
    FsckPhase debris_phase(on_progress, "listing manifests", "keys", kListTickKeys);
    for (const String & ns_str : manifest_debris_listing.namespaces)
    {
        const RootNamespace ns{ns_str};
        const String manifests_prefix = layout.manifestNamespacePrefix(ns);
        std::unordered_map<String, uint64_t> manifest_bodies;
        listAll(op, manifests_prefix, manifest_bodies, &debris_phase, deadline, "listing manifests");
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
    debris_phase.end();
}

}

FsckReport runFsck(Pool & store, bool detail, FsckProgress on_progress,
                   const ToolDeadline & deadline,
                   bool partial_on_deadline, const String & namespace_prefix, bool fix_legacy_markers, bool verify_intents,
                   size_t fix_concurrency, bool read_creator_slots)
{
    FsckReport report;
    try
    {
        runFsckImpl(store, detail, on_progress, deadline, namespace_prefix, fix_legacy_markers, verify_intents, fix_concurrency,
                    read_creator_slots, report);
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

LegacyMarkerRemoval removeLegacyMarkers(Pool & pool, const std::vector<String> & keys, ThreadPool & io_pool,
                                        const ToolDeadline & deadline,
                                        const std::function<void(uint64_t)> & on_progress)
{
    std::vector<const String *> markers;
    for (const String & key : keys)
        if (pool.layout().parseLegacyMarkerKey(key))
            markers.push_back(&key);

    std::atomic<uint64_t> deleted{0};
    std::atomic<uint64_t> changed{0};
    const uint64_t generation = pool.openRequests().admit().generation();
    try
    {
        for (size_t begin = 0; begin < markers.size(); begin += kProgressKeys)
        {
            deadline.check("removing legacy markers");
            runBounded(io_pool, ThreadName::UNKNOWN, std::min<size_t>(kProgressKeys, markers.size() - begin), [&](size_t i)
            {
                const String & key = *markers[begin + i];
                CasOperation job = pool.openRequests().resume(generation);
                /// Per request, not per batch: one slow request must not let the rest of a batch run past the deadline.
                const std::optional<Meta> seen = job.head(key, deadline.bound(job, "removing legacy markers"));
                if (!seen)
                    return;
                const Removal removal = job.remove(key, seen->etag, deadline.bound(job, "removing legacy markers"));
                deleted += removal == Removal::Removed;
                changed += removal == Removal::Mismatch;
            });
            if (on_progress)
                on_progress(deleted.load());
        }
    }
    catch (Exception & e)
    {
        if (on_progress)
            on_progress(deleted.load());
        e.addMessage(fmt::format("{} legacy marker(s) deleted before the stop; run again to continue", deleted.load()));
        throw;
    }
    return {deleted.load(), changed.load()};
}

namespace
{

template <uint64_t FsckReport::* member>
uint64_t fsckCount(const FsckReport & report)
{
    return report.*member;
}

uint64_t fsckIntentsVerified(const FsckReport & report)
{
    return report.intents_verified ? 1 : 0;
}

/// Every count both surfaces render, in summary-line order; a field with an empty SQL column name is on the summary
/// line only. Every term of `FsckReport::clean` is a column: a SQL consumer sees no other view of the report. `stale_edge` is nonzero only in `detail` mode, which the SQL scan never
/// runs, and is kept as a column because "absent" and "zero" are different facts. A SQL scan classifies intents from
/// the listing alone, so the per-intent verdicts and the fix's counts are not columns.
constexpr std::array kFsckFields{
    FsckField{"reachable", "reachable", 0, fsckCount<&FsckReport::reachable>},
    FsckField{"dangling", "dangling", 1, fsckCount<&FsckReport::dangling>},
    FsckField{"unreachable", "unreachable", 2, fsckCount<&FsckReport::unreachable>},
    FsckField{"pending_gc", "pending_gc", 3, fsckCount<&FsckReport::pending_gc>},
    FsckField{"awaiting_gc", "awaiting_gc", 4, fsckCount<&FsckReport::awaiting_gc>},
    FsckField{"unaccounted", "unaccounted", 5, fsckCount<&FsckReport::unaccounted>},
    FsckField{"stale_edge", "stale_edge", 6, fsckCount<&FsckReport::stale_edge>},
    FsckField{"stale_edge_superseded", "", 0, fsckCount<&FsckReport::stale_edge_superseded>},
    FsckField{"corrupted_runs", "corrupted_runs", 7, fsckCount<&FsckReport::corrupted_runs>},
    FsckField{"chain_broken", "chain_broken", 8, fsckCount<&FsckReport::chain_broken>},
    FsckField{"lifeless_keys", "lifeless_keys", 10, fsckCount<&FsckReport::lifeless_keys>},
    FsckField{"intents_listed", "intents_listed", 19, fsckCount<&FsckReport::intents_listed>},
    FsckField{"intent_current", "", 0, fsckCount<&FsckReport::intent_current>},
    FsckField{"intent_replaced", "", 0, fsckCount<&FsckReport::intent_replaced>},
    FsckField{"intent_body_gone", "", 0, fsckCount<&FsckReport::intent_body_gone>},
    FsckField{"intent_malformed", "intent_malformed", 20, fsckCount<&FsckReport::intent_malformed>},
    FsckField{"intent_unread", "intent_unread", 21, fsckCount<&FsckReport::intent_unread>},
    FsckField{"pending_without_intent", "pending_without_intent", 22, fsckCount<&FsckReport::pending_without_intent>},
    FsckField{"pending_intent_absent", "pending_intent_absent", 23, fsckCount<&FsckReport::pending_intent_absent>},
    FsckField{"pending_token_rejected", "pending_token_rejected", 24, fsckCount<&FsckReport::pending_token_rejected>},
    FsckField{"standing_present", "standing_present", 25, fsckCount<&FsckReport::standing_present>},
    FsckField{"intents_verified", "", 0, fsckIntentsVerified},
    FsckField{"legacy_marker", "legacy_marker", 26, fsckCount<&FsckReport::legacy_marker>},
    FsckField{"legacy_markers_deleted", "", 0, fsckCount<&FsckReport::legacy_markers_deleted>},
    FsckField{"legacy_markers_changed", "", 0, fsckCount<&FsckReport::legacy_markers_changed>},
    FsckField{"janitor_pending", "namespace_janitor_pending", 11, fsckCount<&FsckReport::namespace_janitor_pending>},
    FsckField{"janitor_pending_bytes", "namespace_janitor_pending_bytes", 12, fsckCount<&FsckReport::namespace_janitor_pending_bytes>},
    FsckField{"janitor_pending_lives", "namespace_janitor_pending_lives", 13, fsckCount<&FsckReport::namespace_janitor_pending_lives>},
    FsckField{"stalled_creating", "", 0, fsckCount<&FsckReport::stalled_creating>},
    FsckField{"creating_unproven", "", 0, fsckCount<&FsckReport::creating_unproven>},
    FsckField{"unchecked", "unchecked", 9, fsckCount<&FsckReport::unchecked>},
    FsckField{"ref_records_walked", "ref_records_walked", 14, fsckCount<&FsckReport::ref_records_walked>},
    FsckField{"physical_bytes", "physical_bytes", 15, fsckCount<&FsckReport::physical_bytes>},
    FsckField{"referenced_logical_bytes", "referenced_logical_bytes", 16, fsckCount<&FsckReport::referenced_logical_bytes>},
    FsckField{"distinct_blobs", "distinct_blobs", 17, fsckCount<&FsckReport::distinct_blobs>},
    FsckField{"total_blob_refs", "total_blob_refs", 18, fsckCount<&FsckReport::total_blob_refs>},
};

/// The SQL positions are 0..n-1, each once.
constexpr bool sqlPositionsArePermutation()
{
    size_t columns = 0;
    for (const FsckField & field : kFsckFields)
        columns += !field.sql_column.empty();
    for (size_t position = 0; position < columns; ++position)
    {
        size_t found = 0;
        for (const FsckField & field : kFsckFields)
            found += !field.sql_column.empty() && field.sql_position == position;
        if (found != 1)
            return false;
    }
    return true;
}
static_assert(sqlPositionsArePermutation(), "every SQL column of `kFsckFields` needs its own position");

}

std::vector<FsckField> fsckSqlColumns()
{
    std::vector<FsckField> columns;
    for (const FsckField & field : kFsckFields)
        if (!field.sql_column.empty())
            columns.push_back(field);
    std::sort(columns.begin(), columns.end(), [](const FsckField & a, const FsckField & b) { return a.sql_position < b.sql_position; });
    return columns;
}

String formatFsckProgress(FsckProgressStep step, std::string_view phase, std::string_view unit, uint64_t done, double seconds)
{
    switch (step)
    {
        case FsckProgressStep::Start:
            return fmt::format("cas-fsck: {}: started", phase);
        case FsckProgressStep::Tick:
            return fmt::format("cas-fsck: {}: {} {}, {:.1f} s", phase, done, unit, seconds);
        case FsckProgressStep::End:
            return fmt::format("cas-fsck: {}: done, {} {}, {:.1f} s", phase, done, unit, seconds);
    }
    UNREACHABLE();
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
    for (const FsckField & field : kFsckFields)
        out << (&field == kFsckFields.data() ? "" : " ") << field.summary_key << "=" << field.value(report);
    out << " dedup_ratio=" << report.dedupRatio();
    if (report.partial)
        out << " partial=1 reason='" << report.partial_reason << "'";
    return out.str();
}

String formatFsckPipelineNote(const FsckReport & report)
{
    const uint64_t in_pipeline = report.pending_gc + report.awaiting_gc;
    if (in_pipeline == 0)
        return {};
    /// GC suppresses every delete while it has no namespace to walk and the catalog is not empty.
    const char * const verdict = report.catalog_has_no_walkable_row
        ? "GC deletes none of them while every catalog row is `Creating`; `stalled_creating` counts the rows GC cancels"
        : "expected, no action needed";
    return fmt::format(
        "note: {} unreferenced object(s) are inside the normal GC deletion pipeline "
        "(condemn and create the intent -> delete the body -> remove the intent takes ~3 rounds) — {}",
        in_pipeline, verdict);
}

String formatFsckCreatingRow(const FsckCreatingRow & row)
{
    if (!row.slot)
        return fmt::format("note: namespace '{}' is `Creating` under creator '{}' at writer epoch {}; its mount slot is "
                           "unreadable ({}): GC keeps the row while the slot cannot be read",
                           row.ns, row.creator_root, row.writer_epoch, row.slot_read_error);
    const char * verdict = "";
    switch (*row.slot)
    {
        case CreatorSlotStatus::Terminal:
            verdict = "terminal: the next GC round cancels the row";
            break;
        case CreatorSlotStatus::Live:
            verdict = "live: the creator may still finish; GC cancels the row once that server remounts or GC fences "
                      "its unrenewed mount slot";
            break;
        case CreatorSlotStatus::Absent:
            verdict = "absent on this read: GC does not cancel the row while the slot is absent";
            break;
        case CreatorSlotStatus::Undecodable:
            verdict = "undecodable: GC cannot cancel the row while the slot does not decode";
            break;
    }
    return fmt::format("note: namespace '{}' is `Creating` under creator '{}' at writer epoch {}; its mount slot is {}",
                       row.ns, row.creator_root, row.writer_epoch, verdict);
}

}
