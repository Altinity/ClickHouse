#pragma once

#include <Analyzer/IQueryTreePass.h>
#include <Interpreters/Context_fwd.h>

namespace DB
{

/// Adds CASTs for Hybrid segments when physical types differ from the Hybrid schema
/// and reorders the SELECT list to match the schema order (needed because planner
/// later aligns remote headers by position).
class HybridCastsPass : public IQueryTreePass
{
public:
    String getName() override { return "HybridCastsPass"; }
    String getDescription() override { return "Inject casts for Hybrid columns to match schema types"; }
    void run(QueryTreeNodePtr & query_tree_node, ContextPtr context) override;
};

}
