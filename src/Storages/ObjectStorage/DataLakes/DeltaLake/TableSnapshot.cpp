#include "config.h"

#if USE_DELTA_KERNEL_RS

#include <Storages/ObjectStorage/DataLakes/DeltaLake/TableSnapshot.h>
#include <AggregateFunctions/AggregateFunctionGroupBitmapData.h>

#include <Core/ColumnWithTypeAndName.h>
#include <Core/Types.h>
#include <Core/NamesAndTypes.h>
#include <Core/Field.h>
#include <Core/Settings.h>

#include <Columns/IColumn.h>
#include <Common/CurrentThread.h>
#include <Common/Exception.h>
#include <Common/FailPoint.h>
#include <Common/logger_useful.h>
#include <Common/ThreadPool.h>
#include <Common/ThreadStatus.h>
#include <Common/ThreadGroupSwitcher.h>
#include <Common/escapeForFileName.h>
#include <Common/setThreadName.h>

#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Interpreters/Context.h>
#include <Interpreters/ActionsDAG.h>

#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>

#include <Storages/ObjectStorage/DataLakes/DeltaLake/getSchemaFromSnapshot.h>
#include <Storages/ObjectStorage/DataLakes/DeltaLake/PartitionPruner.h>
#include <Storages/ObjectStorage/DataLakes/DeltaLake/KernelUtils.h>
#include <Storages/ObjectStorage/DataLakes/Common/Common.h>
#include <Storages/ObjectStorage/DataLakes/DeltaLake/ExpressionVisitor.h>
#include <Storages/ObjectStorage/DataLakes/DeltaLake/EnginePredicate.h>
#include <delta_kernel_ffi.hpp>
#include <fmt/ranges.h>
#include <roaring/roaring.hh>

#include <algorithm>

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int DELTA_KERNEL_ERROR;
    extern const int INCORRECT_DATA;
}

namespace DB::Setting
{
    extern const SettingsBool delta_lake_enable_expression_visitor_logging;
    extern const SettingsInt64 delta_lake_snapshot_version;
    extern const SettingsBool delta_lake_throw_on_engine_predicate_error;
    extern const SettingsBool delta_lake_enable_engine_predicate;
    extern const SettingsBool delta_lake_clustering_predicate_fallback;
}

namespace ProfileEvents
{
    extern const Event DeltaLakePartitionPrunedFiles;
    extern const Event DeltaLakeSnapshotInitializations;
    extern const Event DeltaLakeScannedFiles;
    extern const Event DeltaLakeClusteringPredicateFallbacks;
}

namespace DB::FailPoints
{
    extern const char delta_kernel_force_stale_token_error[];
    extern const char delta_kernel_force_stale_token_error_on_clustering_read[];
    extern const char delta_kernel_pause_before_clustering_read[];
}

namespace DeltaLake
{

namespace
{

/// A data file with the size its `add` action records.
struct DataObjectInfo : public DB::ObjectInfo
{
    DataObjectInfo(std::string path_, int64_t size_)
        : DB::ObjectInfo(DB::RelativePathWithMetadata(std::move(path_)))
        , size(size_)
    {
    }

    std::optional<size_t> getFileSizeHint() const override
    {
        if (size > 0)
            return static_cast<size_t>(size);
        return std::nullopt;
    }

    const int64_t size;
};

/// Errors after which retrying with refreshed credentials can succeed.
bool isStaleCredentialsError(const DB::Exception & e)
{
    if (e.code() != DB::ErrorCodes::DELTA_KERNEL_ERROR)
        return false;
    const auto & msg = e.message();
    return msg.contains("ExpiredToken")
        || msg.contains("InvalidToken")
        || msg.contains("TokenRefreshRequired");
}

/// Whether the table protocol has the `clustering` writer feature (liquid clustering).
/// Only such tables can have `delta.clustering` domain metadata. Reads the in-memory protocol, no I/O.
bool hasClusteringFeature(ffi::SharedSnapshot * snapshot)
{
    using KernelProtocol = KernelPointerWrapper<ffi::SharedProtocol, ffi::free_protocol>;
    KernelProtocol protocol(ffi::snapshot_get_protocol(snapshot));

    bool has_feature = false;
    ffi::visit_protocol(
        protocol.get(),
        &has_feature,
        [](ffi::NullableCvoid, int32_t, int32_t) {},
        [](ffi::NullableCvoid context, bool is_reader, ffi::KernelStringSlice feature)
        {
            if (!is_reader && std::string_view(feature.ptr, feature.len) == "clustering")
                *static_cast<bool *>(context) = true;
        });
    return has_feature;
}

/// Parses the `delta.clustering` domain metadata into the logical names used by the filter DAG.
/// A clustering column is skipped unless its name resolves unambiguously: pushing a wrong
/// name would silently lose file skipping, while skipping a column only loses this optimization.
DB::Names parseClusteringColumns(
    const std::string & json_str,
    const DB::NameToNameMap & physical_names_map,
    const std::string & table_location,
    const LoggerPtr & log)
{
    std::vector<std::vector<std::string>> paths;
    try
    {
        Poco::JSON::Parser parser;
        auto object = parser.parse(json_str).extract<Poco::JSON::Object::Ptr>();
        auto columns = object->getArray("clusteringColumns");
        if (!columns)
            throw DB::Exception(DB::ErrorCodes::INCORRECT_DATA, "delta.clustering: missing clusteringColumns array");

        for (unsigned int i = 0; i < columns->size(); ++i)
        {
            auto path_array = columns->getArray(i);
            if (!path_array || path_array->size() == 0)
                continue;

            auto & path = paths.emplace_back();
            for (unsigned int j = 0; j < path_array->size(); ++j)
                path.push_back(path_array->getElement<std::string>(j));
        }
    }
    catch (const Poco::Exception & e)
    {
        throw DB::Exception(
            DB::ErrorCodes::INCORRECT_DATA,
            "Failed to parse delta.clustering domain metadata of table {}: {}",
            table_location, e.displayText());
    }

    /// `physical_names_map` maps full dotted logical paths to full dotted physical paths,
    /// nested fields included. It is empty when column mapping is disabled.
    DB::NameToNameMap physical_to_logical;
    for (const auto & [logical, physical] : physical_names_map)
        physical_to_logical.emplace(physical, logical);

    DB::Names result;
    for (const auto & path : paths)
    {
        const auto joined = fmt::format("{}", fmt::join(path, "."));

        /// A dot inside a name makes `a.b` indistinguishable from the nested path `a`, `b`.
        if (std::ranges::any_of(path, [](const auto & name) { return name.contains('.'); }))
        {
            LOG_TRACE(log, "Clustering column `{}` of table {} has a dot in its name, not using it", joined, table_location);
            continue;
        }

        if (physical_to_logical.empty())
        {
            result.push_back(joined);
            continue;
        }

        auto it = physical_to_logical.find(joined);
        if (it == physical_to_logical.end())
        {
            LOG_TRACE(log, "Clustering column `{}` of table {} is not in the schema, not using it", joined, table_location);
            continue;
        }
        result.push_back(it->second);
    }
    return result;
}

}

class TableSnapshot::Iterator final : public DB::IObjectIterator, private DB::WithContext
{
private:
    /// Struct to hold ObjectInfo along with FFI handles for lazy parsing
    struct ScannedDataFile
    {
        DB::ObjectInfoPtr object;
        KernelDvInfo dv_info_handle;
        std::optional<KernelExpression> transform_handle;

