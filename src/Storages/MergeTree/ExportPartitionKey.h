#pragma once

#include <Common/escapeForFileName.h>
#include <base/types.h>

namespace DB
{

namespace ExportPartitionUtils
{

/// Identifies a partition export task by its (partition, destination) triple. Both MergeTree
/// flavours key their task registry on it, and both use it to name the node holding the task
/// descriptor: a ZooKeeper child of the table's `exports` path for a `Replicated*MergeTree`, a
/// file under the table's export directory for a plain `MergeTree`. So it has to be injective
/// and safe to use as a path component.
///
/// `escapeForFileName` percent-encodes every character that is not alphanumeric or `_`, so an
/// escaped component can never contain the `.` used here as a separator. Note that `_` survives
/// escaping and therefore cannot be used as a separator: without the escaping, destinations whose
/// qualified names flatten to the same string (`` `db.x`.`y` `` and `` `db`.`x.y` ``) would share
/// a key, and the second export of a partition would be rejected as a duplicate of the first.
inline String compositeKey(
    const String & partition_id, const String & destination_database, const String & destination_table)
{
    return escapeForFileName(partition_id) + "."
        + escapeForFileName(destination_database) + "."
        + escapeForFileName(destination_table);
}

}

}
