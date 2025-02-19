#include <Processors/Formats/Impl/ParquetFileMetaDataCache.h>

#ifdef USE_PARQUET

namespace DB
{

ParquetFileMetaDataCache::ParquetFileMetaDataCache()
    : CacheBase<String, parquet::FileMetaData>(0)
{}

ParquetFileMetaDataCache * ParquetFileMetaDataCache::instance()
{
    static ParquetFileMetaDataCache instance;
    return &instance;
}

}

#endif
