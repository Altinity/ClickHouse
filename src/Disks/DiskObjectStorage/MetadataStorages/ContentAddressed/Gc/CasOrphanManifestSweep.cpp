#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>
#include <algorithm>
#include <map>
#include <set>
#include <limits>

namespace ProfileEvents
{
    extern const Event CasGcEnumerationPages;
}

namespace DB::Cas
{

namespace
{

/// Hook used by this file's GC-owned enumeration calls to `forEachListedKey` and `recoverRefTable`.
/// It increments once per physical LIST page, never once per listed key.
void onGcEnumerationPage()
{
    ProfileEvents::increment(ProfileEvents::CasGcEnumerationPages);
}

/// The durable build floor is stored in the per-server mount lease together with the writer epoch. A
/// namespace is rooted by `server_root_id`, but that id is a clean relative path and can contain slashes.
/// Try namespace prefixes from longest to shortest and accept the first durable mount body. Without a
/// mount there is no deletion authority, so the caller must leave the prefix untouched. The mount's
/// `writer_epoch` and `min_active` are the single durable epoch/floor pair used for eligibility, including
/// across process replacement and the retired sentinel.
std::optional<MountLease> floorForNamespace(Pool & store, const RootNamespace & ns)
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
            if (const auto got = store.backend().get(store.layout().mountKey(server_root_id)))
                return decodeMountLease(got->bytes);
        }
        if (pos == 0)
            break;
    }
    return std::nullopt;
}

struct ListedManifestObject
{
    RootNamespace ns;
    BuildPrefix prefix;
    String key;
};

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
        .prefix = BuildPrefix{.writer_epoch = parsed->ref.writer_epoch, .build_sequence = parsed->ref.build_sequence},
        .key = key};
}

/// The fold seal `gc/state` currently adopts, or `nullopt` when the pool has no `gc/state` or no seal
/// at `(snap_generation, snap_attempt)` — a pool whose GC has never completed a round. It is read ONCE
/// per sweep pass; every namespace the pass touches takes its coverage row out of the same object.
std::optional<CasFoldSeal> readAdoptedFoldSeal(Pool & store)
{
    const Layout & layout = store.layout();
    const auto state_got = store.backend().get(layout.gcStateKey());
    if (!state_got)
        return std::nullopt;
    const GcState state = decodeGcState(state_got->bytes);
    const auto got = store.backend().get(layout.foldSealKey(state.snap_generation, state.snap_attempt));
    if (!got)
        return std::nullopt;
    return decodeFoldSeal(got->bytes, state.snap_generation);
}


/// One namespace's shard-0 coverage row out of an adopted seal. Absent means no round has sealed a
/// cursor for the namespace, which the premise treats as "nothing about its ref stream is proven".
std::optional<ShardCoverage> coverageOf(const std::optional<CasFoldSeal> & seal, const RootNamespace & ns)
{
    if (!seal)
        return std::nullopt;
    const auto it = seal->per_ns_shard.find(cursorKey(ns, /*shard*/0));
    if (it == seal->per_ns_shard.end())
        return std::nullopt;
    return it->second;
}

/// The durable `last_folded_ref_id` of a coverage row, `{0, 0}` when there is none — as for a fresh
/// pool. A manifest removed by a log above this cursor has not had its `-1` decrement folded, so its
/// body remains load-bearing until the fold catches up.
RefTxnId sealedRefCursor(const std::optional<ShardCoverage> & coverage)
{
    return coverage ? coverage->last_folded_ref_id : RefTxnId{};
}

/// One namespace's protection view: the manifest object keys the sweep must never delete, split so the
/// §6 premise can test the removal half on its own. `active` is the pre-existing union the delete sites
/// consult; `tail_removal_targets` is the subset the unconsumed tail above the cursor names as removal
/// targets, which is what rule (2) is stated over.
struct NamespaceProtection
{
    std::set<String> active;
    std::set<String> tail_removal_targets;
};


/// The active manifest-object-key set for one namespace, built as the
/// same complete view writer recovery uses:
///   owners in the newest snapshot + owner changes in every later log (== `recoverRefTable`'s committed
///   rows and live precommits) + manifests removed anywhere in the tail above the durable
///   `last_folded_ref_id` (their `-1` is not yet folded, so the GC fold still needs the body).
/// Keys (not ManifestIds) so a listed object key can be tested directly. Throws on a corrupt snapshot /
/// invalid transaction (via `recoverRefTable` / `decodeRefLogTxn`); the caller SKIPS the namespace's
/// deletions on such a throw rather than substituting an empty owner set.
NamespaceProtection activeManifestKeys(Pool & store, const RootNamespace & ns,
                                       const std::optional<ShardCoverage> & coverage)

