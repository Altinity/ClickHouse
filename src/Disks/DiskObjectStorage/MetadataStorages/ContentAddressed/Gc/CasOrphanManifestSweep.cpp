#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcKeyReader.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/ThreadPool.h>
#include <Common/logger_useful.h>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <limits>

namespace ProfileEvents
{
    extern const Event CASGCEnumerationPages;
}

namespace DB::Cas
{

namespace
{

/// Hook used by this file's GC-owned enumeration calls to `forEachListedKey` and `recoverRefTable`.
/// It increments once per physical LIST page, never once per listed key.
void onGcEnumerationPage()
{
    ProfileEvents::increment(ProfileEvents::CASGCEnumerationPages);
}

/// The durable build floor is stored in the per-server mount lease together with the writer epoch. A
/// namespace is rooted by `server_root_id`, but that id is a clean relative path and can contain slashes.
/// Try namespace prefixes from longest to shortest and accept the first durable mount body. Without a
/// mount there is no deletion authority, so the caller must leave the prefix untouched. The mount's
/// `writer_epoch` and `min_active_build_sequence` are the single durable epoch/floor pair used for eligibility, including
/// across process replacement and the retired sentinel.
std::optional<MountLease> mountLeaseForNamespace(CasOperation & op, const Layout & layout, const RootNamespace & ns,
                                               uint64_t * reads = nullptr)
{
    const String & value = ns.string();
    size_t pos = value.size();
    while (true)
    {
        pos = value.rfind('/', pos == 0 ? 0 : pos - 1);
        if (pos == String::npos)
            break;

        const String server_root_id = value.substr(0, pos);
        if (!server_root_id.empty())
        {
            if (reads)
                ++*reads;
            if (const auto got = op.read(layout.mountKey(server_root_id), Retry::standard()))
                return decodeMountLease(got->bytes);
        }
        if (pos == 0)
            break;
    }
    return std::nullopt;
}

BuildFloor toBuildFloor(const MountLease & mount_lease)
{
    return BuildFloor{
        .writer_epoch = mount_lease.writer_epoch,
        .min_active_build_sequence = mount_lease.min_active_build_sequence
    };
}

struct ListedManifestObject
{
    RootNamespace ns;
    BuildPrefix build_prefix;
    ManifestRef ref;
    String key;
};

bool isBuildUnderFloor(
    CasOperation & op,
    const Layout & layout,
    const ListedManifestObject & manifest,
    std::map<String, std::optional<MountLease>> & mount_lease_by_ns,
    ManifestSweepResult & result)
{
    auto [lease_it, lease_inserted] = mount_lease_by_ns.emplace(manifest.ns.string(), std::nullopt);
    if (lease_inserted)
    {
        ++result.floor_lookups;
        lease_it->second = mountLeaseForNamespace(op, layout, manifest.ns, &result.floor_reads);
    }
    return buildPrefixEligibleUnderFloor(lease_it->second.transform(toBuildFloor), manifest.build_prefix);
}

/// Delegates to the shared `Layout::parseManifestKey`, which validates the canonical manifest-key
/// encoding. Keeping parsing in `Layout` avoids a second interpretation of the manifest path and ensures
/// the sweep and filesystem checker derive the same namespace and build identity.
std::optional<ListedManifestObject> parseListedManifestObject(const Layout & layout, const String & key)
{
    const auto parsed = layout.parseManifestKey(key);
    if (!parsed)
        return std::nullopt;

    return ListedManifestObject{
        .ns = parsed->root_namespace,
        .build_prefix = BuildPrefix{.writer_epoch = parsed->ref.writer_epoch, .build_sequence = parsed->ref.build_sequence},
        .ref = parsed->ref,
        .key = key};
}

/// The `(snap_generation, snap_attempt)` of the fold seal `gc/state` adopts; `nullopt` without `gc/state`.
using FoldSealRef = std::pair<uint64_t, uint64_t>;

std::optional<FoldSealRef> readAdoptedFoldSealRef(CasOperation & op, const Layout & layout)
{
    const auto state_got = op.read(layout.gcStateKey(), Retry::standard());
    if (!state_got)
        return std::nullopt;
    const GcState state = decodeGcState(state_got->bytes);
    return FoldSealRef{state.snap_generation, state.snap_attempt};
}

/// The seal at `ref`, or `nullopt` when there is none — a pool whose GC has never completed a round.
std::optional<CasFoldSeal> readFoldSealAt(CasOperation & op, const Layout & layout, const std::optional<FoldSealRef> & ref)
{
    if (!ref)
        return std::nullopt;
    const auto got = op.read(layout.foldSealKey(ref->first, ref->second), Retry::standard());
    if (!got)
        return std::nullopt;
    return decodeFoldSeal(got->bytes, ref->first);
}

/// The fold seal `gc/state` currently adopts and its reference. It is read once per sweep pass; every
/// namespace the pass touches takes its coverage row out of the same object. After a namespace's removal
/// walk only `gc/state` is read again: a seal key is write-once, so the seal is re-read only when
/// `gc/state` names another one.
struct AdoptedFoldSeal
{
    std::optional<FoldSealRef> ref;
    std::optional<CasFoldSeal> seal;
};

AdoptedFoldSeal readAdoptedFoldSeal(CasOperation & op, const Layout & layout)
{
    AdoptedFoldSeal adopted;
    adopted.ref = readAdoptedFoldSealRef(op, layout);
    adopted.seal = readFoldSealAt(op, layout, adopted.ref);
    return adopted;
}


/// One catalog-named life row out of an adopted seal. The catalog cut supplies the name-to-id join;
/// neither an absent name nor a `Creating` row may recover coverage from a historical life.
std::optional<RefCoverage> coverageOf(
    const std::optional<CasFoldSeal> & seal,
    const CasRefCatalog::Snapshot & catalog_cut,
    const RootNamespace & ns)
{
    if (!seal)
        return std::nullopt;

    const auto catalog_it = std::find_if(
        catalog_cut.catalog.entries.begin(), catalog_cut.catalog.entries.end(),
        [&](const CatalogEntry & entry) { return entry.ns == ns; });
    if (catalog_it == catalog_cut.catalog.entries.end() || catalog_it->state == NsState::Creating)
        return std::nullopt;

    const auto row_it = seal->ref_lives.find(catalog_it->incarnation);
    if (row_it == seal->ref_lives.end())
        return std::nullopt;
    return row_it->second.coverage;
}

/// Return this operation's ONE catalog row for `ns`. Callers hold the enclosing `Snapshot` for their
/// entire decision; a later name resolution would splice its life into an earlier coverage decision.
const CatalogEntry * catalogEntryOf(const CasRefCatalog::Snapshot & catalog_cut, const RootNamespace & ns)
{
    const auto it = std::find_if(
        catalog_cut.catalog.entries.begin(), catalog_cut.catalog.entries.end(),
        [&](const CatalogEntry & entry) { return entry.ns == ns; });
    return it == catalog_cut.catalog.entries.end() ? nullptr : &*it;
}

void excludeCandidatesWithChangedCatalogEntries(
    CasOperation & op,
    const Layout & layout,
    const CasRefCatalog::Snapshot & catalog_cut,
    std::vector<ListedManifestObject> & candidates,
    ManifestSweepResult & result)
{
    if (candidates.empty())
        return;

    const CasRefCatalog::Snapshot rechecked_catalog = CasRefCatalog::read(op, layout);
    rechecked_catalog.life_index.throwIfAmbiguous("CAS orphan manifest sweep post-floor catalog");
    std::erase_if(candidates, [&](const ListedManifestObject & candidate)
    {
        const CatalogEntry * before = catalogEntryOf(catalog_cut, candidate.ns);
        const CatalogEntry * after = catalogEntryOf(rechecked_catalog, candidate.ns);
        if ((!before && !after) || (before && after && *before == *after))
            return false;
        ++result.skipped;
        return true;
    });
}

/// The durable `last_folded_ref_id` of a coverage row, `{0, 0}` when there is none — as for a fresh
/// pool. A manifest removed by a log above this cursor has not had its `-1` decrement folded, so its
/// body remains load-bearing until the fold catches up.
RefTxnId sealedRefCursor(const std::optional<RefCoverage> & coverage)
{
    return coverage ? coverage->last_folded_ref_id : RefTxnId{};
}

/// One namespace's protection view: the manifest object keys the sweep must never delete, split so the
/// deletion premise can test the removal half on its own. `active` is the pre-existing union the delete sites
/// consult; `tail_removal_targets` is the subset the unconsumed tail above the cursor names as removal
/// targets, which is what rule (2) is stated over.
struct NamespaceProtection
{
    std::set<String> active;
    std::set<String> tail_removal_targets;
    /// Set when `work_budget`'s recovery-op cap stopped either walk, or was already spent by an earlier
    /// namespace this round. `active`/`tail_removal_targets` are then a PARTIAL view: the caller retains
    /// every one of this namespace's candidates on the current page.
    bool recovery_incomplete = false;
    /// Set when a walk could not prove where the tail ends: the cursor's successor is unknown, `_ckpt`
    /// kept moving, or the adopted cursor moved twice. The view is partial; the caller retains every
    /// candidate.
    bool unfinished = false;
    /// The coverage row the removal walk started from, re-sampled after it.
    std::optional<RefCoverage> coverage;
};

/// The active manifest-object-key set for one namespace, built as the same complete view writer
/// recovery uses: owners after replaying the chain from the `_ckpt` base, plus manifests removed by any
/// record above the durable `last_folded_ref_id` (their `-1` is not yet folded, so the GC fold still
/// needs the body). Keys (not ManifestIds) so a listed object key can be tested directly. Throws on a
/// corrupt base or chain; the caller SKIPS the namespace's deletions on such a throw rather than
/// substituting an empty owner set.
///
/// The removal walk resumes after the cursor by `resolveRefCursorSuccessor` and runs to the first absent
/// key. The cursor comes from a seal this sweep sampled itself, so it can lag the round that cleans:
/// after the walk the seal is sampled again through `gc/state`, and a moved cursor restarts the walk once.
///
/// `work_budget`, when set, is charged one unit per record the owner recovery replays and the removal walk
/// visits, shared with every other namespace this round touches. The owner recovery needs the charge: under
/// generation 3 it follows records appended while it runs, so a writer that stays ahead holds it. A hinting
/// reader may prefetch up to one window beyond the charged position; those prefetches are discarded, not
/// charged.
NamespaceProtection activeManifestKeys(
    CasOperation & op, KeyReader & reader, const Layout & layout, const CasRefCatalog::Snapshot & catalog_cut,
    const CatalogEntry & catalog_entry, const CkptSample & ckpt_sample, const std::optional<FoldSealRef> & seal_ref,
    const std::optional<RefCoverage> & coverage, GcRoundWorkBudget * work_budget = nullptr)
{
    NamespaceProtection protection;
    protection.coverage = coverage;
    if (work_budget && !work_budget->sweepRecoveryOpAvailable())
    {
        protection.recovery_incomplete = true;
        return protection;
    }
    const RootNamespace & ns = catalog_entry.ns;
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(catalog_entry.ns, catalog_entry.incarnation);

    /// Current owners = snapshot + replayed chain. The exact row comes from the caller's frozen catalog
    /// cut: a later cut can name a reborn life and turn this old life into an apparent orphan.
    std::optional<CkptSample> sample = ckpt_sample;
    std::function<bool()> may_read_next;
    if (work_budget)
        may_read_next = [work_budget]
        {
            ++work_budget->sweep_recovery_ops_used;
            return work_budget->sweepRecoveryOpAvailable();
        };
    std::optional<RecoveredRefTable> recovered;
    for (size_t restarts = 0;; ++restarts)
    {
        BoundedRefRecovery recovery
            = recoverRefTableDetailedFromAuthorityBounded(op, layout, catalog_entry, sample, &reader, may_read_next);
        if (recovery.end == BoundedRefRecovery::End::Recovered)
        {
            recovered = std::move(recovery.table);
            break;
        }
        if (recovery.end == BoundedRefRecovery::End::Stopped)
        {
            protection.recovery_incomplete = true;
            return protection;
        }
        if (restarts == kRefReadOnlyRecoveryMaxRestarts)
        {
            protection.unfinished = true;
            return protection;
        }
        sample = readCkpt(op, layout, life);
        if (!sample)
        {
            protection.unfinished = true;
            return protection;
        }
    }
    for (const auto [ref_name, row] : recovered->state.getCommitted())
        protection.active.insert(layout.manifestKey(ManifestId{ns, row.manifest_ref}));
    for (const auto & [ref_name, manifest_ref] : recovered->state.getPrecommits())
        protection.active.insert(layout.manifestKey(ManifestId{ns, manifest_ref}));

    const std::optional<RefTxnId> old_frontier
        = sample->ckpt.generation < G_REF_CHAIN ? sample->ckpt.committed_through : std::nullopt;

    /// Walks the removals above `cursor` into `removals`. False: the walk cannot vouch for the tail.
    const auto walk_removals = [&](const RefTxnId & cursor, std::set<String> & removals) -> bool
    {
        RefChainOrigin origin{
            .first_key = RefTxnId{*sample->ckpt.life_epoch, 1},
            .open_transition = std::nullopt,
            .ckpt_token = sample->etag,
            .confirm_base_after_end = true,
            .old_frontier = old_frontier};
        if (cursor != RefTxnId{})
        {
            const RefCursorResume resume = resolveRefCursorSuccessor(
                op, layout, life, cursor, sample->ckpt.checkpoint_snapshot_id);
            /// The cursor was read because it is not below the sampled snapshot id. A later snapshot can
            /// let cleanup empty that key and a late `RemoveNamespace` fill it, so the terminal end holds
            /// only while `_ckpt` has not moved.
            if (resume.kind == RefCursorResumeKind::Terminal)
            {
                if (!ckptTokenMoved(op, layout, life, sample->etag))
                    return true;
                protection.unfinished = true;
                return false;
            }
            if (resume.kind == RefCursorResumeKind::Hold)
            {
                LOG_DEBUG(getLogger("CasOrphanManifestSweep"),
                    "CAS orphan sweep: namespace {} has no provable successor of cursor {}: {}",
                    ns.string(), renderRefTxnId(cursor), resume.detail);
                protection.unfinished = true;
                return false;
            }
            origin.first_key = resume.next_key;
            origin.open_transition = resume.open_transition;
        }
        const RefChainWalk walk = walkRefChain(op, layout, life, origin, [&](RefLogTxn && txn, uint64_t)
        {
            for (const RefManifestEdge & edge : manifestEdgesOfTxn(txn))
                if (edge.change < 0)
                    removals.insert(layout.manifestKey(edge.manifest_id));
            if (!work_budget)
                return true;
            ++work_budget->sweep_recovery_ops_used;
            return work_budget->sweepRecoveryOpAvailable();
        }, &reader);
        if (walk.end == RefChainEnd::Stopped)
        {
            protection.recovery_incomplete = true;
            return false;
        }
        if (walk.end == RefChainEnd::BaseMoved)
        {
            protection.unfinished = true;
            return false;
        }
        return true;
    };

    std::optional<RefCoverage> walked = coverage;
    std::optional<FoldSealRef> walked_ref = seal_ref;
    for (size_t walks = 1;; ++walks)
    {
        std::set<String> removals;
        if (!walk_removals(sealedRefCursor(walked), removals))
            return protection;
        const std::optional<FoldSealRef> current_ref = readAdoptedFoldSealRef(op, layout);
        std::optional<RefCoverage> resampled = walked;
        if (current_ref != walked_ref)
            resampled = coverageOf(readFoldSealAt(op, layout, current_ref), catalog_cut, ns);
        walked_ref = current_ref;
        if (sealedRefCursor(resampled) == sealedRefCursor(walked))
        {
            protection.coverage = std::move(resampled);
            for (const String & key : removals)
                protection.active.insert(key);
            protection.tail_removal_targets = std::move(removals);
            return protection;
        }
        if (walks == 2)
        {
            protection.unfinished = true;
            return protection;
        }
        walked = std::move(resampled);
    }
}

}

NamespaceFoldView namespaceFoldView(Pool & store, const RootNamespace & ns)
{
    CasOperation op = store.openRequests().admit();
    NamespaceFoldView view;
    const CasRefCatalog::Snapshot catalog_cut = CasRefCatalog::read(op, store.layout());
    catalog_cut.life_index.throwIfAmbiguous("CAS orphan manifest sweep");
    view.coverage = coverageOf(readAdoptedFoldSeal(op, store.layout()).seal, catalog_cut, ns);
    return view;
}

std::string_view sweepRetainClassName(SweepRetainClass c)
{
    switch (c)
    {
        case SweepRetainClass::None:           return "none";
        case SweepRetainClass::NoCoverage:     return "no_coverage";
        case SweepRetainClass::Hold:           return "hold";
        case SweepRetainClass::UnconsumedSeal: return "unconsumed_seal";
        case SweepRetainClass::TailRemoval:    return "tail_removal";
        case SweepRetainClass::WorkBudgetExhausted: return "work_budget_exhausted";
    }
    return "unknown";
}

std::pair<SweepRetainClass, uint64_t> ManifestSweepResult::topRetainReason() const
{
    /// Enum order, so a tie resolves the same way on every pass and an unchanged pool keeps reporting
    /// the same verdict instead of alternating between two equally-large classes.
    const std::pair<SweepRetainClass, uint64_t> candidates[] = {
        {SweepRetainClass::NoCoverage, retained_no_coverage},
        {SweepRetainClass::Hold, retained_hold},
        {SweepRetainClass::UnconsumedSeal, retained_unconsumed_seal},
        {SweepRetainClass::TailRemoval, retained_tail_removal},
        {SweepRetainClass::WorkBudgetExhausted, retained_work_budget},
    };
    std::pair<SweepRetainClass, uint64_t> top{SweepRetainClass::None, 0};
    for (const auto & c : candidates)
        if (c.second > top.second)
            top = c;
    return top;
}

bool manifestDeletionPremise(const NamespaceFoldView & view, const ManifestKey & manifest,
                             String * retain_reason, SweepRetainClass * retain_class)
{
    const auto retain = [&](SweepRetainClass klass, String why)
    {
        if (retain_reason)
            *retain_reason = std::move(why);
        if (retain_class)
            *retain_class = klass;
        return false;
    };
    if (retain_class)
        *retain_class = SweepRetainClass::None;
    const String build_epoch = std::to_string(manifest.prefix.writer_epoch);

    /// UNCERTAINTY, and it comes first because it is the case where the predicate knows NOTHING. With no
    /// sealed coverage row, no round has folded a ref cursor for this namespace, so no epoch's closing
    /// seal can be shown consumed. The sweep's own protection view cannot stand in for the missing
    /// proof: that view is assembled from the very enumeration arithmetic intake distrusts.
    if (!view.coverage)
        return retain(SweepRetainClass::NoCoverage,
                      "no sealed fold coverage for the namespace: no round has folded a ref cursor for "
                      "it, so epoch " + build_epoch + "'s closing seal cannot be shown consumed");

    const RefCoverage & cov = *view.coverage;

    /// UNCERTAINTY, hold arm. A hold names the exact position the fold could not resolve, and everything
    /// at or above it is unaccounted -- including, for all this predicate can tell, the record that
    /// grants or removes this very manifest. `classification == Clamped` is tested separately from the
    /// hold even though the seal's strict grammar pairs them: the thing standing between a clamped
    /// namespace and an irreversible delete must not be a codec invariant enforced somewhere else.
    if (cov.hold)
        return retain(SweepRetainClass::Hold, "namespace held at " + renderRefTxnId(cov.hold->offending_position) + " ("
                      + String{holdReasonToWord(cov.hold->reason)} + ", retried "
                      + std::to_string(cov.hold->retry_count) + " round(s)): every record at or above "
                      "that position is unaccounted for");
    if (cov.classification == CoverageClass::Clamped)
        return retain(SweepRetainClass::Hold,
                      "namespace coverage is classified clamped with no hold recorded: whatever "
                      "stopped the fold was not carried, so nothing above its cursor is accounted for");
    if (cov.classification == CoverageClass::Absent)
        return retain(SweepRetainClass::NoCoverage,
                      "namespace coverage is classified absent: no round folded it, so its cursor "
                      "is not the result of any walk");

    /// RULE 1 (spec §6). Grants do not cross epochs, so every `+1` that could name an epoch-`E` build
    /// lives among epoch `E`'s own records; and an epoch is left ONLY over its consumed `EpochSeal`
    /// (INV-2), so a sealed cursor in a STRICTLY HIGHER epoch is durable proof that every one of those
    /// records has folded -- proof by arithmetic, which is what makes it independent of the listing.
    ///
    /// A cursor still INSIDE epoch `E` proves nothing of the sort, and that stays true even when it
    /// happens to sit on `E`'s own seal: the durable cursor records a POSITION, never the KIND of the
    /// record there, so "the cursor is the seal" is not a readable fact here. Retaining that case costs
    /// one more round and resolves itself -- the next epoch's first record crosses the seal, and the
    /// round after that sweeps the build.
    if (!(manifest.prefix.writer_epoch < cov.last_folded_ref_id.writer_epoch))
        return retain(SweepRetainClass::UnconsumedSeal,
                      "epoch " + build_epoch + "'s closing seal is not consumed: the sealed cursor is at "
                      + (cov.last_folded_ref_id == RefTxnId{} ? String{"none"} : renderRefTxnId(cov.last_folded_ref_id))
                      + ", so a grant naming this build may "
                      "still be unfolded above it");

    /// RULE 2 (spec §6). Removals DO cross epochs, so a record in a LATER epoch can name this build as a
    /// removal target; deleting the body before that `-1` folds leaves the fold clamping forever on a
    /// manifest it must read to emit the decrement (the GC-WEDGE-2026-07-10 shape).
    ///
    /// The sweep's protection set already spares a tail removal target before this predicate is
    /// consulted, so this test is belt over suspenders BY DESIGN: the rule belongs to the premise, so
    /// that a future sweep path that assembles its protection set differently cannot lose it. Note what
    /// it is NOT: its negative direction is no proof, because the set comes from the same enumeration
    /// rule (1) exists to stop trusting. Rule (1) is what makes the tail decidable; this makes the
    /// decision explicit.
    if (view.tail_removal_targets.contains(manifest.key))
        return retain(SweepRetainClass::TailRemoval,
                      "an unconsumed tail record above the cursor names this manifest as a removal "
                      "target: its `-1` has not folded, so the fold still needs the body");

    if (retain_reason)
        retain_reason->clear();
    return true;
}

namespace
{

/// Eligibility comes only from the durable mount-lease floor. A missing floor means NOT eligible;
/// do not replace that authority check with a frozen-sequence or judged-dead guess. Compare
/// `writer_epoch` first, then `build_sequence`, so old-epoch
/// debris drains after a process restart even when its build_sequence is above the current min_active_build_sequence.
bool buildPrefixEligibleUnderFloorOn(
    CasOperation & op, const Layout & layout, const RootNamespace & ns, const BuildPrefix & build_prefix)
{
    return buildPrefixEligibleUnderFloor(mountLeaseForNamespace(op, layout, ns).transform(toBuildFloor), build_prefix);
}

}

bool buildPrefixEligibleUnderFloor(const std::optional<BuildFloor> & floor, const BuildPrefix & build_prefix)
{
    if (!floor)
        return false;

    if (floor->writer_epoch > build_prefix.writer_epoch)
        return true;
    if (floor->writer_epoch < build_prefix.writer_epoch)
        return false;
    if (floor->min_active_build_sequence == std::numeric_limits<uint64_t>::max())
        return true;   /// farewell/retired sentinel: every seq is retired
    return floor->min_active_build_sequence > build_prefix.build_sequence;
}

bool buildPrefixEligibleUnderFloor(Pool & store, const RootNamespace & ns, const BuildPrefix & build_prefix)
{
    CasOperation op = store.openRequests().admit();
    return buildPrefixEligibleUnderFloorOn(op, store.layout(), ns, build_prefix);
}

uint64_t sweepNamespace(Pool & store, const RootNamespace & ns, const BuildPrefix & prefix,
                        std::vector<String> * warnings)
{
    const Layout & layout = store.layout();
    /// The decommission drain runs under the admin's lease; a fenced admin must stop deleting.
    CasOperation op = store.mountRequests().admit();
    if (!buildPrefixEligibleUnderFloorOn(op, layout, ns, prefix))
        return 0;   /// not eligible by the durable watermark fact — delete nothing (controls #8/#9)

    /// Build the protection view. A missing snapshot body, an invalid transaction, or an incomplete
    /// ordered view throws, causing the sweep to skip deletion and surface the error; it never substitutes
    /// an empty owner set. Skip this namespace's deletions on such a throw rather than deleting against a
    /// wrong (empty) view. When a
    /// caller opted in (`warnings != nullptr`), this "cannot confirm emptiness" also lands in `*warnings`
    /// -- not just the log -- so a decommission run does not silently report a clean drain that never
    /// happened; the log-only default stays exactly as before for every other caller, which treats this
    /// the same way the periodic sweep always has: skip and retry next round.
    /// The §6 premise's durable half, read before the protection view so both share one seal read.
    NamespaceFoldView view;
    const CasRefCatalog::Snapshot catalog_cut = CasRefCatalog::read(op, layout);
    catalog_cut.life_index.throwIfAmbiguous("CAS orphan manifest sweep");
    const AdoptedFoldSeal adopted_seal = readAdoptedFoldSeal(op, layout);
    view.coverage = coverageOf(adopted_seal.seal, catalog_cut, ns);
    const CatalogEntry * catalog_entry = catalogEntryOf(catalog_cut, ns);
    if (!catalog_entry || catalog_entry->state == NsState::Creating)
        return 0;   /// absent/Creating names have no recovery authority and therefore no deletion authority

    NamespaceProtection protection;
    try
    {
        const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(catalog_entry->ns, catalog_entry->incarnation);
        const std::optional<CkptSample> ckpt = readCkpt(op, layout, life);
        if (!ckpt)
        {
            const String warning = "CAS orphan sweep: namespace " + ns.string()
                + " is catalog-named but its required _ckpt is absent; skipped, emptiness not confirmed";
            LOG_WARNING(getLogger("CasOrphanManifestSweep"), "{}", warning);
            if (warnings)
                warnings->push_back(warning);
            return 0;
        }
        InlineKeyReader reader(op);
        protection = activeManifestKeys(
            op, reader, layout, catalog_cut, *catalog_entry, *ckpt, adopted_seal.ref, view.coverage);
        if (protection.unfinished)
        {
            const String warning = "CAS orphan sweep: namespace " + ns.string()
                + " ref chain could not be walked to a proven end; skipped, emptiness not confirmed";
            LOG_WARNING(getLogger("CasOrphanManifestSweep"), "{}", warning);
            if (warnings)
                warnings->push_back(warning);
            return 0;
        }
        view.coverage = protection.coverage;
        view.tail_removal_targets = protection.tail_removal_targets;

        /// The catalog cut predates the walk. A janitor deletes only lives a later catalog read does not
        /// name, so a row still present proves the walk saw no janitor delete.
        if (!catalogStillNamesLife(CasRefCatalog::read(op, layout).catalog, life))
        {
            const String warning = "CAS orphan sweep: namespace " + ns.string()
                + " life was removed while its ref chain was walked; skipped";
            LOG_WARNING(getLogger("CasOrphanManifestSweep"), "{}", warning);
            if (warnings)
                warnings->push_back(warning);
            return 0;
        }
    }
    catch (const Exception & e)
    {
        LOG_WARNING(getLogger("CasOrphanManifestSweep"),
                    "CAS orphan sweep: namespace {} protection view unavailable ({}); skipping its deletions",
                    ns.string(), e.message());
        if (warnings)
            warnings->push_back("CAS orphan sweep: namespace " + ns.string() + " protection view unavailable ("
                                 + e.message() + "); skipped, emptiness not confirmed");
        return 0;
    }

    /// Enumerate the ONE build prefix: cas/manifests/<ns>/<epoch-hex>-<seq-hex>/ in the canonical
    /// hexadecimal form -- the same rendering `Layout::manifestKey` uses.
    const String prefix_key = layout.manifestNamespacePrefix(ns)
        + renderRefTxnId(RefTxnId{prefix.writer_epoch, prefix.build_sequence}) + "/";

    uint64_t deleted = 0;
    op.forEachListedKey(prefix_key, [&](const ListedKey & listed)
    {
        if (protection.active.contains(listed.key))
            return true;   /// owned by a committed or precommit owner — never sweep

        /// THE §6 SAFETY FLOOR, under the watermark eligibility already established above. The
        /// watermark says the build is retired; the premise says the ref stream can be shown not to
        /// name this body. A refusal is recorded rather than silent.
        String retain_reason;
        if (!manifestDeletionPremise(view, ManifestKey{listed.key, prefix}, &retain_reason))
        {
            LOG_DEBUG(getLogger("CasOrphanManifestSweep"),
                      "CAS orphan sweep: retaining {} -- {}", listed.key, retain_reason);
            if (warnings)
                warnings->push_back("CAS orphan sweep: retained " + listed.key + " -- " + retain_reason);
            return true;
        }

        /// Exact-token delete: HEAD for the current incarnation, then remove exactly it -- never
        /// `removeCurrent`, whose re-head would delete whatever a fresh owner put there instead. A
        /// `Gone` (a 404 between the two) or a `Mismatch` (a fresh owner reclaimed the key) is
        /// tolerated, same as always, regardless of `warnings` -- that is the normal "someone else
        /// already reclaimed it" race, not a failure. A THROWN exception (a transient backend hiccup)
        /// is the one thing `warnings` changes: opted-in (non-null), it is recorded and the sweep moves
        /// to the next key; opted-out (nullptr, every pre-existing caller), it propagates exactly as
        /// before (fail-close).
        try
        {
            const std::optional<Meta> head = op.head(listed.key, Retry::standard());
            if (!head)
                return true;
            if (op.remove(listed.key, head->etag, Retry::standard()) == Removal::Removed)
                ++deleted;
        }
        catch (...)
        {
            if (!warnings)
                throw;
            warnings->push_back("CAS orphan sweep: " + listed.key + " delete failed: "
                                 + getCurrentExceptionMessage(/*with_stacktrace=*/false));
            return op.admitted();
        }
        return true;
    }, Retry::standard(), CasOperation::kCasListPageKeys, onGcEnumerationPage);
    return deleted;
}

ManifestSweepResult planManifestCursorPage(
    Pool & store,
    const String & cursor,
    uint64_t list_budget,
    uint64_t nomination_budget,
    bool catalog_recovery_authoritative,
    GcRoundWorkBudget * work_budget,
    ThreadPool * read_pool,
    size_t read_concurrency)
{
    ManifestSweepResult result;
    result.next_cursor = cursor;
    if (list_budget == 0)
        return result;

    const Layout & layout = store.layout();
    CasOperation op = store.openRequests().admit();
    /// The page's reader. With a pool and concurrency above one the candidates' bodies and the two
    /// ref-stream walks overlap their round trips; otherwise every read is inline and the page is the
    /// sequential one, request for request.
    std::optional<GcReadAhead> read_ahead;
    std::unique_ptr<KeyReader> reader;
    if (read_pool && read_concurrency > 1)
    {
        read_ahead.emplace(op, store.openRequests(), *read_pool, read_concurrency);
        reader = std::make_unique<ReadAheadKeyReader>(*read_ahead);
    }
    else
        reader = std::make_unique<InlineKeyReader>(op);
    const ListPage page = op.list(layout.casManifestsPrefix(), cursor, list_budget, Retry::standard());
    result.orphan_manifest_sweep_page_listed = true;
    /// This pass fetches exactly one page per round (the cursor advances across rounds, not within this
    /// call), so the metric increments once per call, not once per listed key.
    ProfileEvents::increment(ProfileEvents::CASGCEnumerationPages);

    /// One seal and one later catalog cut for the whole page; every namespace joins through those same
    /// immutable observations.
    const AdoptedFoldSeal adopted_seal = readAdoptedFoldSeal(op, layout);
    const CasRefCatalog::Snapshot catalog_cut = CasRefCatalog::read(op, layout);
    catalog_cut.life_index.throwIfAmbiguous("CAS orphan manifest sweep");

    /// One floor observation per namespace for the whole page. Every build of a namespace is judged
    /// against the same mount body; reading it per build prefix would cost three requests per listed
    /// key on a pool where every INSERT is its own build.
    std::map<String, std::optional<MountLease>> mount_lease_by_ns;
    std::map<String, NamespaceFoldView> view_by_ns;
    std::map<String, std::set<String>> active_by_ns;
    std::set<String> errored_namespaces;   /// protection view unavailable => skip, never delete

    /// The key of the last candidate this page actually DECIDED on. The cursor resumes strictly after
    /// it (`ListPage::next_cursor` is the last returned key), so a candidate the page never decided on
    /// stays ahead of the cursor and is examined next pass. See the budget rule below.
    String decided_through;
    bool budget_exhausted = false;

    /// Keys that passed every retain check. Their bodies are read AFTER the loop, and after the
    /// catalog cut: a manifest key is write-once, so its bytes do not depend on when they are read,
    /// and the only thing a later read can observe differently is absence, which retains.
    std::vector<ListedManifestObject> candidates;

    for (const ListedKey & listed : page.keys)
    {
        ++result.listed;

        /// UNCERTAINTY, budget arm (§6). Once a NON-ZERO delete budget is used up, the page stops
        /// deciding: the candidates behind it are retained AND the cursor does not step over them.
        /// Advancing past a candidate nothing examined would turn "retained this round" into
        /// "unexamined until the cursor wraps the whole `cas/manifests/` keyspace" -- the same trade
        /// the round-level gate refuses when it freezes the cursor along with a suppressed sweep.
        /// A budget of ZERO is not exhaustion but a list-only pass: nothing is ever deletable, so
        /// freezing the cursor on it would make the sweep spin on one page forever. That pass keeps
        /// its pre-existing behaviour and advances.
        if (budget_exhausted || (nomination_budget > 0 && candidates.size() >= nomination_budget))
        {
            budget_exhausted = true;
            ++result.skipped;
            continue;
        }

        const auto parsed = parseListedManifestObject(layout, listed.key);
        if (!parsed)
        {
            ++result.skipped;
            decided_through = listed.key;
            continue;
        }

        if (nomination_budget == 0)
        {
            /// The list-only pass: examined, not deletable, cursor advances (see the budget rule above).
            ++result.skipped;
            decided_through = listed.key;
            continue;
        }

        /// Within one `writer_epoch`, `build_sequence < min_active_build_sequence` means the build is no longer active.
        /// It may have completed `promote`, been cancelled, or finished cleanup. This does not yet make its manifests
        /// safe to delete: ownership and fold coverage must also be checked.
        /// If `build_sequence >= min_active_build_sequence`, the build may still be active, so retain its manifests.
        if (!isBuildUnderFloor(op, layout, *parsed, mount_lease_by_ns, result))
        {
            ++result.skipped;
            decided_through = listed.key;
            continue;
        }

        const CatalogEntry * catalog_entry = catalogEntryOf(catalog_cut, parsed->ns);
        auto [view_it, view_inserted] = view_by_ns.emplace(parsed->ns.string(), NamespaceFoldView{});
        auto [active_it, inserted] = active_by_ns.emplace(parsed->ns.string(), std::set<String>{});
        if (inserted)
        {
            /// UNCERTAINTY, work-budget arm: building a fresh namespace's protection view
            /// is the expensive step (a catalog-authoritative table recovery plus a committed-tail
            /// walk) the LIST/nomination budgets never bounded. Once the round's per-page namespace cap
            /// is spent, a NEW namespace gets no view at all -- retained, exactly like every other
            /// "cannot confirm emptiness" cause below, never a partial or best-effort one.
            if (work_budget && !work_budget->sweepNamespaceAvailable())
            {
                LOG_DEBUG(getLogger("CasOrphanManifestSweep"),
                    "CAS orphan sweep: retaining {} -- round's per-page namespace work budget exhausted",
                    parsed->key);
                errored_namespaces.insert(parsed->ns.string());
                ++result.retained_work_budget;
                ++result.skipped;
                decided_through = listed.key;
                continue;
            }
            if (work_budget)
                ++work_budget->sweep_namespaces_used;
            view_it->second.coverage = coverageOf(adopted_seal.seal, catalog_cut, parsed->ns);
            if (catalog_entry && !catalog_recovery_authoritative)
            {
                LOG_DEBUG(getLogger("CasOrphanManifestSweep"),
                    "CAS orphan sweep: retaining {} -- caller did not authorize catalog-named recovery",
                    parsed->key);
                errored_namespaces.insert(parsed->ns.string());
                ++result.retained_no_coverage;
                ++result.skipped;
                decided_through = listed.key;
                continue;
            }
            if (catalog_entry && catalog_entry->state == NsState::Creating)
            {
                LOG_DEBUG(getLogger("CasOrphanManifestSweep"),
                    "CAS orphan sweep: retaining {} -- catalog namespace is Creating and has no recovery authority",
                    parsed->key);
                errored_namespaces.insert(parsed->ns.string());
                ++result.retained_no_coverage;
                ++result.skipped;
                decided_through = listed.key;
                continue;
            }
            /// A corrupt snapshot or invalid transaction means the protection view is unavailable. Skip
            /// this namespace's deletions and surface the error; never substitute an empty owner set.
            try
            {
                if (catalog_entry)
                {
                    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(
                        catalog_entry->ns, catalog_entry->incarnation);
                    const std::optional<CkptSample> ckpt = readCkpt(op, layout, life);
                    if (!ckpt)
                    {
                        LOG_WARNING(getLogger("CasOrphanManifestSweep"),
                            "CAS orphan sweep: namespace {} is catalog-named but its required _ckpt is absent; skipping",
                            parsed->ns.string());
                        errored_namespaces.insert(parsed->ns.string());
                    }
                    else
                    {
                        NamespaceProtection protection = activeManifestKeys(
                            op, *reader, layout, catalog_cut, *catalog_entry, *ckpt, adopted_seal.ref,
                            view_it->second.coverage, work_budget);
                        if (protection.unfinished)
                        {
                            LOG_DEBUG(getLogger("CasOrphanManifestSweep"),
                                "CAS orphan sweep: retaining {} -- the namespace's ref chain could not be walked "
                                "to a proven end", parsed->key);
                            errored_namespaces.insert(parsed->ns.string());
                        }
                        else if (protection.recovery_incomplete)
                        {
                            /// A walk stopped early: `active`/`tail_removal_targets`
                            /// are a PARTIAL view. Discard them and retain the whole namespace on this
                            /// page instead of deciding from an incomplete protection set (fail-closed).
                            LOG_DEBUG(getLogger("CasOrphanManifestSweep"),
                                "CAS orphan sweep: retaining {} -- the round's recovery work budget ran out "
                                "before it could confirm the namespace's protection view",
                                parsed->key);
                            errored_namespaces.insert(parsed->ns.string());
                            ++result.retained_work_budget;
                        }
                        else
                        {
                            view_it->second.coverage = std::move(protection.coverage);
                            view_it->second.tail_removal_targets = std::move(protection.tail_removal_targets);
                            active_it->second = std::move(protection.active);
                        }
                    }
                }

            }
            catch (const Exception & e)
            {
                LOG_WARNING(getLogger("CasOrphanManifestSweep"),
                            "CAS orphan sweep: namespace {} protection view unavailable ({}); skipping",
                            parsed->ns.string(), e.message());
                errored_namespaces.insert(parsed->ns.string());
            }
        }
        if (errored_namespaces.contains(parsed->ns.string()))
        {
            ++result.skipped;
            decided_through = listed.key;
            continue;
        }
        if (catalog_entry && active_it->second.contains(parsed->key))
        {
            ++result.skipped;
            decided_through = listed.key;
            continue;
        }

        if (catalog_entry)
        {
            /// THE §6 SAFETY FLOOR, the same predicate `sweepNamespace` calls, under the same watermark
            /// eligibility. A refusal is recorded rather than silent.
            String retain_reason;
            SweepRetainClass retain_class = SweepRetainClass::None;
            if (!manifestDeletionPremise(view_it->second, ManifestKey{parsed->key, parsed->build_prefix},
                                         &retain_reason, &retain_class))
            {
                /// The sentence goes to the debug log for whoever is chasing ONE object; the class goes to
                /// a counter, which is the only form in which this path can report itself (see
                /// `ManifestSweepResult`). Both come from the predicate; neither is derived from the other.
                LOG_DEBUG(getLogger("CasOrphanManifestSweep"),
                          "CAS orphan sweep: retaining {} -- {}", parsed->key, retain_reason);
                switch (retain_class)
                {
                    case SweepRetainClass::NoCoverage:     ++result.retained_no_coverage; break;
                    case SweepRetainClass::Hold:           ++result.retained_hold; break;
                    case SweepRetainClass::UnconsumedSeal: ++result.retained_unconsumed_seal; break;
                    case SweepRetainClass::TailRemoval:    ++result.retained_tail_removal; break;
                    case SweepRetainClass::WorkBudgetExhausted: ++result.retained_work_budget; break;  /// unreachable: the premise never returns this class itself
                    case SweepRetainClass::None:           break;   /// unreachable: the premise refused
                }
                ++result.skipped;
                decided_through = listed.key;
                continue;
            }
        }

        candidates.push_back(*parsed);
        decided_through = listed.key;
    }

    /// A writer may create a namespace, commit a ref and advance the floor after `catalog_cut` was read.
    /// Recheck after floor reads so the old catalog cut cannot make its committed manifest look unowned.
    excludeCandidatesWithChangedCatalogEntries(op, layout, catalog_cut, candidates, result);

    size_t next_body_hint = 0;
    for (const ListedManifestObject & candidate : candidates)
    {
        while (next_body_hint < candidates.size() && reader->pending() < reader->window())
            reader->hint(candidates[next_body_hint++].key);
        const std::optional<Object> got = reader->take(candidate.key);
        if (!got)
        {
            /// Gone since the LIST: a fresh writer never reuses the key, so there is nothing to
            /// nominate and nothing to retain. The key was decided above; the cursor stands.
            ++result.skipped;
            continue;
        }
        std::optional<PartManifest> body;
        try
        {
            body = decodePartManifest(openObject(FormatId::PartManifest, got->bytes));
        }
        catch (Exception & e)
        {
            /// Malformed bytes are retained and counted. Any other error fails the round: retaining would label a
            /// valid object undecodable.
            if (!isMalformedStoredRecordError(e))
            {
                e.addMessage("while the orphan sweep decodes the manifest `{}` of namespace `{}`", candidate.key,
                             candidate.ns.string());
                throw;
            }
            LOG_ERROR(getLogger("CasOrphanManifestSweep"),
                "CAS orphan sweep: manifest at {} cannot be decoded and was retained; run cas-fsck to "
                "enumerate such objects", candidate.key);
            ++result.undecodable;
            ++result.skipped;
            continue;
        }
        const ManifestId id{candidate.ns, candidate.ref};
        if (!refMatchesBody(id.ref, *body) || !manifestNamespaceMatches(id.root_namespace, *body))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS orphan sweep: manifest identity mismatch at {} while deriving exact source edges",
                candidate.key);

        ManifestSweepResult::Nomination nomination{
            .id = id,
            .key = candidate.key,
            .token = PersistedEtag::capture(got->etag),
            .source_retirements = {}};
        for (const ManifestEntry & entry : body->entries)
            if (entry.placement == EntryPlacement::Blob)
                nomination.source_retirements.push_back(BlobSourceRetirement{
                    .ref = entry.ref,
                    .source_id = sourceEdgeId(id, entry.path)});
        result.nominations.push_back(std::move(nomination));
    }

    if (budget_exhausted)
    {
        /// Resume strictly after the last DECIDED key, leaving every undecided candidate ahead of the
        /// cursor. `wrapped` stays false because this page did not reach the end of the keyspace — it
        /// stopped early, and reporting a wrap would tell the caller the sweep had made a full circuit
        /// over keys it never looked at.
        result.next_cursor = decided_through;
        result.wrapped = false;
        return result;
    }

    result.next_cursor = page.next_cursor;
    result.wrapped = page.next_cursor.empty();
    return result;
}

}
