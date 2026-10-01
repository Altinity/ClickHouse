#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Common/ThreadPool.h>
#include <Common/logger_useful.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace DB::Cas
{

/// Owns the bounded pool for a GC round's per-hash freshness-meta writes (condemn / spare / delete)
/// AND everything those writes touch.
///
/// There is deliberately NO way to hand this class a closure. The only paths onto the pool are the
/// two typed operations below, and each captures nothing but a `shared_ptr` to `State`. A job
/// therefore cannot reach anything owned by the enclosing `Gc`, which is what keeps a job that
/// outlives its owner well-defined instead of dependent on member-declaration order.
class GcMetaWriter
{
public:
    GcMetaWriter(PoolPtr store_, LoggerPtr logger_, size_t pool_size);

    GcMetaWriter(const GcMetaWriter &) = delete;
    GcMetaWriter & operator=(const GcMetaWriter &) = delete;

    /// Write `Condemned` at `condemn_round` for one blob: create it over an absent marker, replace `Clean`
    /// or an older-round `Condemned`, leave one at `condemn_round` or newer. A thrown write is counted and
    /// logged, a write that lost its condition is dropped silently; either way the graduation gate reads the
    /// marker itself and schedules the write again.
    void scheduleCondemnMarkerWrite(const BlobRef & ref, uint64_t condemn_round, uint64_t size);

    /// Drop the marker of a blob whose body is confirmed deleted or absent, but only while it is `Condemned`
    /// at `condemn_round` or older: a newer marker, or `Clean`, belongs to a later incarnation.
    void scheduleConfirmedMetaDelete(const BlobRef & ref, uint64_t condemn_round);

    /// Successful-path protocol barrier. Wait for every job scheduled so far, propagating any
    /// pool/framework exception recorded by `ThreadPool`; per-hash operation exceptions are caught
    /// by the job wrapper.
    void drain();

    /// Round-exit cleanup. Wait for the same pool, but never replace an exception already unwinding
    /// from the round. A cleanup failure is reported best-effort and cannot escape this method.
    void drainOnExitNoThrow() noexcept;

    uint64_t scheduled() const;
    uint64_t completed() const;

private:
    /// Everything a job reaches. Held by `shared_ptr` and captured by value into every job.
    struct State
    {
        PoolPtr store;
        LoggerPtr logger;
        std::atomic<uint64_t> scheduled{0};
        std::atomic<uint64_t> completed{0};
    };

    /// Catch each meta-operation exception, count the job, and put it on the pool -- running it
    /// inline if scheduling itself fails, rather than silently losing the write. A pool/framework
    /// failure may still be recorded and rethrown by the successful-path `drain`. Private, and takes
    /// only what this class produces: the typed operations above are the sole callers.
    void submit(std::function<void()> op);

    std::shared_ptr<State> state;
    ThreadPool pool;
};

}
