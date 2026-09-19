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
class FunctionIdentity final : public FunctionIdentityBase
{
public:
    FunctionIdentity() : FunctionIdentityBase("identity", true) {}
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionIdentity>(); }
};


class FunctionActionName final : public FunctionIdentityBase
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

/** `__aliasMarker(expr, id)` is an internal pass-through identity. It returns `expr` untouched; `id` exists only to
  * give the expression a stable name in the planner's `ActionsDAG`.
  *
  * It is injected when an `ALIAS` column is inlined into its defining expression for transport to a shard. The
  * initiator still sees the un-inlined column, so without the marker the shard would name the output after the
  * expression (`multiply(__table1.value, 2)`) while the initiator expects the column (`__table1.computed`), and the
  * two headers could not be matched by name.
  *
  * The marker carries the low-level column identity, not the user's SQL alias. A SQL alias cannot do this job: it
  * participates in user-visible query semantics, it can collide with names the user chose, and in the
  * mergeable-state path the projection step that would normally apply it is skipped.
  *
  * This is also why the marker is not `__actionName`. `__actionName` survives into the `ActionsDAG` as a function node
  * with a forced result name; `__aliasMarker` is consumed into an alias on top of its child, which is what keeps the
  * expression behaving like a distinct logical column.
  *
  * The second argument travels in two forms. Between injection and serialization it is a `ColumnNode`, so the ordinary
  * analyzer passes keep transforming it along with everything else -- in particular `createUniqueAliasesIfNecessary`,
  * which assigns the final `__tableN` aliases. `finalizeAliasMarkersForDistributedSerialization` then materializes it
  * into a `String` constant just before the query is rendered as SQL. Freezing the id any earlier captures a table
  * alias that later changes, and the shard then emits a name the initiator never asked for.
  *
  * Both forms are accepted here, and so is anything else: a user can write `__aliasMarker(x, x)` and it behaves as an
  * identity. Rejecting unexpected arguments in the function would turn user SQL into a server-side error for no gain,
  * since the marker is harmless by construction.
  *
  * This is a bridge for as long as distributed transport still goes through SQL text. Once query plan serialization
  * replaces that boundary, the marker should become unnecessary.
  */
class FunctionAliasMarker : public IFunction
{
public:
    static constexpr auto name = AliasMarkerName::name;
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionAliasMarker>(); }

    String getName() const override { return name; }
    size_t getNumberOfArguments() const override { return 2; }
    /// The id argument is a constant only after finalization. Before that it is a `ColumnNode`, and between the two
    /// the function must still resolve.
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
