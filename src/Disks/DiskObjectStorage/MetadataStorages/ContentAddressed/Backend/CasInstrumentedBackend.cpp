#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.h>

namespace ProfileEvents
{
/// The CA per-namespace and per-operation events declared in `ProfileEvents.cpp`.
extern const Event CASBlobPut;
extern const Event CASBlobPutDeduplicated;
extern const Event CASBlobOverwrite;
extern const Event CASBlobCompareSwap;
extern const Event CASBlobCompareSwapConflict;
extern const Event CASBlobHead;
extern const Event CASBlobHeadMiss;
extern const Event CASBlobGet;
extern const Event CASBlobGetStream;
extern const Event CASBlobDelete;
extern const Event CASBlobList;

extern const Event CASBulkDeleteRequests;

extern const Event CASManifestPut;
extern const Event CASManifestPutDeduplicated;
extern const Event CASManifestOverwrite;
extern const Event CASManifestCompareSwap;
extern const Event CASManifestCompareSwapConflict;
extern const Event CASManifestHead;
extern const Event CASManifestHeadMiss;
extern const Event CASManifestGet;
extern const Event CASManifestGetStream;
extern const Event CASManifestDelete;
extern const Event CASManifestList;

extern const Event CASRootPut;
extern const Event CASRootPutDeduplicated;
extern const Event CASRootOverwrite;
extern const Event CASRootCompareSwap;
extern const Event CASRootCompareSwapConflict;
extern const Event CASRootHead;
extern const Event CASRootHeadMiss;
extern const Event CASRootGet;
extern const Event CASRootGetStream;
extern const Event CASRootDelete;
extern const Event CASRootList;

extern const Event CASGCPut;
extern const Event CASGCPutDeduplicated;
extern const Event CASGCOverwrite;
extern const Event CASGCCompareSwap;
extern const Event CASGCCompareSwapConflict;
extern const Event CASGCHead;
extern const Event CASGCHeadMiss;
extern const Event CASGCGet;
extern const Event CASGCGetStream;
extern const Event CASGCDelete;
extern const Event CASGCList;

extern const Event CASServerPut;
extern const Event CASServerPutDeduplicated;
extern const Event CASServerOverwrite;
extern const Event CASServerCompareSwap;
extern const Event CASServerCompareSwapConflict;
extern const Event CASServerHead;
extern const Event CASServerHeadMiss;
extern const Event CASServerGet;
extern const Event CASServerGetStream;
extern const Event CASServerDelete;
extern const Event CASServerList;

extern const Event CASOtherPut;
extern const Event CASOtherPutDeduplicated;
extern const Event CASOtherOverwrite;
extern const Event CASOtherCompareSwap;
extern const Event CASOtherCompareSwapConflict;
extern const Event CASOtherHead;
extern const Event CASOtherHeadMiss;
extern const Event CASOtherGet;
extern const Event CASOtherGetStream;
extern const Event CASOtherDelete;
extern const Event CASOtherList;

extern const Event CASBlobIntentGet;
extern const Event CASBlobIntentPut;
extern const Event CASBlobIntentDelete;
extern const Event CASBlobIntentList;
}

