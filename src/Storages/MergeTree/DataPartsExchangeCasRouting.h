#pragma once

#include <Core/Types_fwd.h>

#include <optional>
#include <vector>

namespace DB::DataPartsExchange
{

/// The receiver's content-addressed pool advertise as it goes on the wire (the `cas_pool_uuid` request
/// parameter): the pool ids of every disk of its storage policy that could take a relink — sorted,
/// deduplicated, joined with ", ". The list form and the ", " splitter are the ones the zero-copy
/// `remote_fs_metadata` capability list already uses, so the exchange keeps one list convention. A
/// single id is written verbatim: a receiver with one pool puts on the wire exactly the string that a
/// sender comparing the whole value with its own pool id matches. Empty ids are dropped (a storage that
/// never started has no pool id and nothing to advertise).
String encodeCasPoolAdvertise(Strings pool_uuids);
Strings decodeCasPoolAdvertise(const String & text);

/// Which pool a relink offer is for. The sender names it in the `cas_pool_uuid` response cookie; a
/// sender that predates the cookie can only have matched a one-element advertise, so an absent cookie
/// means that single pool. Several advertised pools and no cookie is not a state an honest sender can
/// produce, and the answer is "no pool" — the receiver never guesses.
String resolveOfferedCasPool(const Strings & advertised_pools, const String & offered_pool_cookie);

/// One content-addressed disk of the RECEIVING table's storage policy, in policy order.
struct CasRelinkCandidate
{
    String disk_name;
    String pool_uuid;        /// empty: the storage never started; never a candidate
    bool read_only = false;  /// a static property of the disk's configuration; the one exclusion
};

/// Which candidate receives the offered relink: the index of the first candidate on the offered pool
/// (`resolveOfferedCasPool`) that is not read-only. `nullopt` means no disk of this policy may take
/// the offer, which the caller turns into a byte fetch. Whether the pool is LIVE is deliberately not
/// part of this decision — a not-live pool disk is still the target, the relink's own write gate
/// refuses it, and the fetch fails and is retried rather than landing on another disk.
std::optional<size_t> resolveForcedCaCandidate(
    const std::vector<CasRelinkCandidate> & candidates,
    const Strings & advertised_pools,
    const String & offered_pool_cookie);

/// One content-addressed disk of the SENDING table's storage policy, as the confirm routing sees it.
struct CasConfirmRoutingCandidate
{
    const void * exchange_identity = nullptr;  /// the `IContentAddressedExchange` behind the disk
    String pool_uuid;
    bool owns_namespace = false;
};

/// Which candidate answers a relink confirm for `pool_uuid`: EXACTLY one distinct mount that owns the
/// namespace, else `nullopt` — zero owners, or two distinct mounts, are both ambiguous and `Unknown`
/// is the only honest answer. Disks that alias one mount (a base disk and its cache wrapper share the
/// exchange object) count once, as the first of them.
std::optional<size_t> resolveConfirmRoutingCandidate(
    const std::vector<CasConfirmRoutingCandidate> & candidates,
    const String & pool_uuid);

}
