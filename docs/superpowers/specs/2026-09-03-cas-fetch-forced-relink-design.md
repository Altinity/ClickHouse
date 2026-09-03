---
description: 'Design for forced fetch-by-relink: when the sender holds the part on a content-addressed pool that the receiving table also has a disk for, the fetch always relinks onto that disk, overriding storage-policy order and TTL placement; the receiver advertises every pool of its policy and the sender names the one it matched.'
sidebar_label: 'CAS forced relink on fetch'
sidebar_position: 1
slug: /superpowers/specs/cas-fetch-forced-relink-design
title: 'CAS forced relink on fetch design'
doc_type: 'design'
---

# CAS forced relink on fetch design {#cas-fetch-forced-relink-design}

Status: revision 1 of 2026-09-03, the design agreed in the brainstorming interview of the same day.
Line references are against `cas-gc-rebuild` at `9be5befc3ec`. Backlog items this closes or touches:
`[relink-advertises-only-first-ca-pool]` (CAS-134) in
[`BACKLOG/replication.md`](/superpowers/cas/backlog/replication) is closed by it;
`[mixed-ca-tiered-topology]` in `BACKLOG/formats-and-storage.md` gets its fetch-path answer; the two
items recorded from the same interview, `[move-to-ca-relink-from-replica]` and
`[zero-copy-parity-audit]`, are ordered after it.

## The problem {#problem}

Fetch-by-relink exists (`DataPartsExchange.cpp`, CAS replication 2b with the publish-then-confirm
handshake), but it is opportunistic. The receiver advertises ONE guessed pool identity before it knows
where the sender keeps the part — the caller's `dest_disk` if that is content-addressed, else the
FIRST content-addressed disk of the table's storage policy (`fetchSelectedPart`,
`DataPartsExchange.cpp:705-725`). Independently of that guess, it then reserves the target disk the
ordinary way: the TTL move rule's destination, `balancedReservation`, or the first volume with space
(`:803-855`, through `MergeTreeData::reserveSpacePreferringTTLRules` and `StoragePolicy::reserve`).
Only after both has it a relink offer in hand, and the post-check at `:926-933` — added by `f3cd6e1ff1f`
— accepts the offer only when the reservation *happened* to land on a disk of the advertised pool.
Otherwise it re-requests the bytes.

So a part that is already in the shared pool moves as bytes whenever the policy's placement disagrees
with the guess: a tiered policy whose local volume comes first, a TTL rule that names the local tier for
a fresh part, a policy holding two pools with the sender's in second place (CAS-134). The relink is the
whole point of a shared pool — a fetch should move no bytes — and today the storage policy can veto it
by accident.

## Decisions {#decisions}

The interview settled these; the design below follows from them and does not reopen them.

| Question | Decision |
|---|---|
| What is "a CAS disk the receiver knows"? | A disk of the receiving table's storage policy, any volume. Disks configured on the server but absent from the policy are not candidates — a part on such a disk is not loaded at startup (`loadDataParts` walks the policy's disks only). |
| Relink versus the policy's own placement (volume order, JBOD balancing, `max_data_part_size_bytes`)? | Relink wins outright. If the policy has a writable disk on the sender's pool, the part goes there. |
| Relink versus a `TTL ... TO DISK\|VOLUME` rule that names somewhere else? | Relink wins. The part lands on the pool's disk at zero byte cost; `MergeTreePartsMover::selectPartsForMove` sees `!isPartInTTLDestination` and moves it to the TTL destination afterwards (`MergeTreePartsMover.cpp:170-174`, `MergeTreeData.cpp:9169`). The bytes then travel once, as a `GET` from the pool on the receiver instead of a stream from the sender, and the sender is not loaded at all. Precedent: `perform_ttl_move_on_insert=0` already places first and moves later. |
| A caller-supplied `dest_disk`? | Authoritative, untouched. The only caller is zero-copy `MOVE` (`MergeTreePartsMover::clonePart` → `tryToFetchIfShared` → `fetchExistsPart`, `StorageReplicatedMergeTree.cpp:5945`, `:6010`), which asserts the part landed on that disk (`LOGICAL_ERROR` otherwise) and renames it into `moving/`. A content-addressed disk never enters that path — `supportZeroCopyReplication()` is `false` for `MetadataStorageType::CAS` (`DiskObjectStorage.h:53-57`) — so this is another feature's invariant that this one must not break, not a placement rule of its own. |
| A read-only or broken disk on the right pool? | Not a candidate: nothing can publish a ref there. It is left out of the advertise and out of the placement. |
| A new setting to turn the forcing off? | None. The rule is unconditional; the recursion brake (`allow_ca_relink`) stays the only internal switch. |