{
    NamespaceProtection protection;
    std::set<String> & active = protection.active;
    const Layout & layout = store.layout();
    Backend & backend = store.backend();

    /// Current owners = snapshot + replayed tail (committed rows + live precommits).
    const RecoveredRefTable recovered = recoverRefTableDetailed(backend, layout, ns, onGcEnumerationPage);
    const RefTableState & state = recovered.state;
    for (const auto [ref_name, row] : state.getCommitted())
        active.insert(layout.manifestKey(ManifestId{ns, row.manifest_ref}));
    for (const auto & [ref_name, manifest_ref] : state.getPrecommits())
        active.insert(layout.manifestKey(ManifestId{ns, manifest_ref}));

    /// Tail-removal protection: every manifest removed by a log ABOVE the durable fold cursor stays active
    /// until its `-1` folds. LIST the table's logs, decode those above the cursor, and protect each
    /// removed manifest (a `-1` edge). A namespace-removal transaction names every removed owner explicitly,
    /// so this also protects a whole removed namespace's bodies until the fold catches up.
    const RefTxnId cursor = sealedRefCursor(coverage);
    /// Stage B (Task 4-C): see `CasRefCatalog::resolveLifeOrSentinel`'s doc for why the sentinel
    /// fallback is correct, not a guess, for a namespace the catalog does not name.
    const NamespaceLifeId life = CasRefCatalog::resolveLifeOrSentinel(backend, layout, ns);
    std::vector<RefTxnId> logs;
    forEachListedKey(backend, layout.namespaceStreamPrefix(life), [&](const ListedKey & lk)
    {
        if (const auto parsed = layout.parseRefObjectKey(lk.key);
            parsed && parsed->life_id == life.incarnation && parsed->kind == RefObjectKind::Log)
            logs.push_back(parsed->txn_id);
    }, 1000, onGcEnumerationPage);
    std::sort(logs.begin(), logs.end());

    for (const RefTxnId & id : logs)
    {
        if (!(cursor < id))
            continue;   /// id <= cursor: its edges are already folded
        const auto got = backend.get(layout.refLogKey(life, id));
        if (!got)
            continue;   /// vanished (a concurrent cleanup published a covering snapshot) -- its -1 was folded
        const RefLogTxn txn = decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns.string(), id);
        for (const RefManifestEdge & edge : manifestEdgesOfTxn(txn))
            if (edge.change < 0)
            {
                const String key = layout.manifestKey(edge.manifest_id);
                active.insert(key);
                protection.tail_removal_targets.insert(key);
            }
    }
    return protection;
}

}

