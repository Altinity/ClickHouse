---
description: 'Design for forced fetch-by-relink: when the sender holds the part on a content-addressed pool that the receiving table also has a live, writable disk for, the fetch always relinks onto that disk, overriding storage-policy order and TTL placement; the receiver advertises every such pool of its policy and the sender names the one it matched.'
sidebar_label: 'CAS forced relink on fetch'
sidebar_position: 1
slug: /superpowers/specs/cas-fetch-forced-relink-design
title: 'CAS forced relink on fetch design'
doc_type: 'design'
---

# CAS forced relink on fetch design {#cas-fetch-forced-relink-design}

Status: revision 3 of 2026-09-03. Revision 1 was the design agreed in the brainstorming interview;
revision 2 folded in the codex review of the same day (`tmp/codex_review_forced_relink.md`, verdict
"approve with changes"): the TTL test's SQL and choreography corrected, the byte-fallback and
read-only claims qualified, aliases of one mount deduplicated, unit vectors and three cheap
integration cases added. Revision 3 is the user's answer to the one question revision 2 had decided
on its own: a pool disk that is not live is NOT excluded from placement — the fetch fails closed and
is retried, exactly as a single-disk policy behaves today — so the liveness predicate revision 2
proposed (`canAcceptRelink`) is gone, and the sender-side alias dedupe is confirmed in scope. Line references are against `cas-gc-rebuild` at
`9be5befc3ec`. Backlog items this closes or touches: `[relink-advertises-only-first-ca-pool]`
(CAS-134) in [`BACKLOG/replication.md`](/superpowers/cas/backlog/replication) is closed by it;
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
| Relink versus the policy's own placement (volume order, JBOD balancing, `max_data_part_size_bytes`)? | Relink wins outright. If the policy has a candidate disk on the sender's pool, the part goes there. |
| Relink versus a `TTL ... TO DISK\|VOLUME` rule that names somewhere else? | Relink wins. The part lands on the pool's disk at zero byte cost; `MergeTreePartsMover::selectPartsForMove` sees `!isPartInTTLDestination` and moves it to the TTL destination afterwards (`MergeTreePartsMover.cpp:145-179`, `MergeTreeData.cpp:9169`). The bytes then travel once, as a `GET` from the pool on the receiver instead of a stream from the sender, and the sender is not loaded at all. Precedent: `perform_ttl_move_on_insert=0` already places first and moves later. |
| A caller-supplied `dest_disk`? | Authoritative, untouched. The only external caller is zero-copy `MOVE` (`MergeTreePartsMover::clonePart` → `tryToFetchIfShared` → `fetchExistsPart`, `StorageReplicatedMergeTree.cpp:5945`, `:6010`), which asserts the part landed on that disk (`LOGICAL_ERROR` otherwise) and renames it into `moving/`. A content-addressed disk never enters that path — `supportZeroCopyReplication()` is `false` for `MetadataStorageType::CAS` (`DiskObjectStorage.h:53-57`), and a cache wrapper over a CA disk is still a `DiskObjectStorage` with CAS metadata — so this is another feature's invariant that this one must not break, not a placement rule of its own. |
| Which disks on the right pool are candidates? | Every content-addressed disk of the policy that is not read-only and has a pool id. Read-only is a static property of the disk's configuration, not a failure, so it is the one exclusion. Whether the pool is LIVE is deliberately NOT consulted: a disk whose pool is `TransientNotLive`, `IdentityLost`, `Vanished*` or shut down (`ContentAddressedMetadataStorage::checkOpAdmitted`, `ContentAddressedMetadataStorage.cpp:1178-1229`) is still the forced target, the relink's own write gate refuses it, the fetch fails, and the replication queue retries — the behaviour a single-disk content-addressed policy already has, and the project's fail-close rule (no fallback path that performs a consequential action; here, a full byte download onto another disk). The alternative — a liveness snapshot that diverts the part to a local disk during a content-addressed incident — was considered and rejected by the user for that reason. `IDisk::isBroken` is not consulted either: nothing in `DiskObjectStorage` overrides it. |
| A new setting to turn the forcing off? | None. The rule is unconditional; the recursion brake (`allow_ca_relink`) stays the only internal switch. |

## Design {#design}

### Wire protocol {#wire}

