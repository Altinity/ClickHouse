#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasProbe.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobIntentFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Common/Exception.h>
#include <Common/thread_local_rng.h>

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

/// A store that refuses a write outright (read-only, access denied) has answered: report its own error,
/// not a missing capability.
void throwIfRefused(const WriteResult & result, std::string_view what, const String & key)
{
    if (const auto * refused = std::get_if<Refused>(&result))
        throw DB::Exception(refused->store_error, "CasProbe: the store refused the {} of '{}': {}", what, key, refused->message);
}

/// Rethrows the request's failure with the right it needs; keeps the store's code where it has one.
[[noreturn]] void throwMissingRight(const String & what, const String & right)
{
    try
    {
        throw;
    }
    catch (const DB::Exception & e)
    {
        throw DB::Exception(e.code(), "CasProbe: {} failed ({}): {}", what, e.message(), right);
    }
    catch (const std::exception & e)
    {
        throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "CasProbe: {} failed ({}): {}", what, e.what(), right);
    }
}

/// Stops at the first match, so a long listing is not walked past the key.
bool listingShows(CasOperation & op, const String & prefix, const String & key)
{
    bool found = false;
    op.forEachListedKey(prefix, [&](const ListedKey & listed)
    {
        found = listed.key == key;
        return !found;
    }, Retry::standard());
    return found;
}

/// Best-effort cleanup of a probe key. `Etag` can only be minted from an actual observation, so the
/// remove is reachable only through the `HEAD` that saw the key.
void removeIfPresent(CasOperation & op, const String & key) noexcept
{
    try
    {
        if (const auto seen = op.head(key, Retry::standard()))
            op.remove(key, seen->etag, Retry::standard());
    }
    catch (...) {} /// NOLINT(bugprone-empty-catch)
}

}

void runCapabilityProbe(CasOperation & op, const String & probe_prefix)
{
    // Sub-directory style ("probe_prefix/token") ensures that list(probe_prefix, …) works for both the
    // in-memory backend (prefix match) and the LocalObjectStorage backend (directory listing).
    const String key = probe_prefix + "/token";

    try
    {
        // ---- Step 1: create fresh -> Committed; read-after-write returns the bytes. ----
        Etag t1 = [&]
        {
            WriteResult r = op.create(key, "probe-v1", Retry::standard());
            throwIfRefused(r, "create", key);
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
            throwIfRefused(r, "create", key);
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
            throwIfRefused(r, "replace", key);
            if (!std::holds_alternative<Committed>(r))
                throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                    "CasProbe: replace with the correct incarnation was rejected — backend does not accept a valid overwrite");
            Etag next = std::get<Committed>(r).etag;
            if (identicalToken(next, t1))
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
            throwIfRefused(r, "replace", key);
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
            if (!listingShows(op, probe_prefix, key))
                throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                    "CasProbe: list-after-write failed — the probe key '{}' is not visible in the listing under prefix '{}'",
                    key, probe_prefix);
        }

        // ---- Step 6b: the token a HEAD returns must be canonical: blob identities are
        //               compared only in that spelling, so a weak, wildcard, list or over-long token is refused. ----
        {
            const auto meta = op.head(key, Retry::standard());
            if (!meta)
                throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                    "CasProbe: head-after-write failed — the probe key '{}' has no metadata", key);
            if (!meta->etag.canonical())
                throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                    "CasProbe: the store returns a token that cannot serve as a blob identity: {}", meta->etag.render());
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
            if (listingShows(op, probe_prefix, key))
                throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                    "CasProbe: list-after-delete failed — the deleted probe key '{}' is still visible in the listing under prefix '{}'",
                    key, probe_prefix);
        }
    }
    catch (...)
    {
        removeIfPresent(op, key);
        throw;
    }

    // The key was already deleted in step 7; the HEAD finds it gone.
    removeIfPresent(op, key);
}

void runIntentPrefixProbe(CasOperation & op, const Layout & layout, IntentPrefixRights rights)
{
    const String prefix = layout.blobIntentsPrefix();
    const String probe_prefix = layout.blobIntentsProbePrefix();
    const String key = layout.blobIntentProbeKey(u128ToHex((static_cast<UInt128>(thread_local_rng()) << 64) | thread_local_rng()));

    /// On S3 an absent key answers 403, not 404, to a credential without list rights; a writer would then fail every
    /// reuse instead of adopting the body.
    const auto expect_absent = [&](std::string_view when)
    {
        std::optional<Object> got;
        try
        {
            got = op.read(key, Retry::standard());
        }
        catch (...) // Ok: `throwMissingRight` throws
        {
            throwMissingRight(fmt::format("an exact GET of the absent key '{}' {}", key, when),
                fmt::format("the credential cannot tell an absent intent from a denied one: grant GET and LIST on '{}'", prefix));
        }
        if (got)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: an exact GET of '{}' {} found an object where none may be", key, when);
    };
    const auto expect_listed = [&](bool listed)
    {
        bool found = false;
        try
        {
            found = listingShows(op, probe_prefix, key);
        }
        catch (...) // Ok: `throwMissingRight` throws
        {
            throwMissingRight(fmt::format("a LIST of '{}'", probe_prefix), fmt::format("grant LIST on '{}'", prefix));
        }
        if (found != listed)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "CasProbe: a LIST of '{}' {} the probe key '{}'", probe_prefix,
                listed ? "does not show" : "still shows", key);
    };

    expect_absent("before the create");
    /// GC's sweep lists the prefix itself, and a store may grant LIST by the exact prefix of the request. Local object
    /// storage has no such grants, and its listing walks the whole subtree whatever the limit.
    if (op.backendDialect() != Dialect::Emulated)
    {
        try
        {
            [[maybe_unused]] const ListPage page = op.list(prefix, /*cursor*/ "", /*limit*/ 1, Retry::standard());
        }
        catch (...) // Ok: `throwMissingRight` throws
        {
            throwMissingRight(fmt::format("a LIST of '{}'", prefix), fmt::format("grant LIST on '{}'", prefix));
        }
    }
    if (rights == IntentPrefixRights::Read)
    {
        expect_listed(false);
        return;
    }

    try
    {
        const WriteResult created = op.create(key, blobIntentRecord(), Retry::standard());
        if (const auto * refused = std::get_if<Refused>(&created))
            throw DB::Exception(refused->store_error, "CasProbe: the store refused the conditional create of '{}' ({}): grant PUT "
                "with If-None-Match on '{}'", key, refused->message, prefix);
        const auto * committed = std::get_if<Committed>(&created);
        if (!committed)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CasProbe: the conditional create of '{}' did not commit on a fresh key", key);
        expect_listed(true);
        /// The right GC's intent deletes need; whether the store also takes them in one batch is its own capability.
        Removal removal = Removal::Gone;
        try
        {
            removal = op.remove(key, committed->etag, Retry::standard());
        }
        catch (...) // Ok: `throwMissingRight` throws
        {
            throwMissingRight(fmt::format("the delete of '{}'", key), fmt::format("grant DELETE on '{}'", prefix));
        }
        if (removal != Removal::Removed)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "CasProbe: the delete of '{}' at its own token did not remove it", key);
        expect_absent("after its delete");
    }
    catch (...)
    {
        /// Best effort: a probe object left behind is skipped by every reader of `blob-intents/`.
        removeIfPresent(op, key);
        throw;
    }
}

}
