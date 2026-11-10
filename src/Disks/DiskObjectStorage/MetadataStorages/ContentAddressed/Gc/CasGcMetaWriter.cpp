#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobMeta.h>

#include <Common/ProfileEvents.h>

#include <algorithm>
#include <variant>

namespace ProfileEvents
{
    extern const Event CASGCMetaOps;
    extern const Event CASGCMetaWriteAnomaly;
}

namespace CurrentMetrics
{
    extern const Metric LocalThread;
    extern const Metric LocalThreadActive;
    extern const Metric LocalThreadScheduled;
}

namespace DB::Cas
{

namespace
{

/// The per-hash marker operations GC schedules on the bounded pool. The marker is the writer's adopt gate:
/// a writer that reads `Condemned` republishes instead of adopting the incarnation GC will delete. The ledger
/// and the exact-token body delete stay the safety core; round rules keep a marker from standing for the
/// wrong incarnation:
///   - every GC write stamps the round of the attempt that issues it, and replaces an absent marker, `Clean`
///     or an older-round `Condemned`;
///   - graduation accepts only `Condemned` at the entry's round or newer, read in that round (`Gc::fold`);
///   - a delete job removes only `Condemned` at its entry's round or older, `If-Match` the etag it read.
/// While `gc/state` is readable, a later incarnation that can own a job is condemned at a larger round, so no job
/// removes its marker; a rebuild that regresses rounds breaks this.
///
/// GC never writes `Clean`, not even on a spare: a deposed leader that cleared a spare's marker and then lost
/// its round CAS would leave `Clean` over a still-condemned body, and a stale exact-token redelete would delete
/// what a writer adopted. Only a writer that displaced the body writes `Clean` (`PartWriteTxn::ensureBlobPresent`).
void writeCondemnedMeta(CasOperation & op, const Layout & layout, const BlobRef & ref,
                        uint64_t condemn_round, uint64_t size)
{
    const auto lm = loadMeta(op, layout, ref);
    const BlobMeta desired{.state = MetaState::Condemned, .condemn_round = condemn_round, .size = size};
    /// An older round may be another incarnation's marker, which that incarnation's delete job may remove.
    if (!lm)
        putMetaIfAbsent(op, layout, ref, desired);
    else if (lm->meta.state != MetaState::Condemned || lm->meta.condemn_round < condemn_round)
        casMeta(op, layout, ref, lm->etag, desired);
}

/// Rounds only grow while `gc/state` is readable, so a marker at a newer round guards a later incarnation
/// and `Clean` means a writer displaced the body: both are left alone. `If-Match` the etag read here, so a
/// marker changed since the read is left alone too.
void deleteConfirmedMeta(CasOperation & op, const Layout & layout, const BlobRef & ref, uint64_t condemn_round)
{
    const auto lm = loadMeta(op, layout, ref);
    if (!lm || lm->meta.state != MetaState::Condemned || lm->meta.condemn_round > condemn_round)
        return;
    deleteMetaExact(op, layout, ref, lm->etag);
}

}

GcMetaWriter::GcMetaWriter(PoolPtr store_, LoggerPtr logger_, size_t pool_size)
    : state(std::make_shared<State>())
    , pool(CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive,
           CurrentMetrics::LocalThreadScheduled, std::max<size_t>(1, pool_size))
{
    state->store = std::move(store_);
    state->logger = std::move(logger_);
}

void GcMetaWriter::submit(std::function<void()> op)
{
    /// `run` is safe to invoke either on the pool or inline (the scheduling-failure path below). A
    /// per-hash meta-operation exception is caught because the ledger and the exact-token body
    /// delete are the actual safety core. Pool/framework failures, including a failure while
    /// reporting an operation exception, remain visible to the throwing protocol barrier. The job
    /// captures only `state`, so it stays well-defined however long it outlives the writer that
    /// scheduled it.
    auto run = [op, st = state]()
    {
        ProfileEvents::increment(ProfileEvents::CASGCMetaOps);
        try
        {
            op();
        }
        catch (...)
        {
            ProfileEvents::increment(ProfileEvents::CASGCMetaWriteAnomaly);
            tryLogCurrentException(st->logger,
                "CAS gc: a per-hash freshness-meta op failed on the bounded pool (advisory-only; "
                "never wedges the round)");
        }
        /// A job that threw still FINISHED: this counter reports drain progress, not success.
        st->completed.fetch_add(1, std::memory_order_relaxed);
    };
    state->scheduled.fetch_add(1, std::memory_order_relaxed);
    try
    {
        pool.scheduleOrThrowOnError(run);
    }
    catch (...)
    {
        /// Scheduling itself failed (e.g. resource exhaustion under a mass-DROP burst) -- run inline
        /// rather than silently lose the meta write. The operation exception remains contained;
        /// infrastructure or diagnostic failures still propagate.
        ProfileEvents::increment(ProfileEvents::CASGCMetaWriteAnomaly);
        tryLogCurrentException(state->logger,
            "CAS gc: meta pool scheduling failed; running the op inline on the round's own thread");
        run();
    }
}

void GcMetaWriter::scheduleCondemnMarkerWrite(const BlobRef & ref, uint64_t condemn_round, uint64_t size)
{
    /// The job admits its OWN operation: a `CasOperation` carries per-call state and belongs to one
    /// task, while several of these run concurrently on the pool.
    submit([st = state, ref, condemn_round, size]()
    {
        CasOperation op = st->store->openRequests().admit();
        writeCondemnedMeta(op, st->store->layout(), ref, condemn_round, size);
    });
}

void GcMetaWriter::scheduleConfirmedMetaDelete(const BlobRef & ref, uint64_t condemn_round)
{
    submit([st = state, ref, condemn_round]()
    {
        CasOperation op = st->store->openRequests().admit();
        deleteConfirmedMeta(op, st->store->layout(), ref, condemn_round);
    });
}

void GcMetaWriter::drain()
{
    pool.wait();
}

void GcMetaWriter::drainOnExitNoThrow() noexcept
{
    try
    {
        pool.wait();
    }
    catch (...)
    {
        try
        {
            tryLogCurrentException(state->logger,
                "CAS gc: meta pool drain failed during round-exit cleanup");
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            /// Cleanup is `noexcept`; diagnostic logging must not replace the round's exception.
        }
    }
}

uint64_t GcMetaWriter::scheduled() const
{
    return state->scheduled.load(std::memory_order_relaxed);
}

uint64_t GcMetaWriter::completed() const
{
    return state->completed.load(std::memory_order_relaxed);
}

}
