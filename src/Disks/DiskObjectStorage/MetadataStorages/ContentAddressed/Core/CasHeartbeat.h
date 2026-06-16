#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Common/Logger.h>
#include <Common/ThreadPool.h>
#include <base/extended_types.h>
#include <base/types.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace DB::Cas
{

/// Build heartbeat object (protocol spec §4): one object per in-flight build under
/// builds/<build_id>, keyed by build_id ALONE — a random u128, globally unique by construction.
/// Heartbeats gate only DEBRIS reclamation by full GC (M-F): a wedged heartbeat delays cleanup,
/// never correctness — the publish GATE, not the heartbeat, is the safety mechanism (spec §5).
/// Incremental GC additionally consults a per-server build watermark (`CasWatermark.h`,
/// `servers/<server_id>`) to spare in-flight builds' blobs (co-liveness); the publish gate
/// remains the safety mechanism (B167, spec 2026-06-16-ca-build-watermark-design).
///
/// Non-hashed metadata object => STRICT JSON ("cas_heartbeat" v1, spec §4 encoding split,
/// decision 2026-06-11):
///   {"format":"cas_heartbeat","version":1,"server_id":"<32 lowercase hex>",
///    "heartbeat_seq":3,"created_at_ms":1765459200000}
/// Fail-closed decode (wrong format / unknown key / missing key / wrong type / bad hash hex /
/// malformed document => CORRUPTED_DATA; future version => NOT_IMPLEMENTED).
struct Heartbeat
{
    UInt128 server_id{};
    uint64_t heartbeat_seq = 0;   /// strictly monotone within one build
    uint64_t created_at_ms = 0;   /// DIAGNOSTIC ONLY (spec §3.1) — no protocol decision reads writer clocks
};

String encodeHeartbeat(const Heartbeat & heartbeat);
Heartbeat decodeHeartbeat(std::string_view data);

/// W-HEARTBEAT (spec §5): the build heartbeat is durable BEFORE the first object PUT and renewed
/// in background. Renewal is putOverwrite against our own last token: this key has exactly one
/// writer (build_id is a random u128), so any precondition failure means a foreign touch — the
/// world is broken. Fail closed with an exception; never re-mint the key.
class HeartbeatKeeper
{
public:
    HeartbeatKeeper(BackendPtr backend_, const Layout & layout_, UInt128 build_id_, UInt128 server_id_);

    /// Stops the background thread; deliberately does NOT discard the key. Destruction without
    /// discard is the crash path: the heartbeat persists, stops advancing, and full GC applies
    /// the debris rules (spec §5).
    ~HeartbeatKeeper();

    /// putIfAbsent with seq=1 — the heartbeat is durable when start returns (W-HEARTBEAT).
    /// The key already existing means the globally-unique build id collided: LOGICAL_ERROR.
    void start();

    /// seq++ via putOverwrite against the last token we minted; LOGICAL_ERROR on a foreign touch.
    void renewOnce();

    /// deleteExact with the last token — used by Build::abandon. Stops the background thread
    /// first. The keeper is dead afterwards: renew/discard then are LOGICAL_ERROR.
    void discard();

    void startBackground(std::chrono::milliseconds period);
    void stopBackground();   /// idempotent

    /// Local-clock bound for the publish gate's sanity check: when the background loop stops
    /// (renewal failure), this stops advancing and the gate observes it.
    std::chrono::steady_clock::time_point lastRenewTime() const;

private:
    void backgroundLoop(std::chrono::milliseconds period);

    BackendPtr backend;
    String key;
    UInt128 server_id;

    mutable std::mutex state_mutex;
    uint64_t seq = 0;            /// 0 = not started
    uint64_t created_at_ms = 0;  /// minted at start, reused in renewals (records build creation time)
    Token last_token;            /// the incarnation WE wrote — the only one we ever renew or delete
    bool dead = false;           /// set by discard
    std::chrono::steady_clock::time_point last_renew_time;

    std::mutex background_mutex;
    std::condition_variable wakeup;
    bool stop_requested = false;
    ThreadFromGlobalPool thread;

    LoggerPtr log;
};

}
