#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

String toString(CasEventType type)
{
    switch (type)
    {
        case CasEventType::BlobPut:               return "blob_put";
        case CasEventType::BlobReuseAdopt:        return "blob_reuse_adopt";
        case CasEventType::BlobReuseResurrect:    return "blob_reuse_resurrect";
        case CasEventType::BlobRetire:            return "blob_retire";
        case CasEventType::BlobDelete:            return "blob_delete";
        case CasEventType::BlobForget:            return "blob_forget";
        case CasEventType::TreePut:               return "tree_put";
        case CasEventType::TreeExpand:            return "tree_expand";
        case CasEventType::TreeRetire:            return "tree_retire";
        case CasEventType::TreeDelete:            return "tree_delete";
        case CasEventType::TreeStrip:             return "tree_strip";
        case CasEventType::RefPublish:            return "ref_publish";
        case CasEventType::RefDrop:               return "ref_drop";
        case CasEventType::RefRepoint:            return "ref_repoint";
        case CasEventType::RootAdd:               return "root_add";
        case CasEventType::RootRemove:            return "root_remove";
        case CasEventType::RootRepoint:           return "root_repoint";
        case CasEventType::IndegZero:             return "indeg_zero";
        case CasEventType::GcFoldBegin:           return "gc_fold_begin";
        case CasEventType::GcFoldEnd:             return "gc_fold_end";
        case CasEventType::GcRetireObserve:       return "gc_retire_observe";
        case CasEventType::GcRetireDecision:      return "gc_retire_decision";
        case CasEventType::GcRecheckVerdict:      return "gc_recheck_verdict";
        case CasEventType::GcFence:               return "gc_fence";
        case CasEventType::GcSnapPersist:         return "gc_snap_persist";
        case CasEventType::GcCursorAdvance:       return "gc_cursor_advance";
        case CasEventType::GcTrim:                return "gc_trim";
        case CasEventType::GcLeaseAcquire:        return "gc_lease_acquire";
        case CasEventType::GcLeaseSteal:          return "gc_lease_steal";
        case CasEventType::GcLeaseHeartbeat:      return "gc_lease_heartbeat";
        case CasEventType::BuildStart:            return "build_start";
        case CasEventType::BuildPublish:          return "build_publish";
        case CasEventType::BuildAbort:            return "build_abort";
        case CasEventType::GateRevalidate:        return "gate_revalidate";
        case CasEventType::GateResurrect:         return "gate_resurrect";
        case CasEventType::WatermarkRenew:        return "watermark_renew";
        case CasEventType::Heartbeat:             return "heartbeat";
        case CasEventType::RefResolve:            return "ref_resolve";
        case CasEventType::ReadMissing:           return "read_missing";
        case CasEventType::DanglingAccess:        return "dangling_access";
        case CasEventType::FailClosed:            return "fail_closed";
        case CasEventType::CorruptDecode:         return "corrupt_decode";
        case CasEventType::SnapJournalIncoherent: return "snap_journal_incoherent";
        case CasEventType::Exception:             return "exception";
    }
    throw DB::Exception(
        DB::ErrorCodes::LOGICAL_ERROR,
        "CasEvent: unknown CasEventType value {}",
        static_cast<int>(type));
}

String toString(CasEventObjectKind kind)
{
    switch (kind)
    {
        case CasEventObjectKind::None: return "none";
        case CasEventObjectKind::Blob: return "blob";
        case CasEventObjectKind::Tree: return "tree";
        case CasEventObjectKind::Pack: return "pack";
        case CasEventObjectKind::Root: return "root";
        case CasEventObjectKind::Snap: return "snap";
    }
    throw DB::Exception(
        DB::ErrorCodes::LOGICAL_ERROR,
        "CasEvent: unknown CasEventObjectKind value {}",
        static_cast<int>(kind));
}

}
