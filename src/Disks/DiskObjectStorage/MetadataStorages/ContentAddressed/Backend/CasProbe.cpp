#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasProbe.h>
#include <Common/Exception.h>
#include <base/scope_guard.h>

#include <initializer_list>

namespace DB
{
namespace ErrorCodes
{
    extern const int CAS_DELETE_MARKER;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

namespace
{

/// `op.remove`'s own delete-marker signal (`CAS_DELETE_MARKER`, thrown outside the attempt loop because
/// a versioned bucket answers this way every time) is re-signalled here as the operator-facing message
/// the probe has always thrown for it — everything else propagates unchanged. Every `remove` the battery
/// issues against a live incarnation goes through this: a store that ignores the delete precondition AND
/// mints delete markers must still be reported with this message, not the engine's terse one.
Removal removeOrReportDeleteMarker(CasOperation & op, const String & key, const Etag & seen)
{
    try
    {
        return op.remove(key, seen, Retry::standard());
    }
    catch (const DB::Exception & e)
    {
        if (e.code() != DB::ErrorCodes::CAS_DELETE_MARKER)
            throw;
        throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
            "CasProbe: remove succeeded but created a versioning delete marker — the bucket has "
            "object VERSIONING enabled, and a content-addressed pool cannot run on a versioned bucket: "
            "every GC delete would archive a noncurrent version instead of reclaiming storage (the bucket "
            "grows forever), and the constantly-rewritten ref objects would pile up versions on every "
            "commit. Use a bucket where versioning was NEVER enabled; skip_access_check skips this probe "
            "but does not make a versioned bucket usable — note that merely SUSPENDING versioning is not enough (deletes on a "
            "versioning-suspended bucket still mint delete markers, so this probe will refuse again)");
    }
}

/// Best-effort cleanup uses a fresh token and skips keys that are already absent.
void cleanupProbeKeys(CasOperation & op, std::initializer_list<const String *> keys) noexcept
{
    try
    {
        for (const String * cleanup_key : keys)
        {
            const auto current = op.head(*cleanup_key, Retry::standard());
            if (current)
                op.remove(*cleanup_key, current->etag, Retry::standard());
        }
    }
    catch (...) {} /// NOLINT(bugprone-empty-catch)
}

}

bool probeConditionalBatchDelete(CasOperation & op, const String & probe_prefix, const ConditionalRemove & absent_key)
{
    const String first_matching_key = probe_prefix + "/batch-first";
    const String replaced_key = probe_prefix + "/batch-second";
    const String second_matching_key = probe_prefix + "/batch-third";
    SCOPE_EXIT({ cleanupProbeKeys(op, {&first_matching_key, &replaced_key, &second_matching_key}); });

    const Etag first_matching_token = std::get<Committed>(op.create(first_matching_key, "probe-batch-first", Retry::standard())).etag;
    const Etag stale_token = std::get<Committed>(op.create(replaced_key, "probe-batch-second-v1", Retry::standard())).etag;
    const String replacement_bytes = "probe-batch-second-v2";
    (void)std::get<Committed>(op.replace(replaced_key, replacement_bytes, stale_token, Retry::standard()));
    const Etag second_matching_token = std::get<Committed>(op.create(second_matching_key, "probe-batch-third", Retry::standard())).etag;
    try
    {
        const auto removals = op.removeManyByConditional(
            {{first_matching_key, first_matching_token}, {replaced_key, stale_token},
             {second_matching_key, second_matching_token}, absent_key},
            Retry::standard(), kBulkDeleteMaxKeys);
        if (removals.size() != 4 || removals[0] != Removal::Removed || removals[1] != Removal::Mismatch
            || removals[2] != Removal::Removed || removals[3] != Removal::Gone)
            return false;

        /// Check before cleanup, which would otherwise hide a successful response without deletion.
        const auto first = op.head(first_matching_key, Retry::standard());
        const auto replacement = op.read(replaced_key, Retry::standard());
        const auto second = op.head(second_matching_key, Retry::standard());
        return !first && !second && replacement && replacement->bytes == replacement_bytes;
    }
    catch (const DB::Exception & e)
    {
        if (e.code() != DB::ErrorCodes::NOT_IMPLEMENTED)
            throw;
        return false;
    }
}

CapabilityProbeResult runCapabilityProbe(CasOperation & op, const String & probe_prefix)
{
    // Sub-directory style ("probe_prefix/token") ensures that list(probe_prefix, …) works for both the
    // in-memory backend (prefix match) and the LocalObjectStorage backend (directory listing).
    const String key = probe_prefix + "/token";
    SCOPE_EXIT({ cleanupProbeKeys(op, {&key}); });

    // ---- Step 1: create fresh -> Committed; read-after-write returns the bytes. ----
    Etag t1 = [&]
    {
        WriteResult r = op.create(key, "probe-v1", Retry::standard());
        if (!std::holds_alternative<Committed>(r))
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: create on a fresh key did not commit — backend is unexpectedly occupied or broken");
        return std::get<Committed>(r).etag;
    }();
    {
        const auto g = op.read(key, Retry::standard());
        if (!g || g->bytes != "probe-v1")
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: read-after-write failed — create succeeded but the object is not readable");
    }

    // ---- Step 2: create the same key again -> Conflict; bytes intact. ----
    {
        WriteResult r = op.create(key, "should-not-land", Retry::standard());
        const auto * conflict = std::get_if<Conflict>(&r);
        if (!conflict)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: create on an existing key was not rejected (a conflict was expected) — "
                "backend does not enforce conditional create");
        const auto * seen = std::get_if<Object>(&conflict->seen);
        if (!seen || seen->bytes != "probe-v1")
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: create's conflict was reported but the original bytes were clobbered — "
                "backend does not enforce conditional create");
    }

    // ---- Step 3: replace against the CURRENT incarnation (t1) -> Committed; incarnation changed;
    //              bytes replaced. Every "wrong incarnation" step below reuses THIS key's own prior
    //              incarnations (never a synthetic value) — an `Etag` is minted only from an
    //              actual backend observation, so there is no other way to name one that is
    //              guaranteed wrong yet dialect-valid. ----
    Etag t2 = [&]
    {
        WriteResult r = op.replace(key, "probe-v2", t1, Retry::standard());
        if (!std::holds_alternative<Committed>(r))
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: replace with the correct incarnation was rejected — backend does not accept a valid overwrite");
        Etag next = std::get<Committed>(r).etag;
        if (next == t1)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: replace succeeded but did not mint a new incarnation — an incarnation must "
                "change on every write");
        return next;
    }();
    {
        const auto g = op.read(key, Retry::standard());
        if (!g || g->bytes != "probe-v2")
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: replace succeeded but the new bytes are not readable");
    }

    // ---- Step 4: replace against t1, now STALE (the key committed to t2 in step 3) -> Conflict;
    //              bytes intact. ----
    {
        WriteResult r = op.replace(key, "clobbered", t1, Retry::standard());
        const auto * conflict = std::get_if<Conflict>(&r);
        if (!conflict)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: replace with a stale incarnation was not rejected (a conflict was expected) — "
                "backend does not enforce conditional overwrite");
        const auto * seen = std::get_if<Object>(&conflict->seen);
        if (!seen || seen->bytes != "probe-v2")
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: replace with a stale incarnation was 'rejected' but the original bytes were clobbered");
    }

    // ---- Step 5: remove with a STALE incarnation (t1) -> Mismatch; the object survives. A store
    //              that ignores this precondition removes the object here, so this path can reach a
    //              delete marker exactly like step 7's — route it through the same reporter. ----
    {
        const Removal d = removeOrReportDeleteMarker(op, key, t1);
        if (d != Removal::Mismatch)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: remove with a stale incarnation was not rejected (a mismatch was expected) — "
                "backend does not enforce conditional deletes");
        const auto g = op.read(key, Retry::standard());
        if (!g)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: remove with a stale incarnation was rejected (correctly) but the object was deleted anyway — "
                "backend does not enforce conditional deletes");
    }

    // ---- Step 6: list(probe_prefix) contains the probe key (list-after-write). ----
    {
        bool found = false;
        op.forEachListedKey(probe_prefix, [&](const ListedKey & listed) -> bool
        {
            if (listed.key != key)
                return true;
            found = true;
            return false;
        }, Retry::standard());
        if (!found)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: list-after-write failed — the probe key '{}' is not visible in the listing under prefix '{}'",
                key, probe_prefix);
    }

    // ---- Step 7: remove with the CORRECT incarnation (t2) -> Removed; no delete marker; object
    //              gone; list no longer contains the key. ----
    {
        const Removal d = removeOrReportDeleteMarker(op, key, t2);
        if (d != Removal::Removed)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: remove with the correct incarnation was not Removed — backend rejected a valid incarnation-exact delete");
        const auto g = op.read(key, Retry::standard());
        if (g)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: remove succeeded (Removed) but the object is still readable — backend delete is not effective");
        bool still_listed = false;
        op.forEachListedKey(probe_prefix, [&](const ListedKey & listed) -> bool
        {
            if (listed.key != key)
                return true;
            still_listed = true;
            return false;
        }, Retry::standard());
        if (still_listed)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: list-after-delete failed — the deleted probe key '{}' is still visible in the listing under prefix '{}'",
                key, probe_prefix);
    }

    /// Step 7 verified that `key` was gone, so reuse its token for the absent entry.
    return {.conditional_batch_delete_supported = probeConditionalBatchDelete(op, probe_prefix, {key, t2})};
}

}