        ScannedDataFile(
            DB::ObjectInfoPtr object_,
            KernelDvInfo dv_info_,
            std::optional<KernelExpression> transform_)
            : object(std::move(object_))
            , dv_info_handle(std::move(dv_info_))
            , transform_handle(std::move(transform_))
        {}
    };

public:
    using UpdateStatsFunc = std::function<void(SnapshotStats &&)>;

    Iterator(
        std::shared_ptr<KernelSnapshotState> kernel_snapshot_state_,
        KernelHelperPtr helper_,
        const ReadSchema & read_schema_,
        const TableSchema & table_schema_,
        const DB::NameToNameMap & physical_names_map_,
        const DB::Names & partition_columns_,
        const DB::ActionsDAG * filter_,
        DB::IDataLakeMetadata::FileProgressCallback callback_,
        size_t list_batch_size_,
        bool enable_expression_visitor_logging_,
        bool throw_on_engine_predicate_error_,
        bool enable_engine_predicate_,
        DB::ContextPtr query_context_,
        bool enable_clustering_predicate_fallback_,
        UpdateStatsFunc update_stats_func_,
        LoggerPtr log_)
        : DB::WithContext(query_context_)
        , kernel_snapshot_state(kernel_snapshot_state_)
        , captured_credentials_fingerprint(helper_->getCredentialsFingerprint())
        , helper(helper_)
        , read_schema(read_schema_)
        , expression_schema(table_schema_)
        , partition_columns(partition_columns_)
        , physical_names_map(physical_names_map_)
        , callback(callback_)
        , list_batch_size(list_batch_size_)
        , log(log_)
        , enable_expression_visitor_logging(enable_expression_visitor_logging_)
        , throw_on_engine_predicate_error(throw_on_engine_predicate_error_)
        , enable_engine_predicate(enable_engine_predicate_)
        , enable_clustering_predicate_fallback(enable_clustering_predicate_fallback_)
        , update_stats_func(update_stats_func_)
    {
        if (filter_)
        {
            filter = filter_->clone();
            pruner.emplace(
                *filter,
                table_schema_,
                partition_columns_,
                physical_names_map_,
                DB::Context::getGlobalContextInstance());

            LOG_TEST(log, "Using filter expression");
        }
        else
        {
            LOG_TEST(log, "No filter expression passed");
        }

        if (!physical_names_map_.empty())
        {
            for (auto & [name, value] : expression_schema)
                name = getPhysicalName(name, physical_names_map_);

            for (auto & name : partition_columns)
                name = getPhysicalName(name, physical_names_map_);
        }

        thread = ThreadFromGlobalPool(
            [&, thread_group = DB::CurrentThread::getGroup()]
            {
                /// Attach to current query thread group, to be able to
                /// have query id in logs and metrics from scanDataFunc.
                DB::ThreadGroupSwitcher switcher(thread_group, DB::ThreadName::DATALAKE_TABLE_SNAPSHOT);
                scanDataFunc();
            });
    }

    ~Iterator() override
    {
        shutdown.store(true);
        schedule_next_batch_cv.notify_one();
        if (thread.joinable())
            thread.join();
    }

    const std::string & getDataPath() const { return helper->getDataPath(); }

    const std::string & getTableLocation() const { return helper->getTableLocation(); }

    void setScanException()
    {
        if (!scan_exception)
        {
            scan_exception = std::current_exception();
            shutdown = true;
        }
    }

    void initScanState()
    {
        if (filter.has_value() && enable_engine_predicate)
        {
            auto predicate = getEnginePredicate(filter.value(), engine_predicate_exception, getContext());
            scan = KernelUtils::unwrapResult(
                ffi::scan(
                    kernel_snapshot_state->snapshot.get(),
                    kernel_snapshot_state->engine.get(),
                    predicate.get(),
                    /* schema */nullptr),
                "scan");

            /// Untranslatable nodes become Unknown inside `AND` and keep the other conjuncts,
            /// so only a translation exception can lose the clustering-column conjuncts.
            /// With `throw_on_engine_predicate_error` the query rethrows that exception anyway.
            if (engine_predicate_exception && enable_clustering_predicate_fallback && !throw_on_engine_predicate_error)
                tryUseClusteringPredicateFallback();
        }
        else
        {
            scan = KernelUtils::unwrapResult(
                ffi::scan(
                    kernel_snapshot_state->snapshot.get(),
                    kernel_snapshot_state->engine.get(),
                    /* predicate */nullptr,
                    /* schema */nullptr),
                "scan");
        }

        scan_data_iterator = KernelUtils::unwrapResult(
            ffi::scan_metadata_iter_init(kernel_snapshot_state->engine.get(), scan.get()),
            "scan_metadata_iter_init");
    }

