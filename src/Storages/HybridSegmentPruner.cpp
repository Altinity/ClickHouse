#include <Storages/HybridSegmentPruner.h>

#include <Core/Field.h>
#include <Core/Range.h>
#include <DataTypes/IDataType.h>
#include <Formats/FormatSettings.h>
#include <Interpreters/Context.h>
#include <Interpreters/convertFieldToType.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/IAST.h>

#include <optional>
#include <set>
#include <unordered_map>

namespace DB
{

namespace
{

/// Bounded DNF defaults: expand at most this many multi-disjunct OR groups,
/// and at most this many total branches. Anything beyond returns Keep.
constexpr size_t MAX_OR_GROUPS = 2;
constexpr size_t MAX_TOTAL_BRANCHES = 4;

enum class CompareOp : uint8_t
{
    Less,
    LessOrEqual,
    Greater,
    GreaterOrEqual,
    Equals,
};

/// Per-column domain accumulator. `range` is the typed interval (closed/open ends, possibly
/// unbounded) tightened by each comparison atom. `allowed`, when set, constrains the column
/// to a finite set of typed values (introduced by `=` or `IN` atoms).
struct ColumnDomain
{
    Range range = Range::createWholeUniverse();
    std::optional<std::set<Field>> allowed;
    bool empty = false;
};

/// Trim `allowed` against the current range and check overall emptiness. Returns true if the
/// domain is still satisfiable; sets `empty` and returns false if it's been narrowed to nothing.
bool finalizeDomain(ColumnDomain & domain)
{
    if (domain.empty || domain.range.empty())
        return !(domain.empty = true);
    if (domain.allowed)
    {
        for (auto it = domain.allowed->begin(); it != domain.allowed->end();)
        {
            /// Use intersectsRange(Range(point)) rather than Range::contains(FieldRef) because the
            /// Core/Range.cpp implementation has a buggy (effectively always-false) semantics.
            if (domain.range.intersectsRange(Range(*it)))
                ++it;
            else
                it = domain.allowed->erase(it);
        }
        if (domain.allowed->empty())
            return !(domain.empty = true);
    }
    return true;
}

/// One typed atom extracted from a supported AST shape.
struct TypedAtom
{
    String column;
    /// For comparisons: op + single value. For IN: op == Equals + values populated.
    CompareOp op = CompareOp::Equals;
    Field value;
    std::vector<Field> values; /// Used only for IN.
    bool is_in = false;
};

/// Look up Hybrid column type. Returns nullptr if the column is not part of the Hybrid schema.
DataTypePtr findColumnType(const NamesAndTypesList & cols, const String & name)
{
    for (const auto & c : cols)
        if (c.name == name)
            return c.type;
    return nullptr;
}

/// Collect top-level conjuncts, flattening nested `and(and(...), ...)`.
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

void collectDisjuncts(const ASTPtr & ast, std::vector<ASTPtr> & out)
{
    if (!ast)
        return;
    if (const auto * func = ast->as<ASTFunction>(); func && func->name == "or" && func->arguments)
    {
        for (const auto & child : func->arguments->children)
            collectDisjuncts(child, out);
        return;
    }
    out.push_back(ast);
}

/// Try to extract a Field of the given type from an arbitrary AST expression by constant-folding.
/// Returns nullopt if the expression is not foldable or the result cannot be coerced to `target_type`.
std::optional<Field> evalAndCoerce(
    const ASTPtr & ast, const IDataType & target_type, const ContextPtr & context)
{
    auto evaluated = tryEvaluateConstantExpression(ast, context);
    if (!evaluated)
        return {};
    if (!evaluated->second)
        return {};
    return convertFieldToTypeStrict(evaluated->first, *evaluated->second, target_type, FormatSettings{});
}

/// If `ast` is a tuple-of-literals or constant-folds to one, return the typed elements.
std::optional<std::vector<Field>> evalTupleAndCoerce(
    const ASTPtr & ast, const IDataType & target_type, const ContextPtr & context)
{
    auto evaluated = tryEvaluateConstantExpression(ast, context);
    if (!evaluated)
        return {};
    if (evaluated->first.getType() != Field::Types::Tuple)
        return {};
    const auto & tup = evaluated->first.safeGet<Tuple>();
    std::vector<Field> out;
    out.reserve(tup.size());
    for (const auto & f : tup)
    {
        /// Each element's source type is the tuple's element type. We pass nullptr as
        /// the from-type hint here; convertFieldToTypeStrict will conservatively reject
        /// imprecise/lossy coercions, which is what we want.
        Field coerced = convertFieldToType(f, target_type, nullptr, FormatSettings{});
        if (coerced.isNull() && !f.isNull())
            return {};
        out.push_back(std::move(coerced));
    }
    return out;
}

/// Extract a typed atom from a comparison/IN AST. `negated` reflects an outer `NOT`.
/// Returns nullopt for unsupported shapes (caller treats branch as satisfiable).
std::optional<TypedAtom> extractAtom(
    ASTPtr ast,
    bool negated,
    const NamesAndTypesList & hybrid_cols,
    const ContextPtr & context)
{
    /// Peel off outer `not` once.
    if (const auto * func = ast->as<ASTFunction>(); func && func->name == "not")
    {
        if (!func->arguments || func->arguments->children.size() != 1)
            return {};
        return extractAtom(func->arguments->children[0], !negated, hybrid_cols, context);
    }

    const auto * func = ast->as<ASTFunction>();
    if (!func || !func->arguments || func->arguments->children.size() != 2)
        return {};

    String fname = func->name;
    /// Effective op after applying `negated`.
    auto invert = [](CompareOp op) -> std::optional<CompareOp>
    {
        switch (op)
        {
            case CompareOp::Less: return CompareOp::GreaterOrEqual;
            case CompareOp::LessOrEqual: return CompareOp::Greater;
            case CompareOp::Greater: return CompareOp::LessOrEqual;
            case CompareOp::GreaterOrEqual: return CompareOp::Less;
            case CompareOp::Equals: return std::nullopt; /// `!=` is deferred.
        }
        return std::nullopt;
    };

    if (fname == "in")
    {
        if (negated)
            return {}; /// NOT IN deferred.

        const auto & lhs = func->arguments->children[0];
        const auto & rhs = func->arguments->children[1];

        const auto * ident = lhs->as<ASTIdentifier>();
        if (!ident)
            return {};
        /// Use the unqualified column name. The analyzer rewrites bare `ts` to
        /// `__table1.ts`; ownership-by-table is enforced separately by the
        /// caller (which skips pruning when the query has a JOIN).
        auto col_type = findColumnType(hybrid_cols, ident->shortName());
        if (!col_type)
            return {};

        auto values = evalTupleAndCoerce(rhs, *col_type, context);
        if (!values)
            return {};

        TypedAtom atom;
        atom.column = ident->shortName();
        atom.is_in = true;
        atom.values = std::move(*values);
        return atom;
    }

    CompareOp op;
    if (fname == "less") op = CompareOp::Less;
    else if (fname == "lessOrEquals") op = CompareOp::LessOrEqual;
    else if (fname == "greater") op = CompareOp::Greater;
    else if (fname == "greaterOrEquals") op = CompareOp::GreaterOrEqual;
    else if (fname == "equals") op = CompareOp::Equals;
    else
        return {}; /// Unsupported function (notEquals, like, etc.)

    if (negated)
    {
        auto inv = invert(op);
        if (!inv)
            return {};
        op = *inv;
    }

    /// Identify which side is the column and which is the constant.
    ASTPtr col_ast = func->arguments->children[0];
    ASTPtr val_ast = func->arguments->children[1];
    bool flipped = false;
    const auto * col_ident = col_ast->as<ASTIdentifier>();
    if (!col_ident)
    {
        col_ident = val_ast->as<ASTIdentifier>();
        if (!col_ident)
            return {};
        std::swap(col_ast, val_ast);
        flipped = true;
    }
    auto col_type = findColumnType(hybrid_cols, col_ident->shortName());
    if (!col_type)
        return {};

    /// If we swapped sides, the comparison is reversed: `5 < x` ≡ `x > 5`.
    if (flipped)
    {
        switch (op)
        {
            case CompareOp::Less: op = CompareOp::Greater; break;
            case CompareOp::LessOrEqual: op = CompareOp::GreaterOrEqual; break;
            case CompareOp::Greater: op = CompareOp::Less; break;
            case CompareOp::GreaterOrEqual: op = CompareOp::LessOrEqual; break;
            case CompareOp::Equals: break;
        }
    }

    auto coerced = evalAndCoerce(val_ast, *col_type, context);
    if (!coerced)
        return {};

    TypedAtom atom;
    atom.column = col_ident->shortName();
    atom.op = op;
    atom.value = std::move(*coerced);
    return atom;
}

/// Apply an atom to the per-column domain map. Returns true if the branch can still
/// be satisfiable; false if the atom proves the branch unsatisfiable.
bool applyAtomToDomains(
    std::unordered_map<String, ColumnDomain> & domains, const TypedAtom & atom)
{
    auto & domain = domains[atom.column];
    if (domain.empty)
        return false;

    if (atom.is_in)
    {
        std::set<Field> incoming(atom.values.begin(), atom.values.end());
        if (incoming.empty())
            return !(domain.empty = true);
        if (!domain.allowed)
            domain.allowed = std::move(incoming);
        else
        {
            std::set<Field> intersection;
            for (const auto & f : *domain.allowed)
                if (incoming.contains(f))
                    intersection.insert(f);
            domain.allowed = std::move(intersection);
        }
        return finalizeDomain(domain);
    }

    Range atom_range = Range::createWholeUniverse();
    switch (atom.op)
    {
        case CompareOp::Less:           atom_range = Range::createRightBounded(atom.value, /*included*/ false); break;
        case CompareOp::LessOrEqual:    atom_range = Range::createRightBounded(atom.value, /*included*/ true);  break;
        case CompareOp::Greater:        atom_range = Range::createLeftBounded(atom.value, /*included*/ false);  break;
        case CompareOp::GreaterOrEqual: atom_range = Range::createLeftBounded(atom.value, /*included*/ true);   break;
        case CompareOp::Equals:         atom_range = Range(atom.value); break;
    }

    auto narrowed = domain.range.intersectWith(atom_range);
    if (!narrowed)
        return !(domain.empty = true);
    domain.range = std::move(*narrowed);

    if (atom.op == CompareOp::Equals)
    {
        if (!domain.allowed)
            domain.allowed = std::set<Field>{atom.value};
        else if (!domain.allowed->contains(atom.value))
            return !(domain.empty = true);
        else
            domain.allowed = std::set<Field>{atom.value};
    }

    return finalizeDomain(domain);
}

/// True if every atom in `branch` extracts to a supported typed atom AND the
/// per-column intersection is empty. Unsupported atoms make the branch satisfiable
/// (keep), as required by the fail-open contract.
bool branchIsUnsatisfiable(
    const std::vector<ASTPtr> & branch_atoms,
    const NamesAndTypesList & hybrid_columns,
    const ContextPtr & context)
{
    std::unordered_map<String, ColumnDomain> domains;
    for (const auto & ast : branch_atoms)
    {
        auto atom = extractAtom(ast, /*negated*/ false, hybrid_columns, context);
        if (!atom)
            return false; /// Unsupported atom → branch is "unknown" → treat as satisfiable.
        if (!applyAtomToDomains(domains, *atom))
            return true;
    }
    return false;
}

}

bool canPruneHybridSegment(
    const ASTPtr & prewhere,
    const ASTPtr & where,
    const ASTPtr & segment_predicate,
    const NamesAndTypesList & hybrid_columns,
    const ContextPtr & context)
try
{
    /// Step 1: split top-level AND of (prewhere, where, segment_predicate) into conjuncts.
    std::vector<ASTPtr> conjuncts;
    collectConjuncts(prewhere, conjuncts);
    collectConjuncts(where, conjuncts);
    collectConjuncts(segment_predicate, conjuncts);
    if (conjuncts.empty())
        return false;

    /// Step 2: classify each conjunct as a mandatory atom (singleton) or a multi-disjunct
    /// OR group (alternative). Atoms inside an OR alternative are themselves AND-flattened
    /// so each disjunct can be a small conjunction such as `date = today() AND id IN (...)`.
    std::vector<ASTPtr> mandatory;
    std::vector<std::vector<std::vector<ASTPtr>>> or_groups; /// group → branch → atoms

    for (const auto & c : conjuncts)
    {
        std::vector<ASTPtr> disjuncts;
        collectDisjuncts(c, disjuncts);
        if (disjuncts.size() == 1)
        {
            mandatory.push_back(disjuncts.front());
            continue;
        }
        std::vector<std::vector<ASTPtr>> branches;
        branches.reserve(disjuncts.size());
        for (const auto & d : disjuncts)
        {
            std::vector<ASTPtr> b;
            collectConjuncts(d, b);
            branches.push_back(std::move(b));
        }
        or_groups.push_back(std::move(branches));
    }

    /// Step 3: enforce bounded DNF.
    if (or_groups.size() > MAX_OR_GROUPS)
        return false;
    size_t total = 1;
    for (const auto & g : or_groups)
    {
        if (g.empty())
            return false;
        total *= g.size();
        if (total > MAX_TOTAL_BRANCHES)
            return false;
    }

    /// Step 4: if there are no OR groups, evaluate the single mandatory branch directly.
    if (or_groups.empty())
        return branchIsUnsatisfiable(mandatory, hybrid_columns, context);

    /// Step 5: cartesian product over OR groups; each combination ANDed with `mandatory`
    /// forms a DNF branch. Prune iff every branch is provably unsatisfiable.
    std::vector<size_t> idx(or_groups.size(), 0);
    while (true)
    {
        std::vector<ASTPtr> branch = mandatory;
        for (size_t i = 0; i < or_groups.size(); ++i)
        {
            const auto & disjunct = or_groups[i][idx[i]];
            branch.insert(branch.end(), disjunct.begin(), disjunct.end());
        }

        if (!branchIsUnsatisfiable(branch, hybrid_columns, context))
            return false;

        /// Increment cartesian-product indices.
        size_t k = 0;
        for (; k < or_groups.size(); ++k)
        {
            if (++idx[k] < or_groups[k].size())
                break;
            idx[k] = 0;
        }
        if (k == or_groups.size())
            break;
    }

    return true;
}
catch (...)
{
    /// Fail-open: any unexpected exception in atom extraction, type coercion, or
    /// constant evaluation must not prevent the segment from being scanned normally.
    return false;
}

}
