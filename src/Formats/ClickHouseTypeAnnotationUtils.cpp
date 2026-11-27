#include <Formats/ClickHouseTypeAnnotationUtils.h>

#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypeFactory.h>
#include <Parsers/ASTDataType.h>
#include <Parsers/ASTIdentifier.h>

namespace DB
{

String getAggregateFunctionTypeAnnotation(const DataTypePtr & type)
{
    return hasAggregateFunctionOrSimpleAggregateFunctionType(type) ? type->getName() : String{};
}

namespace
{

/// Checks a parsed type name without resolving aggregate functions through `DataTypeFactory`.
bool astHasAggregateFunctionType(const ASTPtr & ast)
{
    std::string_view name;
    if (const auto * data_type = ast->as<ASTDataType>())
        name = data_type->name;
    else if (const auto * identifier = ast->as<ASTIdentifier>())
        name = identifier->name();

    if (name == "AggregateFunction")
        return true;

    for (const auto & child : ast->children)
        if (astHasAggregateFunctionType(child))
            return true;

    return false;
}

}

bool typeNameHasAggregateFunction(const String & type_name)
{
    return astHasAggregateFunctionType(DataTypeFactory::parse(type_name));
}

DataTypePtr getAnnotatedDataType(const String & type_name)
{
    auto type = DataTypeFactory::instance().get(type_name);
    /// As in stored table metadata, a state without an explicit version has version 0.
    setVersionToAggregateFunctions(type, /*if_empty=*/ true);
    return type;
}

}
