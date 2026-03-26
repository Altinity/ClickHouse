#pragma once

#include "config.h"

#if USE_PARQUET

namespace parquet
{

class FileMetaData;

}

#include <Common/CacheBase.h>

namespace DB
{

struct ParquetFileMetaDataWeightFunction
{
    size_t operator()(const parquet::FileMetaData & metadata) const;
};

class ParquetFileMetaDataCache : public CacheBase<String, parquet::FileMetaData, std::hash<String>, ParquetFileMetaDataWeightFunction>
{
public:
    static ParquetFileMetaDataCache * instance();

private:
    ParquetFileMetaDataCache();
};

}

#endif
