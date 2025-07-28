#pragma once

#include <Storages/MergeTree/BackgroundProcessList.h>
#include <Interpreters/StorageID.h>
#include <Common/Stopwatch.h>
#include <Common/CurrentMetrics.h>
#include <boost/noncopyable.hpp>

namespace CurrentMetrics
{
extern const Metric Export;
}

namespace DB
{

struct ExportInfo
{
    std::string source_database;
    std::string destination_database;
    std::string source_table;
    std::string destination_table;
    std::string part_name;
    std::string destination_path;

    Float64 elapsed;
    UInt64 thread_id;
};

struct ExportsListElement : private boost::noncopyable
{
    const StorageID source_table_id;
    const StorageID destination_table_id;
    const std::string part_name;
    const std::string destination_path;

    Stopwatch watch;
    const UInt64 thread_id;

    ExportsListElement(
        const StorageID & source_table_id_,
        const StorageID & destination_table_id_,
        const std::string & part_name_,
        const std::string & destination_path_);

    ExportInfo getInfo() const;
};


/// List of currently processing moves
class ExportsList final : public BackgroundProcessList<ExportsListElement, ExportInfo>
{
private:
    using Parent = BackgroundProcessList<ExportsListElement, ExportInfo>;

public:
    ExportsList()
        : Parent(CurrentMetrics::Export)
    {}
};

}
