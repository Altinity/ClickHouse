#pragma once
#include <base/types.h>
#include <functional>
#include <map>

namespace DB::ContentAddressed
{

/// The decoupled, pure-data record the Disks-layer GC scheduler emits per round. It carries NO
/// Interpreters dependency: the metadata storage converts it into a
/// ContentAddressedGarbageCollectionLogElement and forwards it to the SystemLog. Keeping it a plain
/// POD lets the scheduler (and its unit tests) stay free of the system-log machinery.
struct GcRoundLogRecord
{
    enum class EventType { Start, Finish };
    enum class Outcome { Unknown, Success, NotALeader, Failed };
    enum class Trigger { Scheduled, Manual };

    EventType event_type = EventType::Start;
    Outcome outcome = Outcome::Unknown;   /// Unknown until a round finishes
    Trigger trigger = Trigger::Scheduled;
    String disk_name;
    String gc_id;        /// hex of the scheduler's gc_id
    UInt64 round = 0;
    UInt64 candidates_marked = 0;
    UInt64 objects_deleted = 0;
    UInt64 objects_absent = 0;
    UInt64 objects_replaced = 0;
    UInt64 objects_spared = 0;
    UInt64 manifests_deleted = 0;   /// owner-removed manifest bodies deleted (B11 — distinct from blob deletes)
    /// Ack-floor pipeline transitions (RoundReport pass-through, 2026-07-02 copy-forward Task 3).
    UInt64 entries_condemned = 0;   /// entries newly condemned into the retired list this round
    UInt64 entries_graduated = 0;   /// entries newly floor-passed (published delete_pending) this round
    UInt64 entries_redeleted = 0;   /// pending exact-token blob deletes executed this round
    UInt64 fence_outs = 0;          /// expired mounts fenced-out by the round's heartbeat floor
    UInt64 min_ack = 0;             /// heartbeat ack floor latched at round start (UINT64_MAX = empty floor set)
    UInt64 anomalies = 0;           /// fold clamps surfaced (never wedging) this round
    UInt64 duration_ms = 0;
    String error;
    std::map<String, UInt64> profile_events;
};

using GcRoundLogger = std::function<void(const GcRoundLogRecord &)>;

}
