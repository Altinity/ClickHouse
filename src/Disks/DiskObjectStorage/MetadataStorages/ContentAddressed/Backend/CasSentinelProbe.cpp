#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasSentinelProbe.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>

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

namespace
{

/// Is `key` structurally-valid `_probe/` capability-battery debris — i.e. an object strictly under the
/// reserved `<prefix>/_probe/` subtree? `runCapabilityProbe` (invoked with `<prefix>/_probe/<u128hex>` by
/// `Pool::open`) is the ONLY writer under `_probe/`; a content-addressed pool NEVER stores durable
/// data/control state there, so the whole subtree is ephemeral capability-probe scratch that a crash or a
/// concurrent fresh opener may leave behind ([D2]). Ignoring it can therefore never strand real data —
/// `pool_prefix` is exclusively CAS-owned. The trailing `/` in `probe_root` keeps a mere sibling
/// look-alike (`<prefix>/_probe`, `<prefix>/_probelike/…`) OUT of the reserved subtree, so it is still
/// treated as genuine residual and fails the bootstrap closed.
bool isProbeSubtreeDebris(const String & probe_root, const String & key)
{
    return key.starts_with(probe_root);
}

}

BootstrapResidual probePoolBootstrapResidual(Backend & backend, const Layout & layout)
{
    const String pool_meta_key = layout.poolMetaKey();
    const String prefix = layout.poolPrefix() + "/";
    const String probe_root = layout.poolPrefix() + "/_probe/";

    /// Classification is order-independent for correctness: every listed key is examined, and finding
    /// `_pool_meta` anywhere is decisive. It relies on lexicographic LIST order only for COST — `_pool_meta`
    /// sorts first under `<prefix>/`, so a healthy pool short-circuits on the first page rather than
    /// enumerating its whole content on every open.
    bool has_residual = false;
    try
    {
        String cursor;
        for (;;)
        {
            const ListPage page = backend.list(prefix, cursor, 1000);
            for (const ListedKey & listed : page.keys)
            {
                if (listed.key == pool_meta_key)
                    return BootstrapResidual::PoolMetaPresent;   /// decisive — the pool is authoritative
                if (isProbeSubtreeDebris(probe_root, listed.key))
                    continue;   /// crash leftover / concurrent opener's battery — ignore ([D2])
                has_residual = true;   /// a non-`_probe` object, and no `_pool_meta` seen (so far)
            }
            if (page.next_cursor.empty())
                break;
            cursor = page.next_cursor;
        }
    }
    catch (...)
    {
        /// The LIST failed: absence was never proven. Never mint a fresh identity under uncertainty —
        /// but log the swallowed error so the fail-closed startup refusal is diagnosable (the root cause
        /// must not vanish just because the verdict is "Indeterminate").
        LOG_WARNING(getLogger("CasBootstrap"),
            "Pool prefix '{}': the authoritative residual LIST failed; treating the bootstrap as "
            "Indeterminate (fail-closed, will refuse to mint _pool_meta): {}",
            prefix, getCurrentExceptionMessage(/*with_stacktrace=*/false));
        return BootstrapResidual::Indeterminate;
    }
    return has_residual ? BootstrapResidual::ResidualWithoutMeta : BootstrapResidual::EmptyOrProbeOnly;
}

}