## Design {#design}

### Wire protocol {#wire}

The request parameter `cas_pool_uuid` becomes a set. Its wire form copies the zero-copy capability
list exactly — sorted, deduplicated, joined with `", "`, tokenized by the sender with the same
`", "` splitter (`DataPartsExchange.cpp:367-381` and `:751-757`) — so the exchange keeps one
list convention. The relink offer gains a third response cookie, `cas_pool_uuid`, naming the pool the
sender matched, next to `cas_relink` and `cas_source_token`. Reusing the parameter's name for the
answer follows `remote_fs_metadata` (request: the list; response cookie: the choice).

The protocol version does not move. `REPLICATION_PROTOCOL_VERSION_WITH_CA_CONFIRM = 11` encodes a
promise (the receiver confirms before it promotes); the shape of the advertise is not a promise, and
a version bump would only turn a compatible change into a mixed-build cliff. Compatibility is by
construction:

- Old sender, new receiver. The old gate compares the whole parameter string with its own pool id
  (`:410`). It matches exactly when the set has one element — whose wire form is byte-for-byte
  today's — and it sends no `cas_pool_uuid` cookie. The new receiver reads an absent cookie as "the
  single advertised pool" when the set has one element, which is exact because a multi-element string
  never matches an old sender. With more than one element and no cookie the pool is unknown and the
  receiver takes the byte fallback; that state is unreachable with an old sender and is kept only so
  the receiver never guesses.
- New sender, old receiver. The old receiver advertises one pool; the new sender sees a one-element
  set, offers as before, and adds a cookie the old receiver ignores. The old post-check
  (`chosen_ca->getPoolUUID() != advertised_pool_uuid`) works unchanged.

### Sender {#sender}

`Service::processQuery`, the relink gate at `:404-434`: `receiver_pool_uuid == ca_meta->getPoolUUID()`
becomes "the pool id of the part's disk is in the advertised set", and the offer adds the
`cas_pool_uuid` cookie with that id. `getRelinkOffer`, the source token, the manifest payload and
`addLastSentPart` are untouched. The sender still looks only at its own part's disk; it does not care
how many pools the receiver has.

### Receiver {#receiver}

Everything is inside `Fetcher::fetchSelectedPart`, in the `dest_disk == nullptr` branch; the
explicit-disk branch is not modified.

