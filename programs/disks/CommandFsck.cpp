#include <Disks/DiskObjectStorage/DiskObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/DiskType.h>
#include <Common/Exception.h>
#include <Common/assert_cast.h>
#include <ICommand.h>
#include <Utils.h>

#include <chrono>
#include <iostream>
#include <optional>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

class CommandFsck final : public ICommand
{
public:
    CommandFsck() : ICommand("CommandFsck")
    {
        command_name = "cas-fsck";
        exits_nonzero_on_error = true;
        description = "Independently verify content-addressed pool reachability and deletion intents. Reads only, "
                      "except `--fix legacy-markers`. "
                      "Exits 1 on a hard finding: a reachable object missing (dangling), a hole in a ref-log "
                      "stream (chain_broken), a corrupted GC run, a malformed key (lifeless_keys), a malformed "
                      "deletion intent (intent_malformed), a deletion intent or its body that could not be read "
                      "(intent_unread), or a `pending` GC row whose deletion intent is absent while its body is "
                      "present (pending_without_intent).";
        options_description.add_options()("detail", "list per-object rows (class, key, size, reachable_from)")(
            "timeout", po::value<UInt64>(), "abort the scan after N seconds with a clear error instead of hanging (default 600; 0 = unbounded)")(
            "namespace", po::value<String>(), "scope the scan to namespaces with this prefix (skips the pool-wide "
                                               "physical/pipeline classification; still reports the scoped namespaces' "
                                               "dangling refs and orphan-manifest debris as unreachable)")(
            "partial", "on --timeout, print the counts accumulated so far flagged partial=1 instead of aborting empty-handed")(
            "fix", po::value<String>(), "repair: `legacy-markers` deletes the `.meta` markers left from the marker protocol "
                                        "(whole-pool scans only; nothing else is ever deleted; not on a local object "
                                        "storage, where `cas-migrate-pool` removes them)")(
            "verify-intents", "also read every deletion intent and `HEAD` its body (two requests per intent): finds intents "
                              "with other bytes than the record, and counts intents over a current, replaced or absent body; "
                              "without it intents are classified from the listing only");
    }

