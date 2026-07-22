#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasSentinelProbe.h>

namespace DB::Cas
{

SentinelProbeResult probeSentinel(Backend & backend, const String & key)
{
    return backend.probeSentinelRaw(key);
}

SentinelProbeResult probePrefixEmptiness(Backend & backend, const String & pool_root_prefix)
{
    return backend.probePrefixEmptinessRaw(pool_root_prefix);
}

}
