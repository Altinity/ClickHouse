#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// The byte bound every namespace name admitted into `ref_catalog` must satisfy (spec INV-3:
/// "namespace names get a byte bound"). This is not merely a sanity limit -- the admission check's
/// predicate (2) charges every entry's fold-seal reservation at EXACTLY this bound, worst-case
/// escaped, regardless of the admitted name's real length (`worstCaseEntryFoldReservationBytes`), so
/// a name over this bound would make that reservation an UNDER-estimate. Both directions of the
/// codec enforce it.
constexpr size_t kMaxNamespaceBytes = 512;

/// One namespace's catalog lifecycle state (spec INV-3, §3). `Creating` blocks publication and
/// requires a `creator` fence identity; `Live` is the steady state and forbids `creator`;
/// `Removing` forbids new positive ownership and, like `Live`, forbids `creator` (a namespace at
/// this state was already `Live`, so no creation fence identity applies to it any longer).
///
/// THESE ARE WIRE VALUES, AND THEY ARE APPEND-ONLY, exactly like `HoldReason`: a catalog object
/// written by one build is read by another, so a renumbered or repurposed value would make an older
/// catalog name a different lifecycle than the one it recorded. Add new states at the end; never
/// renumber, never repurpose a retired word.
enum class NsState : uint8_t
{
    Creating = 1,
    Live = 2,
    Removing = 3,
};

/// The wire word one `NsState` is persisted as. Exported so any other reader of a lifecycle state
/// renders the SAME three words this codec does, rather than a second, independently drifting copy.
std::string_view nsStateToWord(NsState s);
/// Inverse of `nsStateToWord`; throws `CORRUPTED_DATA` for anything but the three registered words.
NsState nsStateFromWord(std::string_view w);

/// The fence identity of the mounted writer CREATING one namespace (spec §3): the server root plus
/// the writer epoch and admission fence generation captured at the moment `Creating` was minted. It
/// is what a reconciler compares against `CasServerRoot`'s liveness/fence machinery before a stalled
/// `Creating` entry may be CAS-reconciled away (INV-3: "stalled creators occupy entries until
/// fence-terminal reconciliation").
struct CreatorFence
{
    String server_root_id;
    uint64_t writer_epoch = 0;
    uint64_t fence_generation = 0;

    bool operator==(const CreatorFence &) const = default;
};

/// One namespace's catalog row. `incarnation` is the ref-layer-scoped life identity minted once, at
/// `Creating` (spec INV-3; consumed as a `NamespaceLifeId` by every ref/namespace-file key helper --
/// see `NamespaceLifeId::fromCatalogEntry`), and never changes for the rest of this row's life: a
/// namespace dropped and recreated gets a FRESH row with a FRESH incarnation, never a reused one --
/// that is what makes rebirth structurally inert instead of an alias. `incarnation == 0` is always
/// invalid, at every state -- "0 never names a life", the same rule `NamespaceLifeId` enforces.
///
/// `creator` is a STRICT GRAMMAR pairing: REQUIRED iff `state == Creating`, FORBIDDEN otherwise. Both
/// directions of the codec enforce it, so a `Live`/`Removing` row can never carry a stale creator
/// fence, and a `Creating` row can never lose the identity a reconciler needs to judge it.
struct CatalogEntry
{
    RootNamespace ns;
    NsState state = NsState::Creating;
    UInt128 incarnation = 0;
    std::optional<CreatorFence> creator = std::nullopt;

    bool operator==(const CatalogEntry &) const = default;
};

/// The whole-pool namespace catalog (spec INV-3): one object, key `cas/ref_catalog`
/// (`Layout::refCatalogKey`), read on every fold round and every recovery, mutated by one token-CAS
/// write per lifecycle transition. `entries` is CANONICALLY ORDERED by namespace bytes, strictly
/// ascending -- no duplicate namespace -- and both directions of the codec enforce it, so an
/// out-of-order or duplicate-keyed catalog can never become durable.
struct RefCatalog
{
    std::vector<CatalogEntry> entries;

    bool operator==(const RefCatalog &) const = default;
};

