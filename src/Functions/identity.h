#pragma once
#include <DataTypes/IDataType.h>
#include <Functions/IFunction.h>
#include <Interpreters/Context_fwd.h>

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

struct IdentityName
{
    static constexpr auto name = "identity";
};

template<typename Name>
class FunctionIdentityBase : public IFunction
{
public:
    static constexpr auto name = Name::name;
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionIdentityBase<Name>>(); }

    String getName() const override { return name; }
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
        return Name::name == IdentityName::name && canBeNativeType(result_type);
    }

    llvm::Value *
    compileImpl(llvm::IRBuilderBase & /*builder*/, const ValuesWithType & arguments, const DataTypePtr & /*result_type*/) const override
    {
        return arguments[0].value;
    }
#endif
};


struct ScalarSubqueryResultName
{
    static constexpr auto name = "__scalarSubqueryResult";
};

using FunctionIdentity = FunctionIdentityBase<IdentityName>;
using FunctionScalarSubqueryResult = FunctionIdentityBase<ScalarSubqueryResultName>;

struct ActionNameName
{
    static constexpr auto name = "__actionName";
};

class FunctionActionName : public FunctionIdentityBase<ActionNameName>
{
public:
    using FunctionIdentityBase::FunctionIdentityBase;
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

        return FunctionIdentityBase<ActionNameName>::getReturnTypeImpl(arguments);
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
