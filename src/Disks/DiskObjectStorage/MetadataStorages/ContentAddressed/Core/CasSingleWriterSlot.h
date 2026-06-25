#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Common/Logger.h>
#include <Common/ThreadPool.h>
#include <base/types.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace DB::Cas
{

/// Durable single-writer-slot state machine used by the per-server build watermark
/// (`CasWatermark.h`, `WatermarkKeeper`): anchors a key that has EXACTLY ONE writer, renews it
/// asynchronously off the write path, and ends with a terminal op; any precondition failure during
/// renewal means a foreign touch — fail closed with an exception, never re-mint.
///
/// This base owns the common machinery (the `seq`/`last_token`/`dead` state, the renewal thread and
/// its loop, `renewOnce`/`startBackground`/`stopBackground`/`lastRenewTime`, and the start/terminal
/// bookkeeping). The noun ("watermark") and verb ("farewell") used in every fail-closed message are
/// passed to the base constructor. Subclasses differ ONLY in EXPLICIT policy hooks:
///   - `prepareRenew`     — runs OFF the state lock before each body encode, returning a per-call
///                          payload (the watermark reads `min_active` from a Store callback here).
///                          Keeping it off `state_mutex` is load-bearing: the watermark callback
///                          reaches into the Store's own lock.
///   - `encodeBody`       — builds the slot's exact JSON bytes for a given `seq` and payload;
///   - `claim`            — the slot-specific anchor sequence in `start` (the watermark's
///                          head→putIfAbsent/putOverwrite dance), returning the token we now hold;
///   - `terminate`        — the terminal op (the watermark's retiring putOverwrite), run under the
///                          state lock after `dead` is set, owning its own fail-closed throws and
///                          final bookkeeping.
///
/// Every observable op (the JSON body bytes, the anchor put sequence, the renew cadence, the
/// foreign-touch fail-close conditions and message wording, the stop semantics) is reproduced
/// exactly by the subclass hooks, with no conditionals in the base that would blur the
/// single-writer/fail-closed contract.
class SingleWriterSlot
{
public:
    /// `slot_name_` is the noun in every message ("watermark"); `terminal_verb_` is the
    /// verb used in the start/renew/terminal guards ("farewell").
    SingleWriterSlot(
        BackendPtr backend_, String key_, std::string_view slot_name_, std::string_view terminal_verb_,
        std::string_view logger_name_);

    /// Stops the background thread only (no terminal op). Destruction without a terminal op is the
    /// crash path: the slot persists, its seq stops advancing, and full GC observes the frozen seq.
    virtual ~SingleWriterSlot();

    /// seq++ via putOverwrite against the last token we wrote; LOGICAL_ERROR on a foreign touch
    /// (single-writer fail-closed contract).
    void renewOnce();

    void startBackground(std::chrono::milliseconds period);
    void stopBackground();   /// idempotent

    /// Local-clock bound for diagnostics: when the background loop stops (renewal failure), this
    /// stops advancing.
    std::chrono::steady_clock::time_point lastRenewTime() const;

protected:
    /// Per-call payload prepared OFF the state lock and handed to `encodeBody`. Subclasses needing a
    /// dynamic value (the watermark's `min_active`) carry it through this opaque token.
    struct RenewPayload
    {
        uint64_t value = 0;
    };

    using Token = ::DB::Cas::Token;

    /// === policy hooks (see class comment) ===
    virtual RenewPayload prepareRenew() const = 0;
    virtual String encodeBody(uint64_t seq, const RenewPayload & payload) const = 0;
    virtual Token claim(const String & body) = 0;

    /// Runs the slot's terminal op against the held `last_token`. Called under `state_mutex` with
    /// `dead` already set (so renewal can never race it). Owns its own fail-closed throws and final
    /// bookkeeping (the watermark bumps seq/last_token/last_renew_time on its retiring putOverwrite).
    /// May throw — `dead` stays set regardless.
    virtual void terminate() = 0;

    /// Anchors the slot for seq=1 — durable when `doStart` returns. Subclasses expose this under their
    /// own public name (`start`). Computes the payload off the lock, then runs the policy `claim`.
    void doStart();

    /// Stops the background thread, takes the state lock, runs the dead/seq guards (e.g.
    /// "double farewell" / "discard before start"), sets `dead`, and delegates the op to `terminate`.
    /// Subclasses expose this under their own public name (`farewell`/`discard`).
    void doTerminate();

    /// Bookkeeping after a successful write of the given seq/token: records seq, the token we now
    /// hold, and the local-clock renew time. Must be called under `state_mutex`.
    void recordWrite(uint64_t new_seq, const Token & token);

    BackendPtr backend;
    String key;

    mutable std::mutex state_mutex;
    uint64_t seq = 0;            /// 0 = not started
    Token last_token;            /// the incarnation WE wrote — the only one we ever renew
    bool dead = false;           /// set by the terminal op

private:
    void backgroundLoop(std::chrono::milliseconds period);

    std::string_view slot_name;
    std::string_view terminal_verb;

    std::chrono::steady_clock::time_point last_renew_time;

    std::mutex background_mutex;
    std::condition_variable wakeup;
    bool stop_requested = false;
    ThreadFromGlobalPool thread;

    LoggerPtr log;
};

}
