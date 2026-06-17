#pragma once
#include <Interpreters/SystemLog.h>
#include <Core/NamesAndTypes.h>
#include <Core/NamesAndAliases.h>
#include <Storages/ColumnsDescription.h>

namespace DB
{

struct ContentAddressedGarbageCollectionLogElement
{
    enum EventType : int8_t { START = 1, FINISH = 2 };
    enum Outcome   : int8_t { UNKNOWN = 1, SUCCESS = 2, NOT_A_LEADER = 3, FAILED = 4 };
    enum Trigger   : int8_t { SCHEDULED = 1, MANUAL = 2 };

    time_t event_time = 0;
    Decimal64 event_time_microseconds = 0;

    EventType event_type = START;
    String disk_name;
    String gc_id;
    Trigger trigger = SCHEDULED;

    UInt64 round = 0;
    Outcome outcome = UNKNOWN;      /// UNKNOWN on START; set to SUCCESS/NOT_A_LEADER/FAILED on FINISH
    UInt64 candidates_marked = 0;
    UInt64 objects_deleted = 0;
    UInt64 objects_absent = 0;
    UInt64 objects_replaced = 0;
    UInt64 objects_spared = 0;
    UInt64 children_cascaded = 0;
    UInt64 duration_ms = 0;
    String error;
    std::map<String, UInt64> profile_events;   /// per-round delta (FINISH)

    static std::string name() { return "ContentAddressedGarbageCollectionLog"; }
    static ColumnsDescription getColumnsDescription();
    static NamesAndAliases getNamesAndAliases() { return {}; }
    void appendToBlock(MutableColumns & columns) const;
};

class ContentAddressedGarbageCollectionLog : public SystemLog<ContentAddressedGarbageCollectionLogElement>
{
    using SystemLog<ContentAddressedGarbageCollectionLogElement>::SystemLog;
};

}