    /// Reads the clustering columns of the iterator's snapshot, which is pinned to the query's version.
    DB::Names readClusteringColumns() const
    {
        /// Test hook: lets a test commit a newer table version before the read.
        DB::FailPointInjection::pauseFailPoint(DB::FailPoints::delta_kernel_pause_before_clustering_read);
        fiu_do_on(DB::FailPoints::delta_kernel_force_stale_token_error_on_clustering_read,
        {
            throw DB::Exception(
                DB::ErrorCodes::DELTA_KERNEL_ERROR,
                "ExpiredToken: forced by delta_kernel_force_stale_token_error_on_clustering_read failpoint");
        });

        void * result = KernelUtils::unwrapResult(
            ffi::get_domain_metadata_internal(
                kernel_snapshot_state->snapshot.get(),
                KernelUtils::toDeltaString("delta.clustering"),
                kernel_snapshot_state->engine.get(),
                KernelUtils::allocateString),
            "get_domain_metadata_internal(delta.clustering)");
        if (!result)
        {
            LOG_TRACE(log, "Table {} has no delta.clustering domain metadata", getTableLocation());
            return {};
        }

        std::unique_ptr<std::string> json_str(static_cast<std::string *>(result));
        return parseClusteringColumns(*json_str, physical_names_map, getTableLocation(), log);
    }

    /// Replaces the scan with one that pushes only the top-level conjuncts of the filter that
    /// reference nothing but clustering columns. A subset of the conjuncts of `AND` is implied
    /// by the full filter, so this scan can only skip files the full filter would skip too.
    /// Called after the full predicate translation threw.
    void tryUseClusteringPredicateFallback()
    {
        if (!hasClusteringFeature(kernel_snapshot_state->snapshot.get()))
            return;

        DB::Names clustering_columns;
        try
        {
            clustering_columns = readClusteringColumns();
        }
        catch (const DB::Exception & e)
        {
            /// Rethrown to the stale-credentials retry in `scanDataFunc`, which keeps the snapshot version.
            if (isStaleCredentialsError(e))
                throw;
            /// Only a failed kernel read or malformed metadata skips the fallback; other errors propagate.
            if (e.code() != DB::ErrorCodes::DELTA_KERNEL_ERROR && e.code() != DB::ErrorCodes::INCORRECT_DATA)
                throw;
            LOG_WARNING(
                log, "Cannot read the clustering columns of table {}, not using the clustering predicate fallback: {}",
                getTableLocation(), e.message());
            return;
        }

        if (clustering_columns.empty())
            return;

        const DB::NameSet clustering_inputs(clustering_columns.begin(), clustering_columns.end());
        auto references_only_clustering_inputs = [&](const DB::ActionsDAG::Node * root)
        {
            std::vector<const DB::ActionsDAG::Node *> stack{root};
            std::unordered_set<const DB::ActionsDAG::Node *> visited;
            while (!stack.empty())
            {
                const auto * node = stack.back();
                stack.pop_back();
                if (!visited.insert(node).second)
                    continue;
                if (node->type == DB::ActionsDAG::ActionType::INPUT && !clustering_inputs.contains(node->result_name))
                    return false;
                for (const auto * child : node->children)
                    stack.push_back(child);
            }
            return true;
        };

        DB::ActionsDAG::NodeRawConstPtrs clustering_conjuncts;
        for (const auto * conjunct : DB::ActionsDAG::extractConjunctionAtoms(filter->getOutputs().at(0)))
            if (references_only_clustering_inputs(conjunct))
                clustering_conjuncts.push_back(conjunct);

        if (clustering_conjuncts.empty())
            return;

        auto restricted_dag = DB::ActionsDAG::buildFilterActionsDAG(clustering_conjuncts);
        if (!restricted_dag)
            return;

        std::exception_ptr fallback_exception;
        /// Translate under the query's format settings (notably `date_time_overflow_behavior`),
        /// exactly like the original predicate, so both predicates agree on literal conversion.
        auto fallback_predicate = getEnginePredicate(*restricted_dag, fallback_exception, getContext());

        /// The predicate is translated inside `ffi::scan`, so its outcome is known only afterwards.
        KernelScan fallback_scan(KernelUtils::unwrapResult(
            ffi::scan(
                kernel_snapshot_state->snapshot.get(),
                kernel_snapshot_state->engine.get(),
                fallback_predicate.get(),
                /* schema */nullptr),
            "scan"));

        if (fallback_exception)
        {
            LOG_DEBUG(log, "Translation of the clustering-column predicate failed too, keeping the original scan");
            return;
        }

        ProfileEvents::increment(ProfileEvents::DeltaLakeClusteringPredicateFallbacks);
        LOG_DEBUG(
            log,
            "Full predicate translation failed; using the clustering-column predicate for file skipping "
            "(clustering columns: {})",
            fmt::join(clustering_columns, ", "));
        scan = std::move(fallback_scan);
    }