/// Encodes `catalog` as the canonical `cas_ref_catalog` text object: a header line, one "ent" record
/// per entry in canonical (ns-sorted) order, and a record-count trailer -- the same tagged-record
/// container `encodeFoldSeal` uses. Enforces the FULL strict grammar on the way out: canonical order
/// and no duplicate namespace, the `kMaxNamespaceBytes` bound, nonzero incarnation, and the
/// `creator`/state pairing. This is our own state about to become durable, so a violation is
/// `LOGICAL_ERROR`, not `CORRUPTED_DATA`. Also enforces the per-line `LIMIT_EXCEEDED` line-cap gate
/// (mirroring `encodeFoldSeal`'s `checkLineBytes`) -- but deliberately does NOT enforce the
/// whole-object cap itself: that predicate must name the namespace under admission, which only a
/// caller of `checkCatalogAdmission` knows.
String encodeRefCatalog(const RefCatalog & catalog);

/// Decodes and validates a `cas_ref_catalog` object, re-checking every grammar rule `encodeRefCatalog`
/// enforces against bytes that may have come from anywhere: `CORRUPTED_DATA` on a duplicate namespace,
/// non-canonical order, an over-bound namespace, a zero incarnation, an incomplete or forbidden
/// creator fence, an unknown state word, or trailing bytes.
RefCatalog decodeRefCatalog(std::string_view data);

/// PRE-PUT GATE, predicate (1) of INV-3's additive admission: `encoded_bytes <= catalog_object_cap`
/// (the registry's own cap for `FormatId::RefCatalog`). Equality is accepted; refuses
/// (`LIMIT_EXCEEDED`, naming `ns`) one byte over.
void checkCatalogObjectBytes(uint64_t encoded_bytes, const RootNamespace & ns);

/// The fold seal's fixed frame cost (header + trailer, zero entries) -- everything a round's seal
/// costs before a single namespace is added. Measured through `encodeFoldSeal` itself rather than a
/// hand-kept formula, so a later change to the fold seal's wire shape is felt here automatically.
uint64_t foldSealFixedBytes();

/// The worst-case bytes ONE admitted catalog entry could ever add to a fold seal: a `cov` row held at
/// the widest shape (classification 4, every hold field at its widest rendering) PLUS an `nsc` row at
/// the widest removal-cleanup shape -- the two rows one namespace can simultaneously occupy (a hold
/// outstanding on its ref-log tail while its removal's terminal cleanup item is still carried).
/// Namespace bytes are charged at `kMaxNamespaceBytes` with worst-case JSON escaping, regardless of
/// any real admitted name's actual length: admission reserves for the worst name this build will
/// ever accept, not the one in hand. Measured through `encodeFoldSeal` itself, like `foldSealFixedBytes`.
uint64_t worstCaseEntryFoldReservationBytes();

/// PRE-PUT GATE, predicate (2) of INV-3's additive admission:
/// `foldSealFixedBytes() + entry_count * worstCaseEntryFoldReservationBytes() <= fold_seal_object_cap`.
/// Equality is accepted; refuses (`LIMIT_EXCEEDED`, naming `ns`) one entry over. `entry_count` is the
/// CANDIDATE catalog's entry count -- the count AFTER the admission under consideration.
void checkFoldSealReservation(uint64_t entry_count, const RootNamespace & ns);

/// Runs BOTH admission predicates against `candidate` -- the catalog state as it would read
/// immediately AFTER the admission under consideration -- naming `admitting_ns` in whichever
/// predicate refuses (INV-3: "admission refuses loudly"; "TWO INDEPENDENT predicates"). `candidate`
/// is grammar-checked first (via `encodeRefCatalog`; `LOGICAL_ERROR` on our own bug), then predicate
/// (1) and predicate (2), in that order. Returns the encoded bytes on success, so a caller's `casPut`
/// writes EXACTLY what admission checked -- never a second, independently re-encoded copy.
///
/// Constraint 13 (removal is never refused): this function is for entry-ADMITTING mutations only.
/// A removal transition (`Live` -> `Removing`) must go through the catalog's plain update path
/// instead, never through here.
String checkCatalogAdmission(const RefCatalog & candidate, const RootNamespace & admitting_ns);

}
