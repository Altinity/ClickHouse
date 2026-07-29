#include <Disks/DiskObjectStorage/DiskObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Common/Exception.h>
#include <ICommand.h>

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
        command_name = "ca-fsck";
        description = "Independently verify content-addressed pool reachability (read-only). "
                      "Exits nonzero if any reachable object is missing (dangling). "
                      "(`fsck` is a deprecated alias for this command.)";
        options_description.add_options()("detail", "list per-object rows (class, key, size, reachable_from)")(
            "timeout", po::value<UInt64>(), "abort the scan after N seconds with a clear error instead of hanging (default 600; 0 = unbounded)")(
            "namespace", po::value<String>(), "scope the scan to namespaces with this prefix (skips the pool-wide "
                                               "physical/pipeline classification; still reports the scoped namespaces' "
                                               "dangling refs and orphan-manifest debris as unreachable)")(
            "partial", "on --timeout, print the counts accumulated so far flagged partial=1 instead of aborting empty-handed");
    }

    void executeImpl(const CommandLineOptions & options, DisksClient & client) override
    {
        const bool detail = options.contains("detail");
        const UInt64 timeout_sec = getValueFromCommandLineOptionsWithDefault<UInt64>(options, "timeout", 600);
        const String namespace_prefix = options.contains("namespace") ? options["namespace"].as<String>() : "";
        const bool partial = options.contains("partial");
        auto disk = client.getCurrentDiskWithPath().getDisk();

        auto * dos = dynamic_cast<DiskObjectStorage *>(disk.get());
        if (!dos)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "ca-fsck: '{}' is not an object-storage disk", disk->getName());

        auto * ca = dynamic_cast<ContentAddressedMetadataStorage *>(dos->getMetadataStorage().get());
        if (!ca)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "ca-fsck: disk '{}' is not content-addressed", disk->getName());

        if (!ca->isReadOnly())
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "ca-fsck: open the CA disk read-only (<readonly>true</readonly>) so inspection never probes/schedules a live pool");

        /// Progress to stderr so a long scan is visibly working (the reachable=… summary stays on
        /// stdout, machine-parseable). The deadline bounds a slow-but-progressing scan with a clear
        /// error; for a single LIST page stuck in S3-client retries, lower the disk's S3 retry budget.
        Cas::FsckProgress on_progress = [](std::string_view phase, uint64_t objects, uint64_t pages)
        {
            std::cerr << "ca-fsck: " << phase << " — " << objects << " objects, " << pages << " pages\n";
        };
        std::optional<std::chrono::steady_clock::time_point> deadline;
        if (timeout_sec > 0)
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);

        const Cas::FsckReport report = Cas::runFsck(*ca->store(), detail, on_progress, deadline, partial, namespace_prefix);

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
                         "(condemn -> graduate -> exact-token delete takes ~2-3 rounds) — expected, no action needed\n";
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
        if (report.unaccounted > 0)
            std::cout << "note: " << report.unaccounted
                      << " object(s) are outside the current GC view — normal only as a transient "
                         "(created+dropped between GC rounds); re-run ca-fsck after the next round and "
                         "investigate any that persist\n";

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
                    case Cas::FsckClass::SnapshotOracleMismatch: c = "snapshot-oracle-mismatch"; break;
                    case Cas::FsckClass::CorruptedRun: c = "corrupted-run"; break;
                    case Cas::FsckClass::ChainBroken: c = "chain-broken"; break;
                    case Cas::FsckClass::Unchecked:   c = "unchecked"; break;
                }
                std::cout << c << "\t" << o.key << "\t" << o.size;
                for (const auto & r : o.reachable_from)
                    std::cout << "\t" << r;
                std::cout << "\n";
            }
        }

        if (report.dangling > 0)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS, "ca-fsck: {} reachable object(s) MISSING (INV-NO-LOSS violation)", report.dangling);
        /// A hole in a ref stream is loss of a different kind: the records above it are unreachable, so
        /// the table's own history is truncated wherever recovery next reads it. Fatal in the summary AND
        /// in the exit code (spec §7) — a verdict only a `--detail` reader would notice is a verdict no
        /// automation acts on.
        if (report.chain_broken > 0)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "ca-fsck: {} namespace(s) have a HOLE in their ref-log stream — an id is absent below a "
                "durable id of the same epoch, which contiguity (INV-1) makes impossible without a lost "
                "record; every transaction above the hole is unreachable (positions are listed as "
                "`chain-broken` rows under --detail)", report.chain_broken);
        if (report.snapshot_oracle_mismatches > 0)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "ca-fsck: {} table snapshot(s) diverge from an independent replay of their logs "
                "(cache/codec corruption, spec §Snapshot Publication oracle)", report.snapshot_oracle_mismatches);
        /// The third term of `clean()`, and until 2026-07-26 the only one that neither printed nor exited
        /// nonzero — so a corrupt run was invisible twice over. A seal-checksum mismatch is not debris:
        /// `fold`/`zeroInDegree`/`previewDeletes` all fail closed on the same run, so GC cannot make
        /// progress past it, and the audit deliberately continues only so ONE pass enumerates them all.
        if (report.corrupted_runs > 0)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "ca-fsck: {} GC source-edge run(s) failed their whole-file seal checksum — the deletion-"
                "deriving consumers fail closed on these, so GC cannot advance past them (run keys are "
                "listed as `corrupted-run` rows under --detail)", report.corrupted_runs);
    }
};

CommandPtr makeCommandFsck()
{
    return std::make_shared<DB::CommandFsck>();
}

/// Deprecated alias: `fsck` was the original name of `ca-fsck`. Kept working so existing scripts
/// don't break, but prints a one-line deprecation note on every invocation. The framework doesn't
/// expose the invoked argv-name to executeImpl, so this wraps a real `ca-fsck` command instance
/// instead of teaching it to guess its own alias.
class CommandFsckDeprecated final : public ICommand
{
public:
    CommandFsckDeprecated() : ICommand("CommandFsckDeprecated"), inner(std::make_shared<CommandFsck>())
    {
        command_name = "fsck";
        description = "Deprecated alias for `ca-fsck`; use `ca-fsck` instead. " + inner->description;
        options_description.add(inner->options_description);
    }

    void executeImpl(const CommandLineOptions & options, DisksClient & client) override
    {
        std::cerr << "fsck: this command has been renamed to `ca-fsck`; `fsck` is a deprecated alias and may be removed in the future\n";
        inner->executeImpl(options, client);
    }

private:
    std::shared_ptr<CommandFsck> inner;
};

CommandPtr makeCommandFsckDeprecated()
{
    return std::make_shared<DB::CommandFsckDeprecated>();
}

}