    /// One-shot retry on stale-credentials errors. Safe only when no files have been
    /// enqueued yet — replaying would emit duplicates to the consumer.
    bool tryRefreshAndRetryScanState(const DB::Exception & e)
    {
        if (!isStaleCredentialsError(e))
            return false;
        const auto & msg = e.message();

        {
            std::lock_guard lock(next_mutex);
            if (total_data_files > 0)
            {
                LOG_INFO(
                    log,
                    "Cannot safely retry DeltaLake scan iteration after stale-credentials error: "
                    "{} data file(s) already enqueued for the consumer. Propagating exception.",
                    total_data_files);
                return false;
            }
        }

        /// Two paths can yield fresh credentials:
        ///   1. Catalog-vended providers (Glue / Unity / REST) — `refreshCredentials`
        ///      explicitly swaps the underlying S3 client.
        ///   2. Rotating S3 providers (assume-role, web-identity, IMDS) — the C++ SDK
        ///      auto-refreshes its cached session on `getCredentials` when the cached
        ///      session expiry has passed. In that case the fingerprint changes versus
        ///      what we captured at engine-build time, without any callback firing.
        const bool refreshed_via_callback = helper->refreshCredentials();
        const DB::UInt128 post_fingerprint = helper->getCredentialsFingerprint();
        const bool fingerprint_drifted = post_fingerprint != captured_credentials_fingerprint;

        if (!refreshed_via_callback && !fingerprint_drifted)
        {
            LOG_INFO(
                log,
                "Delta kernel reported stale credentials during scan iteration, but neither "
                "a catalog refresh callback nor SDK-side credential rotation produced fresh "
                "credentials. Propagating exception.");
            return false;
        }

        LOG_INFO(
            log,
            "Delta kernel reported stale credentials during scan iteration; rebuilding "
            "scan state (refreshed via callback: {}, fingerprint drifted: {}). Original error: {}",
            refreshed_via_callback, fingerprint_drifted, msg);

        kernel_snapshot_state = std::make_shared<KernelSnapshotState>(
            *helper,
            std::optional<size_t>(kernel_snapshot_state->snapshot_version));
        captured_credentials_fingerprint = post_fingerprint;
        scan = KernelScan();
        scan_data_iterator = KernelScanDataIterator();
        return true;
    }

    void scanDataFunc()
    {
        bool retried = false;
        try
        {
            while (true)
            {
                try
                {
                    initScanState();

                    LOG_TEST(log, "Starting iterator loop (predicate exception: {})", bool(engine_predicate_exception));

                    while (!shutdown.load())
                    {
                        bool have_scan_data_res = KernelUtils::unwrapResult(
                            ffi::scan_metadata_next(scan_data_iterator.get(), this, visitData),
                            "scan_metadata_next");

                        if (have_scan_data_res)
                        {
                            std::unique_lock lock(next_mutex);
                            LOG_TEST(
                                log, "List batch size is {}/{}, shutdown: {}",
                                data_files.size(),
                                list_batch_size ? DB::toString(list_batch_size) : "Unlimitted",
                                shutdown.load());

                            if (!shutdown.load() && list_batch_size && data_files.size() >= list_batch_size)
                            {
                                schedule_next_batch_cv.wait(
                                    lock,
                                    [&]() { return (data_files.size() < list_batch_size) || shutdown.load(); });
                            }
                        }
                        else
                        {
                            {
                                std::lock_guard lock(next_mutex);
                                iterator_finished = true;
                                LOG_TEST(log, "Set finished");
                            }
                            data_files_cv.notify_all();

                            LOG_TRACE(
                                log, "All data files at version {} were listed "
                                "(scan exception: {}, total data files: {}, total rows: {}, total bytes: {})",
                                kernel_snapshot_state->snapshot_version,
                                bool(scan_exception),
                                total_data_files,
                                total_rows ? DB::toString(*total_rows) : "Unknown",
                                total_bytes);

                            if (update_stats_func
                                && !scan_exception
                                && (!filter.has_value() || !enable_engine_predicate))
                            {
                                update_stats_func(SnapshotStats{
                                    .total_bytes = total_bytes,
                                    /// total_rows is an optional statistic, but total_bytes is obligatory.
                                    .total_rows = total_rows
                                });
                            }
                            return;
                        }
                    }
                    return;
                }
                catch (const DB::Exception & e)
                {
                    if (retried || !tryRefreshAndRetryScanState(e))
                        throw;
                    retried = true;
                }
            }
        }
        catch (...) // Ok: exception saved via setScanException for later handling
        {
            setScanException();
            data_files_cv.notify_all();
            LOG_DEBUG(log, "Exception during scan_metadata_next");
        }
    }

    size_t estimatedKeysCount() override
    {
        /// For now do the same as StorageObjectStorageSource::GlobIterator.
        /// TODO: is it possible to do a precise estimation?
        return std::numeric_limits<size_t>::max();
    }

    std::optional<UInt64> getSnapshotVersion() const override
    {
        return kernel_snapshot_state->snapshot_version;
    }

    DB::ObjectInfoPtr next(size_t) override
    {
        while (true)
        {
            std::optional<ScannedDataFile> scan_item;
            {
                std::unique_lock lock(next_mutex);

                if (!iterator_finished && data_files.empty() && !shutdown)
                {
                    LOG_TEST(log, "Waiting for next data file");
                    schedule_next_batch_cv.notify_one();
                    data_files_cv.wait(lock, [&]() { return !data_files.empty() || iterator_finished || shutdown.load(); });
                }

                if (engine_predicate_exception && throw_on_engine_predicate_error)
                    std::rethrow_exception(engine_predicate_exception);

                if (scan_exception)
                    std::rethrow_exception(scan_exception);

                if (data_files.empty() || shutdown)
                {
                    LOG_TEST(log, "Data files: {}", data_files.size());
                    return nullptr;
                }

                LOG_TEST(log, "Current data files: {}", data_files.size());

                scan_item = std::move(data_files.front());
                data_files.pop_front();
            }

            schedule_next_batch_cv.notify_one();

            auto object = std::move(scan_item->object);

            /// Needed for partition values.
            parseTransformHandle(*scan_item, object);
            if (pruner.has_value() && pruner->canBePruned(*object))
            {
                ProfileEvents::increment(ProfileEvents::DeltaLakePartitionPrunedFiles);

                LOG_TEST(log, "Skipping file {} according to partition pruning", object->getPath());
                continue;
            }

            parseDVHandle(*scan_item, object);

            if (callback)
                callback(DB::FileProgress(0, object->getFileSizeHint().value_or(0)));
            return object;
        }
    }

