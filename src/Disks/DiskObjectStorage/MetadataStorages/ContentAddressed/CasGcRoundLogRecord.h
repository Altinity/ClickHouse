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
    UInt64 children_cascaded = 0;
    UInt64 forgotten_on_delete = 0;
    UInt64 forgotten_absent = 0;
    UInt64 duration_ms = 0;
    String error;
    std::map<String, UInt64> profile_events;
};

using GcRoundLogger = std::function<void(const GcRoundLogRecord &)>;

}
