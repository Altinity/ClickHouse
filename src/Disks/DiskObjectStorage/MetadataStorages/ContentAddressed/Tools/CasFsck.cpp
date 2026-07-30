#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>

#include <Common/Exception.h>
#include <Common/HashTable/Hash.h>
#include <Common/scope_guard_safe.h>

#include <algorithm>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace DB
{
namespace ErrorCodes
{
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

void listAll(Backend & backend, const String & prefix, std::unordered_map<String, uint64_t> & out,
             const FsckProgress & on_progress, const Deadline & deadline, std::string_view phase)
{
    static constexpr size_t kPageLimit = 1000;
    uint64_t pages = 0;
    size_t count_in_page = 0;
    forEachListedKey(backend, prefix, [&](const ListedKey & k)
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
    }, kPageLimit);
    /// The walk's `backend.list` lands at least once even for an empty/undersized final page --
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
/// Before counting a HEAD-absent blob as `Dangling`, re-resolve every `"ns/ref"` label that named it
/// freshly (`resolveRef` with `allow_stale=false`, never the walk's cached/stale view) and check whether
/// the CURRENT manifest still lists this blob as a `Blob` entry. `label` is split on the LAST '/' —
/// mirroring exactly how the walk built it (`ns_str + "/" + ref_name`): `ref_name` never contains '/',
/// but `ns_str` may, so the join separator is always the rightmost one.
///
/// Fails CLOSED on any ambiguity (a malformed label, a dropped-then-recreated ref that throws, a
/// corrupt current manifest): treated as "still referenced", i.e. the original conservative verdict.
/// The fix can only SHRINK false positives — it must never hide a real one.
bool blobStillReferenced(Pool & store, const Layout & layout,
                          const String & bkey, const std::vector<String> & labels, const Deadline & deadline)
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
            /// Read freshly via `recoverRefTable` (a full LIST + replay), not `store.resolveRef`: the
            /// mounted Pool caches its `RefTableState` and never re-recovers it, so a concurrent external
            /// ref write is invisible to the cache. The recovery equation sees every log.
            const RootNamespace rns{ns_part};
            const RefTableState table = recoverRefTable(store.backend(), layout, rns);
            const auto rit = table.getCommitted().find(ref_name);
            if (rit == table.getCommitted().end())
                continue;   /// the ref was DROPPED since the walk — this label no longer applies
            const PartManifest body = store.readManifest(ManifestId{rns, rit->second.manifest_ref});
            for (const ManifestEntry & e : body.entries)
            {
                if (e.placement != EntryPlacement::Blob)
                    continue;
                if (layout.blobKey(e.ref) == bkey)
                    return true;   /// a CURRENT ref still names this exact blob — a real dangle
            }
        }
        catch (...)
        {
            return true;   /// cannot confirm the ref moved away — keep the conservative verdict
        }
    }
    return false;   /// every label re-resolved away (re-published or dropped) — a stale-walk artifact
}

/// The manifest sibling of the `blobStillReferenced` recheck above. The ref-walk captures each committed
/// `(ref_name -> manifest_ref)` from a FRESH per-namespace recovery, but the `backend.get(mkey)` that
/// confirms the manifest body runs LATER in the same (possibly long) namespace loop. A ref republished to
/// a DIFFERENT manifest — or DROPPED — in that window, combined with a legitimate GC delete of the OLD
/// manifest body, makes the stale captured row look like a committed ref over a missing manifest (a
/// "phantom dangling manifest"), the same dishonest-oracle failure `blobStillReferenced` kills for blobs.
///
/// Before counting a missing manifest body as `Dangling`, re-resolve the EXACT ref from a FRESH
/// authoritative recovery (`recoverRefTable`, never the walk's captured row) and check whether the CURRENT
/// committed row still names THIS exact manifest key. Only a current ref over the SAME still-missing
/// manifest is a real dangle. Fails CLOSED on any ambiguity (a throw, a corrupt table): treated as "still
/// referenced", the original conservative verdict — the fix can only SHRINK false positives, never hide a
/// real loss.
bool manifestStillReferenced(Backend & backend, const Layout & layout, const RootNamespace & ns,
                             const String & ref_name, const String & mkey, const Deadline & deadline)
{
    checkDeadline(deadline, "re-resolving ref at missing-manifest");
    try
    {
        const RefTableState table = recoverRefTable(backend, layout, ns);
        const auto rit = table.getCommitted().find(ref_name);
        if (rit == table.getCommitted().end())
            return false;   /// the ref was DROPPED since the walk — no longer a committed owner
        /// A republish moved the ref to a different manifest key: this old key is no longer owned.
        return layout.manifestKey(ManifestId{ns, rit->second.manifest_ref}) == mkey;
    }
    catch (...)
    {
        return true;   /// cannot confirm the ref moved away — keep the conservative verdict
    }
}

