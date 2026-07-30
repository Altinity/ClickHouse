#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <base/types.h>
#include <cstdint>
#include <optional>
#include <string_view>

namespace DB::Cas
{

/// One namespace LIFE's checkpoint object (spec INV-4), persisted as the mutable, token-CAS
/// `cas_ref_ckpt` control object at `Layout::refCkptKey`, whose argument is a `NamespaceLifeId`.
///
/// It exists because prefix cleaning made the ref stream unreadable from a LIST alone: a cleaned
/// prefix plus a hidden snapshot is indistinguishable from an empty one, so recovery cannot decide
/// which snapshot is its base by enumerating keys. `_ckpt` is the point-read answer -- it NAMES the
/// base -- and, being the only authority on it, it also becomes the gate on destructive cleanup:
///
///   - snapshots are deletable only STRICTLY BELOW `checkpoint_snapshot_id` (so a STALE pointer can
///     only ever under-clean, never delete the base a live recovery is about to fetch);
///   - a sampled base that 404s is adjudicated against this object's TOKEN, not its content: an
///     advanced token means cleanup moved the base while we read (restart), an unchanged token means
///     the base was deleted under a live checkpoint, which is corruption.
///
/// TWO writers update it -- the snapshot publisher and the sealer -- and both run the SAME algorithm
/// (`mergeCkpt` + `publishCkpt` in `Pool/CasRefCkpt.h`): read the whole body, merge by SEMANTIC
/// MAXIMUM per field, token-CAS. Writing the whole body is what makes a stale field dangerous, and
/// the merge is what contains it: a writer that skipped it and wrote back the value it sampled
/// earlier would silently regress the OTHER writer's progress (TLC counterexample
/// `_sab_sealclobbersbase`, which loses an acked transaction).
struct RefCkpt
{
    /// The namespace's birth epoch -- the `writer_epoch` of its `NamespaceBirth` record. It is what
    /// makes the epoch-seal grammar checkable without walking to the beginning of the stream
    /// (`validateEpochSealGrammarContextual` takes exactly this value), and it is a namespace-lifetime
    /// constant, so its semantic maximum is itself.
    ///
    /// OPTIONAL, and the option is load-bearing rather than a convenience. Exactly ONE writer knows
    /// this value -- the transaction that births the namespace -- and a table recovered from durable
    /// objects written before it existed has no way to learn it. Every OTHER writer therefore
    /// contributes `nullopt` and the merge leaves whatever is there alone. Making it mandatory would
    /// force those writers to supply a number they do not have, and the semantic-max merge can never
    /// lower a wrong one: a guess here is permanent. A consumer that NEEDS the genesis epoch (Stage B's
    /// cross-epoch GC fold) must fail closed on `nullopt`, never substitute a floor.
    std::optional<uint64_t> life_epoch;
    /// The snapshot recovery point-reads as its base, and the floor cleanup deletes strictly below.
    /// `nullopt` until this namespace's first snapshot publication commits.
    std::optional<RefTxnId> checkpoint_snapshot_id;
    /// The `EpochSeal` transaction that closed the newest epoch known to have been closed. Consumed by
    /// a later mount locating the previous epoch's terminating record and by the GC fold crossing
    /// epochs. `nullopt` before this namespace has ever had an epoch closed.
    std::optional<RefTxnId> last_epoch_seal;

    bool operator==(const RefCkpt &) const = default;
};

/// Encode `ckpt` as the canonical `cas_ref_ckpt` text object: a versioned header line followed by one
/// JSON body object. STRICT IN BOTH DIRECTIONS -- the same `checkRefCkptInvariants` that guards decode
/// runs here first, so a struct this build would refuse to read can never be written by it either
/// (`CORRUPTED_DATA`). Encoding is canonical and deterministic, which is what lets `publishCkpt`
/// compare a merged result against what it read.
///
/// These bytes go to and come from the backend DIRECTLY: this pair bypasses `sealObject`/`openObject`,
/// which are the identity under this class's `CompressionPolicy::Never` and would add nothing. A
/// policy flip to `Always` therefore breaks this silently -- and is caught, because `storedSuffix`
/// would stop being empty and the registry test asserting `storedSuffix(FormatId::RefCkpt) == ""`
/// fails. That assertion is the tripwire for this shortcut, not an incidental check of the key shape.
String encodeRefCkpt(const RefCkpt & ckpt);

/// Decode a complete `cas_ref_ckpt` text object. STRICT (`KeyStrictness::Strict`): an unknown ordinary
/// key, a duplicate key, a truncated object (a missing body line, or half of an optional
/// id pair), or trailing bytes all raise `CORRUPTED_DATA` -- never a partially-populated struct. This
/// object gates destructive cleanup and names recovery's base, so "decoded something" must mean
/// "decoded exactly what a writer of this format wrote".
RefCkpt decodeRefCkpt(std::string_view data);

/// The shared field-level validity rule, applied on both encode and decode: every PRESENT field is a
/// real value -- a set `life_epoch` is nonzero, and a present id has both components nonzero. `what`
/// identifies the direction in the exception message. Exposed so a caller that assembles a `RefCkpt`
/// from several sources can fail closed before it reaches the wire.
void checkRefCkptInvariants(const RefCkpt & ckpt, std::string_view what);

}
