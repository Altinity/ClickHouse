#include <Storages/ObjectStorage/StorageObjectStorageStableTaskDistributor.h>
#include <Storages/ObjectStorage/Utils.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Utils.h>
#include <Common/SipHash.h>
#include <consistent_hashing.h>
#include <optional>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

StorageObjectStorageStableTaskDistributor::StorageObjectStorageStableTaskDistributor(
    std::shared_ptr<IObjectIterator> iterator_,
    std::vector<std::string> && ids_of_nodes_,
    bool send_over_whole_archive_)
    : iterator(std::move(iterator_))
    , send_over_whole_archive(send_over_whole_archive_)
    , connection_to_files(ids_of_nodes_.size())
    , ids_of_nodes(std::move(ids_of_nodes_))
    , iterator_exhausted(false)
{
}

ObjectInfoPtr StorageObjectStorageStableTaskDistributor::getNextTask(size_t number_of_current_replica)
{
    LOG_TRACE(log, "Received request from replica {} looking for a file", number_of_current_replica);

    // 1. Check pre-queued files first
    if (auto file = getPreQueuedFile(number_of_current_replica))
        return file;

    // 2. Try to find a matching file from the iterator
    if (auto file = getMatchingFileFromIterator(number_of_current_replica))
        return file;

    // 3. Process unprocessed files if iterator is exhausted
    return getAnyUnprocessedFile(number_of_current_replica);
}

size_t StorageObjectStorageStableTaskDistributor::getReplicaForFile(const String & file_path)
{
    size_t nodes_count = ids_of_nodes.size();

    /// Trivial case
    if (nodes_count < 2)
        return 0;

    /// Rendezvous hashing
    size_t best_id = 0;
    UInt64 best_weight = sipHash64(ids_of_nodes[0] + file_path);
    for (size_t id = 1; id < nodes_count; ++id)
    {
        UInt64 weight = sipHash64(ids_of_nodes[id] + file_path);
        if (weight > best_weight)
        {
            best_weight = weight;
            best_id = id;
        }
    }
    return best_id;
}

ObjectInfoPtr StorageObjectStorageStableTaskDistributor::getPreQueuedFile(size_t number_of_current_replica)
{
    std::lock_guard lock(mutex);

    if (connection_to_files.size() <= number_of_current_replica)
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Replica number {} is out of range. Expected range: [0, {})",
            number_of_current_replica,
            connection_to_files.size()
        );

    auto & files = connection_to_files[number_of_current_replica];

    while (!files.empty())
    {
        auto next_file = files.back();
        files.pop_back();

        auto file_identifier = send_over_whole_archive ? next_file->getPathOrPathToArchiveIfArchive() : next_file->getIdentifier();
        auto it = unprocessed_files.find(file_identifier);
        if (it == unprocessed_files.end())
            continue;

        unprocessed_files.erase(it);

        LOG_TRACE(
            log,
            "Assigning pre-queued file {} to replica {}",
            file_identifier,
            number_of_current_replica
        );

        return next_file;
    }

    return {};
}

ObjectInfoPtr StorageObjectStorageStableTaskDistributor::getMatchingFileFromIterator(size_t number_of_current_replica)
{
    {
        std::lock_guard lock(mutex);
        if (iterator_exhausted)
            return {};
    }

    while (true)
    {
        ObjectInfoPtr object_info;

        {
            std::lock_guard lock(mutex);
            object_info = iterator->next(0);

            if (!object_info)
            {
                LOG_TEST(log, "Iterator is exhausted");
                iterator_exhausted = true;
                break;
            }
        }

        String file_identifier;
        if (send_over_whole_archive && object_info->isArchive())
        {
            file_identifier = object_info->getPathOrPathToArchiveIfArchive();
            LOG_TEST(log, "Will send over the whole archive {} to replicas. "
                     "This will be suboptimal, consider turning on "
                     "cluster_function_process_archive_on_multiple_nodes setting", file_identifier);
        }
        else
        {
            file_identifier = object_info->getIdentifier();
        }

        size_t file_replica_idx = getReplicaForFile(file_identifier);
        if (file_replica_idx == number_of_current_replica)
        {
            LOG_TRACE(
                log, "Found file {} for replica {}",
                file_identifier, number_of_current_replica
            );

            return object_info;
        }
        LOG_TEST(
            log,
            "Found file {} for replica {} (number of current replica: {})",
            file_identifier,
            file_replica_idx,
            number_of_current_replica
        );

        // Queue file for its assigned replica
        {
            std::lock_guard lock(mutex);
            unprocessed_files.emplace(file_identifier, object_info);
            connection_to_files[file_replica_idx].push_back(object_info);
        }
    }

    return {};
}