    void parseTransformHandle(ScannedDataFile & scan_item, DB::ObjectInfoPtr & object)
    {
        auto & metadata = object->data_lake_metadata;
        chassert(metadata.has_value());

        if (scan_item.transform_handle.has_value())
        {
            if (!partition_columns.empty())
            {
                metadata->schema_transform = visitScanCallbackExpression(
                    scan_item.transform_handle->get(),
                    read_schema,
                    expression_schema,
                    enable_expression_visitor_logging);

                LOG_TEST(
                    log,
                    "Parsed transform for file: {}, transform: {}",
                    object->getPath(),
                    metadata->schema_transform->dumpNames());
            }
        }
    }

    void parseDVHandle(ScannedDataFile & scan_item, DB::ObjectInfoPtr & object)
    {
        auto & metadata = object->data_lake_metadata;
        chassert(metadata.has_value());

        if (auto * dv_info_ptr = scan_item.dv_info_handle.get(); dv_info_ptr && ffi::dv_info_has_vector(dv_info_ptr))
        {
            /// `row_indexes_from_dv` returns a vector of row indexes
            /// that should be *removed* from the result set
            ffi::KernelRowIndexArray row_indexes = KernelUtils::unwrapResult(
                ffi::row_indexes_from_dv(
                    dv_info_ptr,
                    kernel_snapshot_state->engine.get(),
                    KernelUtils::toDeltaString(getTableLocation())),
                "row_indexes_from_dv");

            SCOPE_EXIT({
                ffi::free_row_indexes(row_indexes);
            });

            if (row_indexes.len > 0)
            {
                LOG_TEST(log, "Row indexes size {} for file {}", row_indexes.len, object->getPath());

                auto bitmap = std::make_shared<DB::DataLakeObjectMetadata::ExcludedRows>();
                for (size_t i = 0; i < row_indexes.len; ++i)
                {
                    bitmap->add(row_indexes.ptr[i]);
                }
                metadata->excluded_rows = std::move(bitmap);
            }
        }
    }

    static void visitData(
        void * engine_context,
        ffi::SharedScanMetadata * scan_metadata)
    {
        auto * iter = static_cast<Iterator *>(engine_context);
        /// Release the handle on all exit paths to avoid leaking it.
        SCOPE_EXIT({
            ffi::free_scan_metadata(scan_metadata);
        });
        /// Runs inside Rust's `scan_metadata_next`: a C++ exception must not cross the `extern "C"`
        /// frame, so store it (like `scanCallback`) and let `scanDataFunc` rethrow after it returns.
        try
        {
            KernelUtils::unwrapResult(
                ffi::visit_scan_metadata(
                    scan_metadata,
                    iter->kernel_snapshot_state->engine.get(),
                    engine_context,
                    Iterator::scanCallback),
                "visit_scan_metadata");
        }
        catch (...) // Ok: exception saved via setScanException, rethrown by scanDataFunc
        {
            iter->setScanException();
            iter->data_files_cv.notify_all();
        }
    }

    static bool scanCallback(
        ffi::NullableCvoid engine_context,
        struct ffi::KernelStringSlice path,
        int64_t size,
        int64_t /*mod_time*/,
        const ffi::Stats * stats,
        ffi::SharedDvInfo * dv_info,
        ffi::OptionalValue<ffi::SharedExpression *> transform,
        const struct ffi::CStringMap * deprecated)
    {
        try
        {
            return scanCallbackImpl(engine_context, path, size, stats, dv_info, transform, deprecated);
        }
        catch (...)
        {
            auto * context = static_cast<TableSnapshot::Iterator *>(engine_context);
            /// We cannot allow to throw exceptions from ScanCallback,
            /// otherwise delta-kernel will panic and call terminate.
            context->setScanException();
            context->data_files_cv.notify_all();

            return false;  /// Stop iteration on exception
        }
    }

    static bool scanCallbackImpl(
        ffi::NullableCvoid engine_context,
        struct ffi::KernelStringSlice path,
        int64_t size,
        const ffi::Stats * stats,
        ffi::SharedDvInfo * dv_info,
        ffi::OptionalValue<ffi::SharedExpression *> transform,
        const struct ffi::CStringMap * /* deprecated */)
    {
        /// Wrap handles in RAII immediately to ensure cleanup on any exit path
        KernelDvInfo dv_info_handle(dv_info);
        std::optional<KernelExpression> transform_handle;
        if (transform.tag == ffi::OptionalValue<ffi::SharedExpression *>::Tag::Some)
            transform_handle.emplace(transform.some._0);

        auto * context = static_cast<TableSnapshot::Iterator *>(engine_context);
        if (context->shutdown)
        {
            LOG_TEST(
                context->log, "Callback: shutdown detected at first check");

            context->data_files_cv.notify_all();
            return false; /// Break iteration
        }

        if (context->list_batch_size > 0)
        {
            std::unique_lock lock(context->next_mutex);
            if (context->data_files.size() >= context->list_batch_size
                && !context->shutdown.load())
            {
                LOG_TEST(
                    context->log, "Callback pausing: queue size {}/{}",
                    context->data_files.size(), context->list_batch_size);

                context->schedule_next_batch_cv.wait(lock, [&]()
                {
                    return (context->data_files.size() < context->list_batch_size)
                        || context->shutdown.load();
                });
            }

            if (context->shutdown.load())
            {
                LOG_TEST(
                    context->log,
                    "Callback: shutdown detected after queue wait");

                context->data_files_cv.notify_all();
                return false; /// Break iteration
            }
        }

        ProfileEvents::increment(ProfileEvents::DeltaLakeScannedFiles);

        std::string full_path = DB::resolvePathInsideTable(
            context->getDataPath(), DB::unescapeForFileName(KernelUtils::fromDeltaString(path)));
        auto object = std::make_shared<DataObjectInfo>(std::move(full_path), size);
        object->data_lake_metadata.emplace();

        LOG_TEST(
            context->log,
            "Scanned file: {}, size: {}, num records: {}",
            object->getPath(), size, stats ? DB::toString(stats->num_records) : "Unknown");

        {
            std::lock_guard lock(context->next_mutex);
            context->data_files.emplace_back(std::move(object), std::move(dv_info_handle), std::move(transform_handle));
        }

        context->total_data_files += 1;
        context->total_bytes += size;
        if (stats && context->total_rows.has_value())
            context->total_rows.value() += stats->num_records;
        else
            context->total_rows = std::nullopt;

        context->data_files_cv.notify_one();
        return true;  /// Continue iteration
    }

private:
    using KernelScan = KernelPointerWrapper<ffi::SharedScan, ffi::free_scan>;
    using KernelScanDataIterator = KernelPointerWrapper<ffi::SharedScanMetadataIterator, ffi::free_scan_metadata_iter>;

