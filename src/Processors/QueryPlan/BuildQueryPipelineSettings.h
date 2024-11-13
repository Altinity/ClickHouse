#pragma once

#include <IO/Progress.h>
#include <Interpreters/ExpressionActionsSettings.h>

#include <cstddef>


namespace DB
{

struct Settings;
class QueryStatus;
using QueryStatusPtr = std::shared_ptr<QueryStatus>;

struct BuildQueryPipelineSettings
{
    bool enable_multiple_filters_transforms_for_and_chain = true;

    ExpressionActionsSettings actions_settings;
    QueryStatusPtr process_list_element;
    ProgressCallback progress_callback = nullptr;

    const ExpressionActionsSettings & getActionsSettings() const { return actions_settings; }
    static BuildQueryPipelineSettings fromContext(ContextPtr from);
};

}