The request parameter `cas_pool_uuid` becomes a set. Its wire form copies the zero-copy capability
list exactly — sorted, deduplicated, joined with `", "`, tokenized by the sender with the same
`", "` splitter (`DataPartsExchange.cpp:367-381` and `:751-757`) — so the exchange keeps one
list convention. A pool id is `u128ToHex` output, so the delimiter can never occur inside one. The
relink offer gains a third response cookie, `cas_pool_uuid`, naming the pool the sender matched, next
to `cas_relink` and `cas_source_token`. Reusing the parameter's name for the answer follows
`remote_fs_metadata` (request: the list; response cookie: the choice).

The protocol version does not move. `REPLICATION_PROTOCOL_VERSION_WITH_CA_CONFIRM = 11` encodes a
promise (the receiver confirms before it promotes); the shape of the advertise is not a promise, and
a version bump would only turn a compatible change into a mixed-build cliff. Compatibility is by
construction:

- Old sender, new receiver. The old gate compares the whole parameter string with its own pool id
  (`:410`). It matches exactly when the set has one element — whose wire form is byte-for-byte
  today's — and it sends no `cas_pool_uuid` cookie. The new receiver reads an absent cookie as "the
  single advertised pool" when the set has one element, which is exact because a multi-element string
  never matches an old sender. With more than one element and no cookie the pool is unknown and the
  receiver takes the byte fallback; that state is unreachable with a compliant old sender and is kept
  only so the receiver never guesses about a malformed peer.
- New sender, old receiver. The old receiver advertises one pool; the new sender sees a one-element
  set, offers as before, and adds a cookie the old receiver ignores. The old post-check
  (`chosen_ca->getPoolUUID() != advertised_pool_uuid`) works unchanged.

The argument is by construction and by the unit vectors in the verification section; a mixed-binary
interoperability test is NOT part of this design (it would need a released content-addressed build
on the other side), and the rollout section says so.

### Pure helpers, so the routing is unit-testable {#helpers}

No change to `IContentAddressedExchange`. The decisions that this design adds to the exchange are
all pure functions of a few strings and booleans, so they live in a new small pair,
`src/Storages/MergeTree/DataPartsExchangeCasRouting.{h,cpp}`, under `namespace DB::DataPartsExchange`,
with no dependency on disks, storages or HTTP — a gtest drives them directly, and
`DataPartsExchange.cpp` only builds their inputs from `data.getDisks()`:

- `String encodeCasPoolAdvertise(Strings pool_uuids)` — sort, unique, join with `", "`; empty in,
  empty out. `Strings decodeCasPoolAdvertise(const String &)` — the inverse, the same `", "`
  splitter the zero-copy capability list uses; an empty string decodes to no pools.
- `String resolveOfferedCasPool(const Strings & advertised_pools, const String & offered_pool_cookie)`
  — which pool an offer is for: the cookie, or the single advertised pool when the cookie is absent,
  or nothing. Used on its own by the receiver's post-check and inside the next helper.
- `struct CasRelinkCandidate { String disk_name; String pool_uuid; bool read_only; }` and
  `std::optional<size_t> resolveForcedCaCandidate(const std::vector<CasRelinkCandidate> & candidates,
  const Strings & advertised_pools, const String & offered_pool_cookie)` — the receiver's placement:
  the index of the first candidate on the offered pool that is not read-only and has a pool id.
- `struct CasConfirmRoutingCandidate { const void * exchange_identity; String pool_uuid; bool owns_namespace; }`
  and `std::optional<size_t> resolveConfirmRoutingCandidate(const std::vector<CasConfirmRoutingCandidate> &,
  const String & pool_uuid)` — the sender's confirm routing: among candidates on `pool_uuid` that own
  the namespace, distinct by `exchange_identity`, exactly one may answer; zero or several distinct
  identities give no answer, and aliases (equal identity) collapse to the first.

The pool id read from a disk is set once at startup (`ContentAddressedMetadataStorage.cpp:884`) and
never cleared, so it stays readable after `shutdown` nulls the pool; an empty pool id means only
"never started", and such a disk is simply not advertised.

### Sender {#sender}

`Service::processQuery`, the relink gate at `:404-434`: `receiver_pool_uuid == ca_meta->getPoolUUID()`
becomes "the pool id of the part's disk is in the decoded advertised set", and the offer adds the
`cas_pool_uuid` cookie with that id. `getRelinkOffer`, the source token, the manifest payload and
`addLastSentPart` are untouched. The sender still looks only at its own part's disk; it does not care
how many pools the receiver has.

