#include <Storages/MergeTree/MergeTreeExportTTLIndex.h>

#include <Core/Defines.h>
#include <Disks/IDisk.h>
#include <IO/ReadBufferFromFileBase.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteHelpers.h>
#include <Interpreters/Context.h>
#include <Storages/StorageMergeTree.h>
#include <Common/ProfileEvents.h>
#include <Common/escapeForFileName.h>
#include <Common/logger_useful.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace ProfileEvents
{
    extern const Event ExportTTLIndexSnapshotRefreshes;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
}

MergeTreeExportTTLIndex::MergeTreeExportTTLIndex(StorageMergeTree & storage_)
    : storage(storage_)
    , snapshot(ExportTTLIndexSnapshot::build(/* version */ -1, {}))
{
}

String MergeTreeExportTTLIndex::getRootPath() const
{
    return fs::path(storage.getRelativeDataPath()) / "export_ttl";
}

String MergeTreeExportTTLIndex::getDestinationPath(const String & destination_key) const
{
    return fs::path(getRootPath()) / destination_key;
}

String MergeTreeExportTTLIndex::getEntryPath(const String & destination_key, const String & partition_id) const
{
    return fs::path(getDestinationPath(destination_key)) / "partitions" / (escapeForFileName(partition_id) + ".json");
}

void MergeTreeExportTTLIndex::load()
{
    auto disk = storage.getDisks().front();
    const auto root_path = getRootPath();

    std::lock_guard lock(mutex);
    entries.clear();

    if (disk->existsDirectory(root_path))
    {
        for (auto destination_it = disk->iterateDirectory(root_path); destination_it->isValid(); destination_it->next())
        {
            const auto destination_key = destination_it->name();
            auto & by_partition = entries[destination_key];

            const auto partitions_path = fs::path(root_path) / destination_key / "partitions";
            if (!disk->existsDirectory(partitions_path))
                continue;

            for (auto it = disk->iterateDirectory(partitions_path); it->isValid(); it->next())
            {
                const auto file_name = it->name();
                /// Skip temporary files left by an interrupted write.
                if (!file_name.ends_with(".json"))
                    continue;

                const auto partition_id = unescapeForFileName(file_name.substr(0, file_name.size() - strlen(".json")));
                const auto path = fs::path(partitions_path) / file_name;

                /// Losing an entry would export its parts again, so an unreadable one fails the table load.
                String content;
                try
                {
                    auto in = disk->readFile(path, getReadSettings());
                    readStringUntilEOF(content, *in);
                    by_partition[partition_id] = ExportTTLVersionedEntry{
                        .entry = ExportTTLIndexEntry::fromJSONString(partition_id, content),
                        .version = 0,
                    };
                }
                catch (Exception & e)
                {
                    e.addMessage("While loading the TTL export index entry {}", path.string());
                    throw;
                }
                catch (...)
                {
                    throw Exception(ErrorCodes::INCORRECT_DATA, "Cannot load the TTL export index entry {}: {}",
                        path.string(), getCurrentExceptionMessage(false));
                }
            }
        }
    }

    publishSnapshot();
}

bool MergeTreeExportTTLIndex::updateEntry(const String & destination_key, const ExportTTLVersionedEntry & versioned)
{
    std::lock_guard lock(mutex);

    auto & by_partition = entries[destination_key];
    const auto it = by_partition.find(versioned.entry.partition_id);
    const int32_t stored_version = it == by_partition.end() ? -1 : it->second.version;
    if (stored_version != versioned.version)
        return false;

    auto disk = storage.getDisks().front();
    const auto path = getEntryPath(destination_key, versioned.entry.partition_id);
    const auto tmp_path = path + ".tmp";
    disk->createDirectories(fs::path(path).parent_path());

    {
        auto out = disk->writeFile(tmp_path, DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Rewrite, storage.getContext()->getWriteSettings());
        writeString(versioned.entry.toJSONString(), *out);
        out->finalize();
        /// The entry must survive a crash before the task it claims for is created or completed.
        out->sync();
    }
    disk->replaceFile(tmp_path, path);

    by_partition[versioned.entry.partition_id] = ExportTTLVersionedEntry{.entry = versioned.entry, .version = stored_version + 1};
    publishSnapshot();
    return true;
}

void MergeTreeExportTTLIndex::removeDestination(const String & destination_key)
{
    std::lock_guard lock(mutex);

    auto disk = storage.getDisks().front();
    const auto path = getDestinationPath(destination_key);
    if (disk->existsDirectory(path))
        disk->removeRecursive(path);

    entries.erase(destination_key);
    publishSnapshot();

    LOG_INFO(storage.log, "Removed the TTL export index of destination {}", destination_key);
}

Int64 MergeTreeExportTTLIndex::maxBlock() const
{
    std::lock_guard lock(mutex);
    Int64 result = 0;
    for (const auto & [_, by_partition] : entries)
        for (const auto & partition_entry : by_partition)
            result = std::max(result, partition_entry.second.entry.maxBlock());
    return result;
}

void MergeTreeExportTTLIndex::publishSnapshot()
{
    ProfileEvents::increment(ProfileEvents::ExportTTLIndexSnapshotRefreshes);
    snapshot.set(ExportTTLIndexSnapshot::build(/* version */ -1, entries));
}

}
