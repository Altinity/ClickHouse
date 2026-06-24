#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInstrumentedBackend.h>

namespace ProfileEvents
{
/// B168 P0: the 70 CA per-namespace S3 op events (7 namespaces × 10 ops). Declared in ProfileEvents.cpp.
extern const Event CasBlobPut;
extern const Event CasBlobPutDedup;
extern const Event CasBlobOverwrite;
extern const Event CasBlobCas;
extern const Event CasBlobCasConflict;
extern const Event CasBlobHead;
extern const Event CasBlobHeadMiss;
extern const Event CasBlobGet;
extern const Event CasBlobDelete;
extern const Event CasBlobList;

extern const Event CasTreePut;
extern const Event CasTreePutDedup;
extern const Event CasTreeOverwrite;
extern const Event CasTreeCas;
extern const Event CasTreeCasConflict;
extern const Event CasTreeHead;
extern const Event CasTreeHeadMiss;
extern const Event CasTreeGet;
extern const Event CasTreeDelete;
extern const Event CasTreeList;

extern const Event CasRootPut;
extern const Event CasRootPutDedup;
extern const Event CasRootOverwrite;
extern const Event CasRootCas;
extern const Event CasRootCasConflict;
extern const Event CasRootHead;
extern const Event CasRootHeadMiss;
extern const Event CasRootGet;
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
extern const Event CasGcDelete;
extern const Event CasGcList;

extern const Event CasBuildPut;
extern const Event CasBuildPutDedup;
extern const Event CasBuildOverwrite;
extern const Event CasBuildCas;
extern const Event CasBuildCasConflict;
extern const Event CasBuildHead;
extern const Event CasBuildHeadMiss;
extern const Event CasBuildGet;
extern const Event CasBuildDelete;
extern const Event CasBuildList;

extern const Event CasServerPut;
extern const Event CasServerPutDedup;
extern const Event CasServerOverwrite;
extern const Event CasServerCas;
extern const Event CasServerCasConflict;
extern const Event CasServerHead;
extern const Event CasServerHeadMiss;
extern const Event CasServerGet;
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
                  ProfileEvents::CasBlobHeadMiss, ProfileEvents::CasBlobGet, ProfileEvents::CasBlobDelete,
                  ProfileEvents::CasBlobList},
    /* Tree   */ {ProfileEvents::CasTreePut, ProfileEvents::CasTreePutDedup, ProfileEvents::CasTreeOverwrite,
                  ProfileEvents::CasTreeCas, ProfileEvents::CasTreeCasConflict, ProfileEvents::CasTreeHead,
                  ProfileEvents::CasTreeHeadMiss, ProfileEvents::CasTreeGet, ProfileEvents::CasTreeDelete,
                  ProfileEvents::CasTreeList},
    /* Root   */ {ProfileEvents::CasRootPut, ProfileEvents::CasRootPutDedup, ProfileEvents::CasRootOverwrite,
                  ProfileEvents::CasRootCas, ProfileEvents::CasRootCasConflict, ProfileEvents::CasRootHead,
                  ProfileEvents::CasRootHeadMiss, ProfileEvents::CasRootGet, ProfileEvents::CasRootDelete,
                  ProfileEvents::CasRootList},
    /* Gc     */ {ProfileEvents::CasGcPut, ProfileEvents::CasGcPutDedup, ProfileEvents::CasGcOverwrite,
                  ProfileEvents::CasGcCas, ProfileEvents::CasGcCasConflict, ProfileEvents::CasGcHead,
                  ProfileEvents::CasGcHeadMiss, ProfileEvents::CasGcGet, ProfileEvents::CasGcDelete,
                  ProfileEvents::CasGcList},
    /* Build  */ {ProfileEvents::CasBuildPut, ProfileEvents::CasBuildPutDedup, ProfileEvents::CasBuildOverwrite,
                  ProfileEvents::CasBuildCas, ProfileEvents::CasBuildCasConflict, ProfileEvents::CasBuildHead,
                  ProfileEvents::CasBuildHeadMiss, ProfileEvents::CasBuildGet, ProfileEvents::CasBuildDelete,
                  ProfileEvents::CasBuildList},
    /* Server */ {ProfileEvents::CasServerPut, ProfileEvents::CasServerPutDedup, ProfileEvents::CasServerOverwrite,
                  ProfileEvents::CasServerCas, ProfileEvents::CasServerCasConflict, ProfileEvents::CasServerHead,
                  ProfileEvents::CasServerHeadMiss, ProfileEvents::CasServerGet, ProfileEvents::CasServerDelete,
                  ProfileEvents::CasServerList},
    /* Other  */ {ProfileEvents::CasOtherPut, ProfileEvents::CasOtherPutDedup, ProfileEvents::CasOtherOverwrite,
                  ProfileEvents::CasOtherCas, ProfileEvents::CasOtherCasConflict, ProfileEvents::CasOtherHead,
                  ProfileEvents::CasOtherHeadMiss, ProfileEvents::CasOtherGet, ProfileEvents::CasOtherDelete,
                  ProfileEvents::CasOtherList},
};

CasNs classifyCasNs(const String & key)
{
    if (key.find("/blobs/") != String::npos)
        return CasNs::Blob;
    if (key.find("/trees/") != String::npos)
        return CasNs::Tree;
    /// Phase 6: a server's mutable control state lives under its own `roots/<server-hex>/` subtree —
    /// the watermark at `.../_watermark` and the precommit shards under `.../_precommits/`. Both are
    /// key-wise under `/roots/`, so classify them by their distinguishing suffix/segment BEFORE the
    /// generic `/roots/` fall-through, preserving the Server (watermark) and Build (precommit) labels.
    if (key.ends_with("/_watermark"))
        return CasNs::Server;
    if (key.find("/_precommits/") != String::npos)
        return CasNs::Build;
    if (key.find("/roots/") != String::npos)
        return CasNs::Root;
    if (key.find("/gc/") != String::npos)
        return CasNs::Gc;
    if (key.find("/builds/") != String::npos)
        return CasNs::Build;
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