One adjacent change, confirmed in scope by the user: `resolveContentAddressedConfirm`
(`:227-244`) requires EXACTLY one policy disk to match `(pool, server_root_id, namespace)` and
answers `Unknown` on a second match. A cache disk over a content-addressed disk reuses the base
disk's `ContentAddressedMetadataStorage` (`DiskObjectStorageCache.cpp:21-41`), so a policy that lists
both the base disk and its cache wrapper matches twice and can never confirm — today, independent of
this design. The routing loop becomes a call to `resolveConfirmRoutingCandidate`, which deduplicates
by exchange identity (the `IContentAddressedExchange` pointer). Two genuinely distinct mounts of one
pool on one server keep answering `Unknown`, as designed. The shape is proven by unit vectors, not
by an integration topology: a policy listing a base disk and its cache wrapper would also make
`loadDataParts` see every part twice, so it is not a topology to stand up in a test.

### Receiver {#receiver}

Everything is inside `Fetcher::fetchSelectedPart`, in the `dest_disk == nullptr` branch; the
explicit-disk branch is not modified.

**Advertise.** With `allow_ca_relink` set and no `dest_disk`: the pool ids of every disk in
`data.getDisks()` (the policy's disks, in policy order) for which `tryGetContentAddressedExchange`
is non-null, `disk->isReadOnly()` is false and `getPoolUUID()` is non-empty; `encodeCasPoolAdvertise`
deduplicates by pool id, so a base disk and its cache wrapper count once. Today's `break` on the
first content-addressed disk goes away. With a `dest_disk`: its own pool if it is content-addressed, otherwise nothing — unchanged.
With `allow_ca_relink` clear: no parameter — unchanged, this is the recursion brake.

**Resolve the forced disk before the reservation.** The response cookies are readable right after
the request, before any body field: the buffer is built with `withDelayInit(false)`, whose
constructor receives the response and stores the cookies before exposing the body
(`ReadWriteBufferFromHTTP.cpp:238-247`, `:392-447`), and `getResponseCookie` reads that store without
consuming body bytes. The body-read order (`sum_files_size`, `ttl_infos`, part type, uuid,
projections, then the manifest) is unchanged — the sender writes them in that order and the relink
branch at `:404` comes after all of them. A new block after the cookie reads at `:786-787`, and only
when `cas_relink` is present, so ordinary and zero-copy selection are untouched:

1. `offered_pool` is the `cas_pool_uuid` cookie; if the cookie is absent and the advertised set has
   exactly one element, that element; otherwise empty.
2. `forced_ca_disk` is the first candidate in policy order — the same predicate as the advertise —
   whose pool id equals `offered_pool`; null if none, or if `offered_pool` is empty. Both steps are
   `resolveForcedCaCandidate` over the candidate list built from `data.getDisks()`. Aliases of one
   mount collapse to the first; two genuinely distinct mounts of one pool in one policy also take the
   first, with a `DEBUG` line naming the candidates — that shape is not designed for.

**Reservation.** `if (forced_ca_disk) reservation = data.reserveSpace(sum_files_size,
forced_ca_disk)` — the existing static `reserveSpace(UInt64, SpacePtr)` (`MergeTreeData.h:1237-1242`,
`MergeTreeData.cpp:9009-9014`), whose `checkAndReturnReservation` throws `NOT_ENOUGH_SPACE` if the
disk declines. On an object-storage disk that cannot happen for capacity (`DiskObjectStorage`
reports no total, available or unreserved space, `DiskObjectStorage.h:68-70`, and `tryReserve`
then always succeeds); it stays a throw rather than a fallback so that an impossible state fails
loudly and the replication queue retries. The `balancedReservation`,
`reserveSpacePreferringTTLRules` and `reserveSpace(size)` branches are skipped in this case — that
is the whole of "relink overrides the policy and TTL". Skipping `balancedReservation` drops only its
JBOD accounting; the optional `tagger_ptr` is populated only when a balanced reservation succeeds,
and nothing downstream of `fetchSelectedPart` requires it. Then `disk = reservation->getDisk()` as
today.

**The relink block** (`:889-951`) stays. Its post-check compares the chosen disk's pool with
`offered_pool` instead of `advertised_pool_uuid`; since the chosen disk is the forced one, the check
is now a defensive exit that stays a logged byte fallback, not a `chassert` — release builds must
fail closed the same way as debug builds. If a malformed peer sets both `cas_relink` and
`remote_fs_metadata`, the relink block runs first (`:889` before `:953`) and wins; an honest sender
cannot emit both (its relink branch returns at `:429`, before the zero-copy branch at `:436`).

**Receiver exits, all before taxonomy row 0.**

- E1, an offer for a pool this policy has no candidate disk for (`cas_relink` set, `forced_ca_disk`
  null). A storage-policy reload cannot remove a disk (`StoragePolicy::checkCompatibleWith`,
  `StoragePolicy.cpp:370-417`) and read-only is static, so the only real cause is a sender that is
  not honest (or, with a multi-element advertise, one that omitted the cookie). The reservation runs
  the ordinary way and the relink block takes `fall_back_to_byte_fetch()` with the ordinarily reserved
  disk and `allow_ca_relink=false`. The manifest body is discarded with the response, exactly as
  today.
- E2, the forced-disk reservation declined: `NOT_ENOUGH_SPACE`, queue retry. Named so that it is
  never mistaken for a fallback.
- E3, the forced disk's pool is not `Live` when the relink is staged — the deliberate decision above,
  not a race. `prepareAdoptFromManifest` maps the mount fence's refusal (`ABORTED`/`NETWORK_ERROR`)
  to `MechanismFallbackAllowed` (`ContentAddressedMetadataStorage.cpp:2346-2361`), the byte
  re-request below writes through the same disk and is refused in turn, the fetch throws, and the
  queue retries with its usual backoff. Fail-closed by construction, and NOT a fallback to another
  disk. The cost per attempt is one relink offer (the sender resolves the manifest and mints a
  token) and a byte re-request that is refused at its first write.

**Byte fallback after a mechanism failure.** `relinkPartToDisk` returning null already re-requests
through `fall_back_to_byte_fetch`, which passes `disk` and `allow_ca_relink=false` (`:907-914`; the
zero-copy fallback at `:990-1008` clears the brake too, and there are no other same-sender
re-requests). `disk` is now the forced content-addressed disk, so the byte fetch is ATTEMPTED on the
pool's disk. For the mechanism-only causes (undecodable manifest, body-absent precommit, a precommit
that is no longer the live owner, a ref conflict, the test failpoint) it lands there: the bytes
content-address and deduplicate against the pool, and the placement stays "the pool's disk", with a
TTL rule catching up by the mover as in the relink case. For the fence cause it is E3. No code changes
here; only the provenance of `disk` does.

