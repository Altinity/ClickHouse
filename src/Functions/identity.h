#pragma once
#include <DataTypes/IDataType.h>
#include <Functions/IFunction.h>
#include <Interpreters/Context_fwd.h>
#include <Common/Exception.h>

#if USE_EMBEDDED_COMPILER
#    include <DataTypes/Native.h>
#    include <llvm/IR/IRBuilder.h>
#endif

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

class FunctionIdentityBase : public IFunction
{
public:
    FunctionIdentityBase(const char * name_, [[maybe_unused]] bool is_identity_)
        : function_name(name_)
#if USE_EMBEDDED_COMPILER
        , is_identity(is_identity_)
#endif
    {}

    static FunctionPtr create(ContextPtr, const char * name, bool is_identity)
    {
        return std::make_shared<FunctionIdentityBase>(name, is_identity);
    }

    String getName() const override { return function_name; }
    size_t getNumberOfArguments() const override { return 1; }
    bool isSuitableForConstantFolding() const override { return false; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return false; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        return arguments.front();
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t /*input_rows_count*/) const override
    {
        return arguments.front().column;
    }

#if USE_EMBEDDED_COMPILER
    bool isCompilableImpl(const DataTypes & /*types*/, const DataTypePtr & result_type) const override
    {
        return is_identity && canBeNativeType(result_type);
    }

    llvm::Value *
    compileImpl(llvm::IRBuilderBase & /*builder*/, const ValuesWithType & arguments, const DataTypePtr & /*result_type*/) const override
    {
        return arguments[0].value;
    }
#endif

private:
    const char * function_name;
#if USE_EMBEDDED_COMPILER
    bool is_identity;
#endif
};


/// Default-constructible identity function, used as a template argument in FunctionMapToArrayAdapter
class FunctionIdentity : public FunctionIdentityBase
{
public:
    FunctionIdentity() : FunctionIdentityBase("identity", true) {}
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionIdentity>(); }
};


class FunctionActionName : public FunctionIdentityBase
{
public:
    FunctionActionName() : FunctionIdentityBase("__actionName", false) {}
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionActionName>(); }
    size_t getNumberOfArguments() const override { return 2; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const override { return {0, 1}; }

    /// Do not allow any argument to have type other than String
    bool useDefaultImplementationForNulls() const override { return false; }
    bool useDefaultImplementationForNothing() const override { return false; }
    bool useDefaultImplementationForLowCardinalityColumns() const override { return false; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        for (const auto & arg : arguments)
        {
            if (WhichDataType(arg).isString())
                continue;
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Function __actionName is internal nad should not be used directly");
        }

        return FunctionIdentityBase::getReturnTypeImpl(arguments);
    }
};

struct AliasMarkerName
{
    static constexpr auto name = "__aliasMarker";
};

/**
 * __aliasMarker is a transport-time alias preservation hint for distributed SQL paths.
 *
 * Why it exists:
 * - When a distributed query is planned and mergeable-state flows are used, the final aliasing step
 *   is intentionally skipped.
 * - That is desired, but it also prevents preserving/injecting initiator-side expression names
 *   (for example, names coming from ALIAS columns or certain CAST expressions).
 * - This becomes especially problematic when shard schemas differ slightly.
 * - Some injected alias columns must preserve a specific output name; otherwise remote headers may diverge
 *   from initiator expectations (header mismatch, wrong column association, and similar inconsistencies).
 *
 * Lifecycle/invariants:
 * 1) Injected only around rewritten alias expressions that require stable output identity.
 * 2) Materialized before SQL serialization: the marker id is converted to a String alias identifier.
 * 3) Consumed by analyzer/planner on receiver to enforce alias naming in actions.
 * 4) Removed/stripped before forwarding to the next hop, then (if needed) re-injected for that hop only.
 *
 * This is a temporary bridge while distributed plan transport still relies on SQL text in these paths.
 * As query plan serialization fully replaces that boundary, this marker path should become unnecessary.
 */
class FunctionAliasMarker : public IFunction
{
public:
    static constexpr auto name = AliasMarkerName::name;
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionAliasMarker>(); }

    String getName() const override { return name; }
    size_t getNumberOfArguments() const override { return 2; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const override { return {}; }
    bool isSuitableForConstantFolding() const override { return false; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return false; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (arguments.size() != 2)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Function __aliasMarker expects 2 arguments");

        return arguments.front();
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t /*input_rows_count*/) const override
    {
        return arguments.front().column;
    }
};

}
