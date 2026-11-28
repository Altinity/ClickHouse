#pragma once

#include <DataTypes/IDataType.h>
#include <base/types.h>

namespace DB
{

/// The ClickHouse type name to record in the metadata of a Parquet file or an Iceberg table for `type`:
/// the name of `type` if it has an `AggregateFunction` or `SimpleAggregateFunction`, empty otherwise.
String getAggregateFunctionTypeAnnotation(const DataTypePtr & type);

/// The data type named `type_name`. A state without an explicit version has version 0.
DataTypePtr getAnnotatedDataType(const String & type_name);

}