/// For the newest published snapshot `X` of `ns`, independently reconstruct
/// One namespace's ref-object ids from a SINGLE `LIST` of its prefix, ascending.
///
/// A HINT, and nothing more. The arithmetic walk reads every id it cares about by exact key, so an
/// omission costs one `GET` and changes no verdict; this supplies the walk's witness set and the
/// oracle's snapshot candidates. Both consumers share one listing rather than each taking their own,
/// which is also what keeps a namespace's audit at one enumeration.
struct NsRefListing
{
    std::vector<RefTxnId> logs;
    std::vector<RefTxnId> snapshots;
};

NsRefListing listNsRefObjects(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    /// Stage B (Task 4-C): see `CasRefCatalog::resolveLifeOrSentinel`'s doc for why the sentinel
    /// fallback is correct, not a guess, for a namespace the catalog does not name.
    const NamespaceLifeId life = CasRefCatalog::resolveLifeOrSentinel(backend, layout, ns);
    NsRefListing out;
    forEachListedKey(backend, layout.refsNamespacePrefix(life), [&](const ListedKey & lk)
    {
        const auto parsed = layout.parseRefObjectKey(lk.key);
        if (!parsed || parsed->id.ns != ns)
            return;
        if (parsed->kind == RefObjectKind::Snap)
            out.snapshots.push_back(parsed->txn_id);
        else if (parsed->kind == RefObjectKind::Log)
            out.logs.push_back(parsed->txn_id);
    });
    std::sort(out.logs.begin(), out.logs.end());
    std::sort(out.snapshots.begin(), out.snapshots.end());
    return out;
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
};