    void executeImpl(const CommandLineOptions & options, DisksClient & client) override
    {
        const bool detail = options.contains("detail");
        const UInt64 timeout_sec = getValueFromCommandLineOptionsWithDefault<UInt64>(options, "timeout", 600);
        const String namespace_prefix = options.contains("namespace") ? options["namespace"].as<String>() : "";
        const bool partial = options.contains("partial");
        const bool verify_intents = options.contains("verify-intents");
        bool fix_legacy_markers = false;
        if (options.contains("fix"))
        {
            const String what = options["fix"].as<String>();
            if (what != "legacy-markers")
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "cas-fsck: unknown --fix '{}'; the only fix is `legacy-markers`", what);
            if (!namespace_prefix.empty())
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "cas-fsck: --fix legacy-markers needs a scan of the whole pool, not --namespace");
            fix_legacy_markers = true;
        }
        auto disk = client.getCurrentDiskWithPath().getDisk();
        auto * ca = &contentAddressedStorageOf(*disk, "cas-fsck");

        if (!ca->isReadOnly())
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "cas-fsck: open the CA disk read-only (<readonly>true</readonly>) so inspection never probes/schedules a live pool");
        /// A read-only local object storage refuses every delete; the migration tool removes markers through a writable
        /// open that mounts nothing.
        if (fix_legacy_markers && assert_cast<DiskObjectStorage &>(*disk).getObjectStorage()->getType() == ObjectStorageType::Local)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "cas-fsck: --fix legacy-markers cannot delete through a read-only local object storage; run `{}`, which "
                "removes the legacy markers of a migrated pool", Cas::kMigratePoolCommandLine);

        /// Progress to stderr so a long scan is visibly working (the reachable=… summary stays on
        /// stdout, machine-parseable). The deadline bounds a slow-but-progressing scan with a clear
        /// error; for a single LIST page stuck in S3-client retries, lower the disk's S3 retry budget.
        Cas::FsckProgress on_progress = [](std::string_view phase, uint64_t objects, uint64_t pages)
        {
            std::cerr << "cas-fsck: " << phase << " — " << objects << " objects";
            if (pages)
                std::cerr << ", " << pages << " pages";
            std::cerr << "\n";
        };
        std::optional<std::chrono::steady_clock::time_point> deadline;
        if (timeout_sec > 0)
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);

        const Cas::FsckReport report = Cas::runFsck(*ca->store(), detail, on_progress, deadline, partial, namespace_prefix, fix_legacy_markers, verify_intents);

        /// Built by `Cas::formatFsckSummary` rather than here, so the line is reachable from a unit test.
        /// It was assembled inline until 2026-07-26, and in that time `corrupted_runs` was added to the
        /// report and to `clean()` without ever being rendered — a hard finding no run could report.
        std::cout << Cas::formatFsckSummary(report) << "\n";

        /// De-alarm the pipeline classes for humans: on an active pool a nonzero pending/awaiting
        /// count is the ack-floor deletion pipeline working as designed, not a leak. `stale_edge` is
        /// deliberately NOT part of this sentence: those blobs look exactly like an `AwaitingGc`
        /// backlog but will never drain, and being swept into "expected, no action needed" is what
        /// hid them.
        if (report.pending_gc + report.awaiting_gc > 0)
            std::cout << "note: " << report.pending_gc + report.awaiting_gc
                      << " unreferenced object(s) are inside the normal GC deletion pipeline "
                         "(condemn and create the intent -> delete the body -> remove the intent takes ~3 rounds) — "
                         "expected, no action needed\n";
        if (report.stale_edge > 0)
            std::cout << "note: " << report.stale_edge
                      << " unreferenced object(s) carry ONLY source edges naming manifests that no longer "
                         "exist: their in-degree can never reach zero, so the incremental GC will never "
                         "reclaim them — NOT expected, investigate (a rebuild of the in-degree state is the "
                         "only way to clear them)\n";
        /// `unchecked` is not a finding and does not exit nonzero — it says the audit could not PROVE
        /// those namespaces either way, which is a statement about coverage. Saying so out loud is the
        /// whole point: a silent verdict of "no complaints" would read as a clean bill of health.
        if (report.unchecked > 0)
            std::cout << "note: " << report.unchecked
                      << " namespace(s) could NOT be proved either way (an unprovable epoch crossing, an "
                         "unreadable record, or a namespace the scan could not examine) — this run says "
                         "nothing about them; the per-namespace reason is listed as an `unchecked` row "
                         "under --detail\n";
        /// A deletion intent outlives its body until GC's later round, so a missing body is the normal state
        /// of a deleted blob. Saying so keeps `intent_body_gone` from reading as damage.
        if (!report.intents_verified && report.intent_listed > 0)
            std::cout << "note: " << report.intent_listed
                      << " deletion intent(s) were counted from the listing; the bytes of an intent and the body behind it "
                         "are read only with --verify-intents (two requests per intent)\n";
        if (report.intent_body_gone > 0)
            std::cout << "note: " << report.intent_body_gone
                      << " deletion intent(s) have no body — normal: GC removes an intent in a later round than "
                         "the body, no action needed\n";
        if (report.intent_replaced > 0)
            std::cout << "note: " << report.intent_replaced
                      << " deletion intent(s) name an incarnation that is no longer the body — GC removes them "
                         "later, no action needed\n";
        if (report.pending_intent_absent > 0)
            std::cout << "note: " << report.pending_intent_absent
                      << " `pending` GC row(s) have no deletion intent, but their body is gone or replaced — the row is "
                         "about to become `standing` or to move, nothing is at risk\n";
        if (report.pending_token_rejected > 0)
            std::cout << "note: " << report.pending_token_rejected
                      << " `pending` GC row(s) carry a token with no canonical identity, so no intent key exists for them\n";
        if (report.standing_present > 0)
            std::cout << "note: " << report.standing_present
                      << " `standing` GC row(s) have their body present again with the same token — GC records it and "
                         "deletes nothing, and the intent is present so a writer publishes fresh; investigate if it persists\n";
        if (report.legacy_marker > 0)
            std::cout << "note: " << report.legacy_marker
                      << " legacy `.meta` marker(s) are debris of the marker protocol — nothing reads them; "
                         "`cas-fsck --fix legacy-markers` deletes them\n";
        if (report.legacy_markers_deleted > 0)
            std::cout << "note: deleted " << report.legacy_markers_deleted << " legacy `.meta` marker(s)\n";
        if (report.legacy_markers_changed > 0)
            std::cout << "note: " << report.legacy_markers_changed
                      << " legacy `.meta` marker(s) changed between their `HEAD` and the delete and were left; run the fix again\n";
        if (report.unaccounted > 0)
            std::cout << "note: " << report.unaccounted
                      << " object(s) are outside the current GC view — normal only as a transient "
                         "(created+dropped between GC rounds); re-run cas-fsck after the next round and "
                         "investigate any that persist\n";
        /// Not a finding: a canonical namespace-life key whose life is absent from the catalog is the
        /// protocol-produced interval between a fenced GC exact-deleting a `Removing` row and the
        /// perpetual namespace janitor reaching it on a later bounded page (its own deletes are
        /// suppressed for the whole of Stage A). Persistent non-convergence is a leak/liveness question
        /// for `CASGCNamespaceCleanupLeaks` and the `namespace_cleanup` GC-log phase, not this scan.
        if (report.namespace_janitor_pending > 0)
            std::cout << "note: " << report.namespace_janitor_pending
                      << " namespace-life object(s) (" << report.namespace_janitor_pending_bytes
                      << " byte(s) across " << report.namespace_janitor_pending_lives
                      << " life/lives) are janitor-pending — their catalog row is already gone, and the "
                         "perpetual namespace janitor is the sole intended reclaimer, but its deletes can "
                         "be deferred (e.g. a destructive-round suppression policy) — not corruption; "
                         "investigate only if the same objects persist across many completed janitor "
                         "cycles (listed as `janitor-pending` rows under --detail)\n";

        if (detail)
        {
            for (const auto & o : report.objects)
            {
                const char * c = "unreachable"; // NOLINT(clang-analyzer-deadcode.DeadStores) - defensive fallback if the enum grows
                switch (o.cls)
                {
                    case Cas::FsckClass::Reachable:   c = "reachable"; break;
                    case Cas::FsckClass::Dangling:    c = "dangling"; break;
                    case Cas::FsckClass::Unreachable: c = "unreachable"; break;
                    case Cas::FsckClass::PendingGc:   c = "pending-gc"; break;
                    case Cas::FsckClass::AwaitingGc:  c = "awaiting-gc"; break;
                    case Cas::FsckClass::Unaccounted: c = "unaccounted"; break;
                    case Cas::FsckClass::StaleEdge:   c = "stale-edge"; break;
                    case Cas::FsckClass::CorruptedRun: c = "corrupted-run"; break;
                    case Cas::FsckClass::ChainBroken: c = "chain-broken"; break;
                    case Cas::FsckClass::Unchecked:   c = "unchecked"; break;
                    case Cas::FsckClass::LifelessKey: c = "lifeless-key"; break;
                    case Cas::FsckClass::JanitorPending: c = "janitor-pending"; break;
                    case Cas::FsckClass::IntentCurrent: c = "intent-current"; break;
                    case Cas::FsckClass::IntentReplaced: c = "intent-replaced"; break;
                    case Cas::FsckClass::IntentBodyGone: c = "intent-body-gone"; break;
                    case Cas::FsckClass::IntentMalformed: c = "intent-malformed"; break;
                    case Cas::FsckClass::IntentUnread: c = "intent-unread"; break;
                    case Cas::FsckClass::PendingWithoutIntent: c = "pending-without-intent"; break;
                    case Cas::FsckClass::PendingIntentAbsent: c = "pending-intent-absent"; break;
                    case Cas::FsckClass::PendingTokenRejected: c = "pending-token-rejected"; break;
                    case Cas::FsckClass::StandingPresent: c = "standing-present"; break;
                    case Cas::FsckClass::LegacyMarker: c = "legacy-marker"; break;
                }
                std::cout << c << "\t" << o.key << "\t" << o.size;
                for (const auto & r : o.reachable_from)
                    std::cout << "\t" << r;
                std::cout << "\n";
            }
        }

        if (report.dangling > 0)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS, "cas-fsck: {} reachable object(s) MISSING (INV-NO-LOSS violation)", report.dangling);
        /// A hole in a ref stream is loss of a different kind: the records above it are unreachable, so
        /// the table's own history is truncated wherever recovery next reads it. Fatal in the summary AND
        /// in the exit code (spec §7) — a verdict only a `--detail` reader would notice is a verdict no
        /// automation acts on.
        if (report.chain_broken > 0)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "cas-fsck: {} namespace(s) have a HOLE in their ref-log stream — an id is absent below a "
                "durable id of the same epoch, which contiguity (INV-1) makes impossible without a lost "
                "record; every transaction above the hole is unreachable (positions are listed as "
                "`chain-broken` rows under --detail)", report.chain_broken);
        /// A term of `clean()`, and until 2026-07-26 the only one that neither printed nor exited
        /// nonzero — so a corrupt run was invisible twice over. A seal-checksum mismatch is not debris:
        /// `fold`/`zeroInDegree`/`previewDeletes` all fail closed on the same run, so GC cannot make
        /// progress past it, and the audit deliberately continues only so ONE pass enumerates them all.
        if (report.corrupted_runs > 0)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "cas-fsck: {} GC source-edge run(s) failed their whole-file seal checksum — the deletion-"
                "deriving consumers fail closed on these, so GC cannot advance past them (run keys are "
                "listed as `corrupted-run` rows under --detail)", report.corrupted_runs);
        /// A key the `Layout` parsers refuse (no current writer can produce it), or a catalog
        /// incarnation that is ambiguous or unreadable, is corruption nothing clears on its own: the
        /// namespace enumeration now reports it instead of aborting, which is what makes an exit code
        /// the only signal automation can act on. A COMPLETE, canonical namespace-life key whose life is
        /// simply absent from the catalog is NOT counted here — see the `janitor-pending` note above.
        if (report.lifeless_keys > 0)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "cas-fsck: {} key(s) under this pool name are malformed or unresolvable — no current "
                "writer could have produced them, or their catalog incarnation is ambiguous/unreadable "
                "(the keys are listed as `lifeless-key` rows under --detail)", report.lifeless_keys);
        /// A key under `blob-intents/` that is no intent key, or an intent with other bytes than the record, is
        /// not something this build writes: GC's confirmation and the reuse read both compare those bytes.
        if (report.intent_malformed > 0)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "cas-fsck: {} object(s) under `blob-intents/` are not deletion intents (the keys are listed as "
                "`intent-malformed` rows under --detail)", report.intent_malformed);
        /// The scan could not answer for an intent or its body: a refused prefix must never read as a clean pool.
        if (report.intent_unread > 0)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "cas-fsck: {} read(s) of a deletion intent or of its body failed, so the intents could not be checked; "
                "first failure: {}",
                report.intent_unread, report.intent_unread_reason);
        /// `pending` is the one GC state a body is deleted from without re-reading the intent.
        if (report.pending_without_intent > 0)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "cas-fsck: {} `pending` GC row(s) have no deletion intent while their body is present: GC would delete "
                "those bodies without warning any writer. Stop GC on this pool and report it (the keys are listed as "
                "`pending-without-intent` rows under --detail)", report.pending_without_intent);
        /// `unreachable` stays a soft finding: it counts every present blob no ref names, and that includes
        /// the GC pipeline's `pending_gc`/`awaiting_gc` and an in-flight build's not-yet-adopted blobs
        /// (`unaccounted`), so a healthy pool under load is never clean by that measure.
    }
};

CommandPtr makeCommandFsck()
{
    return std::make_shared<DB::CommandFsck>();
}

}