    std::shared_ptr<KernelSnapshotState> kernel_snapshot_state;
    /// Fingerprint of the credentials embedded in `kernel_snapshot_state` at build time.
    /// Used by the retry path to detect SDK-side auto-refresh of rotating S3 providers
    /// (assume-role, web-identity, IMDS) — those have no `credentials_refresh_callback`,
    /// but `helper->getCredentialsFingerprint()` re-reads from the live S3 client and
    /// reflects any refresh the SDK performed transparently between attempts.
    DB::UInt128 captured_credentials_fingerprint;
    KernelScan scan;
    KernelScanDataIterator scan_data_iterator;
    std::optional<PartitionPruner> pruner;
    std::optional<DB::ActionsDAG> filter;

    KernelHelperPtr helper;
    DB::NamesAndTypesList read_schema;
    DB::NamesAndTypesList expression_schema;
    DB::Names partition_columns;
    /// Logical to physical column names, empty when column mapping is disabled.
    const DB::NameToNameMap physical_names_map;
    const DB::IDataLakeMetadata::FileProgressCallback callback;
    const size_t list_batch_size;
    const LoggerPtr log;
    const bool enable_expression_visitor_logging;
    const bool throw_on_engine_predicate_error;
    const bool enable_engine_predicate;
    const bool enable_clustering_predicate_fallback;
    const UpdateStatsFunc update_stats_func;

    std::exception_ptr scan_exception;
    std::exception_ptr engine_predicate_exception;

    /// Whether scanDataFunc should stop scanning.
    /// Set in destructor.
    std::atomic<bool> shutdown = false;
    /// A CV to notify that new data_files are available.
    std::condition_variable data_files_cv;
    /// A flag meaning that all data files were scanned
    /// and data scanning thread is finished.
    bool iterator_finished = false;

    std::optional<size_t> total_rows = 0;
    size_t total_bytes = 0;
    size_t total_data_files = 0;

    /// A CV to notify data scanning thread to continue,
    /// as current data batch is fully read.
    std::condition_variable schedule_next_batch_cv;

    std::deque<ScannedDataFile> data_files;
    std::mutex next_mutex;

