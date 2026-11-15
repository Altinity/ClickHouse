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
/// `_ckpt` is created at generation 3 by namespace creation or raised to it by the generation 3 stamp,
/// and a generation 3 body is replaced only by snapshot publication (`Pool/CasRefCkpt.h`).
///
/// INVARIANT (Constraint 15), and it constrains what may ever be ADDED here, not merely what is here
/// today: `_ckpt` is a fixed-size product of SCALAR MONOTONE FACTS. Its encoded size is `O(1)` in refs,
/// files, transactions and writer epochs. Maps, collections and cardinality-growing fields belong in a
/// separate immutable object or ledger -- never in this one, because this one is MUTABLE, is rewritten
/// whole on every publish, and has NO REPAIR PATH: a `_ckpt` that grows with
/// the table is a body that eventually cannot be rewritten atomically, on the single object that names
/// recovery's base and gates destructive cleanup.
///
/// The four dimensions do not all hold the same way, and the difference is worth stating so the
/// invariant is checkable rather than merely believed:
///   - refs and files enter in NO form. Two namespaces differing only in how many refs they hold encode
///     BYTE-IDENTICAL `_ckpt` bodies.
///   - transactions and writer epochs enter as the DECIMAL WIDTH of the two id pairs -- four orders of
///     magnitude of `ref_sequence` cost four bytes. That is `O(1)` because the components are
///     `uint64_t`, so the width is ceilinged at twenty digits and the whole object at a constant far
///     below `traitsFor(FormatId::RefCkpt).object_cap` (which stays what it is documented to be: a
///     corruption brake this object cannot approach, never the thing that bounds its size).
/// Both halves are fenced by `gtest_cas_ref_ckpt_join.cpp`, which also carries the compile-time
/// `std::is_trivially_copyable_v<RefCkpt>` assertion that a heap-owning field would break.
struct RefCkpt
{
    /// The namespace's genesis epoch: `{life_epoch,1}` is the first key of its chain, and
    /// `validateEpochSealGrammarContextual` takes it. Set once, by `completeCreation`; a generation 1 or 2
    /// body may carry a value an older build's birth raised. Required from generation 3.
    std::optional<uint64_t> life_epoch;
    /// The greatest transaction admitted to durable logical history. Absent means this life has no
    /// committed transaction; a snapshot or epoch seal is then invalid because neither can describe
    /// history that was never committed.
    std::optional<RefTxnId> committed_through = std::nullopt;
    /// The snapshot recovery point-reads as its base, and the floor cleanup deletes strictly below.
    /// `nullopt` until this namespace's first snapshot publication commits.
    std::optional<RefTxnId> checkpoint_snapshot_id;
    /// The `EpochSeal` transaction that closed the newest epoch known to have been closed. Consumed by
    /// a later mount locating the previous epoch's terminating record and by the GC fold crossing
    /// epochs. `nullopt` before this namespace has ever had an epoch closed.
    std::optional<RefTxnId> last_epoch_seal;
    /// The header generation the body was decoded from; the encoders ignore it. Part of equality, so a
    /// generation 1 body and a generation 3 body with equal fields differ.
    uint32_t generation = 1;

    bool operator==(const RefCkpt &) const = default;
};

/// Encode `ckpt` as the canonical `cas_ref_ckpt` text object: a versioned header line followed by one
/// JSON body object. STRICT IN BOTH DIRECTIONS -- the same `checkRefCkptInvariants` that guards decode
/// runs here first, so a struct this build would refuse to read can never be written by it either
/// (`CORRUPTED_DATA`). Encoding is canonical and deterministic.
///
/// These bytes go to and come from the backend DIRECTLY: this pair bypasses `sealObject`/`openObject`,
/// which are the identity under this class's `CompressionPolicy::Never` and would add nothing. A
/// policy flip to `Always` therefore breaks this silently -- and is caught, because `storedSuffix`
/// would stop being empty and the registry test asserting `storedSuffix(FormatId::RefCkpt) == ""`
/// fails. That assertion is the tripwire for this shortcut, not an incidental check of the key shape.
String encodeRefCkpt(const RefCkpt & ckpt);

/// The generation `encodeRefCkpt` writes.
uint32_t refCkptWriteGeneration();

/// Encodes under an explicit header generation (1 to 3, else CORRUPTED_DATA), checking the invariants of that
/// generation first.
String encodeRefCkptAtGeneration(const RefCkpt & ckpt, uint32_t generation);

/// Decode a complete `cas_ref_ckpt` text object. A duplicate key, a truncated object (a missing body
/// line, or half of an optional id pair), or trailing bytes raise `CORRUPTED_DATA` -- never a
/// partially-populated struct. An unknown `!`-prefixed key raises `UNKNOWN_FORMAT_VERSION`; an unknown
/// ordinary key is skipped. This object gates destructive cleanup and names recovery's base, so
/// "decoded something" must mean "decoded everything the writer made decision-relevant".
RefCkpt decodeRefCkpt(std::string_view data);

/// The shared field-level validity rule, applied on both encode and decode: every PRESENT field is a
/// real value -- a set `life_epoch` is nonzero, and a present id has both components nonzero. When a
/// frontier is present, its writer epoch may not precede `life_epoch`, and a snapshot may not exceed
/// it. Its `last_epoch_seal` is either that exact frontier or closes the immediately preceding numeric
/// writer epoch; without a seal, a known `life_epoch` permits only that genesis epoch. `what`
/// identifies the direction in the exception message. The rules above are those of generations 1 and 2;
/// `ckpt.generation` 3 instead requires `life_epoch` and forbids `committed_through` and `last_epoch_seal`,
/// so a snapshot without a frontier is valid there. Exposed so a caller that assembles a `RefCkpt`
/// from several sources can fail closed before it reaches the wire.
void checkRefCkptInvariants(const RefCkpt & ckpt, std::string_view what);

}