/// THE ARITHMETIC STREAM WALK (spec §7). Read-only, one namespace.
///
/// fsck may not rest a verdict on an enumeration any more than the fold may. Ids are dense `1..T`
/// within `(namespace, writer_epoch)` (INV-1), so the next id is COMPUTABLE and every position is read
/// by exact key from `_ckpt.checkpoint`'s successor upward. An absent expected-next is the decision
/// point, and there are exactly three answers:
///
///   * nothing above it            => the namespace's FRONTIER. Proven, silent, the healthy case.
///   * a CONFIRMED durable id in the SAME epoch above it => `chain-broken`. Under contiguity this
///     cannot be the end of anything: a durable record is missing and every transaction above it is
///     unreachable. The witness is confirmed by its own exact `GET` before it may convict — a listing
///     that can omit a key can equally well name one that is gone, and this verdict costs an exit code.
///   * only LATER-epoch ids above it => attempt the crossing, which is PROVED through the next epoch's
///     `prev_epoch_seal` back-chain (`crossEpochFromSeal`, shared with the fold) or else reported
///     `unchecked`.
///
/// It writes nothing and mints nothing. The writer's recovery closes a dead epoch by putting a seal in
/// the slot it walked to; an auditor has no business doing that, so a stream whose epoch was never
/// closed reads as unprovable rather than being repaired into provable.
void checkRefStream(Backend & backend, const Layout & layout, const RootNamespace & ns,
                    const NsRefListing & listing, const Deadline & deadline,
                    FsckReport & report, NsVerdicts & verdicts)
{
    checkDeadline(deadline, "ref stream");
    /// Stage B (Task 4-C): resolve `ns`'s real catalog life -- production no longer keys a mounted
    /// namespace's ref-layer objects at the Stage-A sentinel once it has been through a real birth, so
    /// this walk must ask the catalog what incarnation to read, exactly like `recoverRefTableDetailed`
    /// does (`resolveLifeOrSentinel`'s own doc covers the raw-fixture fallback).
    const NamespaceLifeId life = CasRefCatalog::resolveLifeOrSentinel(backend, layout, ns);
    const std::optional<CkptSample> sampled_ckpt = readCkpt(backend, layout, life);

    /// The base: the greater of what the listing offered and what the checkpoint NAMES. The checkpoint
    /// is what makes this robust against a cleaned prefix — INV-4 forbids deleting the snapshot it
    /// names, so that one is fetchable even when no listing mentions it.
    std::optional<RefTxnId> base_id;
    if (!listing.snapshots.empty())
        base_id = listing.snapshots.back();
    if (sampled_ckpt && sampled_ckpt->ckpt.checkpoint_snapshot_id
        && (!base_id || *base_id < *sampled_ckpt->ckpt.checkpoint_snapshot_id))
        base_id = sampled_ckpt->ckpt.checkpoint_snapshot_id;

    /// Where the tail starts, by the same grounding rule recovery uses (spec §4) minus its writes: the
    /// base's successor, else the namespace's birth epoch, else the listing's floor. A namespace with
    /// none of the three was never born, and the empty stream IS its proof — not an `unchecked`.
    std::optional<RefTxnId> expected;
    if (base_id)
        expected = RefTxnId{base_id->writer_epoch, base_id->ref_sequence + 1};
    else if (sampled_ckpt && sampled_ckpt->ckpt.life_epoch)
        expected = RefTxnId{*sampled_ckpt->ckpt.life_epoch, 1};
    else if (!listing.logs.empty())
        expected = RefTxnId{listing.logs.front().writer_epoch, 1};
    if (!expected)
        return;

    /// The GREATEST candidate in `epoch` strictly above `above_sequence` — the one most likely to still
    /// be durable, so a hole is caught with a single confirming read. `_ckpt.last_epoch_seal` joins the
    /// listing as a second, listing-INDEPENDENT witness: a checkpoint that records a seal at `{E, S}`
    /// had a durable object there, whatever any enumeration says.
    const auto greatestSameEpochAbove = [&](uint64_t epoch, uint64_t above_sequence) -> std::optional<RefTxnId>
    {
        std::optional<RefTxnId> best;
        const auto consider = [&](const RefTxnId & w)
        {
            if (w.writer_epoch == epoch && w.ref_sequence > above_sequence && (!best || *best < w))
                best = w;
        };
        for (const RefTxnId & id : listing.logs)
            consider(id);
        if (sampled_ckpt && sampled_ckpt->ckpt.last_epoch_seal)
            consider(*sampled_ckpt->ckpt.last_epoch_seal);
        return best;
    };

    /// The NEAREST candidate in a LATER epoch — the epoch a crossing would aim at. Nearest rather than
    /// greatest: the crossing proves its way back down the chain, so aiming at the closest epoch that
    /// something claims exists is both sufficient and cheapest.
    const auto nearestLaterEpochAbove = [&](const RefTxnId & id) -> std::optional<RefTxnId>
    {
        std::optional<RefTxnId> best;
        const auto consider = [&](const RefTxnId & w)
        {
            if (w.writer_epoch > id.writer_epoch && (!best || w < *best))
                best = w;
        };
        for (const RefTxnId & l : listing.logs)
            consider(l);
        if (sampled_ckpt && sampled_ckpt->ckpt.last_epoch_seal)
            consider(*sampled_ckpt->ckpt.last_epoch_seal);
        return best;
    };

    RefTxnId resolved_through{};
    std::optional<bool> last_applied_is_seal;

    for (;;)
    {
        checkDeadline(deadline, "ref stream");
        const auto got = backend.get(layout.refLogKey(life, *expected));
        if (got)
        {
            bool is_seal = false;
            try
            {
                is_seal = refLogTxnIsEpochSeal(
                    decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns.string(), *expected));
            }
            catch (const Exception & e)
            {
                verdicts.recordUnchecked(report, ns, layout.refLogKey(life, *expected),
                    "ref stream: the record at " + renderId(*expected) + " could not be decoded, so nothing "
                    "above it can be proved either: " + e.message());
                return;
            }
            ++report.ref_records_walked;
            resolved_through = *expected;
            last_applied_is_seal = is_seal;
            /// A seal gets NO special case: it advances the position like any other record, the walk
            /// then finds nothing at the position after it, and the crossing below is what proves where
            /// the stream continues. Jumping to `{epoch + 1, 1}` here would be a guess.
            expected = RefTxnId{expected->writer_epoch, expected->ref_sequence + 1};
            continue;
        }

        /// ---- Absent: hole, frontier, or an epoch boundary ----
        if (const auto witness = greatestSameEpochAbove(expected->writer_epoch, expected->ref_sequence))
        {
            if (backend.get(layout.refLogKey(life, *witness)))
            {
                verdicts.recordChainBroken(report, ns, layout.refLogKey(life, *expected),
                    "ref stream: id " + renderId(*expected) + " is absent while " + renderId(*witness)
                    + " in the same epoch is durable — the stream is dense by construction (INV-1), so this "
                      "is a HOLE, not the end of the stream, and every record above it is unreachable");
                return;
            }
            /// The listing named a key that is not there. That convicts nobody; fall through and let a
            /// later-epoch witness (if any) decide whether there is a crossing to attempt.
        }

        const auto later = nearestLaterEpochAbove(*expected);
        if (!later)
            return;   /// frontier: nothing above claims to exist, and the walk proved everything below it

        const EpochCrossResult crossing =
            crossEpochFromSeal(backend, layout, ns, resolved_through, last_applied_is_seal, *later);
        if (!crossing.proved())
        {
            verdicts.recordUnchecked(report, ns, layout.refLogKey(life, *expected),
                "ref stream: records of a later epoch are reachable but this epoch's closing seal was "
                "never consumed (or the position they chain from is not one), so the crossing at "
                + renderId(*expected) + " has no proof"
                + (crossing.detail.empty() ? "" : ": " + crossing.detail));
            return;
        }
        /// Every iteration must move strictly forward. A crossing that lands back on the position just
        /// read as absent means the epoch-start record answered the crossing's `GET` and then stopped
        /// answering ours — an above-cursor object vanishing under the walk, which nothing may
        /// legitimately do. Unprovable, and reported as such rather than spun on.
        if (!(*expected < crossing.start))
        {
            verdicts.recordUnchecked(report, ns, layout.refLogKey(life, *expected),
                "ref stream: the epoch crossing resolved back to " + renderId(crossing.start)
                + ", the position that just read absent — the epoch-start record is not stably readable");
            return;
        }
        expected = crossing.start;
    }
}

