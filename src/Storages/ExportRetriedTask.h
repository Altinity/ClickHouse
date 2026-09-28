#pragma once

#include <base/types.h>
#include <Common/Exception.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>

#include <ctime>
#include <utility>
#include <vector>

namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
}

/// A failed task of the `EXPORT` TTL whose parts a later task exports again, and whose commit may
/// still land at the destination, e.g. a request that the destination applies late. The later task
/// commits only the parts that no landed task exported, so it records the blocks of each of them.
struct ExportRetriedTask
{
    String transaction_id;
    /// `[min_block, max_block]` of its parts, in the partition of the later task.
    std::vector<std::pair<Int64, Int64>> block_ranges;
    /// When the scheduler found it failed.
    time_t failed_time = 0;

    bool operator==(const ExportRetriedTask &) const = default;
};

using ExportRetriedTasks = std::vector<ExportRetriedTask>;

namespace ExportRetriedTaskUtils
{

inline std::vector<String> transactionIds(const ExportRetriedTasks & tasks)
{
    std::vector<String> result;
    result.reserve(tasks.size());
    for (const auto & task : tasks)
        result.push_back(task.transaction_id);
    return result;
}

inline Poco::JSON::Array::Ptr toJSON(const ExportRetriedTasks & tasks)
{
    Poco::JSON::Array::Ptr array = new Poco::JSON::Array();
    for (const auto & task : tasks)
    {
        Poco::JSON::Array::Ptr ranges = new Poco::JSON::Array();
        for (const auto & [min_block, max_block] : task.block_ranges)
        {
            Poco::JSON::Array::Ptr range = new Poco::JSON::Array();
            range->add(min_block);
            range->add(max_block);
            ranges->add(range);
        }

        Poco::JSON::Object::Ptr object = new Poco::JSON::Object();
        object->set("transaction_id", task.transaction_id);
        object->set("block_ranges", ranges);
        object->set("failed_time", static_cast<Int64>(task.failed_time));
        array->add(object);
    }
    return array;
}

inline ExportRetriedTasks fromJSON(const Poco::JSON::Array::Ptr & array)
{
    ExportRetriedTasks tasks;
    if (!array)
        return tasks;

    for (size_t i = 0; i < array->size(); ++i)
    {
        const auto object = array->getObject(static_cast<unsigned int>(i));
        if (!object)
            throw Exception(ErrorCodes::INCORRECT_DATA, "Invalid `retry_of` element in an export task descriptor");

        ExportRetriedTask task;
        task.transaction_id = object->getValue<String>("transaction_id");
        task.failed_time = static_cast<time_t>(object->optValue<Int64>("failed_time", 0));

        if (const auto ranges = object->getArray("block_ranges"))
        {
            for (size_t j = 0; j < ranges->size(); ++j)
            {
                const auto range = ranges->getArray(static_cast<unsigned int>(j));
                if (!range || range->size() != 2)
                    throw Exception(ErrorCodes::INCORRECT_DATA,
                        "Invalid block range of retried task {} in an export task descriptor", task.transaction_id);
                task.block_ranges.emplace_back(range->getElement<Int64>(0), range->getElement<Int64>(1));
            }
        }

        tasks.push_back(std::move(task));
    }
    return tasks;
}

}

}
