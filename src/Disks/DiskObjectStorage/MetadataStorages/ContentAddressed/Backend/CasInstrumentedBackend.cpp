#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.h>

namespace ProfileEvents
{
/// B168 P0: the CA per-namespace S3 op events (6 namespaces × 11 ops). Declared in ProfileEvents.cpp.
extern const Event CasBlobPut;
extern const Event CasBlobPutDedup;
extern const Event CasBlobOverwrite;
extern const Event CasBlobCas;
extern const Event CasBlobCasConflict;
extern const Event CasBlobHead;
extern const Event CasBlobHeadMiss;
extern const Event CasBlobGet;
extern const Event CasBlobGetStream;
extern const Event CasBlobDelete;
extern const Event CasBlobList;

extern const Event CasManifestPut;
extern const Event CasManifestPutDedup;
extern const Event CasManifestOverwrite;
extern const Event CasManifestCas;
extern const Event CasManifestCasConflict;
extern const Event CasManifestHead;
extern const Event CasManifestHeadMiss;
extern const Event CasManifestGet;
extern const Event CasManifestGetStream;
extern const Event CasManifestDelete;
extern const Event CasManifestList;

extern const Event CasRootPut;
extern const Event CasRootPutDedup;
extern const Event CasRootOverwrite;
extern const Event CasRootCas;
extern const Event CasRootCasConflict;
extern const Event CasRootHead;
extern const Event CasRootHeadMiss;
extern const Event CasRootGet;
extern const Event CasRootGetStream;
extern const Event CasRootDelete;
extern const Event CasRootList;

extern const Event CasGcPut;
extern const Event CasGcPutDedup;
extern const Event CasGcOverwrite;
extern const Event CasGcCas;
extern const Event CasGcCasConflict;
extern const Event CasGcHead;
extern const Event CasGcHeadMiss;
extern const Event CasGcGet;
extern const Event CasGcGetStream;
extern const Event CasGcDelete;
extern const Event CasGcList;

extern const Event CasServerPut;
extern const Event CasServerPutDedup;
extern const Event CasServerOverwrite;
extern const Event CasServerCas;
extern const Event CasServerCasConflict;
extern const Event CasServerHead;
extern const Event CasServerHeadMiss;
extern const Event CasServerGet;
extern const Event CasServerGetStream;
extern const Event CasServerDelete;
extern const Event CasServerList;

extern const Event CasOtherPut;
extern const Event CasOtherPutDedup;
extern const Event CasOtherOverwrite;
extern const Event CasOtherCas;
extern const Event CasOtherCasConflict;
extern const Event CasOtherHead;
extern const Event CasOtherHeadMiss;
extern const Event CasOtherGet;
extern const Event CasOtherGetStream;
extern const Event CasOtherDelete;
extern const Event CasOtherList;
}