ObjectInfoPtr StorageObjectStorageStableTaskDistributor::getAnyUnprocessedFile(size_t number_of_current_replica)
{
    std::lock_guard lock(mutex);

    if (!unprocessed_files.empty())
    {
        auto it = unprocessed_files.begin();
        auto next_file = it->second;
        unprocessed_files.erase(it);

        auto file_path = send_over_whole_archive ? next_file->getPathOrPathToArchiveIfArchive() : next_file->getPath();
        LOG_TRACE(
            log,
            "Iterator exhausted. Assigning unprocessed file {} to replica {}",
            file_path,
            number_of_current_replica
        );

        return next_file;
    }

    return {};
}

<<<<<<< HEAD
=======
void StorageObjectStorageStableTaskDistributor::saveLastNodeActivity(size_t number_of_current_replica)
{
    Poco::Timestamp now;
    std::lock_guard lock(mutex);
    last_node_activity[number_of_current_replica] = now;
}

void StorageObjectStorageStableTaskDistributor::rescheduleTasksFromReplica(size_t number_of_current_replica)
{
    LOG_INFO(log, "Replica {} is marked as lost, tasks are returned to queue", number_of_current_replica);
    std::lock_guard lock(mutex);

    auto processed_file_list_ptr = replica_to_files_to_be_processed.find(number_of_current_replica);
    if (processed_file_list_ptr == replica_to_files_to_be_processed.end())
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Replica number {} was marked as lost already",
            number_of_current_replica
        );

    if (replica_to_files_to_be_processed.size() < 2)
        throw Exception(
            ErrorCodes::CANNOT_READ_ALL_DATA,
            "All replicas were marked as lost"
        );

    auto files = std::move(processed_file_list_ptr->second);
    replica_to_files_to_be_processed.erase(number_of_current_replica);
    for (const auto & file : files)
    {
        auto file_identifier = getFileIdentifier(file);
        auto file_replica_idx = getReplicaForFile(file_identifier);
        unprocessed_files.emplace(file_identifier, std::make_pair(file, file_replica_idx));
        connection_to_files[file_replica_idx].push_back(file);
    }
}

String StorageObjectStorageStableTaskDistributor::getFileIdentifier(ObjectInfoPtr file_object, bool write_to_log) const
{
    if (send_over_whole_archive && file_object->isArchive())
    {
        auto file_identifier = file_object->getPathOrPathToArchiveIfArchive();
        if (write_to_log)
        {
            LOG_TEST(log, "Will send over the whole archive {} to replicas. "
                        "This will be suboptimal, consider turning on "
                        "cluster_function_process_archive_on_multiple_nodes setting", file_identifier);
        }
        return file_identifier;
    }
    /// Prefer the original Iceberg metadata path (possibly absolute / on another storage) when available,
    /// so the same file is identified consistently across replicas.
    return getMetadataPathFromObjectInfo(file_object).value_or(file_object->getIdentifier());
}

>>>>>>> 6a83f974ee6 (Merge pull request #1859 from Altinity/feat/antalya-26.3/90740)
}