    /// A thread for async data scanning.
    ThreadFromGlobalPool thread;
};

TableSnapshot::TableSnapshot(
    std::optional<size_t> version_,
    KernelHelperPtr helper_,
    DB::ObjectStoragePtr object_storage_,
    LoggerPtr log_)
    : helper(helper_)
    , object_storage(object_storage_)
    , log(log_)
    , snapshot_version_to_read(version_)
{
    chassert(object_storage);
}

size_t TableSnapshot::getVersion() const
{
    std::lock_guard lock(mutex);
    return getVersionUnlocked();
}

size_t TableSnapshot::getVersionUnlocked() const
{
    return getKernelSnapshotState()->snapshot_version;
}

TableSnapshot::SnapshotStats TableSnapshot::getSnapshotStats() const
{
    if (snapshot_stats.has_value())
        return snapshot_stats.value();

    const auto pre_fingerprint = kernel_state_credentials_fingerprint;
    try
    {
        snapshot_stats = getSnapshotStatsImpl();
    }
    catch (const DB::Exception & e)
    {
        if (!tryRefreshAfterStaleTokenError(e, pre_fingerprint, "stats scan"))
            throw;
        /// Invalidate the cached engine so `initOrUpdateSnapshot` rebuilds with the
        /// freshened credentials, then re-run the stats scan against the new engine.
        kernel_snapshot_state.reset();
        initOrUpdateSnapshot();
        snapshot_stats = getSnapshotStatsImpl();
    }
    LOG_TEST(
        log, "Updated statistics for snapshot version {}",
        getVersionUnlocked());
    return snapshot_stats.value();
}

TableSnapshot::SnapshotStats TableSnapshot::getSnapshotStatsImpl() const
{
    auto state = getKernelSnapshotState();

    KernelScan fallback_scan;
    fallback_scan = KernelUtils::unwrapResult(
        ffi::scan(
            state->snapshot.get(),
            state->engine.get(),
            /* predicate */nullptr,
            /* schema */nullptr),
        "scan");

    KernelScanMetadataIterator fallback_scan_data_iterator;
    fallback_scan_data_iterator = KernelUtils::unwrapResult(
        ffi::scan_metadata_iter_init(
            state->engine.get(), fallback_scan.get()),
        "scan_metadata_iter_init");

    struct StatsVisitor
    {
        explicit StatsVisitor(ffi::SharedExternEngine * engine_) : engine(engine_) {}

        ffi::SharedExternEngine * const engine;
        size_t total_data_files = 0;
        size_t total_bytes = 0;
        /// Not all writers add rows count to metadata
        std::optional<size_t> total_rows = 0;
        /// Set when `visitData` catches an exception; rethrown by the caller after
        /// `scan_metadata_next` returns (the callback runs inside a Rust `extern "C"` frame).
        std::exception_ptr exception;

        static bool visit(
            ffi::NullableCvoid engine_context,
            struct ffi::KernelStringSlice /* path */,
            int64_t size,
            int64_t /* mod_time */,
            const ffi::Stats * stats,
            ffi::SharedDvInfo * dv_info,
            ffi::OptionalValue<ffi::SharedExpression *> transform,
            const struct ffi::CStringMap * /* deprecated */)
        {
            /// Wrap handles in RAII immediately to ensure cleanup on any exit path
            /// TODO: Actually we do not need any transforms/dv_info to exist here,
            /// so it would be better to implement in delta-kernel scanCallback
            /// which will only collect stats.
            KernelDvInfo dv_info_handle(dv_info);
            std::optional<KernelExpression> transform_handle;
            if (transform.tag == ffi::OptionalValue<ffi::SharedExpression *>::Tag::Some)
                transform_handle.emplace(transform.some._0);

            auto * visitor = static_cast<StatsVisitor *>(engine_context);
            visitor->total_data_files += 1;
            visitor->total_bytes += static_cast<size_t>(size);
            if (stats && visitor->total_rows.has_value())
                visitor->total_rows.value() += stats->num_records;
            else
                visitor->total_rows = std::nullopt;
            return true;
        }

        static void visitData(void * engine_context, ffi::SharedScanMetadata * scan_metadata)
        {
            auto * visitor = static_cast<StatsVisitor *>(engine_context);
            /// Release the handle on all exit paths to avoid leaking it.
            SCOPE_EXIT({
                ffi::free_scan_metadata(scan_metadata);
            });
            /// Runs inside Rust's `scan_metadata_next`: a C++ exception must not cross the
            /// `extern "C"` frame, so store it and let the caller rethrow after the call returns.
            try
            {
                KernelUtils::unwrapResult(
                    ffi::visit_scan_metadata(
                        scan_metadata,
                        visitor->engine,
                        engine_context,
                        StatsVisitor::visit),
                    "visit_scan_metadata");
            }
            catch (...)
            {
                visitor->exception = std::current_exception();
            }
        }
    };

    StatsVisitor visitor(state->engine.get());

    while (true)
    {
        bool have_scan_data = KernelUtils::unwrapResult(
            ffi::scan_metadata_next(
                fallback_scan_data_iterator.get(),
                &visitor,
                StatsVisitor::visitData),
            "scan_metadata_next");

        /// Rethrow only now that `scan_metadata_next` has returned to C++ (see `visitData`).
        if (visitor.exception)
            std::rethrow_exception(visitor.exception);

        if (!have_scan_data)
            break;
    }

    LOG_TEST(
        log, "Snapshot at version {} data files: {}, total rows: {}, total bytes: {}",
        state->snapshot_version,
        visitor.total_data_files,
        visitor.total_rows ? DB::toString(*visitor.total_rows) : "Unknown",
        visitor.total_bytes);

    return SnapshotStats{
        .total_bytes = visitor.total_bytes,
        /// total_rows is an optional statistic, but total_bytes is obligatory.
        .total_rows = visitor.total_rows,
    };
}

std::optional<size_t> TableSnapshot::getTotalRows() const
{
    std::lock_guard lock(mutex);
    return getSnapshotStats().total_rows;
}

std::optional<size_t> TableSnapshot::getTotalBytes() const
{
    std::lock_guard lock(mutex);
    return getSnapshotStats().total_bytes;
}

bool TableSnapshot::tryRefreshAfterStaleTokenError(
    const DB::Exception & e,
    DB::UInt128 pre_fingerprint,
    const char * context_for_log) const
{
    if (!isStaleCredentialsError(e))
        return false;
    const auto & msg = e.message();

    const bool refreshed_via_callback = helper->refreshCredentials();
    const auto post_fingerprint = helper->getCredentialsFingerprint();
    const bool fingerprint_drifted = post_fingerprint != pre_fingerprint;
    if (!refreshed_via_callback && !fingerprint_drifted)
        return false;

    LOG_INFO(
        log,
        "Delta kernel reported stale credentials during {}; rebuilding "
        "(refreshed via callback: {}, fingerprint drifted: {}). Original error: {}",
        context_for_log, refreshed_via_callback, fingerprint_drifted, msg);
    return true;
}

void TableSnapshot::initOrUpdateSnapshot() const
{
    /// Rebuild when credentials rotate so the engine never outlives its embedded STS token.
    const auto current_credentials_fingerprint = helper->getCredentialsFingerprint();
    if (kernel_snapshot_state && current_credentials_fingerprint == kernel_state_credentials_fingerprint)
        return;

    /// Pin rebuilds to the already-resolved version so cached latest snapshots don't drift.
    const std::optional<size_t> version_to_build = kernel_snapshot_state
        ? std::optional<size_t>(kernel_snapshot_state->snapshot_version)
        : snapshot_version_to_read;

    ProfileEvents::increment(ProfileEvents::DeltaLakeSnapshotInitializations);

    LOG_TEST(
        log, "{}",
        kernel_snapshot_state ? "Rebuilding kernel snapshot state (credentials rotated)" : "Initializing snapshot");

    try
    {
        kernel_snapshot_state = std::make_shared<KernelSnapshotState>(*helper, version_to_build);
    }
    catch (const DB::Exception & e)
    {
        if (!tryRefreshAfterStaleTokenError(e, current_credentials_fingerprint, "snapshot init"))
            throw;
        kernel_snapshot_state = std::make_shared<KernelSnapshotState>(*helper, version_to_build);
    }
    kernel_state_credentials_fingerprint = helper->getCredentialsFingerprint();

    LOG_TRACE(
        log, "Initialized snapshot. Snapshot version: {}",
        kernel_snapshot_state->snapshot_version);
}

std::shared_ptr<TableSnapshot::KernelSnapshotState> TableSnapshot::getKernelSnapshotState() const
{
    initOrUpdateSnapshot();
    return kernel_snapshot_state;
}

TableSnapshot::KernelSnapshotState::KernelSnapshotState(const IKernelHelper & helper_, std::optional<size_t> snapshot_version_)
{
    fiu_do_on(DB::FailPoints::delta_kernel_force_stale_token_error,
    {
        throw DB::Exception(
            DB::ErrorCodes::DELTA_KERNEL_ERROR,
            "ExpiredToken: forced by delta_kernel_force_stale_token_error failpoint");
    });

    auto * engine_builder = helper_.createBuilder();
    engine = KernelUtils::unwrapResult(ffi::builder_build(engine_builder), "builder_build");

    using KernelSnapshotBuilder = KernelPointerWrapper<ffi::MutableFfiSnapshotBuilder, ffi::free_snapshot_builder>;
    KernelSnapshotBuilder snapshot_builder(KernelUtils::unwrapResult(
        ffi::get_snapshot_builder(
            KernelUtils::toDeltaString(helper_.getTableLocation()),
            engine.get()),
        "get_snapshot_builder"));
    if (snapshot_version_.has_value())
    {
        auto * builder_handle = snapshot_builder.get();
        ffi::snapshot_builder_set_version(&builder_handle, snapshot_version_.value());
    }
    /// `snapshot_builder_build` consumes the handle, so release() prevents the RAII destructor
    /// from double-freeing on success. The destructor still frees on early exception paths.
    snapshot = KernelUtils::unwrapResult(
        ffi::snapshot_builder_build(snapshot_builder.release()),
        "snapshot_builder_build");

    snapshot_version = ffi::version(snapshot.get());
    scan = KernelUtils::unwrapResult(
        ffi::scan(snapshot.get(), engine.get(), /* predicate */{}, /* engine_schema */nullptr),
        "scan");
}

DB::ObjectIterator TableSnapshot::iterate(
    const DB::ActionsDAG * filter_dag,
    DB::IDataLakeMetadata::FileProgressCallback callback,
    size_t list_batch_size,
    DB::ContextPtr context)
{
    const auto & settings = context->getSettingsRef();
    std::lock_guard lock(mutex);
    initOrUpdateSchemaIfChanged();
    auto state = getKernelSnapshotState();
    auto update_stats_func = [self = shared_from_this(), version = state->snapshot_version, this]
        (SnapshotStats && stats)
        {
            std::unique_lock lk(mutex, std::defer_lock);
            if (lk.try_lock())
            {
                if (!snapshot_stats.has_value())
                {
                    snapshot_stats.emplace(std::move(stats));
                    LOG_TEST(
                        log, "Updated statistics from data files iterator for snapshot version {}",
                        version);
                }
            }
        };
    return std::make_shared<TableSnapshot::Iterator>(
        state,
        helper,
        schema->read_schema,
        schema->table_schema,
        schema->physical_names_map,
        schema->partition_columns,
        filter_dag,
        callback,
        list_batch_size,
        settings[DB::Setting::delta_lake_enable_expression_visitor_logging],
        settings[DB::Setting::delta_lake_throw_on_engine_predicate_error],
        settings[DB::Setting::delta_lake_enable_engine_predicate],
        context,
        settings[DB::Setting::delta_lake_clustering_predicate_fallback],
        std::move(update_stats_func),
        log);
}

void TableSnapshot::initOrUpdateSchemaIfChanged() const
{
    if (!schema.has_value())
    {
        auto state = getKernelSnapshotState();
        auto [table_schema, physical_names_map] = getTableSchemaFromSnapshot(state->snapshot.get(), state->engine.get());

        if (table_schema.empty())
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "Table schema cannot be empty");

        auto read_schema = getReadSchemaFromSnapshot(state->scan.get(), state->engine.get());
        auto partition_columns = getPartitionColumnsFromSnapshot(state->snapshot.get());

        /// Both names are logical here; the rename to physical names happens later, on copies.
        for (const auto & column_name : partition_columns)
            if (!table_schema.tryGetByName(column_name))
                throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                    "Partition column {} is not present in the table schema", column_name);

