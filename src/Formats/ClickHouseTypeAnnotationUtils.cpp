#include <Formats/ClickHouseTypeAnnotationUtils.h>

#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypeFactory.h>

namespace DB
{

String getAggregateFunctionTypeAnnotation(const DataTypePtr & type)
{
    return hasAggregateFunctionOrSimpleAggregateFunctionType(type) ? type->getName() : String{};
}

DataTypePtr getAnnotatedDataType(const String & type_name)
{
    auto type = DataTypeFactory::instance().get(type_name);
    /// As in stored table metadata, a state without an explicit version has version 0.
    setVersionToAggregateFunctions(type, /*if_empty=*/ true);
    return type;
}

}