NamespaceFoldView namespaceFoldView(Pool & store, const RootNamespace & ns)
{
    NamespaceFoldView view;
    view.coverage = coverageOf(readAdoptedFoldSeal(store), ns);
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

    const ShardCoverage & cov = *view.coverage;

    /// UNCERTAINTY, hold arm. A hold names the exact position the fold could not resolve, and everything
    /// at or above it is unaccounted -- including, for all this predicate can tell, the record that
    /// grants or removes this very manifest. `classification == 4` is tested separately from the hold
    /// even though the seal's strict grammar pairs them: the thing standing between a clamped namespace
    /// and an irreversible delete must not be a codec invariant enforced somewhere else.
    if (cov.hold)
        return retain(SweepRetainClass::Hold, "namespace held at " + renderRefTxnId(cov.hold->offending_position) + " ("
                      + String{holdReasonToWord(cov.hold->reason)} + ", retried "
                      + std::to_string(cov.hold->retry_count) + " round(s)): every record at or above "
                      "that position is unaccounted for");
    if (cov.classification == 4)
        return retain(SweepRetainClass::Hold,
                      "namespace coverage is classified clamped (4) with no hold recorded: whatever "
                      "stopped the fold was not carried, so nothing above its cursor is accounted for");
    if (cov.classification == 0)
        return retain(SweepRetainClass::NoCoverage,
                      "namespace coverage is classified absent (0): no round folded it, so its cursor "
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
                      + renderRefTxnId(cov.last_folded_ref_id) + ", so a grant naming this build may "
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

bool prefixEligible(Pool & store, const RootNamespace & ns, const BuildPrefix & prefix)
{
    /// Eligibility comes only from the durable mount-lease floor. A missing floor means NOT eligible;
    /// do not replace that authority check with a frozen-sequence or judged-dead guess. Compare
    /// `writer_epoch` first, then `build_sequence`, so old-epoch
    /// debris drains after a process restart even when its build_sequence is above the current min_active.
    const auto floor = floorForNamespace(store, ns);
    if (!floor)
        return false;

    const MountLease & w = *floor;
    if (prefix.writer_epoch < w.writer_epoch)
        return true;
    if (prefix.writer_epoch > w.writer_epoch)
        return false;
    if (w.min_active == std::numeric_limits<uint64_t>::max())
        return true;   /// farewell/retired sentinel: every seq is retired
    return w.min_active > prefix.build_sequence;
}

uint64_t sweepNamespace(Pool & store, const RootNamespace & ns, const BuildPrefix & prefix,
                        std::vector<String> * warnings)
{
    if (!prefixEligible(store, ns, prefix))
        return 0;   /// not eligible by the durable watermark fact — delete nothing (controls #8/#9)

    const Layout & layout = store.layout();
    Backend & backend = store.backend();

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
    view.coverage = coverageOf(readAdoptedFoldSeal(store), ns);

    NamespaceProtection protection;
    try
    {
        protection = activeManifestKeys(store, ns, view.coverage);
        view.tail_removal_targets = protection.tail_removal_targets;

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
    forEachListedKey(backend, prefix_key, [&](const ListedKey & listed)
    {
        if (protection.active.contains(listed.key))
            return;   /// owned by a committed or precommit owner — never sweep

        /// THE §6 SAFETY FLOOR, under the watermark eligibility already established above. The
        /// watermark says the build is retired; the premise says the ref stream can be shown not to
        /// name this body. A refusal is recorded rather than silent (Constraint 10).
        String retain_reason;
        if (!manifestDeletionPremise(view, ManifestKey{listed.key, prefix}, &retain_reason))
        {
            LOG_DEBUG(getLogger("CasOrphanManifestSweep"),
                      "CAS orphan sweep: retaining {} -- {}", listed.key, retain_reason);
            if (warnings)
                warnings->push_back("CAS orphan sweep: retained " + listed.key + " -- " + retain_reason);
            return;
        }

        /// Exact-token delete: HEAD for the current token, then deleteExact. A 404 between HEAD and
        /// delete (or a TokenMismatch — a fresh owner reclaimed it) is tolerated (record-and-continue),
        /// same as always, regardless of `warnings` -- that is the normal "someone else already reclaimed
        /// it" race, not a failure. A THROWN exception (a transient backend hiccup) is the one thing
        /// `warnings` changes: opted-in (non-null), it is recorded and the sweep moves to the next key;
        /// opted-out (nullptr, every pre-existing caller), it propagates exactly as before (fail-close).
        try
        {
            const HeadResult head = backend.head(listed.key);
            if (!head.exists)
                return;
            const DeleteOutcome outcome = backend.deleteExact(listed.key, head.token);   /// NotFound/TokenMismatch spared
            if (classifyDeleteOutcome(outcome) == DeleteClass::Deleted)
                ++deleted;
        }
        catch (...)
        {
            if (!warnings)
                throw;
            warnings->push_back("CAS orphan sweep: " + listed.key + " delete failed: "
                                 + getCurrentExceptionMessage(/*with_stacktrace=*/false));
        }
    }, 1000, onGcEnumerationPage);
    return deleted;
}

ManifestSweepResult sweepManifestCursorPage(
    Pool & store,
    const String & cursor,
    uint64_t list_budget,
    uint64_t delete_budget)
{
    ManifestSweepResult result;
    result.next_cursor = cursor;
    if (list_budget == 0)
        return result;

    Backend & backend = store.backend();
    const Layout & layout = store.layout();
    const ListPage page = backend.list(layout.casManifestsPrefix(), cursor, list_budget);
    /// This pass fetches exactly one page per round (the cursor advances across rounds, not within this
    /// call), so the metric increments once per call, not once per listed key.
    ProfileEvents::increment(ProfileEvents::CasGcEnumerationPages);

    /// One seal read for the whole page; every namespace on it takes its coverage row from this object.
    const std::optional<CasFoldSeal> adopted_seal = readAdoptedFoldSeal(store);

    std::map<String, bool> eligible_by_prefix;
    std::map<String, NamespaceFoldView> view_by_ns;
    std::map<String, std::set<String>> active_by_ns;
    std::set<String> errored_namespaces;   /// protection view unavailable => skip, never delete

    /// The key of the last candidate this page actually DECIDED on. The cursor resumes strictly after
    /// it (`ListPage::next_cursor` is the last returned key), so a candidate the page never decided on
    /// stays ahead of the cursor and is examined next pass. See the budget rule below.
    String decided_through;
    bool budget_exhausted = false;

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
        if (budget_exhausted || (delete_budget > 0 && result.deleted >= delete_budget))
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

        if (delete_budget == 0)
        {
            /// The list-only pass: examined, not deletable, cursor advances (see the budget rule above).
            ++result.skipped;
            decided_through = listed.key;
            continue;
        }

        const String eligibility_key = parsed->ns.string() + "\n"
            + std::to_string(parsed->prefix.writer_epoch) + "\n"
            + std::to_string(parsed->prefix.build_sequence);
        auto [eligible_it, eligible_inserted] = eligible_by_prefix.emplace(eligibility_key, false);
        if (eligible_inserted)
            eligible_it->second = prefixEligible(store, parsed->ns, parsed->prefix);
        if (!eligible_it->second)
        {
            ++result.skipped;
            decided_through = listed.key;
            continue;
        }

        auto [view_it, view_inserted] = view_by_ns.emplace(parsed->ns.string(), NamespaceFoldView{});
        auto [active_it, inserted] = active_by_ns.emplace(parsed->ns.string(), std::set<String>{});
        if (inserted)
        {
            view_it->second.coverage = coverageOf(adopted_seal, parsed->ns);
            /// A corrupt snapshot or invalid transaction means the protection view is unavailable. Skip
            /// this namespace's deletions and surface the error; never substitute an empty owner set.
            try
            {
                NamespaceProtection protection =
                    activeManifestKeys(store, parsed->ns, view_it->second.coverage);
                view_it->second.tail_removal_targets = std::move(protection.tail_removal_targets);
                active_it->second = std::move(protection.active);

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
        if (active_it->second.contains(parsed->key))
        {
            ++result.skipped;
            decided_through = listed.key;
            continue;
        }

        /// THE §6 SAFETY FLOOR, the same predicate `sweepNamespace` calls, under the same watermark
        /// eligibility. A refusal is recorded rather than silent (Constraint 10).
        String retain_reason;
        SweepRetainClass retain_class = SweepRetainClass::None;
        if (!manifestDeletionPremise(view_it->second, ManifestKey{parsed->key, parsed->prefix},
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
                case SweepRetainClass::None:           break;   /// unreachable: the premise refused
            }
            ++result.skipped;
            decided_through = listed.key;
            continue;
        }

        Token token;
        if (listed.token)
            token = *listed.token;
        else
        {
            const HeadResult head = backend.head(parsed->key);
            if (!head.exists)
            {
                ++result.skipped;
                decided_through = listed.key;
                continue;
            }
            token = head.token;
        }

        const DeleteOutcome outcome = backend.deleteExact(parsed->key, token);
        const DeleteClass outcome_class = classifyDeleteOutcome(outcome);
        if (outcome_class == DeleteClass::Deleted)
            ++result.deleted;
        else
            ++result.skipped;
        decided_through = listed.key;

        /// Every manifest-body deletion emits an audit row. The full raw key is used as `object_hash`, so
        /// namespace-qualified keys cannot collide when different namespaces use the same manifest-ref
        /// string.
        EventEmitter{store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::ManifestDelete;
            e.namespace_ = parsed->ns.string();
            e.object_kind = CasEventObjectKind::Manifest;
            e.object_hash = parsed->key;
            e.outcome = String{deleteClassName(outcome_class)};
            e.reason = "orphan-manifest sweep: exact-token delete of an eligible+unowned build-prefix body";
            e.detail = {{"writer_epoch", std::to_string(parsed->prefix.writer_epoch)},
                        {"build_sequence", std::to_string(parsed->prefix.build_sequence)}};
        });
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