        LOG_TRACE(
            log, "Table logical schema: {}, read schema: {}, "
            "partition columns: {}, physical names map size: {}",
            fmt::join(table_schema.getNames(), ", "),
            fmt::join(read_schema.getNames(), ", "),
            fmt::join(partition_columns, ", "),
            physical_names_map.size());

        schema.emplace(SchemaInfo{
            .table_schema = std::move(table_schema),
            .read_schema = std::move(read_schema),
            .physical_names_map = std::move(physical_names_map),
            .partition_columns = std::move(partition_columns),
        });
    }
}

const DB::NamesAndTypesList & TableSnapshot::getTableSchema() const
{
    std::lock_guard lock(mutex);
    initOrUpdateSchemaIfChanged();
    return schema->table_schema;
}

const DB::NamesAndTypesList & TableSnapshot::getReadSchema() const
{
    std::lock_guard lock(mutex);
    initOrUpdateSchemaIfChanged();
    return schema->read_schema;
}

const DB::Names & TableSnapshot::getPartitionColumns() const
{
    std::lock_guard lock(mutex);
    initOrUpdateSchemaIfChanged();
    return schema->partition_columns;
}

const DB::NameToNameMap & TableSnapshot::getPhysicalNamesMap() const
{
    std::lock_guard lock(mutex);
    initOrUpdateSchemaIfChanged();
    return schema->physical_names_map;
}

}

#endif
