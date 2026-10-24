#pragma once

#include <base/types.h>
#include <Common/Exception.h>
#include <Poco/JSON/Array.h>

#include <utility>
#include <vector>

namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
}

/// Block numbers `[first, second]` whose rows an export of a part reads, as stored in task
/// descriptors. Empty means every row of the part.
using ExportBlockNumberRanges = std::vector<std::pair<Int64, Int64>>;

inline Poco::JSON::Array::Ptr exportBlockNumberRangesToJSON(const ExportBlockNumberRanges & ranges)
{
    Poco::JSON::Array::Ptr array = new Poco::JSON::Array();
    for (const auto & [min_block, max_block] : ranges)
    {
        Poco::JSON::Array::Ptr pair = new Poco::JSON::Array();
        pair->add(min_block);
        pair->add(max_block);
        array->add(pair);
    }
    return array;
}

inline ExportBlockNumberRanges exportBlockNumberRangesFromJSON(const Poco::JSON::Array::Ptr & array)
{
    ExportBlockNumberRanges ranges;
    if (!array)
        return ranges;

    ranges.reserve(array->size());
    for (size_t i = 0; i < array->size(); ++i)
    {
        const auto pair = array->getArray(static_cast<unsigned int>(i));
        if (!pair || pair->size() != 2)
            throw Exception(ErrorCodes::INCORRECT_DATA, "Invalid block range in an export task descriptor");
        ranges.emplace_back(pair->getElement<Int64>(0), pair->getElement<Int64>(1));
    }
    return ranges;
}

}