**Advertise.** With `allow_ca_relink` set and no `dest_disk`: the pool ids of every disk in
`data.getDisks()` (the policy's disks) for which `tryGetContentAddressedExchange` is non-null,
`!isReadOnly() && !isBroken()`, and `getPoolUUID()` is non-empty (empty means the storage has not
started). Today's `break` on the first content-addressed disk goes away. With a `dest_disk`: its own
pool if it is content-addressed, otherwise nothing — unchanged. With `allow_ca_relink` clear: no
parameter — unchanged, this is the recursion brake.

**Resolve the forced disk before the reservation.** The response cookies are readable right after
the request, before any body field (`:786-787`), so the placement decision needs no reordering of
the body reads (`sum_files_size`, `ttl_infos`, part type, uuid, projections, then the manifest — the
sender writes them in that order and the relink branch at `:404` comes after them). A new block after
the cookie reads:

1. `cas_relink` empty → `forced_ca_disk = nullptr`; the byte and zero-copy flows proceed unchanged.
2. Otherwise `offered_pool` is the `cas_pool_uuid` cookie; if the cookie is absent and the advertised
   set has exactly one element, that element; otherwise empty.
3. `forced_ca_disk` is the first disk of `data.getDisks()` that is content-addressed, has
   `getPoolUUID() == offered_pool`, and is `!isReadOnly() && !isBroken()`; null if none, or if
   `offered_pool` is empty. Several disks on one pool (one server mounting one pool twice) take the
   first in policy order, with a `DEBUG` line naming the candidates; the shape is not designed for.

**Reservation.** `if (forced_ca_disk) reservation = data.reserveSpace(sum_files_size,
forced_ca_disk)` — the existing `reserveSpace(UInt64, SpacePtr)` (`MergeTreeData.cpp:9009`), whose
`checkAndReturnReservation` throws `NOT_ENOUGH_SPACE` if the disk declines. On an object-storage disk
that cannot happen (`DiskObjectStorage::getAvailableSpace()` is `nullopt`, so `tryReserve` always
succeeds); it stays a throw rather than a fallback so that an impossible state fails loudly and the
replication queue retries. The `balancedReservation`, `reserveSpacePreferringTTLRules` and
`reserveSpace(size)` branches are skipped in this case — that is the whole of "relink overrides the
policy and TTL". Then `disk = reservation->getDisk()` as today.

**The relink block** (`:889-951`) stays. Its post-check compares the chosen disk's pool with
`offered_pool` instead of `advertised_pool_uuid`; since the chosen disk is the forced one, the check
is now a defensive exit that stays a logged byte fallback, not a `chassert` — release builds must
fail closed the same way as debug builds.

**Two receiver exits, both before taxonomy row 0.**

- E1, an offer for a pool this policy has no writable disk for (`cas_relink` set, `forced_ca_disk`
  null: the policy changed between the advertise and the answer, or the sender is not honest). The
  reservation runs the ordinary way and the relink block takes `fall_back_to_byte_fetch()` with the
  ordinarily reserved disk and `allow_ca_relink=false`. The manifest body is discarded with the
  response, exactly as today.
- E2, the forced-disk reservation declined: `NOT_ENOUGH_SPACE`, queue retry. Named so that it is
  never mistaken for a fallback.

**Byte fallback after a mechanism failure.** `relinkPartToDisk` returning null already re-requests
through `fall_back_to_byte_fetch`, which passes `disk` and `allow_ca_relink=false`. `disk` is now the
forced content-addressed disk, so the bytes land on the pool's disk, content-address and deduplicate
against the pool, and the placement stays "the pool's disk" — the TTL rule catches up by the mover as
in the relink case. No code changes here; only the provenance of `disk` does.

**Read-only disks.** Excluded from both the advertise and the resolution. When the only disk on the
sender's pool is read-only the pool is not advertised, the sender streams bytes, and the ordinary
reservation skips read-only volumes on its own (`StoragePolicy::reserve`, `StoragePolicy.cpp:269`).

**Unchanged.** `to_detached` (`relinkPartToDisk` already stages under `detached/`); the recursion
brake (every same-sender re-request clears `allow_ca_relink`); the seven-row failure taxonomy of
`relinkPartToDisk`; the confirm handshake.

**Logging.** One `DEBUG` line on a forced placement: the part, the offered pool, the disk, and that
storage-policy order and TTL rules were bypassed. The positive test signal stays the existing
`Relink of part <p> onto disk <d> finished (no bytes transferred).`

### Invariants {#invariants}

- I1. A caller-supplied `dest_disk` is never overridden.
- I2. After a relink the part is on a disk of the receiving table's storage policy whose pool id equals
  the pool id of the part's disk on the sender.
- I3. At most one relink attempt per fetch (the recursion brake).

## Verification {#verification}

### Tests {#tests}

All in `tests/integration/test_cas_replicated_relink` (two nodes over RustFS; node2 already loads
`storage_conf_other_pool.xml`). The sender, node1, keeps the single-disk `cas_shared` policy in every
test — otherwise its part is not on the pool and there is no offer to force. Every relink assertion is
positive, per the file's own rule: the `Relink of part … finished (no bytes transferred).` line for a
relink, `Download of part … onto disk <d> finished.` for bytes; blob counts corroborate, never prove.

Two new policies on node2, no new disks beyond the local `default`:

- `cas_tiered`: volumes `[hot: default]`, `[cold: disk_cas_shared]`. The local volume comes FIRST so
  that today's `StoragePolicy::reserve` is certain to pick it.
- `cas_two_pools`: volumes `[first: disk_cas_other]`, `[second: disk_cas_shared]`. The first
  content-addressed disk is the other pool.

| Test | Proves |
|---|---|
| `test_tiered_policy_relinks_onto_cas_over_volume_order` — node2 on `cas_tiered`, queue fetch after an insert on node1 | relink line, `system.parts.disk_name == 'disk_cas_shared'`. Before this design: bytes onto `default`. |
| `test_relink_wins_over_ttl_then_mover_converges` — as above plus `TTL ts TO DISK 'default' IF EXISTS` with `ts` already expired (`IF EXISTS` keeps node1, whose policy has no `default`, free of warnings) | relink line; the part is first on `disk_cas_shared`; then `system.parts` is polled until `disk_name == 'default'` (the background mover, as `test_ttl_move` does); `count()` and `sum(v)` equal before and after the move. |
| `test_two_pool_policy_relinks_into_second_pool` — node2 on `cas_two_pools` | relink line, part on `disk_cas_shared`. Closes CAS-134. The only test whose advertise has two elements, so it is also the proof of the tokenizer on both sides. |
| `test_mechanism_failure_falls_back_to_bytes_on_forced_disk` — failpoint `cas_relink_receiver_force_mechanism_failure` (existing) with `cas_tiered` | the byte line names `disk_cas_shared`, not `default`: the fallback keeps the forced disk. |

The existing eleven tests do not change. `test_detached_fetch_cross_pool_falls_back_to_bytes`
(node2 on `cas_other` alone — a one-element advertise of the wrong pool) and
`test_version_mix_legacy_peer_gets_bytes` stay valid because the one-element wire form is unchanged.
No gtest for the join/split helper: it lives in the anonymous namespace next to the zero-copy
tokenizer it copies, and the two-pool test catches any tokenizing error on either side. The stateless
content-addressed lane is single-disk and unaffected.

## Documentation to change with the code {#docs}

- `docs/en/antalya/cas/architecture/replication.md`: gate 1 (the receiver advertises the set of its
  policy's pools; the sender offers when its part's pool is in the set and names it), the sequence
  diagram line that shows the advertise, and a new anchored section "Where a relinked part lands":
  the placement rule, what it overrides (volume order, JBOD balancing, `max_data_part_size_bytes`,
  TTL move rules), and how a TTL rule catches up by the mover. The zero-copy `MOVE` path with an
  explicit disk is named as untouched.
- `docs/superpowers/cas/BACKLOG/replication.md`: `{#relink-advertises-only-first-ca-pool}` (CAS-134)
  marked closed by this design, kept for provenance.
- `docs/superpowers/cas/BACKLOG/formats-and-storage.md`: `[mixed-ca-tiered-topology]` gets its
  fetch-path answer (the tiered tests above are the first tests of the mixed topology) with the
  move-out caveat below left open.

## Rollout {#rollout}

No protocol version change, no setting, no persisted format. A mixed-build pair degrades to bytes
exactly as it does today (the wire section above). The behavioural change operators see: a fetched
part that is already in the pool now lands on the pool's disk even when the policy or a TTL rule would
have placed it elsewhere, and the mover carries it to the TTL destination afterwards.

One known open item is exercised more by this design and is recorded rather than fixed here:
CAS-020 (`{#move-out-copies-envelope-bytes}` in `BACKLOG/formats-and-storage.md`) — a move OUT of a
content-addressed disk onto a plain S3 disk on the same endpoint copies envelope bytes and fails
loudly. A policy holding a content-addressed pool and a plain S3 disk on the same endpoint, with a TTL
rule pointing at the plain disk, would reach that failure on the mover leg where the fetch used to
stream the bytes straight to the plain disk. The topology is exotic; declaring it supported needs
CAS-020 first.

## Out of scope {#out-of-scope}

- `[move-to-ca-relink-from-replica]` — a `MOVE`/TTL move onto a content-addressed disk relinking from
  a replica that already holds the part in the pool (the CAS analogue of zero-copy `tryToFetchIfShared`).
- `[zero-copy-parity-audit]` — walking every zero-copy site and classifying it for CAS.
- CAS-020 (above) and CAS-120 (`{#same-pool-move-reads-every-byte}`, the local same-pool CA→CA move).
- Any setting to disable the forcing.