/// the table state AT `X` from an EARLIER base (the greatest snapshot below `X`, or the empty state) plus
/// the surviving logs in `(base, X]`, re-encode it deterministically, and byte-compare to the published
/// `X` object. Byte-determinism of the codec (canonical sort, verified in the codec's own gtests) makes
/// the comparison exact.
///
/// The check runs ONLY when every log needed for the independent reconstruction still survives. `X` reuses
/// its last covered log's id, so `X` itself is a log id; once `GC` folds and covers those logs it deletes
/// them, after which there is nothing independent left to replay from -- such a table is SKIPPED, never
/// failed. A selected object that vanishes mid-check (a concurrent `GC` cleanup published a covering newer
/// snapshot) is likewise a skip, exactly like recovery's restart-on-vanish -- never corruption. This
/// oracle therefore validates freshly-published snapshots and any table whose `GC` cleanup is still
/// behind; it uses only the shared `replay`/`snapshotOf` state machine, never a second copy.
void checkSnapshotOracle(Backend & backend, const Layout & layout, const RootNamespace & ns,
                         const NsRefListing & listing, bool detail, const Deadline & deadline, FsckReport & report)
{
    checkDeadline(deadline, "snapshot oracle");
    /// Stage B (Task 4-C): see `CasRefCatalog::resolveLifeOrSentinel`'s doc for why the sentinel
    /// fallback is correct, not a guess, for a namespace the catalog does not name.
    const NamespaceLifeId life = CasRefCatalog::resolveLifeOrSentinel(backend, layout, ns);

    const std::vector<RefTxnId> & snap_ids = listing.snapshots;
    const std::vector<RefTxnId> & log_ids = listing.logs;
    if (snap_ids.empty())
        return;   /// no published snapshot: recovering from logs alone is valid, nothing to oracle

    const RefTxnId snapshot_x = snap_ids.back();
    const auto got_x = backend.get(layout.refSnapshotKey(life, snapshot_x));
    if (!got_x)
        return;   /// vanished (a covering newer snapshot superseded it): restart-on-vanish -> skip

    /// X reuses its last covered log's id, so X is itself a log id. Once GC covers and cleans that log
    /// (the steady state on a caught-up pool), the state AT X cannot be independently reconstructed from
    /// logs -- skip, never fail. This also guards against replaying an empty tail into a {0,0} state.
    if (!std::binary_search(log_ids.begin(), log_ids.end(), snapshot_x))
        return;

    /// Independent base: the greatest snapshot strictly below X we can still fetch, else the empty state.
    std::optional<RefTableSnapshot> base;
    RefTxnId base_id{};   /// {0,0} = empty base
    for (auto it = snap_ids.rbegin(); it != snap_ids.rend(); ++it)
    {
        if (!(*it < snapshot_x))
            continue;   /// X itself or anything not strictly below it
        const auto got_base = backend.get(layout.refSnapshotKey(life, *it));
        if (!got_base)
            continue;   /// vanished; try the next older snapshot
        base = decodeRefTableSnapshot(openObject(FormatId::RefSnapshot, got_base->bytes), ns.string(), *it);
        base_id = *it;
        break;
    }

    /// Logs in (base_id, X]. X reuses its last log's id, so X itself is a log id here: if its log -- or any
    /// covered log above the base -- was already cleaned, the independent reconstruction is unavailable.
    /// Reconstruct the state AT X by streaming those logs through one builder -- GET -> decode -> applyOne
    /// -> discard, one at a time -- so a long log tail never materializes as a whole-tail vector (spec §5).
    /// The builder revalidates the base snapshot in full (`stateFromSnapshot`) and applies the tail through
    /// the SAME state machine the writer used; the last applied id is X, so `snapshotOf` below yields a
    /// snapshot with id X whose bytes must equal the published object.
    RefReplayBuilder builder(std::move(base));
    for (const RefTxnId & id : log_ids)
    {
        if (!(base_id < id))
            continue;   /// <= base: already folded into the base snapshot
        if (snapshot_x < id)
            continue;   /// > X: not part of the state AT X
        checkDeadline(deadline, "snapshot oracle");
        const auto got_log = backend.get(layout.refLogKey(life, id));
        if (!got_log)
            return;   /// a covered log was cleaned/vanished: oracle unavailable for X -> skip, not error
        RefLogTxn txn = decodeRefLogTxn(openObject(FormatId::RefLog, got_log->bytes), ns.string(), id);
        /// Account the decoded transaction's resident footprint to the memory probe for exactly this
        /// iteration (see `reportReplayMemoryDelta`): the oracle streams one transaction at a time.
        const int64_t footprint = static_cast<int64_t>(decodedRefLogTxnFootprint(txn));
        reportReplayMemoryDelta(footprint);
        SCOPE_EXIT({ reportReplayMemoryDelta(-footprint); });
        builder.applyOne(std::move(txn), got_log->bytes.size());
    }

    const RefTableState reconstructed = std::move(builder).finish().state;
    /// `recomputed` is canonical TEXT; the stored object is Always/`.zst`, so compare against the
    /// DECOMPRESSED stored bytes (zstd byte-determinism is not relied on here -- the canonical text is).
    const String recomputed = encodeRefTableSnapshot(snapshotOf(reconstructed, ns.string()));
    ++report.snapshot_oracle_checked;
    if (recomputed != openObject(FormatId::RefSnapshot, got_x->bytes))
    {
        ++report.snapshot_oracle_mismatches;
        FsckObject o;
        o.key = layout.refSnapshotKey(life, snapshot_x);
        o.kind = ObjectKind::Blob;   /// snapshots have no ObjectKind; reuse Blob as the generic kind
        o.size = got_x->bytes.size();
        o.cls = FsckClass::SnapshotOracleMismatch;
        if (detail)
            o.reachable_from = {"published snapshot bytes diverge from an independent replay of its logs "
                                "(writer cache or codec corruption)"};
        report.objects.push_back(std::move(o));
    }
}