**Read-only disks.** A read-only content-addressed disk is not a candidate (`DiskObjectStorage::isReadOnly`
delegates to the object storage, `DiskObjectStorage.cpp:716-719`, and `ContentAddressedMetadataStorage`
derives its own `read_only` from the same backend at startup, so the disk-level predicate is enough).
Today such a disk in a mixed policy is advertised, draws an offer, fails the pool post-check after a
local reservation and re-requests bytes — a pointless round trip that the exclusion removes. The
exclusion does not make a fetch succeed on its own: the sender streams bytes and the ordinary
reservation places them, which works when the policy has another writable disk and returns
`READONLY` when every volume is read-only (`StoragePolicy.cpp:269-288`), exactly as today.

**Unchanged.** `to_detached` (`relinkPartToDisk` builds its parent from it and stages under
`detached/`, `:1444-1459`); projections (the relinked part loads them from the published manifest
through `loadColumnsChecksumsIndexes`; the projection count the sender writes is read and unused on
relink, as today); the recursion brake; the seven-row failure taxonomy of `relinkPartToDisk`; the
confirm handshake.

**Logging.** One `DEBUG` line on a forced placement: the part, the offered pool, the disk, and that
storage-policy order and TTL rules were bypassed. It is diagnostic and test observability, not an
operator surface: the existing relink and download completion lines are `DEBUG` too (`:1602`,
`:1282`), and no counter is added — a relink now always lands on the pool's disk, so "forced" is
not a distinct event to count. The positive test signal stays the existing
`Relink of part <p> onto disk <d> finished (no bytes transferred).`

### Invariants {#invariants}

- I1. A `dest_disk` supplied by an external caller of `fetchSelectedPart` is never overridden. (The
  two internal same-sender re-requests pass the disk already chosen; they preserve the invariant
  rather than originate it.)
- I2. After a relink the part is on a disk of the receiving table's storage policy whose pool id equals
  the pool id of the part's disk on the sender. Unconditional: a pool disk that cannot take the part
  fails the fetch rather than yielding to another disk.
- I3. At most one relink attempt per fetch (the recursion brake).

## Verification {#verification}

### Unit vectors {#unit}

`src/Storages/tests/gtest_cas_relink_pool_routing.cpp`, suites `CASRelinkPoolAdvertise` and
`CASRelinkConfirmRouting` (the content-addressed gtest gate filter is exactly `CAS*`), over the pure
helpers:

- encode: empty → empty string; one id → that id verbatim (the byte-for-byte-today claim);
  duplicates collapse; several ids sort; decode is the inverse and tolerates a single id with no
  delimiter and an empty string.
- resolve (receiver): cookie names an advertised pool with a candidate → that candidate; cookie
  absent, one advertised pool → its candidate; cookie absent, two advertised pools → none; cookie
  names a pool whose only candidate is read-only → none; cookie names a pool with no candidate →
  none; two candidates on one pool → the first; a candidate with an empty pool id never matches.
- confirm routing (sender): one owning candidate on the pool → it; none → none; two owning
  candidates with distinct identities → none; two with the same identity → the first; a candidate on
  the pool that does not own the namespace is ignored.

### Integration {#tests}

All in `tests/integration/test_cas_replicated_relink` (two nodes over RustFS; node2 already loads
`storage_conf_other_pool.xml`). The sender, node1, keeps the single-disk `cas_shared` policy in every
test — otherwise its part is not on the pool and there is no offer to force. Every relink assertion is
positive, per the file's own rule: the `Relink of part … finished (no bytes transferred).` line for a
relink, `Download of part … onto disk <d> finished.` for bytes; blob counts corroborate, never prove.

Two new policies on node2, no new disks beyond the local `default`:

- `cas_tiered`: volumes `[hot: default]`, `[cold: disk_cas_shared]`. The local volume comes FIRST so
  that today's `StoragePolicy::reserve` is certain to pick it (`min_bytes_to_rebalance_partition_over_jbod`
  defaults to 0, so the balanced reservation does not engage and the first writable volume wins).
- `cas_two_pools`: volumes `[first: disk_cas_other]`, `[second: disk_cas_shared]`. The first
  content-addressed disk is the other pool.

| Test | Proves |
|---|---|
| `test_tiered_policy_relinks_onto_cas_over_volume_order` — node2 on `cas_tiered`, the table carries one projection, queue fetch after an insert on node1 | relink line, `system.parts.disk_name == 'disk_cas_shared'`, the projection is queryable on node2. Before this design: bytes onto `default`. |
| `test_relink_wins_over_ttl_then_mover_converges` — a dedicated table `(id Int64, v UInt64, s String, ts DateTime)` with `TTL ts TO DISK IF EXISTS 'default'` (`IF EXISTS` precedes the name, `ExpressionElementParsers.cpp:2634-2643`; without it `CREATE TABLE` on node1, whose policy has no `default`, fails with `BAD_TTL_EXPRESSION` — `MergeTreeData::checkTTLExpressions` rejects a `TO DISK` destination absent from the policy at create time), rows inserted with `ts = now() - INTERVAL 1 DAY`; node2 runs `SYSTEM STOP MOVES` before the fetch and `SYSTEM START MOVES` after the intermediate assertions, restored in a `finally` | relink line; the part is on `disk_cas_shared` while moves are stopped; after `START MOVES`, `system.parts` is polled until `disk_name == 'default'` (the background mover, as `test_ttl_move` does); `count()` and `sum(v)` equal before and after the move. |
| `test_two_pool_policy_relinks_into_second_pool` — node2 on `cas_two_pools` | relink line, part on `disk_cas_shared`. Closes CAS-134. The only test whose advertise has two elements: today the `break` advertises `disk_cas_other`, the sender's equality gate declines, and the reservation lands on the other pool — so it fails before the change and passes only if both ids are advertised and tokenized on both sides. |
| `test_mechanism_failure_falls_back_to_bytes_on_forced_disk` — failpoint `cas_relink_receiver_force_mechanism_failure` (existing, fires after the token gate, `:1431-1442`) with `cas_tiered` | the byte line names `disk_cas_shared`, not `default`: the fallback keeps the forced disk. Today the pool post-check falls back to `default` before the failpoint is reached. The existing `test_recursion_brake_bounds_relink_to_one_attempt` keeps proving the one-offer bound; this test proves only the destination. |
| `test_detached_fetch_relinks_onto_cas_under_tiered_policy` — `ALTER TABLE … FETCH PART … ` into `detached/` on node2 under `cas_tiered` | relink line, the detached part sits on `disk_cas_shared`, `ATTACH PART` succeeds. The existing detached tests run on the single-disk policy only. |
| `test_offer_for_unavailable_pool_falls_back_to_ordinary_placement` (E1) — new receiver failpoint `cas_relink_receiver_drop_forced_disk`, which nulls `forced_ca_disk` after resolution, under `cas_tiered` | byte line names `default`; no relink line; the sender logged exactly one offer. |
| `test_offer_without_pool_cookie_resolves_to_single_advertised_pool` — new sender failpoint `cas_relink_sender_omit_pool_cookie`, node2 on `cas_tiered` (one advertised pool) | relink line on `disk_cas_shared`: the absent-cookie rule, i.e. the old-sender shape, works. The same failpoint with node2 on `cas_two_pools` (two advertised pools) yields the byte line and no relink: the malformed-peer arm fails closed. |