namespace DB::Cas
{

/// Maps `(CasNs, CasOp)` to the corresponding `ProfileEvents::Event`. The table is row-major: the
/// outer index is the namespace and the inner index is the operation. Its rows and columns must stay
/// in lockstep with the `CasNs` and `CasOp` enum orderings.
///
/// `CasOp::Read` deliberately keeps the `CAS*Get` event names: renaming a user-visible ProfileEvent
/// is its own change, made when the events are retired, and nothing outside this table would have
/// been improved by doing it here.
static const ProfileEvents::Event cas_event_table[CAS_NS_COUNT][CAS_OP_COUNT] =
{
    /* Blob   */ {ProfileEvents::CASBlobPut, ProfileEvents::CASBlobPutDeduplicated, ProfileEvents::CASBlobOverwrite,
                  ProfileEvents::CASBlobCompareSwap, ProfileEvents::CASBlobCompareSwapConflict, ProfileEvents::CASBlobHead,
                  ProfileEvents::CASBlobHeadMiss, ProfileEvents::CASBlobGet, ProfileEvents::CASBlobGetStream,
                  ProfileEvents::CASBlobDelete, ProfileEvents::CASBlobList},
    /* Manifest */ {ProfileEvents::CASManifestPut, ProfileEvents::CASManifestPutDeduplicated, ProfileEvents::CASManifestOverwrite,
                  ProfileEvents::CASManifestCompareSwap, ProfileEvents::CASManifestCompareSwapConflict, ProfileEvents::CASManifestHead,
                  ProfileEvents::CASManifestHeadMiss, ProfileEvents::CASManifestGet, ProfileEvents::CASManifestGetStream,
                  ProfileEvents::CASManifestDelete, ProfileEvents::CASManifestList},
    /* Root   */ {ProfileEvents::CASRootPut, ProfileEvents::CASRootPutDeduplicated, ProfileEvents::CASRootOverwrite,
                  ProfileEvents::CASRootCompareSwap, ProfileEvents::CASRootCompareSwapConflict, ProfileEvents::CASRootHead,
                  ProfileEvents::CASRootHeadMiss, ProfileEvents::CASRootGet, ProfileEvents::CASRootGetStream,
                  ProfileEvents::CASRootDelete, ProfileEvents::CASRootList},
    /* Gc     */ {ProfileEvents::CASGCPut, ProfileEvents::CASGCPutDeduplicated, ProfileEvents::CASGCOverwrite,
                  ProfileEvents::CASGCCompareSwap, ProfileEvents::CASGCCompareSwapConflict, ProfileEvents::CASGCHead,
                  ProfileEvents::CASGCHeadMiss, ProfileEvents::CASGCGet, ProfileEvents::CASGCGetStream,
                  ProfileEvents::CASGCDelete, ProfileEvents::CASGCList},
    /* Server */ {ProfileEvents::CASServerPut, ProfileEvents::CASServerPutDeduplicated, ProfileEvents::CASServerOverwrite,
                  ProfileEvents::CASServerCompareSwap, ProfileEvents::CASServerCompareSwapConflict, ProfileEvents::CASServerHead,
                  ProfileEvents::CASServerHeadMiss, ProfileEvents::CASServerGet, ProfileEvents::CASServerGetStream,
                  ProfileEvents::CASServerDelete, ProfileEvents::CASServerList},
    /* Other  */ {ProfileEvents::CASOtherPut, ProfileEvents::CASOtherPutDeduplicated, ProfileEvents::CASOtherOverwrite,
                  ProfileEvents::CASOtherCompareSwap, ProfileEvents::CASOtherCompareSwapConflict, ProfileEvents::CASOtherHead,
                  ProfileEvents::CASOtherHeadMiss, ProfileEvents::CASOtherGet, ProfileEvents::CASOtherGetStream,
                  ProfileEvents::CASOtherDelete, ProfileEvents::CASOtherList},
    /* BlobIntent */ {ProfileEvents::CASBlobIntentPut, ProfileEvents::CASBlobIntentPut, ProfileEvents::CASOtherOverwrite,
                  ProfileEvents::CASOtherCompareSwap, ProfileEvents::CASOtherCompareSwapConflict, ProfileEvents::CASOtherHead,
                  ProfileEvents::CASOtherHeadMiss, ProfileEvents::CASBlobIntentGet, ProfileEvents::CASOtherGetStream,
                  ProfileEvents::CASBlobIntentDelete, ProfileEvents::CASBlobIntentList},
};

CasNs classifyCasNs(const String & key, std::string_view pool_prefix)
{
    std::string_view rest = key;
    if (!rest.starts_with(pool_prefix) || !rest.substr(pool_prefix.size()).starts_with('/'))
        return CasNs::Other;
    rest.remove_prefix(pool_prefix.size() + 1);
    const auto component = [&rest]
    {
        const std::string_view first = rest.substr(0, rest.find('/'));
        rest.remove_prefix(std::min(rest.size(), first.size() + 1));
        return first;
    };
    const std::string_view first = component();
    if (first == "blob-intents")
        return CasNs::BlobIntent;
    if (first == "blobs")
        return CasNs::Blob;
    if (first == "cas")
    {
        /// Ref streams and namespace-owned state live under `cas/ns/`, part manifests under `cas/manifests/`.
        const std::string_view second = component();
        if (second == "ns")
            return CasNs::Root;
        if (second == "manifests")
            return CasNs::Manifest;
        return CasNs::Other;
    }
    if (first == "roots")
        return CasNs::Root;
    if (first == "gc")
        return CasNs::Gc;
    return CasNs::Other;
}

void incrementCasEvent(CasNs ns, CasOp op)
{
    ProfileEvents::increment(cas_event_table[static_cast<size_t>(ns)][static_cast<size_t>(op)]);
}

void InstrumentedBackend::publish(const BlobPublishRequest & request, TransportAccess & access)
{
    inner->publish(request, access);
    incrementCasEvent(classifyCasNs(request.destination_key, pool_prefix), CasOp::Put);
}

void InstrumentedBackend::removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access)
{
    inner->removeManyWriteOnce(keys, access);
    ProfileEvents::increment(ProfileEvents::CASBulkDeleteRequests);
    for (const WriteOnceKey & key : keys)
        incrementCasEvent(classifyCasNs(key.str(), pool_prefix), CasOp::Delete);
}

std::vector<Backend::RawBatchRemoveResult> InstrumentedBackend::removeManyByConditional(
    const std::vector<RawConditionalRemove> & removals, TransportAccess & access)
{
    std::vector<RawBatchRemoveResult> results = inner->removeManyByConditional(removals, access);
    for (size_t i = 0; i < std::min(removals.size(), results.size()); ++i)
        if (results[i])
            incrementCasEvent(classifyCasNs(removals[i].key, pool_prefix), CasOp::Delete);
    return results;
}

}