/// Perform the scan and accumulate into `report`. This helper owns the read-only traversal: it first
/// recovers authoritative refs, then checks physical objects and GC labels, while preserving the
/// distinction between a missing live object and expected in-flight cleanup. Deadline exceptions are
/// intentionally left to `runFsck`, which decides whether partial results were requested.
void runFsckImpl(Pool & store, bool detail, const FsckProgress & on_progress, const Deadline & deadline,
                  const String & namespace_prefix, FsckReport & report)
{
    const Layout & layout = store.layout();
    Backend & backend = store.backend();
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

    const NamespaceListing ref_listing = store.listNamespaces(namespace_prefix);
    recordLifelessKeys(ref_listing);
    for (const String & ns_str : ref_listing.namespaces)
    {
        const RootNamespace ns{ns_str};
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
            /// One listing of the namespace's ref prefix, shared by the stream walk and the oracle.
            const NsRefListing listing = listNsRefObjects(backend, layout, ns);

            /// The arithmetic stream audit runs FIRST, so a holed stream gets the verdict that EXPLAINS
            /// it (`chain-broken`) rather than the downstream `CORRUPTED_DATA` the replay below would
            /// raise about the same hole.
            checkRefStream(backend, layout, ns, listing, deadline, report, verdicts);

            /// Fresh recovery (LIST + replay), not the mounted Pool's cached `listRefs`: fsck is a read-only
            /// audit that must see the authoritative durable ref state, including any external write.
            const RefTableState table = recoverRefTable(backend, layout, ns);
            /// Snapshot integrity oracle: verify this table's newest published
            /// snapshot is byte-identical to an independent replay of its own logs. Fails closed (records a
            /// mismatch, making the report not `clean()`) on a genuine divergence; skips silently when the
            /// covered logs were already cleaned or an object vanished mid-check.
            checkSnapshotOracle(backend, layout, ns, listing, detail, deadline, report);
            for (const auto [ref_name, row] : table.getCommitted())
            {
                const ManifestId id{ns, row.manifest_ref};
                const String mkey = layout.manifestKey(id);
                owned_manifest_keys.insert(mkey);
                const String label = ns_str + "/" + ref_name;

                const auto got = backend.get(mkey);
                if (!got)
                {
                    /// A committed ref naming a missing manifest body would be an INV-NO-DANGLE violation —
                    /// but the per-ref GET runs later than the namespace's ref recovery, so a stale captured
                    /// row plus a legitimate GC delete of a since-superseded manifest can masquerade as one,
                    /// and a bare GET can lag a present object. Revalidate exactly like the blob `Dangling`
                    /// recheck below: HEAD the exact object AND re-resolve the exact ref freshly. Count the
                    /// dangle ONLY when the exact object is HEAD-absent AND a CURRENT ref still names THIS
                    /// exact manifest — otherwise it is LIST/GET lag or a phantom stale-row, never a loss.
                    if (!backend.head(mkey).exists
                        && manifestStillReferenced(backend, layout, ns, ref_name, mkey, deadline))
                    {
                        ++report.dangling;
                        FsckObject o;
                        o.key = mkey;
                        o.kind = ObjectKind::Blob;   /// manifests have no ObjectKind; reuse Blob as the generic kind
                        o.size = 0;
                        o.cls = FsckClass::Dangling;
                        o.reachable_from = {label};
                        report.objects.push_back(std::move(o));
                    }
                    /// A present object (GET lag) or a ref that moved away (stale-walk artifact) is not a
                    /// dangle; the manifest's blobs are re-walked under the ref's CURRENT manifest elsewhere.
                    ++refs_walked;
                    continue;
                }

                PartManifest body = decodePartManifest(openObject(FormatId::PartManifest, got->bytes));
                if (!refMatchesBody(id.ref, body) || !manifestNamespaceMatches(id.root_namespace, body))
                {
                    ++report.dangling;
                    FsckObject o;
                    o.key = mkey;
                    o.kind = ObjectKind::Blob;
                    o.size = got->bytes.size();
                    o.cls = FsckClass::Dangling;
                    o.reachable_from = {label};
                    report.objects.push_back(std::move(o));
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
            verdicts.recordUnchecked(report, ns,
                layout.refsNamespacePrefix(CasRefCatalog::resolveLifeOrSentinel(backend, layout, ns)),
                "fsck could not examine this namespace: " + e.message());
        }
    }
    report.distinct_blobs = reachable_blobs.size();

    /// Scoped mode skips the GLOBAL physical classification below: it is meaningless under a
    /// filter (blobs owned by other namespaces would read as unreachable) and would cost a
    /// pool-wide LIST for what should be O(scoped refs).
    if (namespace_prefix.empty())
    {
    /// Physical listing: blobs + manifest bodies. The per-hash `.meta` descriptor sibling
    /// (`blobMetaKey(id) == blobKey(id) + ".meta"`) lives under the SAME
    /// `blobsPrefix()` as the body, so partition the raw LIST into bodies vs `.meta` objects up
    /// front — a `.meta` key must never be classified as a content body (it would otherwise be
    /// misread as an unreferenced blob and fall into the dangling/pending/unaccounted pipeline
    /// below), and a body must never be misread as a `.meta`.
    std::unordered_map<String, uint64_t> present_all;
    listAll(backend, layout.blobsPrefix(), present_all, on_progress, deadline, "listing blobs");
    std::unordered_map<String, uint64_t> present_blobs;
    std::unordered_set<BlobRef, BlobRefHash> present_meta_hashes;
    present_blobs.reserve(present_all.size());
    for (const auto & [key, sz] : present_all)
    {
        if (key.ends_with(".meta"))
        {
            if (const std::optional<BlobRef> ref = layout.parseBlobKey(key))
                present_meta_hashes.insert(*ref);
            /// else: foreign key shape under blobs/ — not ours to pair
        }
        else
            present_blobs.emplace(key, sz);
    }
    for (const auto & [_, sz] : present_blobs)
        report.physical_bytes += sz;

    /// Reachable blobs must be present (HEAD-confirm against LIST lag before declaring loss).
    for (const String & bkey : reachable_blobs)
    {
        auto it = present_blobs.find(bkey);
        bool exists = it != present_blobs.end();
        uint64_t size = exists ? it->second : 0;
        if (!exists)
        {
            const HeadResult h = backend.head(bkey);
            if (h.exists)
            {
                exists = true;
                size = h.size;
                report.physical_bytes += h.size;
            }
        }

        const auto lit = blob_labels.find(bkey);
        if (!exists)
        {
            /// Before declaring a loss, re-resolve the referencing refs freshly — a ref
            /// re-published or dropped between the walk and this HEAD-confirm, combined with a
            /// legitimate GC delete of the OLD blob, must NOT surface as a phantom dangle.
            const bool still_referenced = blobStillReferenced(store, layout, bkey,
                lit != blob_labels.end() ? lit->second : std::vector<String>{}, deadline);
            if (!still_referenced)
                continue;   /// stale-walk artifact: neither reachable nor dangling — skip entirely
        }

        if (exists)
            ++report.reachable;
        else
            ++report.dangling;
        if (detail || !exists)
        {
            FsckObject o;
            o.key = bkey;
            o.kind = ObjectKind::Blob;
            o.size = size;
            o.cls = exists ? FsckClass::Reachable : FsckClass::Dangling;
            if (detail && lit != blob_labels.end())
                o.reachable_from = lit->second;
            report.objects.push_back(std::move(o));
        }
    }

    /// Present-but-unreferenced blobs: classify through the GC pipeline view instead of one
    /// suspicious "unreachable" lump (the multi-stage graduation keeps a nonzero churning
    /// set here on ANY active pool, and beta testers read "unreachable" as a leak). The GC state is
    /// read for LABELING ONLY — reachability above never consults it.
    std::unordered_map<BlobRef, RetiredEntry, BlobRefHash> retired_by_hash;
    std::unordered_set<BlobRef, BlobRefHash> unref_hashes;
    std::unordered_set<BlobRef, BlobRefHash> in_run_hashes;
    /// The NON-SENTINEL source edges the snapshot still holds on each unreferenced blob, collected in
    /// `detail` mode only. `in_run_hashes` alone answers "does GC still see this blob at all"; the
    /// stale-edge cross-check below needs the edge IDENTITIES so it can ask whether their source
    /// manifests still exist. Sentinel rows (`source_id == 0` — `kZeroMarker`/`kCondemned`) are not
    /// edges and are excluded.
    std::unordered_map<BlobRef, std::vector<UInt128>, BlobRefHash> unref_edge_sources;
    bool have_gc_state = false;

    for (const auto & [bkey, sz] : present_blobs)
        if (!reachable_blobs.contains(bkey))
        {
            if (const std::optional<BlobRef> ref = layout.parseBlobKey(bkey))
                unref_hashes.insert(*ref);
        }

    if (!unref_hashes.empty())
    {
        if (const auto state_got = backend.get(layout.gcStateKey()))
        {
            have_gc_state = true;
            const GcState gc_state = decodeGcState(state_got->bytes);
            /// The adopted fold seal names the snapshot runs; resolution is by ref, never by key
            /// construction. Every row whose hash is in our candidate set marks "known to GC" —
            /// edges still counted (drop unfolded), an explicit zero-marker mid-pipeline, or a
            /// `kCondemned` sentinel row that carries the condemned state (retired-in-snapshot):
            /// the `kCondemned` rows feed `retired_by_hash` (the `PendingGc` classification) in the
            /// SAME pass, replacing the removed `retired_refs`/`decodeRetiredSet` loop.
            ///
            /// These sets are keyed by the full `BlobRef`, not a narrowed digest. The run's own
            /// algorithm-prefixed key is parsed by `SourceEdgeKeyCodec` and compared directly with
            /// the full identity parsed from the listed blob key. This is required for mixed-algorithm
            /// pools: a 64-hex digest must not be truncated or compared as though it used the pool's
            /// local write algorithm, or its true GC state could be hidden as `Unaccounted`.
            if (const auto seal_got = backend.get(layout.foldSealKey(gc_state.snap_generation, gc_state.snap_attempt)))
            {
                uint64_t rows = 0;
                for (const RunRef & run : decodeFoldSeal(seal_got->bytes, gc_state.snap_generation).blob_target_runs)
                {
                    checkDeadline(deadline, "reading gc snapshot runs");
                    /// Typed open: the source-edge run reader goes through openSourceEdgeRun (the NDJSON
                    /// header gates type == cas_run + kind == source_edge). Fsck keys off the row's hash
                    /// (the record's own algo-prefixed key, never from pool meta).
                    SourceEdgeRunView reader = openSourceEdgeRun(backend, run.key);
                    String key;
                    String payload;
                    while (reader.next(key, payload))
                    {
                        BlobRef ref;
                        UInt128 source_id;
                        SourceEdgeKeyCodec::parse(key, ref, source_id);   // throws CORRUPTED_DATA on malformed (fail-closed)
                        if (unref_hashes.contains(ref))
                        {
                            in_run_hashes.insert(ref);
                            if (detail && source_id != UInt128{0})
                                unref_edge_sources[ref].push_back(source_id);
                            if (!payload.empty() && payload[0] == kCondemned)
                            {
                                const CondemnedRow row = decodeCondemnedRow(payload);
                                RetiredEntry e;
                                e.kind = ObjectKind::Blob;
                                e.ref = ref;
                                e.token = row.token;
                                e.size = row.size;
                                e.condemn_round = row.condemn_round;
                                e.delete_pending = row.delete_pending;
                                retired_by_hash.emplace(ref, std::move(e));
                            }
                        }
                        if (on_progress && ++rows % 65536 == 0)
                            on_progress("reading gc snapshot runs", in_run_hashes.size(), rows);
                    }
                    /// Whole-file seal checksum: compare the drained run's accumulated
                    /// checksum to the seal's `RunRef::checksum`. Fsck is a read-only auditor — instead of
                    /// throwing (which would abort the whole scan on the first corrupt run), catalogue the
                    /// mismatch as a `CorruptedRun` finding (with the run key) and continue so the audit
                    /// enumerates every problem in one pass. The deletion-deriving consumers
                    /// (`fold`/`zeroInDegree`/`previewDeletes`) still fail closed on the same mismatch.
                    if (reader.accumulatedChecksum() != run.checksum)
                    {
                        ++report.corrupted_runs;
                        if (detail)
                            report.objects.push_back(FsckObject{.key = run.key, .cls = FsckClass::CorruptedRun, .reachable_from = {}});
                    }
                }
            }
        }
    }

    /// STALE-EDGE cross-check. A residual `+1` whose matching `-1` never folded pins its blob at
    /// in-degree 1 forever: every GC round recomputes the same nonzero in-degree and never nominates
    /// the blob, so the `AwaitingGc` "expected, no action needed" label is a lie — nothing will ever
    /// reclaim it. The edge names its source, so the check is to ask whether that source still exists:
    /// build the set of source ids that every manifest body PRESENT in the pool would contribute, and
    /// treat an edge outside that set as one whose source manifest is gone.
    ///
    /// COST: one LIST per namespace plus one GET per manifest body. It is therefore gated on `detail`
    /// — the cheap summary path (the ca-soak fixpoint poll calls it in a loop) must not gain a single
    /// extra request — and additionally on some unreferenced blob actually carrying a real edge, so a
    /// pool with nothing to cross-check pays nothing.
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
            listAll(backend, layout.manifestNamespacePrefix(ns), manifest_bodies, on_progress, deadline,
                    "listing manifests for the stale-edge check");
            for (const auto & [mkey, _] : manifest_bodies)
            {
                checkDeadline(deadline, "reading manifests for the stale-edge check");
                const std::optional<ManifestId> id = layout.parseManifestKey(mkey);
                if (!id)
                    continue;   /// foreign/malformed key under `manifests/` — contributes no source edge
                const auto got = backend.get(mkey);
                if (!got)
                    continue;   /// gone between the LIST and the GET — genuinely not a live source
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
        if (const auto rit = retired_by_hash.find(hash); rit != retired_by_hash.end()
            && backend.head(bkey).token == rit->second.token)
        {
            /// The PRESENT incarnation is the condemned one — deletion is scheduled. A token
            /// mismatch means the listed entry belongs to a displaced older incarnation and says
            /// nothing about this object; fall through to the snapshot check.
            cls = FsckClass::PendingGc;
            note = rit->second.delete_pending
                ? "delete_pending: exact-token delete executes next GC round"
                : "condemned at round " + std::to_string(rit->second.condemn_round)
                    + "; graduates once every writer acks past it (expected)";
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
                       "exist — unreclaimable by the incremental GC (needs `ca-gc-rebuild`); NOT expected, investigate";
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

    /// Meta <-> body pairing: a `.meta` object with no
    /// body is an INV-META-BODY violation (the fixed meta/body lifecycle never leaves a meta
    /// orphaned of its body) — a real ERROR, distinct from `dangling` (which is reachability-driven).
    /// A body with no `.meta` is a benign not-yet-adopted (or interrupted-birth) artifact, NOT a dangle
    /// — it still classifies through the ordinary present-but-unreferenced pipeline above.
    std::unordered_set<BlobRef, BlobRefHash> present_body_hashes;
    present_body_hashes.reserve(present_blobs.size());
    for (const auto & [bkey, _] : present_blobs)
        if (const std::optional<BlobRef> ref = layout.parseBlobKey(bkey))
            present_body_hashes.insert(*ref);
        /// else: foreign key shape under blobs/ — not ours to pair
    for (const BlobRef & hash : present_meta_hashes)
        if (!present_body_hashes.contains(hash))
            ++report.meta_without_body;
    for (const BlobRef & hash : present_body_hashes)
        if (!present_meta_hashes.contains(hash))
            ++report.body_without_meta;
    }
    else
    {
        /// Scoped mode: dangling-only for the selected namespaces. Each blob named by a scoped ref
        /// is HEAD-verified (O(scoped refs), no pool-wide LIST); the unreachable/pending pipeline
        /// classification needs the whole pool and is intentionally skipped.
        for (const String & bkey : reachable_blobs)
        {
            checkDeadline(deadline, "head-checking scoped blobs");
            const HeadResult h = backend.head(bkey);
            const auto lit = blob_labels.find(bkey);
            bool exists = h.exists;
            if (!exists)
            {
                /// Use the same HEAD-absent re-resolve as the global-mode loop above.
                const bool still_referenced = blobStillReferenced(store, layout, bkey,
                    lit != blob_labels.end() ? lit->second : std::vector<String>{}, deadline);
                if (!still_referenced)
                    continue;   /// stale-walk artifact — neither reachable nor dangling
            }
            if (exists)
            {
                ++report.reachable;
                report.physical_bytes += h.size;
            }
            else
                ++report.dangling;
            if (detail || !exists)
            {
                FsckObject o;
                o.key = bkey;
                o.kind = ObjectKind::Blob;
                o.size = exists ? h.size : 0;
                o.cls = exists ? FsckClass::Reachable : FsckClass::Dangling;
                if (detail && lit != blob_labels.end())
                    o.reachable_from = lit->second;
                report.objects.push_back(std::move(o));
            }
        }
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
        listAll(backend, manifests_prefix, manifest_bodies, on_progress, deadline, "listing manifests");
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
                if (parsed && prefixEligible(store, ns, prefix))
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
                   bool partial_on_deadline, const String & namespace_prefix)
{
    FsckReport report;
    try
    {
        runFsckImpl(store, detail, on_progress, deadline, namespace_prefix, report);
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
        << " unchecked=" << report.unchecked
        << " ref_records_walked=" << report.ref_records_walked
        << " snapshot_oracle_mismatches=" << report.snapshot_oracle_mismatches
        << " snapshot_oracle_checked=" << report.snapshot_oracle_checked
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
