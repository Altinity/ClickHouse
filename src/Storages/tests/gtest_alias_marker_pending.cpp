#include <gtest/gtest.h>

#include <Analyzer/ColumnNode.h>
#include <Analyzer/ConstantNode.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/JoinNode.h>
#include <Analyzer/ListNode.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/Utils.h>
#include <Common/Exception.h>
#include <Common/tests/gtest_global_context.h>
#include <Common/tests/gtest_global_register.h>
#include <Core/Block.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/identity.h>
#include <Interpreters/Context.h>
#include <Planner/Utils.h>
#include <Processors/QueryPlan/ParallelReplicasLocalPlan.h>
#include <Storages/buildQueryTreeForShard.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}
}

using namespace DB;

namespace
{

class MarkerTestTableNode final : public IQueryTreeNode
{
public:
    MarkerTestTableNode() : IQueryTreeNode(0) {}

    QueryTreeNodeType getNodeType() const override { return QueryTreeNodeType::TABLE; }
    void dumpTreeImpl(WriteBuffer &, FormatState &, size_t) const override {}
    bool isEqualImpl(const IQueryTreeNode &, CompareOptions) const override { return true; }
    void updateTreeHashImpl(HashState &, CompareOptions) const override {}
    QueryTreeNodePtr cloneImpl() const override { return std::make_shared<MarkerTestTableNode>(); }
    ASTPtr toASTImpl(const ConvertToASTOptions &) const override { return nullptr; }
};

FunctionNodePtr makeMarker(QueryTreeNodes arguments)
{
    auto marker = std::make_shared<FunctionNode>(AliasMarkerName::name);
    marker->getArguments().getNodes() = std::move(arguments);
    return marker;
}

}

TEST(AliasMarkerPending, RejectNestedMarkerBeforeShipping)
{
    auto pending = makeMarker({std::make_shared<ConstantNode>(1), std::make_shared<ConstantNode>(1),
        std::make_shared<ConstantNode>(String(AliasMarkerName::pending_token), std::make_shared<DataTypeString>())});
    auto outer = makeMarker({pending, std::make_shared<ConstantNode>(String("outer"), std::make_shared<DataTypeString>())});
    auto query = std::make_shared<QueryNode>(Context::createCopy(getContext().context));
    query->getProjection().getNodes().push_back(outer);
    QueryTreeNodePtr root = query;

    auto expect_pending_rejected = [](auto && ship)
    {
        try
        {
            ship();
            FAIL() << "A nested pending marker must not cross the shipping boundary";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
            EXPECT_NE(e.message().find("cannot be shipped before finalization"), String::npos);
        }
    };

    /// Both shipping entry points check before normalization or planning can discard the inner marker.
    expect_pending_rejected([&] { assertNoPendingAliasMarkersForDistributedSerialization(root); });
    expect_pending_rejected([&] { (void)queryNodeToDistributedSelectQuery(root); });
    expect_pending_rejected([&]
    {
        (void)createRemotePlanForParallelReplicas(root, Block{}, query->getContext(), QueryProcessingStage::FetchColumns);
    });
}

TEST(AliasMarkerPending, ProjectionAliasOnJoinUsingSide)
{
    tryRegisterFunctions();
    auto context = Context::createCopy(getContext().context);
    context->setSetting("enable_alias_marker", Field(1));
    context->setSetting("analyzer_compatibility_join_using_top_level_identifier", Field(1));

    auto left = std::make_shared<MarkerTestTableNode>();
    left->setAlias("__table1");
    auto right = std::make_shared<MarkerTestTableNode>();
    right->setAlias("__table2");

    const auto type = std::make_shared<DataTypeUInt64>();
    auto column = std::make_shared<ColumnNode>(NameAndTypePair("x", type), left);
    /// Model the expression-bearing column that projection-alias resolution places on the left `USING` side.
    auto projection_alias = std::make_shared<ColumnNode>(NameAndTypePair("k", type), column, left);
    auto right_key = std::make_shared<ColumnNode>(NameAndTypePair("k", type), right);
    auto key_sides = std::make_shared<ListNode>(QueryTreeNodes{projection_alias, right_key});
    auto using_key = std::make_shared<ColumnNode>(NameAndTypePair("k", type), key_sides, left);
    auto using_list = std::make_shared<ListNode>(QueryTreeNodes{using_key});

    auto join = std::make_shared<JoinNode>(left, right, using_list,
        JoinLocality::Unspecified, JoinStrictness::All, JoinKind::Inner, true);
    auto query = std::make_shared<QueryNode>(context);
    query->getJoinTree() = join;
    QueryTreeNodePtr root = query;

    inlineAliasColumns(root, context);

    const auto & side = key_sides->getNodes()[0];
    const auto * pending = side->as<FunctionNode>();
    ASSERT_NE(pending, nullptr);
    ASSERT_EQ(pending->getArguments().getNodes().size(), 3);
    EXPECT_EQ(pending->getArguments().getNodes()[1]->as<ColumnNode &>().getColumnName(), "k");
    EXPECT_THROW(assertNoPendingAliasMarkersForDistributedSerialization(root), Exception);

    finalizeAliasMarkersForDistributedSerialization(root, context);

    const auto & finalized_arguments = pending->getArguments().getNodes();
    ASSERT_EQ(finalized_arguments.size(), 2);
    EXPECT_EQ(finalized_arguments[1]->as<ConstantNode &>().getValue().safeGet<String>(), "__table1.k");
    EXPECT_NO_THROW(assertNoPendingAliasMarkersForDistributedSerialization(root));
}
