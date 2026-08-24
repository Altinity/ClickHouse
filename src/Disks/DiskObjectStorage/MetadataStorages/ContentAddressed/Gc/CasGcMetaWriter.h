#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Common/ThreadPool.h>
#include <Common/logger_useful.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>

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

    /// Publish durable Condemned evidence for one (blob, exact incarnation-token) pair. On success the
    /// pair is recorded in the in-process confirmation registry, which the graduation gate reads. A
    /// lost CAS or a thrown error leaves the pair UNCONFIRMED: the gate then carries the entry and a
    /// later round retries the write.
    void scheduleCondemnMarkerWrite(const BlobRef & ref, const Token & token,
                                    uint64_t condemn_round, uint64_t size);

    /// Drop the freshness meta of a blob whose body is confirmed deleted or absent.
    void scheduleConfirmedMetaDelete(const BlobRef & ref);

    /// Wait for every job scheduled so far. Never throws: each job already caught its own exception.
    void drain();

    uint64_t scheduled() const;
    uint64_t completed() const;

    /// The in-process condemn-marker confirmation registry, keyed (blob, exact token value). Pool
    /// completions insert concurrently with the round thread's reads, and the round thread also
    /// inserts directly when it re-checks a marker synchronously.
    void noteCondemnMarkerDurable(const BlobRef & ref, const Token & token);
    bool condemnMarkerConfirmedInProcess(const BlobRef & ref, const Token & token);
    void forgetCondemnMarker(const BlobRef & ref, const Token & token);

private:
    /// Everything a job reaches. Held by `shared_ptr` and captured by value into every job.
    struct State
    {
        PoolPtr store;
        LoggerPtr logger;
        std::atomic<uint64_t> scheduled{0};
        std::atomic<uint64_t> completed{0};
        std::mutex condemn_marker_mutex;
        std::set<std::pair<BlobRef, String>> condemn_markers_confirmed;

        void noteCondemnMarkerDurable(const BlobRef & ref, const Token & token);
        bool condemnMarkerConfirmedInProcess(const BlobRef & ref, const Token & token);
        void forgetCondemnMarker(const BlobRef & ref, const Token & token);
    };

    /// Wrap one meta op so it can never throw, count it, and put it on the pool -- running it inline
    /// if scheduling itself fails, rather than silently losing the write. Private, and takes only
    /// what this class produces: the typed operations above are the sole callers.
    void submit(std::function<void()> op);

    std::shared_ptr<State> state;
    ThreadPool pool;
};

}
