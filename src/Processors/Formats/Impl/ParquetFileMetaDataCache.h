#pragma once

#include "config.h"

#if USE_PARQUET

#include <parquet/metadata.h>
#include <Common/CacheBase.h>

namespace DB
{

class ParquetFileMetaDataCache : public CacheBase<String, parquet::FileMetaData>
{
public:
    static ParquetFileMetaDataCache * instance();

private:
    ParquetFileMetaDataCache();
};

}

#endif
