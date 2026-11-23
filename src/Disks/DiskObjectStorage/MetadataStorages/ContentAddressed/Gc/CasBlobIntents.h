#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <base/types.h>
#include <cstdint>

namespace DB::Cas
{

enum class IntentCreate : uint8_t
{
    Created,             /// this create was answered success, or the read after an unanswered attempt found an intent with these bytes
    ConfirmedExisting,   /// the key already holds the canonical record
    Unconfirmed,         /// anything else: nothing proves the intent exists
};

struct IntentCreateResult
{
    IntentCreate outcome = IntentCreate::Unconfirmed;
    String detail;   /// why the create is unconfirmed; empty otherwise

    [[nodiscard]] bool confirmed() const { return outcome != IntentCreate::Unconfirmed; }
};

/// Creates the intent of `ref` at `identity` with `If-None-Match: *` and reports whether the intent
/// is known to exist afterwards. A request that was sent, or a job that finished, is not a
/// confirmation; only an answered success or an equal occupant is. An unknown outcome and an occupant
/// with other bytes are `Unconfirmed`. Throws, with the store's own error, on a refusal and on a lost
/// fence; any exception of the request propagates.
[[nodiscard]] IntentCreateResult createBlobIntent(CasOperation & op, const Layout & layout, const BlobRef & ref,
                                                  const CanonicalIdentity & identity, const Retry & policy);

}