namespace DB::Cas
{

/// (CasNs, CasOp) → ProfileEvents::Event. Row-major: outer index is the namespace, inner is the op.
/// Must stay in lockstep with the CasNs / CasOp enum orderings.
static const ProfileEvents::Event cas_event_table[CAS_NS_COUNT][CAS_OP_COUNT] =
{
    /* Blob   */ {ProfileEvents::CasBlobPut, ProfileEvents::CasBlobPutDedup, ProfileEvents::CasBlobOverwrite,
                  ProfileEvents::CasBlobCas, ProfileEvents::CasBlobCasConflict, ProfileEvents::CasBlobHead,
                  ProfileEvents::CasBlobHeadMiss, ProfileEvents::CasBlobGet, ProfileEvents::CasBlobGetStream,
                  ProfileEvents::CasBlobDelete, ProfileEvents::CasBlobList},
    /* Manifest */ {ProfileEvents::CasManifestPut, ProfileEvents::CasManifestPutDedup, ProfileEvents::CasManifestOverwrite,
                  ProfileEvents::CasManifestCas, ProfileEvents::CasManifestCasConflict, ProfileEvents::CasManifestHead,
                  ProfileEvents::CasManifestHeadMiss, ProfileEvents::CasManifestGet, ProfileEvents::CasManifestGetStream,
                  ProfileEvents::CasManifestDelete, ProfileEvents::CasManifestList},
    /* Root   */ {ProfileEvents::CasRootPut, ProfileEvents::CasRootPutDedup, ProfileEvents::CasRootOverwrite,
                  ProfileEvents::CasRootCas, ProfileEvents::CasRootCasConflict, ProfileEvents::CasRootHead,
                  ProfileEvents::CasRootHeadMiss, ProfileEvents::CasRootGet, ProfileEvents::CasRootGetStream,
                  ProfileEvents::CasRootDelete, ProfileEvents::CasRootList},
    /* Gc     */ {ProfileEvents::CasGcPut, ProfileEvents::CasGcPutDedup, ProfileEvents::CasGcOverwrite,
                  ProfileEvents::CasGcCas, ProfileEvents::CasGcCasConflict, ProfileEvents::CasGcHead,
                  ProfileEvents::CasGcHeadMiss, ProfileEvents::CasGcGet, ProfileEvents::CasGcGetStream,
                  ProfileEvents::CasGcDelete, ProfileEvents::CasGcList},
    /* Server */ {ProfileEvents::CasServerPut, ProfileEvents::CasServerPutDedup, ProfileEvents::CasServerOverwrite,
                  ProfileEvents::CasServerCas, ProfileEvents::CasServerCasConflict, ProfileEvents::CasServerHead,
                  ProfileEvents::CasServerHeadMiss, ProfileEvents::CasServerGet, ProfileEvents::CasServerGetStream,
                  ProfileEvents::CasServerDelete, ProfileEvents::CasServerList},
    /* Other  */ {ProfileEvents::CasOtherPut, ProfileEvents::CasOtherPutDedup, ProfileEvents::CasOtherOverwrite,
                  ProfileEvents::CasOtherCas, ProfileEvents::CasOtherCasConflict, ProfileEvents::CasOtherHead,
                  ProfileEvents::CasOtherHeadMiss, ProfileEvents::CasOtherGet, ProfileEvents::CasOtherGetStream,
                  ProfileEvents::CasOtherDelete, ProfileEvents::CasOtherList},
};

CasNs classifyCasNs(const String & key)
{
    if (key.find("/blobs/") != String::npos)
        return CasNs::Blob;
    /// Post-relocation layout (hot/cold split): ref shards live under `cas/refs/<ns>/<shard>` and
    /// part manifests under `cas/manifests/<ns>/...`. Without these two rules every ref-shard and
    /// manifest request misclassified as Other — the 2026-07-03 operator-stand CREATE TABLE storm
    /// showed up as `CasOtherHeadMiss=102` when it was really 3 all-shard ref sweeps (see
    /// `Store::listRefs`).
    if (key.find("/cas/refs/") != String::npos)
        return CasNs::Root;
    if (key.find("/cas/manifests/") != String::npos)
        return CasNs::Manifest;
    if (key.find("/roots/") != String::npos)
        return CasNs::Root;
    if (key.find("/gc/") != String::npos)
        return CasNs::Gc;
    return CasNs::Other;
}

void incrementCasEvent(CasNs ns, CasOp op)
{
    ProfileEvents::increment(cas_event_table[static_cast<size_t>(ns)][static_cast<size_t>(op)]);
}

namespace
{

/// Wraps an inner WriteSink: the Put-vs-PutDedup outcome is known only at finalize. The namespace is
/// captured at creation. buffer()/cancel() delegate verbatim; finalize delegates then increments.
class InstrumentedWriteSink final : public WriteSink
{
public:
    InstrumentedWriteSink(WriteSinkPtr inner_, CasNs ns_) : inner(std::move(inner_)), ns(ns_) {}

    WriteBuffer & buffer() override { return inner->buffer(); }

    PutResult finalize() override
    {
        PutResult result = inner->finalize();
        incrementCasEvent(ns, result.outcome == PutOutcome::Done ? CasOp::Put : CasOp::PutDedup);
        return result;
    }

    void cancel() noexcept override { inner->cancel(); }

private:
    WriteSinkPtr inner;
    CasNs ns;
};

}

WriteSinkPtr InstrumentedBackend::putIfAbsentStream(const String & key, const ObjectMeta & meta)
{
    WriteSinkPtr sink = inner->putIfAbsentStream(key, meta);
    if (!sink)
        return sink;
    return std::make_unique<InstrumentedWriteSink>(std::move(sink), classifyCasNs(key));
}

}
