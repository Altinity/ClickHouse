#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobIntentFormat.h>

namespace DB::Cas
{

const String & blobIntentRecord()
{
    static const String record = "{\"type\":\"cas_blob_intent\",\"v\":4}\n{}\n";
    return record;
}

bool isBlobIntentRecord(std::string_view bytes)
{
    return bytes == blobIntentRecord();
}

}