The existing eleven tests do not change. `test_detached_fetch_cross_pool_falls_back_to_bytes`
(node2 on `cas_other` alone — a one-element advertise of the wrong pool) and
`test_version_mix_legacy_peer_gets_bytes` (a raw protocol-version-10 request against the current
sender) stay valid because the one-element wire form is unchanged; neither is a mixed-binary test,
and this design adds none. The stateless content-addressed lane is single-disk and unaffected.

What the plan does NOT test, stated so that it is not read as proven: a read-only content-addressed
disk in a live topology (covered by the resolve vectors only), E3 (a non-live pool disk; the
fail-closed path is `prepareAdoptFromManifest`'s existing behaviour, already exercised by the
single-disk fixture whenever a mount is fenced), the sender-side alias dedupe in a live topology
(unit vectors only, for the `loadDataParts` reason above), and two genuinely distinct mounts of one
pool in one policy (unsupported).

## Documentation to change with the code {#docs}

- `docs/en/antalya/cas/architecture/replication.md`: gate 1 (the receiver advertises the set of its
  policy's live, writable pools; the sender offers when its part's pool is in the set and names it),
  the sequence diagram line that shows the advertise, and a new anchored section "Where a relinked
  part lands": the placement rule, what it overrides (volume order, JBOD balancing,
  `max_data_part_size_bytes`, TTL move rules), how a TTL rule catches up by the mover, and that a
  fetch whose pool disk is not live fails and is retried rather than re-placed. The zero-copy
  `MOVE` path with an explicit disk is named as untouched.
- `docs/superpowers/cas/BACKLOG/replication.md`: `{#relink-advertises-only-first-ca-pool}` (CAS-134)
  marked closed by this design, kept for provenance.
- `docs/superpowers/cas/BACKLOG/formats-and-storage.md`: `[mixed-ca-tiered-topology]` gets its
  fetch-path answer (the tiered tests above are the first tests of the mixed topology) with the
  move-out caveat below left open.

## Rollout {#rollout}

No protocol version change, no setting, no persisted format. A mixed-build pair degrades to bytes by
construction (the wire section above); that argument is checked by the unit vectors and the
omit-cookie failpoint, not by a mixed-binary run. The behavioural changes operators see: a fetched
part that is already in the pool now lands on the pool's disk even when the policy or a TTL rule
would have placed it elsewhere, and the mover carries it to the TTL destination afterwards; and in a
mixed policy a fetch whose pool disk is not live fails and is retried by the queue instead of
quietly landing on the local disk — the single-disk behaviour, now also the mixed-policy one.

One known open item is exercised more by this design and is recorded rather than fixed here:
CAS-020 (`{#move-out-copies-envelope-bytes}` in `BACKLOG/formats-and-storage.md`) — a move OUT of a
content-addressed disk onto a plain S3 disk on the same endpoint copies envelope bytes and fails
loudly. A policy holding a content-addressed pool and a plain S3 disk on the same endpoint, with a TTL
rule pointing at the plain disk, would reach that failure on the mover leg where the fetch used to
stream the bytes straight to the plain disk. The topology is exotic; declaring it supported needs
CAS-020 first. A local `default` destination is not that topology: `clonePart` branches on the
destination, and a local destination reads the content-addressed source through its ordinary file
interface (`DataPartStorageOnDiskBase.cpp:771-836`).

## Out of scope {#out-of-scope}

- `[move-to-ca-relink-from-replica]` — a `MOVE`/TTL move onto a content-addressed disk relinking from
  a replica that already holds the part in the pool (the CAS analogue of zero-copy `tryToFetchIfShared`).
- `[zero-copy-parity-audit]` — walking every zero-copy site and classifying it for CAS.
- CAS-020 (above) and CAS-120 (`{#same-pool-move-reads-every-byte}`, the local same-pool CA→CA move).
- Mixed-binary interoperability tests.
- Any setting to disable the forcing.
