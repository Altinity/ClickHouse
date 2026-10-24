#pragma once

#include <base/types.h>

namespace DB
{

/// What created an export task of a `MergeTree` table.
enum class ExportTaskSource : uint8_t
{
    /// `ALTER TABLE ... EXPORT PARTITION`.
    query,
    /// The table's `TTL ... EXPORT TO TABLE` expression.
    ttl,
};

}
