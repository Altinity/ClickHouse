#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <functional>
#include <map>

namespace DB::Cas
{

/// Decoupled, pure-data per-event record (mirrors GcRoundLogRecord's design: NO Interpreters
/// dependency). The metadata storage converts it into a ContentAddressedLogElement and forwards it
/// to the SystemLog. Keeping it a plain POD lets the Core (and its unit tests) stay free of the
/// system-log machinery. B170: the log must reconstruct every entity's whole lifetime, so every
/// state-changing decision, every GC internal transition, and every error/anomaly is an event,
/// each carrying its full rationale in `reason`/`detail`.
enum class CasEventType
{
    BlobPut, BlobReuseAdopt, BlobReuseResurrect, BlobRetire, BlobDelete, BlobForget,
    TreePut, TreeExpand, TreeRetire, TreeDelete, TreeStrip,
    RefPublish, RefDrop, RefRepoint, RootAdd, RootRemove, RootRepoint, IndegZero,
    GcFoldBegin, GcFoldEnd, GcRetireObserve, GcRetireDecision, GcRecheckVerdict,
    GcFence, GcSnapPersist, GcCursorAdvance, GcTrim, GcLeaseAcquire, GcLeaseSteal, GcLeaseHeartbeat,
    BuildStart, BuildPublish, BuildAbort, Precommit, PrecommitRemoved, PrecommitReclaim,
    GateRevalidate, GateResurrect, WatermarkRenew, Heartbeat,
    RefResolve, ReadMissing, DanglingAccess,
    CorruptDangle, CorruptDecode, SnapJournalIncoherent, Exception,
};

enum class CasEventObjectKind { None, Blob, Tree, Pack, Root, Snap };

/// Map an internal `ObjectKind` to the audit-log `CasEventObjectKind`. Single source for the mapping
/// previously open-coded as a ternary at each emission site.
inline CasEventObjectKind toEventKind(ObjectKind kind)
{
    switch (kind)
    {
        case ObjectKind::Blob: return CasEventObjectKind::Blob;
        case ObjectKind::Tree: return CasEventObjectKind::Tree;
        case ObjectKind::Pack: return CasEventObjectKind::Pack;
    }
}

struct CasEvent
{
    CasEventType type = CasEventType::Heartbeat;
    String namespace_;          /// roots/<ns> (empty if N/A)
    String ref_name;            /// the ref name — a mutable directory handle, git-style (empty if N/A)
    CasEventObjectKind object_kind = CasEventObjectKind::None;
    String object_hash;         /// lowercase hex (empty if N/A)
    String token;               /// incarnation token (empty if N/A)
    UInt64 round = 0;
    UInt64 gen = 0;
    UInt64 at_version = 0;
    String outcome;             /// e.g. "ok","adopt","deleted","zeroed" (empty if N/A)
    String reason;              /// REQUIRED: the human-readable WHY of the decision
    std::map<String, String> detail;
};

using CasEventSink = std::function<void(const CasEvent &)>;

/// Collapses the repeated `if (store->hasEventSink()) { CasEvent e; e.field = …; store->emitEvent(e); }`
/// boilerplate at every emission site into a single `emitter.emit([&](CasEvent & e){ … })`. The emitter
/// seeds the ONLY identity that is constant across every site — the owning Store (the event sink) — and
/// the per-call code that fills the varying fields (type/reason/round/hashes/…) lives in the builder.
///
/// ZERO-COST when the sink is absent: the builder runs ONLY inside the `hasEventSink` guard, so a disabled
/// log constructs no `CasEvent` and does no per-call work (the production hot path stays a single branch).
/// Behavior-preserving: the builder fills the SAME fields with the SAME values it did inline, so the emitted
/// event is byte-for-byte identical. Templated on the store type to avoid a Core header-layering cycle
/// (`CasStore.h` includes this header); any `S` exposing `hasEventSink`/`emitEvent` works.
template <typename S>
class EventEmitter
{
public:
    explicit EventEmitter(const S & store_) : store(store_) {}

    template <typename Builder>
    void emit(Builder && build) const
    {
        if (!store.hasEventSink())
            return;
        CasEvent event;
        build(event);
        store.emitEvent(event);
    }

private:
    const S & store;
};

template <typename S>
EventEmitter(const S &) -> EventEmitter<S>;

/// snake_case names for the SystemLog `event_type` / `object_kind` columns. Every enumerator MUST
/// map to a stable string (the table is queried by these names).
String toString(CasEventType type);
String toString(CasEventObjectKind kind);

}
