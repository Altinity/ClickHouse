---
description: 'Live backlog — replication and MergeTree integration: MOVE PART/PARTITION onto CA disks, merge/insert retry interactions with the mount-lease fence, and cross-replica relink.'
sidebar_label: 'Replication'
sidebar_position: 5
slug: /superpowers/cas/backlog/replication
title: 'CAS Backlog — Replication and MergeTree integration'
doc_type: 'guide'
---

# CAS Backlog — Replication and MergeTree integration {#replication}

Part of the [CAS live backlog](/superpowers/cas/backlog). Topic file for `MOVE PART`/`PARTITION`
onto CA disks, merge/insert retry interactions with the mount-lease fence, and cross-replica relink.

- **[move-part-to-ca-architecturally-unimplemented] ✅ CLOSED at HEAD by L1 (`2f2a3b01aa6`, `4d73e198f6b`, `81eab8b6968`) + L2 (`4229a1477be`) — verified 2031-triage CAS-120; kept for provenance, the header's "architecturally unimplemented" no longer holds** — was HARD — `ALTER TABLE ... MOVE PART|PARTITION TO DISK|VOLUME <ca disk>` throws `ABORTED` ("promote: ref already names a different committed manifest") or `NOT_IMPLEMENTED` depending on which file the generic per-file copy reaches first. Root cause, two layers: (1) `PartPathParser`/`ContentAddressedMetadataStorage::route()` have no case for `MergeTreeData::MOVING_DIR_NAME`, so every moved part's files collide on the literal ref name `"moving"`; (2) deeper and not fixed by (1) alone — `MergeTreePartsMover::clonePart`'s generic per-file copy opens a fresh autocommit `DiskObjectStorageTransaction` per file with no shared transaction threaded through the whole part clone, so the second file's independent `promote()` collides with the first's. A real fix needs a CAS-aware `clonePart`/copy override (mirroring the existing `supportZeroCopyReplication()` special case in the same function) that stages every file into ONE `PartWriteTxn` and commits/promotes once. Blocks all of S36 (MOVE is its first leg) and S37's explicit-MOVE and TTL-MOVE-to-`cas` legs; unaffected: routing at INSERT time (`max_data_part_size_bytes`), merges writing a brand-new part, and off-CA moves (CA→local).
- **[VERIFY-ca-ca-same-pool-move] CA↔CA same-pool `MOVE PART`/`PARTITION` (S37 3-disk), after MOVE-to-CA lands** — VERIFY — The MOVE-to-CA fix targets local↔CA. Moving a part between two CA disks in the SAME pool (the S37 3-disk `ca_local3` variant) is expected to LIKELY work via the same generic L1+L2 code with no special-casing: the moved part's content is already in the pool, so the target publish dedup-resolves the blobs (near-free) and is effectively a ref repoint. `05025_cas_attach_partition_cross_disk` now proves that publish side for same-pool CA→CA `ATTACH PARTITION FROM`, including `CASBlobBodyPutAvoided > 0`; it does NOT answer this item's collision question because ATTACH writes a temporary ref in a different table namespace. UNVERIFIED interaction: whether `MOVE`'s target final ref `<part>` collides benignly with the source's existing ref `<part>` in the SAME table namespace — or whether same-pool CA↔CA needs the source ref dropped before/as the target publishes. TODO: run the S37 CA↔CA-move leg; if it passes → close; only add special handling if it shows a real collision. NOT a blocker for the local↔CA cut.
- **[killed-mid-move-partition-duplicate] killed mid-`MOVE PARTITION` leaves a persistent duplicated part** — {#killed-mid-move-partition} — VERIFY — CONFIRMED (S37 chaos leg): hard-killing a replica mid-`ALTER TABLE ... MOVE PARTITION ID 'all' TO VOLUME 'cas'` leaves the partition DUPLICATED after heal (`count()=200` but `uniqExact(id)=100`, consolidated into one active part by a later merge, so it does NOT self-heal — a merge does not dedup rows). S36's single-part `MOVE PART` chaos leg IS atomic/green — only the multi-generation `MOVE PARTITION` policy/TTL path under kill duplicates. ATTRIBUTION: PRE-EXISTING, not caused by the MOVE-to-CA (R2) fix — the pre-R2 baseline showed the same duplication via a different failure mode (R2 fixed the move mechanism, not the duplication). LIKELY GENERIC: plausibly a generic ClickHouse crash-atomicity/replication-replay duplication (a kill mid-move + queue/replica replay applying it twice), not the CA `moving/`-prefix recovery. TODO: repro `MOVE PARTITION` + hard-kill on a NON-CA multi-disk policy to confirm generic; if generic → upstream/known-limitation, if CA-specific → investigate the `moving/`-ref restart recovery promoting a stale staging part. Chaos-edge; does not block the local↔CA MOVE-to-CA feature.
- **[merge-progress-reset-mount-fence] merge "progress reset in loops" after a sustained S3 fault** — PARTIAL — The common transient-blip arm is fixed: `MountLeaseKeeper` now retries one immutable renewal body inside the last confirmed lease, resolves ambiguous responses by exact `GET`, and records the trip/remount reason at default level and in `ProfileEvents`. The 15-minute S39 gate recovered 50 isolated short pulses without a fence loss. A sustained fault can still close the fence correctly, after which every write fails the `stageManifest` fence gate until self-remount succeeds. The remaining replication-specific defect is unchanged: CAS reports that refusal as `ABORTED`, which `ReplicatedMergeMutateTaskBase` treats as "not an error", so its existing exponential backoff does not engage and the scheduler can recompute the same merge in a tight loop (239 times in one repro). Introduce a retry-later class outside the genuine-cancellation `ABORTED` exemption at this boundary. Also still owed: fsck-to-fixpoint after fence-loss recovery, linked to the S30 DANGLING-PRECOMMIT gap. Repro harness: `utils/ca-soak/docker-compose-s3faultproxy.yml`.
- **[r3-acked-lost-dataloss] acked-then-lost INSERT data loss on cross-request retry — FIXED** — Root cause: on CA, `renameParts` is a pure overlay re-key with no publish, so the Keeper block_id/part-znode multi commits durably BEFORE the CAS manifest — a genuine split-commit window. A part lost in that window left its dedup token behind, so a byte-identical client retry deduped against it and acked with zero rows written. Fixed by `77484196b0d`: closes every part disk-storage transaction in `MergeTreeData::Transaction::renameParts`, so the part is durable before the Keeper block_id registration. Gates all green post-fix (S40 10/10 acked=3796 lost=0; `dl_probe` LOST=0, was ~198/1314; the original R4 chaos recipe that lost 1118 rows now PHASE3 OK with zero deficit). R3 ship-readiness restored. Residual: a narrower hazard (block_id outliving a durably-committed part lost later) stays out of scope — verify-on-dedup is the candidate if it ever matters. Open thread: an upstream submission draft exists (`tmp/upstream_issue_dedup_durability.md`), pending a user decision on whether to send it.
- **[RPL-5 slice] `REPLACE PARTITION`/`ATTACH PARTITION ... FROM` queue-clone relink, untested on CA** — TEST — A `REPLACE_RANGE` log entry cloned to a second replica reduces to fetch (relink or byte) + drop, individually working, but no integration test proves the cloned fetch specifically relinks rather than byte-refetches (RPL-4 disables `to_detached` relink explicitly, so the branch taken isn't obvious a priori). Needs `test_cas_replicated_relink`'s 2-replica rustfs fixture extended with a `REPLACE PARTITION`/`ATTACH ... FROM` scenario plus a blob-count relink proof. Pulled into the publish-confirm fetch-handoff iteration's test package, which touches the same relink-eligibility branch. (An orphaned 2026-08-04-triage finding, C-0098, describes this identical test gap — folded in here rather than inserted as a separate item, since it duplicated this one exactly.)
- **[B66b] relink-into-detached (zero-byte `to_detached` fetch for same-pool parts)** — IN PROGRESS (2026-07-23) — folded into the publish-confirm fetch-handoff iteration: relink already publishes under `tmp-fetch_<part>` and re-keys via `renameTempPartAndReplace`, so detached needs only lifting the `!to_detached` advertise gate (`DataPartsExchange.cpp:540-545`) + the detached temporary name + the same confirm step; collision semantics inherited from the byte path by construction. (RPL-4 perf cliff.)
- **[move-to-ca-relink-from-replica] `MOVE`/TTL-move onto a CA disk should relink from a replica that already holds the part in that pool, the way zero-copy `MOVE` fetches instead of copying** — {#move-to-ca-relink-from-replica} — DESIRABLE (recorded 2026-09-03) — Zero-copy has this today: `MergeTreePartsMover::clonePart` (`MergeTreePartsMover.cpp:241-263`) asks `tryToFetchIfShared(part, disk, moving/<part>)` BEFORE copying when the destination `supportZeroCopyReplication()`, and `fetchExistsPart` (`StorageReplicatedMergeTree.cpp:5945`, the only `fetchSelectedPart` caller passing an explicit `dest_disk`) brings the metadata onto exactly that disk at exactly that path. A CA destination never enters that branch — `supportZeroCopyReplication()` is `false` for `MetadataStorageType::CAS` by design (`DiskObjectStorage.h:53-57`, the B31 honest capability) — so a local→CA move always takes the L1+L2 `clonePart` transaction: every byte is read locally, hashed, `HEAD`-probed and, on the first replica to get there, uploaded. In the realistic tiered topology (hot local + CA cold, `TTL ... TO VOLUME 'cas'`) every replica holds the part locally and all of them hit the TTL at about the same moment, so N replicas race the same upload (the `HEAD`-before-`PUT` dedup only elides a `PUT` whose predecessor has already FINISHED) and the N-1 losers still pay the full local read + hash for a publish that resolves to a ref. Wanted shape: a CA-specific hook at the same seam as `tryToFetchIfShared` (NOT by flipping `supportZeroCopyReplication`, which must stay `false`): ask a replica holding the part on a disk of the target's pool for a relink offer — the `cas_pool_uuid`/`getRelinkOffer` exchange the fetch already uses — adopt it into `moving/<part>` through `prepareAdoptFromManifest` + confirm + promote, and fall back to the byte clone when nobody offers. Open sub-questions the design must answer: replica discovery (zero-copy reads Keeper zero-copy locks via `getSharedDataReplica`; CAS has no cross-server index of who holds a ref, so the candidate is "try the active replicas that list the part, the sender's own pool gate filters"), and whether the N-replica TTL race wants a `zero_copy_merge_mutation_min_parts_size_sleep_before_lock`-style stagger so exactly one replica uploads and the rest relink. Related: `{#same-pool-move-reads-every-byte}` (CAS-120) is the LOCAL leg — same-pool CA→CA where the source manifest is on this server and no replica is needed; this item is the cross-replica leg for a non-CA source. Ordered after the forced-relink-on-fetch design (`docs/superpowers/specs/2026-09-03-cas-fetch-forced-relink-design.md`) lands: that work provides the multi-pool advertise and the receiver-side "put it on the pool's disk" placement this item reuses.
- **[zero-copy-parity-audit] inventory every zero-copy replication feature and classify it for CAS: parity, not-needed-by-construction, or missing** — {#zero-copy-parity-audit} — RESEARCH (recorded 2026-09-03) — CAS is the content-addressed successor of zero-copy on the fetch path (relink) but nobody has walked the rest of the zero-copy surface with that question. Method: enumerate every site behind `allow_remote_fs_zero_copy_replication`, `supportZeroCopyReplication`, the `*SharedData*` family (`lockSharedData`/`unlockSharedData`/`getSharedDataReplica`/`removeSharedDetachedPart`), `tryToFetchIfShared`, `remote_fs_metadata`, and the ten `*zero_copy*` MergeTree settings (`grep -rhoE 'MergeTreeSettings[A-Za-z0-9]* [a-z_]*zero_copy[a-z_]*' src/Storages` lists them), and for each write one row: what zero-copy does there, what CAS does today (code path named), verdict. Starting points, NOT verdicts — the audit has to confirm each: metadata-only fetch (`remote_fs_metadata` → `downloadPartToDiskRemoteMeta`) = parity via relink; `MOVE` via `tryToFetchIfShared` = missing, `[move-to-ca-relink-from-replica]` above; merge/mutation coordination (`zero_copy_merge_mutation_min_parts_size_sleep_before_lock`, `..._no_scale_before_lock`, the zero-copy lock that makes one replica merge while the others fetch) — on CAS every replica executes the merge log entry and publishes, so N identical merges race the same upload unless one finishes first; whether "one merges, the rest relink" is wanted is the audit's central question; Keeper shared-data locks and `remote_fs_zero_copy_zookeeper_path` = replaced by the ref ledger + reachability GC, not needed by construction, but check the observability side (zero-copy can answer "which replicas hold this part's blobs", CAS has no such view); part-removal coordination (`zero_copy_concurrent_part_removal_max_split_times`, `..._max_postpone_ratio`) = removal is a ref drop, verify nothing else is owed; the three `disable_{freeze,detach,fetch}_partition_for_zero_copy_replication` guards = freeze/detach/fetch are supported on CA (`05024_cas_freeze`, B66b), verify each guard's reason does not apply; `remote_fs_zero_copy_path_compatible_mode` = lock-path upgrade compat, not applicable; detached parts on shared storage (`removeSharedDetachedPart`) = detached refs are ordinary `detached/<name>` refs, verify the removal drops the ref; `ATTACH PARTITION FROM`/`REPLACE PARTITION` hardlink clones with `lockSharedData(replace_existing_lock)` = `05025_cas_attach_partition_cross_disk` proves the same-pool publish, verify the cross-replica queue-clone leg (`[RPL-5 slice]`); backups over zero-copy disks = audit. Output: the table in this file, one row per site, plus a new backlog item for every "missing" verdict. Executes as a standalone research task (one ca-arch pass or a human day) after the forced-relink-on-fetch design (`docs/superpowers/specs/2026-09-03-cas-fetch-forced-relink-design.md`) lands, so the fetch row is written against the final shape.

## The relink manifest-decode fallback catches `CORRUPTED_DATA` only, not `UNKNOWN_FORMAT_VERSION` (2031-triage CAS-043) {#relink-fallback-unknown-format-version}

`ContentAddressedMetadataStorage::prepareRelink`'s decode guard degrades to a byte fetch for
`CORRUPTED_DATA` and rethrows everything else
(`ContentAddressedMetadataStorage.cpp:2264-2272`), while `decodePartManifest`'s header gate
(`Formats/CasPartManifestFormat.cpp:129` → `expectHeaderLine` → `checkCompatibility`) and the
critical-key rule (`Formats/CasTextFormat.cpp:249-251`, a `!`-prefixed key) both raise
`UNKNOWN_FORMAT_VERSION`. So the two "this build cannot read the sender's manifest" signals the
format layer was designed to emit are the two the mechanism-unavailable fallback does not accept: the
whole fetch fails loudly instead of re-requesting the bytes.

Not reachable today, and that is why this is hardening rather than a defect: relink is offered only
when both sides carry the same pool UUID (`DataPartsExchange.cpp:925-932`), mounting a pool decodes
`_pool_meta` through an EXACT-generation gate (`Formats/CasPoolMetaFormat.cpp:111-117` backward,
`checkCompatibility` forward, both at `G_BUILD`), and no `!`-prefixed key exists in any codec — so a
generation-skewed pair cannot share a mounted pool in the first place. Wire garbling of the header's
`v` field is the only live path, and it fails loudly and self-heals on the queue's next fetch attempt.

Fix shape (one line plus a test): accept `UNKNOWN_FORMAT_VERSION` alongside `CORRUPTED_DATA` in that
catch, exactly as `Pool/CasRefLedger.cpp:177` already pairs the two codes. Do this before the first
release that admits a mixed-generation pool (see B180 / format-freeze in
`operability-and-introspection.md`), since after that the narrow catch turns a degradable fetch into
a hard replication stall.

## The `.tmp` + `replaceFile` write dance is unusable on a committed CA part (2031-triage CAS-057) {#tmp-replacefile-on-committed-part}

`ContentAddressedTransaction::moveFile` services only sources STAGED in the same transaction; a
source that is not staged throws `LOGICAL_ERROR` ("moveFile source not staged",
`ContentAddressedTransaction.cpp:1529-1534`), and `replaceFile` delegates to it
(`:1537-1552`). So the generic local-disk crash-safety pattern — write `<name>.tmp`, then
`replaceFile` — cannot work against a published part when the two steps run in separate
transactions: the `.tmp` write autocommits into the manifest as an ordinary inline entry (it is not
blob-mandatory, `ContentAddressedTransaction.cpp:65-73`, so the autocommit gate at `:766-771` admits
it), and the following `replaceFile` finds nothing staged and fails loudly. Fail-closed, not silent:
no wrong bytes are ever published; the residue is a `<name>.tmp` entry in the committed manifest.

The supported shape for such callers already exists and is generic, not CA-specific:
`IDataPartStorage::supportsAtomicFileWrites` (`src/Storages/MergeTree/IDataPartStorage.h:198-200`,
true for CA via `ContentAddressedMetadataStorage.h:261`), which
`VersionMetadataOnDisk::storeInfoToDataPartStorage` consults to write `txn_version.txt` in one shot
instead of the rename dance (`src/Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp:329`,
landed in `45e43b37aaf`). The alternative is to run both steps inside ONE
`disk->createTransaction()`, where the `.tmp` entry is staged and `moveFile` re-keys it in place
(`ContentAddressedTransaction.cpp:1504-1527`).

Latent, with no production caller today. The one named by the audit —
`DeleteBitmapFileOps::writeBitmapToStorage` (`src/Storages/MergeTree/UniqueKey/DeleteBitmapFileOps.cpp:47-71`,
`storage.writeFile(tmp)` + `storage.replaceFile`) — is reached only from
`MergeTreeBitmapStore::installBitmap`, which has no caller outside its gtests (the store's own
comment says "no production caller"); the UNIQUE KEY delete-bitmap path is unwired and gated behind
`allow_experimental_unique_key` (`registerStorageMergeTree.cpp:740-748`). Owed when that path is
wired: make `writeBitmapToStorage` take the `supportsAtomicFileWrites` short-circuit (or one
transaction around both steps), and add a CA-disk test for it. Until then the guard's fail-loud throw
is the correct behaviour.

## A same-pool CA→CA move still reads every byte of the part, even though the publish is dedup-free (2031-triage CAS-120) {#same-pool-move-reads-every-byte}

Refines `[VERIFY-ca-ca-same-pool-move]` above with the half it does not state. That item is right that
the TARGET side of a same-pool CA↔CA move is near-free: the blobs are already in the pool, so the
publish's `HEAD`-before-`PUT` dedup gate elides the uploads and the move reduces to a ref publish. The
SOURCE side is not free. `DataPartStorageOnDiskBase::clonePart`'s content-addressed branch
(`DataPartStorageOnDiskBase.cpp:745-768`) runs the whole clone through
`copyDirectoryContentIntoTransaction` (`:697-720`), which is a byte-level
`src_disk.readFile` + `copyData` + `dst_transaction.writeFile` loop per file — so every byte of the
part is `GET`-ed from the pool, written to staging, and re-hashed purely to rediscover blob references
the source manifest already names. Sequentially: one file at a time, deliberately (the rationale is
recorded at `:683-696` — the CA transaction batches all files into one manifest and its staging map is
not mutex-guarded, and MOVE is a background operation).

The interserver path already has the primitive this wants: a same-pool fetch is served by
manifest relink instead of bytes (`DataPartsExchange.cpp` relink sender/receiver, `CA_RELINK_COOKIE`),
and the metadata storage exposes `getRelinkOffer`/`confirmExactRef` for it. A local same-pool move
could publish the target ref from the source manifest the same way and move zero bytes.

Priority is low for a reason worth writing down: two CA disks on the SAME pool is a configuration
whose move accomplishes no physical relocation, so the realistic `MOVE`/TTL target is a DIFFERENT pool
(different bucket/tier), where the bytes genuinely must travel and no relink is possible. Ordering:
after `[VERIFY-ca-ca-same-pool-move]`'s S37 leg actually runs — it decides whether same-pool CA↔CA is
even a supported shape before there is any point optimizing it.

## With no target disk supplied, a fetch advertises only the FIRST CA pool of the policy, so a reservation on a second CA pool loses relink (2031-triage CAS-134) {#relink-advertises-only-first-ca-pool}

**CLOSED 2026-09-03 by the forced-relink-on-fetch design
(`docs/superpowers/specs/2026-09-03-cas-fetch-forced-relink-design.md`): the receiver advertises the
set of its policy's pools, the sender names the match, and the reservation goes to that pool's disk.
`test_two_pool_policy_relinks_into_second_pool` is the proof. Kept for provenance; the text below
describes the pre-fix code.**

P3, performance only — the fallback is a normal byte fetch onto a CA disk (content-addresses and
dedups on arrival), and it is logged with both pool ids.

`fetchSelectedPart` advertises the receiver's pool identity before it knows which disk the
reservation will pick. When the caller supplied a CA `disk`, the advertise is exact
(`src/Storages/MergeTree/DataPartsExchange.cpp:707-712`); when it did not, the code walks
`data.getDisks()` and advertises the FIRST CA disk it meets (`:713-724`, `break`). Three of the four
production callers pass no disk (`StorageReplicatedMergeTree.cpp:3483`, `:3611`, `:5823`); only
`fetchExistsPart` (`:6009`) passes one. So in a policy holding two CA disks belonging to DIFFERENT
pools, a reservation that lands on the second pool cannot use the offer: the post-reservation re-check
(`DataPartsExchange.cpp:926-932`, added by `f3cd6e1ff1f`) sees
`chosen_ca->getPoolUUID() != advertised_pool_uuid` and re-requests the bytes.

Owed (small, only if the topology is supported at all): advertise the SET of CA pool ids reachable
from the policy (comma-separated `cas_pool_uuid`) and let the sender pick the one matching the part's
disk, or defer the advertise until after reservation. Whether the topology is even a supported shape
is `BACKLOG/formats-and-storage.md`{#orphan-triage-2026-08-04} `[mixed-ca-tiered-topology]`, which
this item should be ordered after.
