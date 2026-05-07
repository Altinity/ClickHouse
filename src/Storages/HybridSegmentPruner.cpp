#include <Storages/HybridSegmentPruner.h>

#include <Core/Field.h>
#include <Core/Range.h>
#include <DataTypes/IDataType.h>
#include <Formats/FormatSettings.h>
#include <Interpreters/Context.h>
#include <Interpreters/convertFieldToType.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/IAST.h>
#include <Storages/ColumnsDescription.h>

#include <optional>
#include <unordered_map>

namespace DB
{

namespace
{

/// Build `tuple(col1, col2, ...)` to feed into KeyDescription::getKeyFromAST.
/// Mirrors the pattern in Paimon::PartitionPruner.
ASTPtr makeIdentityKeyAST(const Names & column_names)
{
    auto key_ast = make_intrusive<ASTFunction>();
    key_ast->name = "tuple";
    key_ast->arguments = make_intrusive<ASTExpressionList>();
    key_ast->children.push_back(key_ast->arguments);
    for (const auto & name : column_names)
        key_ast->arguments->children.push_back(make_intrusive<ASTIdentifier>(name));
    return key_ast;
}

/// KeyDescription requires every key column to be comparable. Drop the rest;
/// segment predicates over filtered columns will fail-open in canBePruned().
NamesAndTypesList filterComparable(const NamesAndTypesList & in)
{
    NamesAndTypesList out;
    for (const auto & c : in)
        if (c.type && c.type->isComparable())
            out.push_back(c);
    return out;
}

KeyDescription buildIdentityKey(const NamesAndTypesList & comparable_cols, ContextPtr context)
{
    Names names;
    names.reserve(comparable_cols.size());
    for (const auto & c : comparable_cols)
        names.push_back(c.name);
    return KeyDescription::getKeyFromAST(
        makeIdentityKeyAST(names),
        ColumnsDescription{comparable_cols},
        context);
}

/// Strictly recognize `<`, `<=`, `>`, `>=`, `=`. Returns the typed Range for `column op value`.
std::optional<Range> rangeForCompare(const String & op, const Field & value)
{
    if (op == "less")            return Range::createRightBounded(value, /*included*/ false);
    if (op == "lessOrEquals")    return Range::createRightBounded(value, /*included*/ true);
    if (op == "greater")         return Range::createLeftBounded(value, /*included*/ false);
    if (op == "greaterOrEquals") return Range::createLeftBounded(value, /*included*/ true);
    if (op == "equals")          return Range(value);
    return std::nullopt;
}

/// `5 < x` ≡ `x > 5`, etc.
String swapCompareOp(const String & op)
{
    if (op == "less")            return "greater";
    if (op == "lessOrEquals")    return "greaterOrEquals";
    if (op == "greater")         return "less";
    if (op == "greaterOrEquals") return "lessOrEquals";
    return op; /// equals stays equals
}

void collectConjuncts(const ASTPtr & ast, std::vector<ASTPtr> & out)
{
    if (!ast)
        return;
    if (const auto * func = ast->as<ASTFunction>(); func && func->name == "and" && func->arguments)
    {
        for (const auto & child : func->arguments->children)
            collectConjuncts(child, out);
        return;
    }
    out.push_back(ast);
}

enum class ApplyOutcome
{
    Recognized,        /// atom narrowed the rect on its column
    Unsupported,       /// caller should fail open (keep segment)
    EmptyIntersection, /// segment self-contradicts → segment is provably empty
};

/// Tighten `rect[idx]` for the column referenced by a single comparison conjunct.
/// `column_index_by_name` maps Hybrid column name → rect index (already restricted
/// to comparable columns).
ApplyOutcome applyConjunct(
    const ASTPtr & ast,
    const std::unordered_map<String, size_t> & column_index_by_name,
    const DataTypes & key_types,
    Hyperrectangle & rect,
    const ContextPtr & context)
{
    const auto * func = ast->as<ASTFunction>();
    if (!func || !func->arguments || func->arguments->children.size() != 2)
        return ApplyOutcome::Unsupported;

    const String & fname = func->name;
    if (!rangeForCompare(fname, Field{}))
        return ApplyOutcome::Unsupported;

    ASTPtr lhs = func->arguments->children[0];
    ASTPtr rhs = func->arguments->children[1];
    const auto * lhs_id = lhs->as<ASTIdentifier>();
    const auto * rhs_id = rhs->as<ASTIdentifier>();

    String column_name;
    ASTPtr value_ast;
    String op = fname;
    if (lhs_id && !rhs_id)
    {
        column_name = lhs_id->shortName();
        value_ast = rhs;
    }
    else if (!lhs_id && rhs_id)
    {
        column_name = rhs_id->shortName();
        value_ast = lhs;
        op = swapCompareOp(fname);
    }
    else
    {
        return ApplyOutcome::Unsupported;
    }

    auto idx_it = column_index_by_name.find(column_name);
    if (idx_it == column_index_by_name.end())
        return ApplyOutcome::Unsupported;
    size_t idx = idx_it->second;

    auto evaluated = tryEvaluateConstantExpression(value_ast, context);
    if (!evaluated || !evaluated->second)
        return ApplyOutcome::Unsupported;

    auto coerced = convertFieldToTypeStrict(evaluated->first, *evaluated->second, *key_types[idx], FormatSettings{});
    if (!coerced || (coerced->isNull() && !evaluated->first.isNull()))
        return ApplyOutcome::Unsupported;

    auto atom_range = rangeForCompare(op, *coerced);
    if (!atom_range)
        return ApplyOutcome::Unsupported;

    auto narrowed = rect[idx].intersectWith(*atom_range);
    if (!narrowed)
        return ApplyOutcome::EmptyIntersection;
    rect[idx] = std::move(*narrowed);
    return ApplyOutcome::Recognized;
}

}

HybridSegmentPruner::HybridSegmentPruner(
    const ActionsDAGWithInversionPushDown & filter_dag,
    const NamesAndTypesList & hybrid_columns,
    ContextPtr context_)
    : identity_key(buildIdentityKey(filterComparable(hybrid_columns), context_))
    , user_condition(filter_dag, context_,
                     identity_key.column_names, identity_key.expression,
                     /*single_point=*/ false)
    , context(std::move(context_))
{
    useless = identity_key.column_names.empty() || user_condition.alwaysUnknownOrTrue();
}

bool HybridSegmentPruner::canBePruned(const ASTPtr & substituted_segment_predicate) const
try
{
    if (useless || !substituted_segment_predicate)
        return false;

    std::vector<ASTPtr> conjuncts;
    collectConjuncts(substituted_segment_predicate, conjuncts);
    if (conjuncts.empty())
        return false;

    std::unordered_map<String, size_t> idx_by_name;
    idx_by_name.reserve(identity_key.column_names.size());
    for (size_t i = 0; i < identity_key.column_names.size(); ++i)
        idx_by_name.emplace(identity_key.column_names[i], i);

    Hyperrectangle rect(identity_key.column_names.size(), Range::createWholeUniverse());

    for (const auto & c : conjuncts)
    {
        switch (applyConjunct(c, idx_by_name, identity_key.data_types, rect, context))
        {
            case ApplyOutcome::Unsupported:
                return false;
            case ApplyOutcome::EmptyIntersection:
                /// Segment predicate self-contradicts → segment is provably empty.
                return true;
            case ApplyOutcome::Recognized:
                break;
        }
    }

    return !user_condition.checkInHyperrectangle(rect, identity_key.data_types).can_be_true;
}
catch (...)
{
    /// Fail-open: any unexpected exception in extraction, type coercion, or constant
    /// evaluation must not prevent the segment from being scanned normally.
    return false;
}

}
