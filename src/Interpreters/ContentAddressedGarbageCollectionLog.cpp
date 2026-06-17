#include <Interpreters/ContentAddressedGarbageCollectionLog.h>
#include <base/getFQDNOrHostName.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeEnum.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Common/DateLUTImpl.h>

namespace DB
{

ColumnsDescription ContentAddressedGarbageCollectionLogElement::getColumnsDescription()
{
    auto type_enum = std::make_shared<DataTypeEnum8>(DataTypeEnum8::Values{
        {"Start", static_cast<Int8>(START)}, {"Finish", static_cast<Int8>(FINISH)}});
    auto outcome_enum = std::make_shared<DataTypeEnum8>(DataTypeEnum8::Values{
        {"Unknown", static_cast<Int8>(UNKNOWN)}, {"Success", static_cast<Int8>(SUCCESS)},
        {"NotALeader", static_cast<Int8>(NOT_A_LEADER)}, {"Error", static_cast<Int8>(FAILED)}});
    auto trigger_enum = std::make_shared<DataTypeEnum8>(DataTypeEnum8::Values{
        {"Scheduled", static_cast<Int8>(SCHEDULED)}, {"Manual", static_cast<Int8>(MANUAL)}});
    auto lc_string = std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>());

    return ColumnsDescription
    {
        {"hostname", lc_string, "Host name of the server executing the round."},
        {"event_date", std::make_shared<DataTypeDate>(), "Event date."},
        {"event_time", std::make_shared<DataTypeDateTime>(), "Event time."},
        {"event_time_microseconds", std::make_shared<DataTypeDateTime64>(6), "Event time with microseconds."},
        {"event_type", type_enum, "Start or Finish of a GC round."},
        {"disk_name", lc_string, "Content-addressed disk the round ran on."},
        {"gc_id", std::make_shared<DataTypeString>(), "GC scheduler instance id (which mounter)."},
        {"trigger", trigger_enum, "Scheduled (background tick) or Manual (SYSTEM command)."},
        {"round", std::make_shared<DataTypeUInt64>(), "GC round number (0 on Start)."},
        {"outcome", outcome_enum, "Unknown (Start) / Success (led and completed) / NotALeader (another replica holds the GC lease) / Error (the round threw)."},
        {"candidates_marked", std::make_shared<DataTypeUInt64>(), "Objects retired (marked) this round."},
        {"objects_deleted", std::make_shared<DataTypeUInt64>(), "Objects physically deleted this round."},
        {"objects_absent", std::make_shared<DataTypeUInt64>(), "Retire candidates found already absent."},
        {"objects_replaced", std::make_shared<DataTypeUInt64>(), "412-saves (a resurrection won the race)."},
        {"objects_spared", std::make_shared<DataTypeUInt64>(), "Candidates spared (in-degree > 0 at recheck)."},
        {"children_cascaded", std::make_shared<DataTypeUInt64>(), "Child edges freed by the cascade."},
        {"duration_ms", std::make_shared<DataTypeUInt64>(), "Round wall-clock duration (Finish)."},
        {"error", std::make_shared<DataTypeString>(), "Exception text when outcome = Error."},
        {"ProfileEvents", std::make_shared<DataTypeMap>(lc_string, std::make_shared<DataTypeUInt64>()),
            "Per-round ProfileEvents delta (the Cas* counters and S3 events for this round)."},
    };
}

void ContentAddressedGarbageCollectionLogElement::appendToBlock(MutableColumns & columns) const
{
    size_t i = 0;
    columns[i++]->insert(getFQDNOrHostName());
    columns[i++]->insert(DateLUT::instance().toDayNum(event_time).toUnderType());
    columns[i++]->insert(event_time);
    columns[i++]->insert(event_time_microseconds);
    columns[i++]->insert(static_cast<Int8>(event_type));
    columns[i++]->insert(disk_name);
    columns[i++]->insert(gc_id);
    columns[i++]->insert(static_cast<Int8>(trigger));
    columns[i++]->insert(round);
    columns[i++]->insert(static_cast<Int8>(outcome));
    columns[i++]->insert(candidates_marked);
    columns[i++]->insert(objects_deleted);
    columns[i++]->insert(objects_absent);
    columns[i++]->insert(objects_replaced);
    columns[i++]->insert(objects_spared);
    columns[i++]->insert(children_cascaded);
    columns[i++]->insert(duration_ms);
    columns[i++]->insert(error);
    {
        Map map;
        map.reserve(profile_events.size());
        for (const auto & [k, v] : profile_events)
            map.push_back(Tuple{k, v});
        columns[i++]->insert(map);
    }
}

}
