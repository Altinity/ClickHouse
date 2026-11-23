#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobIntents.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobIntentFormat.h>
#include <Common/Exception.h>

namespace DB::Cas
{

namespace
{

IntentCreateResult unconfirmed(String detail)
{
    return {IntentCreate::Unconfirmed, std::move(detail)};
}

/// Only an unknown outcome and an occupant with other bytes leave the row a candidate. A refusal, or a fence this
/// operation lost, answers every later create the same way, so it fails the round with the store's own error.
IntentCreateResult classify(const String & key, WriteResult && result)
{
    const String what = "CAS gc: the create of the blob intent " + key;
    return std::visit(detail::Overload{
        [](Committed &) -> IntentCreateResult { return {IntentCreate::Created, {}}; },
        [&](Conflict & c) -> IntentCreateResult
        {
            const auto * occupant = std::get_if<Object>(&c.seen);
            if (occupant && isBlobIntentRecord(occupant->bytes))
                return {IntentCreate::ConfirmedExisting, {}};
            return unconfirmed(fmt::format("the create of {} lost to a key observed as {}", key, detail::renderObservation(c.seen)));
        },
        [&](Declined & d) -> IntentCreateResult
        {
            return unconfirmed(fmt::format("the create of {} was declined, observed {}", key, detail::renderObservation(d.seen)));
        },
        [&](Refused & r) -> IntentCreateResult
        {
            orThrow(WriteResult{std::move(r)}, what);
            UNREACHABLE();
        },
        [&](GaveUp & g) -> IntentCreateResult
        {
            if (g.why == GaveUp::Why::FenceLost)
            {
                orThrow(WriteResult{std::move(g)}, what);
                UNREACHABLE();
            }
            return unconfirmed(fmt::format("the create of {} gave up (sent: {}), observed {}",
                key, g.sent_any, detail::renderObservation(g.last_seen)));
        }}, result);
}

}

IntentCreateResult createBlobIntent(CasOperation & op, const Layout & layout, const BlobRef & ref,
                                    const CanonicalIdentity & identity, const Retry & policy)
{
    const String key = layout.blobIntentKey(ref, identity);
    return classify(key, op.create(key, blobIntentRecord(), policy));
}

}
