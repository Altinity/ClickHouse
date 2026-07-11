#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStagingSweeper.h>
#include <Common/logger_useful.h>

namespace DB::Cas
{

void sweepOwnMountStaging(IObjectStorage & object_storage, const String & mount_staging_prefix) noexcept
{
    try
    {
        /// max_keys=0 asks `listObjects` for the FULL listing under the prefix (it paginates until
        /// exhausted rather than capping at some default page size) — see `IObjectStorage::listObjects`.
        /// A mount's own staging debris is bounded (one mount's in-flight + leaked uploads), so a single
        /// unbounded LIST at startup is acceptable; unlike GC's per-round budgets, this runs once per
        /// mount, not on a recurring schedule.
        RelativePathsWithMetadata children;
        object_storage.listObjects(mount_staging_prefix, children, /*max_keys=*/0);

        size_t removed = 0;
        for (const auto & child : children)
        {
            try
            {
                object_storage.removeObjectIfExists(StoredObject(child->relative_path));
                ++removed;
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
                /// Best-effort: one stubborn key must not abort the sweep of the rest — it is retried
                /// by a later mount's sweep.
            }
        }

        if (removed)
            LOG_INFO(getLogger("CasStagingSweeper"),
                "Reclaimed {} leaked S3 staging object(s) under '{}' at mount start",
                removed, mount_staging_prefix);
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
        /// Best-effort: a LIST failure (a transient backend hiccup) at mount time must never fail the
        /// mount — any leaked staging objects are bounded debris, reclaimed by a later mount's sweep.
    }
}

}
