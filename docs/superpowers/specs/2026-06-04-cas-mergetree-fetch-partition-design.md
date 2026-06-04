---
description: Design spec for FETCH PARTITION / FETCH PART on a content-addressed MergeTree disk — byte-fetch into the detached namespace (relink-into-detached deferred).
sidebar_label: 'CAS MergeTree FETCH PARTITION'
sidebar_position: 7
slug: /superpowers/specs/cas-mergetree-fetch-partition
title: 'Content-Addressed MergeTree — FETCH PARTITION/PART Design'
doc_type: 'guide'
---

# Content-Addressed MergeTree — FETCH PARTITION/PART Design {#cas-fetch-partition}

**Status:** design spec, awaiting review. **Date:** 2026-06-04. **Backlog:** B21/B1 (the current gate),
B4 (the partition-clone class). First of the FREEZE+FETCH pair (FETCH first; FREEZE is a separate,
larger spec).

## 1. Goal and scope {#goal}

Support `ALTER TABLE … FETCH PARTITION … FROM '<zk_path>'` and `FETCH PART` on a content-addressed (CA)
disk. Both are gated `Code: 344 SUPPORT_IS_DISABLED` today. FETCH fetches a partition's parts from
another replica/table (via a ZooKeeper path) into the local table's `detached/`; the user then `ATTACH`es
them. This is a `ReplicatedMergeTree` operation, which now works on CA (the replication/relink work
landed — B33 lifted), so the gate is stale.

**In scope:** lift the gate; ensure a FETCH lands a usable part in the CA `detached/` namespace via the
**byte-fetch** path; un-gate the 2 FETCH tests + a focused oracle.
**Out of scope (deferred to backlog):** relink-into-detached (the same-pool optimization that avoids
downloading bytes — FETCH is a niche admin op and content-addressing already dedups, so v1 byte-fetches);
FREEZE (separate spec).

## 2. How FETCH works (verified) {#how}

`StorageReplicatedMergeTree::fetchPartition` resolves the parts under the remote `<zk_path>` and, for each,
calls `Fetcher::fetchSelectedPart(..., to_detached=true, ...)` (`DataPartsExchange.cpp`). For
`to_detached`, the fetched part is written under `<table>/detached/tmp-fetch_<part>` (the staging dir is
`getRelativeDataPath() + DETACHED_DIR_NAME`, `DataPartsExchange.cpp:917`), then `fetchPartition` renames
it to the final `detached/<part>`. The user later `ATTACH PARTITION`es it (ATTACH already works on CA).

`fetchSelectedPart` has two transfer paths: the **relink** path (`relinkPartToDisk`, ~line 712 — wired for
CA, same-pool, ACTIVE fetches; it does NOT take `to_detached`) and the **byte-fetch** path
(`downloadPartToDisk`, which honors `to_detached`). The gate that blocks FETCH on CA is
`MergeTreeData::checkAlterPartitionIsPossible`'s ContentAddressed branch (`MergeTreeData.cpp:~6572`), whose
`supported_commands` set lists DROP/ATTACH/REPLACE/MOVE but not FETCH.

## 3. Design — byte-fetch into detached {#design}

### 3.1 Lift the gate {#gate}
Add `PartitionCommand::FETCH_PARTITION` and `PartitionCommand::FETCH_PART` to the `supported_commands` set
in the ContentAddressed branch of `checkAlterPartitionIsPossible`. (FREEZE/UNFREEZE stay rejected — they're
the separate FREEZE spec.) FETCH only runs on `ReplicatedMergeTree`, which is supported on CA.

### 3.2 Force the byte-fetch path for `to_detached` {#byte-fetch}
The relink path was wired for active, same-pool fetches and ignores `to_detached`, so it would stage at the
active table path, not `detached/`. Gate the relink branch in `fetchSelectedPart` on **`!to_detached`** so a
FETCH-into-`detached` always takes `downloadPartToDisk`. The byte stream is written through the CA
whole-part transaction (the fetch download already opens `beginTransaction()`/`commitTransaction()`), so each
file content-addresses and `condCreateIfAbsent`-dedups against the pool — a same-pool FETCH downloads to self
but stores no duplicate blobs.

