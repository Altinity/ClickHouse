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
- **[VERIFY-ca-ca-same-pool-move] CA↔CA same-pool `MOVE PART`/`PARTITION` (S37 3-disk), after MOVE-to-CA lands** — VERIFY — The MOVE-to-CA fix targets local↔CA. Moving a part between two CA disks in the SAME pool (the S37 3-disk `ca_local3` variant) is expected to LIKELY work via the same generic L1+L2 code with no special-casing: the moved part's content is already in the pool, so the target publish dedup-resolves the blobs (near-free) and is effectively a ref repoint. UNVERIFIED interaction: whether the target's final ref `<part>` collides benignly with the source's existing ref `<part>` (both namespaced by the table uuid) — or whether same-pool CA↔CA needs the source ref dropped before/as the target publishes. TODO after MOVE-to-CA lands: run the S37 CA↔CA-move leg; if it passes → close; only add special handling if it shows a real collision. NOT a blocker for the local↔CA cut.
- **[killed-mid-move-partition-duplicate] killed mid-`MOVE PARTITION` leaves a persistent duplicated part** — {#killed-mid-move-partition} — VERIFY — CONFIRMED (S37 chaos leg): hard-killing a replica mid-`ALTER TABLE ... MOVE PARTITION ID 'all' TO VOLUME 'cas'` leaves the partition DUPLICATED after heal (`count()=200` but `uniqExact(id)=100`, consolidated into one active part by a later merge, so it does NOT self-heal — a merge does not dedup rows). S36's single-part `MOVE PART` chaos leg IS atomic/green — only the multi-generation `MOVE PARTITION` policy/TTL path under kill duplicates. ATTRIBUTION: PRE-EXISTING, not caused by the MOVE-to-CA (R2) fix — the pre-R2 baseline showed the same duplication via a different failure mode (R2 fixed the move mechanism, not the duplication). LIKELY GENERIC: plausibly a generic ClickHouse crash-atomicity/replication-replay duplication (a kill mid-move + queue/replica replay applying it twice), not the CA `moving/`-prefix recovery. TODO: repro `MOVE PARTITION` + hard-kill on a NON-CA multi-disk policy to confirm generic; if generic → upstream/known-limitation, if CA-specific → investigate the `moving/`-ref restart recovery promoting a stale staging part. Chaos-edge; does not block the local↔CA MOVE-to-CA feature.
- **[merge-progress-reset-mount-fence] merge "progress reset in loops" under sustained S3 fault = mount-fence loss + ABORTED-defeated backoff** — HARD — Root cause (not a merge-vs-insert retry gap — the CAS upload-retry stack is caller-agnostic): sustained faulting fails the mount-lease renewal PUT, the fence trips, and every write fails `stageManifest`'s `fence_ok()` gate instantly until self-remount recovers; fail-closed correct (no data loss, self-heals), but the replication scheduler tight-loops recomputing the same merge (239x in one repro). Three CA-side defects to fix, priority order: (1) OVER-FENCING — `SingleWriterSlot`'s renewal loop burns the whole writer incarnation on the first transient renewal-PUT exception instead of retrying while the lease deadline is still valid; fixing this alone kills the common transient-blip case. (2) `ABORTED` DEFEATS THE EXISTING MERGE BACKOFF — CAS throws `ABORTED`, which `ReplicatedMergeMutateTaskBase` treats as "not an error" and never records, so upstream's existing exponential backoff never engages; needs a retry-later class thrown outside the `ABORTED` exemption at the fence-lost boundary specifically (genuine merge cancellation must stay `ABORTED`-exempt). (3) OPACITY — the "retry budget exhausted" message fires when nothing was attempted, and failures log at Information only (invisible in `system.replication_queue`). Also unverified: fsck-to-fixpoint after a fence-loss recovery, to confirm "no orphans" (links to the S30 DANGLING-PRECOMMIT gap). Repro harness: `utils/ca-soak/docker-compose-s3faultproxy.yml`.
- **[r3-acked-lost-dataloss] acked-then-lost INSERT data loss on cross-request retry — FIXED** — Root cause: on CA, `renameParts` is a pure overlay re-key with no publish, so the Keeper block_id/part-znode multi commits durably BEFORE the CAS manifest — a genuine split-commit window. A part lost in that window left its dedup token behind, so a byte-identical client retry deduped against it and acked with zero rows written. Fixed by `77484196b0d`: closes every part disk-storage transaction in `MergeTreeData::Transaction::renameParts`, so the part is durable before the Keeper block_id registration. Gates all green post-fix (S40 10/10 acked=3796 lost=0; `dl_probe` LOST=0, was ~198/1314; the original R4 chaos recipe that lost 1118 rows now PHASE3 OK with zero deficit). R3 ship-readiness restored. Residual: a narrower hazard (block_id outliving a durably-committed part lost later) stays out of scope — verify-on-dedup is the candidate if it ever matters. Open thread: an upstream submission draft exists (`tmp/upstream_issue_dedup_durability.md`), pending a user decision on whether to send it.
- **[RPL-5 slice] `REPLACE PARTITION`/`ATTACH PARTITION ... FROM` queue-clone relink, untested on CA** — TEST — A `REPLACE_RANGE` log entry cloned to a second replica reduces to fetch (relink or byte) + drop, individually working, but no integration test proves the cloned fetch specifically relinks rather than byte-refetches (RPL-4 disables `to_detached` relink explicitly, so the branch taken isn't obvious a priori). Needs `test_cas_replicated_relink`'s 2-replica rustfs fixture extended with a `REPLACE PARTITION`/`ATTACH ... FROM` scenario plus a blob-count relink proof. Pulled into the publish-confirm fetch-handoff iteration's test package, which touches the same relink-eligibility branch. (An orphaned 2026-08-04-triage finding, C-0098, describes this identical test gap — folded in here rather than inserted as a separate item, since it duplicated this one exactly.)
- **[B66b] relink-into-detached (zero-byte `to_detached` fetch for same-pool parts)** — IN PROGRESS (2026-07-23) — folded into the publish-confirm fetch-handoff iteration: relink already publishes under `tmp-fetch_<part>` and re-keys via `renameTempPartAndReplace`, so detached needs only lifting the `!to_detached` advertise gate (`DataPartsExchange.cpp:540-545`) + the detached temporary name + the same confirm step; collision semantics inherited from the byte path by construction. (RPL-4 perf cliff.)

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
