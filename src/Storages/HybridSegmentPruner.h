#pragma once

#include <Core/NamesAndTypes.h>
#include <Interpreters/Context_fwd.h>
#include <Parsers/IAST_fwd.h>

namespace DB
{

/// Conservative satisfiability check for Hybrid segment pruning.
///
/// Combines (PREWHERE AND WHERE AND segment_predicate), restricted to atoms over
/// columns of the Hybrid table, normalizes via top-level AND/OR walking, and tries
/// to prove the resulting condition unsatisfiable through bounded DNF expansion
/// plus per-column typed range/IN intersection.
///
/// Returns true only when the conjunction is provably empty (the segment can be
/// pruned). Returns false in all other cases — including unsupported predicates,
/// constant-folding failures, type-coercion ambiguity, and exceptions — so the
/// caller can fall back to scanning the segment normally.
///
/// Inputs:
/// - prewhere, where, segment_predicate: ASTs (any may be null).
///   The caller is responsible for removing JOIN-side predicates and for
///   substituting hybridParam(...) literals before invoking this function.
/// - hybrid_columns: column names and types from the Hybrid storage snapshot.
///   Atoms referencing columns not in this list are treated as unsupported and
///   keep the segment.
/// - context: used for constant-expression evaluation.
bool canPruneHybridSegment(
    const ASTPtr & prewhere,
    const ASTPtr & where,
    const ASTPtr & segment_predicate,
    const NamesAndTypesList & hybrid_columns,
    const ContextPtr & context);

}
