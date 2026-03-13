#include <Processors/Formats/Impl/ParquetFileMetaDataCache.h>

#if USE_PARQUET

#include <parquet/metadata.h>

namespace DB
{

size_t ParquetFileMetaDataWeightFunction::operator()(const parquet::FileMetaData & metadata) const
{
    /// TODO fix-me: using the size on disk is not ideal, but it is the simplest and best we can do for now.
    /// this implementation is only used by the v1 reader, which is going to be deprecated and a new implementation for the v3
    /// reader will be added in the future.
    return metadata.size();
}

ParquetFileMetaDataCache::ParquetFileMetaDataCache()
    : CacheBase<String, parquet::FileMetaData, std::hash<String>, ParquetFileMetaDataWeightFunction>(CurrentMetrics::end(), CurrentMetrics::end(), 0)
{}

ParquetFileMetaDataCache * ParquetFileMetaDataCache::instance()
{
    static ParquetFileMetaDataCache instance;
    return &instance;
}

}

#endif
