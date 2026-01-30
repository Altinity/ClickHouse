#include <Functions/identity.h>
#include <Functions/FunctionFactory.h>
#include <Common/FunctionDocumentation.h>

namespace DB
{

REGISTER_FUNCTION(Identity)
{
    factory.registerFunction<FunctionIdentity>();
}

REGISTER_FUNCTION(ScalarSubqueryResult)
{
    factory.registerFunction<FunctionScalarSubqueryResult>();
}

REGISTER_FUNCTION(ActionName)
{
    factory.registerFunction<FunctionActionName>();
}

REGISTER_FUNCTION(AliasMarker)
{
    factory.registerFunction<FunctionAliasMarker>(FunctionDocumentation{
        .description = R"(
Internal function that marks ALIAS column expressions for the analyzer. Not intended for direct use.
)",
        .syntax = {"__aliasMarker(expr, alias_name)"},
        .arguments = {
            {"expr", "Expression to mark.", {"Any"}},
            {"alias_name", "Alias name attached to the expression.", {"String"}},
        },
        .returned_value = {"Returns expr unchanged.", {"Any"}},
        .introduced_in = {25, 8},
        .category = FunctionDocumentation::Category::Other,
    });
}

}
