#pragma once

#include <Core/NamesAndTypes.h>
#include <base/types.h>

#include <unordered_map>

namespace DB
{

/// Key of the file-level key-value metadata that records ClickHouse types the file schema cannot represent,
/// as JSON `{"version": 1, "columns": {"<column name>": "<type name>", ...}}`.
constexpr const char * clickhouse_column_types_key = "clickhouse.column_types";

/// The value of the `clickhouse_column_types_key` metadata for `columns`, empty if none of them needs it.
String serializeClickHouseColumnTypesMetadata(const NamesAndTypesList & columns);

/// Column names and type names recorded in a `clickhouse_column_types_key` metadata value.
std::unordered_map<String, String> parseClickHouseColumnTypesMetadata(const String & value);

}