### 3.3 Detached landing on CA (the load-bearing piece) {#detached-landing}
The byte-fetch writes **fresh** files into `detached/tmp-fetch_<part>/…` (unlike DETACH, which clones from
an active ref). Two things must hold on CA, both verified reproduction-driven in the plan:
- **(a) The fresh detached-staging writes content-address and commit a usable `detached` ref.** A write to
  `<uuid>/detached/tmp-fetch_<part>/<file>` parses (`parsePartFilePath`) to the detached namespace; the CA
  transaction must content-address those files and, on commit, publish a `detached` ref whose manifest keys
  the fetched part's files under `tmp-fetch_<part>/<file>` (the same nested-key shape the detached namespace
  already uses). If the CA transaction's commit path does not yet publish a `detached` ref for fresh
  (non-cloned) detached writes, add that (mirror `republishCommittedPartIntoDetached`'s detached-ref publish,
  but for in-transaction `recorded` blobs rather than a re-keyed source manifest).
- **(b) The `detached/tmp-fetch_<part>` → `detached/<part>` rename lands.** This is a detached→detached
  re-key; `ContentAddressedTransaction::rekeyDetachedPartDir` already re-keys a detached part dir
  (used for the `attaching_<part>` staging). Confirm it covers the `tmp-fetch_<part>` staging prefix; if
  not, extend it (it is the same operation — re-key `<old_dir>/` → `<new_dir>/` within the shared detached
  ref's manifest + sidecar).

### 3.4 What does NOT change {#unchanged}
Non-CA FETCH is unchanged (the `!to_detached` relink gate only affects the CA relink branch, which is CA-only).
ATTACH PARTITION of the fetched detached part already works on CA. The relink path for ACTIVE fetches
(replication) is unchanged.

## 4. Error handling {#errors}
Fail-closed, unchanged: an unreachable source / missing remote part throws the normal fetch exception; a
missing blob cannot occur on the byte-fetch path (bytes are downloaded, not relinked). The detached-landing
fails closed if the commit or rename cannot publish a valid ref (never a silent empty detached part).

## 5. Testing {#testing}
- **Stateless (un-gate):** `01650_fetch_patition_with_macro_in_zk_path_long`,
  `03350_alter_table_fetch_partition_thread_pool` → run on the CA-default job; expect pass. Triage any
  residual failure (likely a detached-landing gap → fix per §3.3).
- **Focused oracle (inline CA disk):** a Replicated CA table; `FETCH PARTITION … FROM` the table's own zk
  path into `detached`; `system.detached_parts` shows the fetched part; `ATTACH PARTITION`; `SELECT` reads
  the data back correctly — proving the fetched detached part is usable end-to-end on CA. (Mirror an
  existing FETCH stateless test for the zk-path setup.)
- **Non-CA regression:** a couple of normal FETCH PARTITION tests on the plain job → no regression.

## 6. Plan phasing {#phasing}
1. Gate lift + the `!to_detached` relink gating; build; un-gate `01650`/`03350` and run on CA-default to
   surface the actual detached-landing behavior (the empirical risk probe).
2. Fix the detached-landing per §3.3 (fresh-detached-write commit + the staging→final rename) if the run
   shows a gap; add the inline-CA oracle.
3. Non-CA regression + un-gate the 2 tests + backlog (B21/B1: FETCH supported on CA; relink-into-detached
   deferred); commit + push.

## 7. Risks {#risks}
- **Detached landing (§3.3)** is the one uncertain piece — fresh (non-cloned) writes into the detached
  namespace + the staging rename. Bounded; reproduction-driven; the existing detached machinery
  (`republishCommittedPartIntoDetached`, `rekeyDetachedPartDir`) is the template.
- **FETCH-from-a-different-pool** (cross-pool source): byte-fetch downloads + content-addresses into the
  local pool — correct (no relink assumption). Same-pool just downloads-to-self (the deferred optimization).
