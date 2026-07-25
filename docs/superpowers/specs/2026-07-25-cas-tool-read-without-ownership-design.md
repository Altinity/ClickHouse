---
description: 'Settles Task 19 of the publish-confirm plan: whether a diagnostic tool may read a live content-addressed pool without claiming its mount. Establishes what the mount claim actually protects (all four steps are writer protections), enumerates what a non-claiming reader can get wrong (exactly one wrong-answer class, and it is mount-independent), and recommends observe-only-by-process-role with the read-only contract enforced at the CAS backend rather than at seven facade call sites.'
sidebar_label: 'CAS Tool Read Without Ownership'
sidebar_position: 20260725
slug: /superpowers/specs/cas-tool-read-without-ownership-design
title: 'CAS: reading a live pool without claiming it'
doc_type: 'reference'
---

# CAS: reading a live pool without claiming it {#cas-tool-read-without-ownership}

**Date:** 2026-07-25. **Status:** DESIGN, awaiting approval. **Branch:** `cas-gc-rebuild`.
**Settles:** Task 19 of `docs/superpowers/plans/2026-07-24-cas-publish-confirm-and-ref-lane-safety.md`.

## Problem {#problem}

`clickhouse-local` and `clickhouse-disks` open a CA disk through the ordinary writable path, which runs
`Pool::mountWritable` -> `claimOwnerOrThrow` and fails closed against the real server's persisted owner
uuid. In CI this turns the post-run system-table scrape into a failure that looks exactly like a real
mount-ownership bug; for an operator it means a second process cannot inspect a pool at all. The stand
already routes around it with a bespoke shadow disk (`utils/ca-soak/configs/fsck_only_ca.xml`), and
BACKLOG `[F1-prod]` records the cost of that workaround from the other side. One contract should serve
the scrape, the applets, and the operator.

## What the mount claim actually protects {#what-the-claim-protects}

`Pool::open` calls `mountWritable` only on a writable open (`CasPool.cpp:460-461`). `mountWritable`
(`CasPool.cpp:466`) performs four bootstrap-control steps, described in its own header comment as the
writes that "establish the very right to write":

1. `claimOwnerOrThrow` (`CasServerRoot.cpp:108`) — identity. Refuses a foreign `server_uuid`; claims
   only over a provably-empty subtree. Protects against two servers writing one server-root.
2. `allocateWriterEpoch` (`CasServerRoot.cpp:165`) — the durable monotone `writer_epoch` every manifest
   ref and the watermark carry. Protects against a twin incarnation minting indistinguishable refs.
3. `claimMount` (`CasServerRoot.cpp:320`) plus arming the local write fence. Protects against a live
   double start and bounds how long this process may keep writing after its lease lapses.
4. The watermark anchor (`CasPool.cpp:455-461`) — the mount object's `min_active`, an input to GC's
   heartbeat ack floor (`CasGc.cpp:306`). Protects **this mount's own in-flight builds** from GC.

Every one is a writer protection. None gives a reader a stable view, and that is structural: the claim
is scoped to one `server_root_id`, while a reader (an fsck reachability walk, a system-table scrape)
reads the whole pool including every other server root. Claiming root A excludes a second writer of A
and nothing else. A CAS pool is multi-writer by construction; there is no configuration in which any
reader — mounted or not — sees a quiesced pool.

Two facts make claiming actively *worse* for a reader:

- A **mounted** `Pool` caches its `RefTableState` and never re-recovers it, so a concurrent external ref
  write is invisible to it. `CasFsck.cpp:122` says so verbatim and deliberately bypasses the cache with
  a fresh `recoverRefTable`. On the ref lane a read-only open is *less* stale than a mount, not more.
- A claiming tool becomes a member of GC's heartbeat ack floor (`CasGc.cpp:306`; `04-gc-protocol.md`
  §3.2). A tool that hangs or is `SIGKILL`ed then holds the floor down for a full lease TTL and stalls
  graduation **pool-wide**. A non-claiming reader cannot do that.

### What read-only currently gives up {#read-only-gaps}

Being honest about the cost of not claiming:

- The local write fence is **unarmed** on a read-only pool: `CasMountRuntime.h:413-416` — "the unarmed
  default (`deadline_boot_ms = UINT64_MAX`, `lost = false`) permits mutation until a keeper supplies a
  real lease deadline". Nothing at the `Pool` or `Backend` layer stops a write.
- The whole read-only guarantee therefore rests on seven facade checks (`checkNotReadOnly`,
  `ContentAddressedMetadataStorage.cpp:558`, `:606`, `:964`, `:1058`, `:1158`, `:1952`) plus "no GC
  scheduler" (`:807`). The CAS `Backend` interface has no read-only concept at all — nothing in
  `MetadataStorages/ContentAddressed/Backend/` mentions it.
- `S3ObjectStorage::isReadOnly` (`S3ObjectStorage.h:165`) is advisory: unlike
  `LocalObjectStorage.cpp:566`, the S3 write path never consults it.
- `ca-gc-rebuild` is already a documented hole: it requires `isReadOnly()` and then writes one
  `gc/state` CAS (`programs/disks/CommandCaGcRebuild.cpp:22-28`, `:54`).

### Two corrections to the plan's premises {#premise-corrections}

- **The `_pool_meta` bug is already fixed.** The plan says read-only `Store::open` can still mint
  `_pool_meta` and that this must be fixed as part of (a). It was fixed in `5dc58c91cb7`:
  `CasPool.cpp:436-438` passes `allow_mint=!config.read_only`, and `CasPoolMeta.cpp:139` fails closed.
  BACKLOG line 130 (`[refactor: Store::open modes]`) is stale and should be retired.
- **`ee15c8ade23` was never actually exercised.** Its `sed` patches
  `/etc/clickhouse-server/config.xml`, but the CA disk is defined in
  `config.d/content_addressed_s3_storage_policy_for_merge_tree_by_default.xml`
  (`clickhouse_proc.py:1299`). The file it edits does not contain the marker tag. So (c) failed on a
  path bug, not on a design flaw — the plan's "evidence for (a) or (b) over (c)" is weaker than stated,
  and the honest statement is that (c) has not been tested.

## The safety enumeration {#safety-enumeration}

GC mutates concurrently — condemning, graduating, deleting. What can a non-claiming reader observe, and
is the consequence a WRONG answer or merely an INCONCLUSIVE one? The last column is the crux: in every
row, claiming the mount would change nothing.

| # | Observation | Consequence | Mount fixes it? |
|---|---|---|---|
| 1 | Stale ref view: the LIST-then-replay of the append-only ref log races a commit | Reader sees a valid *prefix* — an internally consistent earlier committed state, never a torn one. Blobs of the newer ref look unreferenced and land in `Unaccounted`, which `CasFsck.h:44` documents as "persistent occurrences should be impossible" — so a transient one reads as an anomaly. **INCONCLUSIVE presented as a finding**; `CommandFsck.cpp:101` already prints the "re-run after the next round" caveat | No |
| 2 | A blob condemned and deleted between the walk and the HEAD | `blobStillReferenced` (`CasFsck.cpp:106`) re-resolves each label from a *fresh* `recoverRefTable` and fails closed to "still referenced". Errs LOUD (possible false-positive dangle), never silent. **Acceptable** | No |
| 3 | A manifest body deleted after its ref was republished or dropped | Same shape, `manifestStillReferenced` (`CasFsck.cpp:159`). **Acceptable** | No |
| 4 | A superseded GC generation mid-scan | fsck reads `gc/state` (`CasFsck.cpp:493`), then the fold seal, then streams every source-edge run. Snap-prune keeps only `gc_snap_generations_to_keep` generations; a scan slower than that many rounds finds a run gone and `openSourceEdgeRun` (`CasBlobInDegree.cpp:283-291`) throws `CORRUPTED_DATA "object … is absent"`. Nothing on this path catches it (only the checksum mismatch is catalogued, `CasFsck.cpp:548`). The operator sees "corrupted" on a healthy pool. On the stand `gc_interval_sec=5` against 180-600 s fsck timeouts, so the window is wide. **WRONG ANSWER** | No |
| 5 | A half-published GC round | Not observable: attempt artifacts live under `gc/gen/<G>/attempt/<a>/` and are reader-invisible until the single `gc/state` CAS (`04-gc-protocol.md` §3.6). The reader always latches a committed generation. **Safe by construction** | n/a |
| 6 | A run whose whole-file seal checksum disagrees | Catalogued as `CorruptedRun` and the scan continues, deliberately (`CasFsck.cpp:548`). **Acceptable** | No |
| 7 | An erased backing that looks like an empty table | Already defended for exactly this case: `confirmPoolIdentityForEmptyEnumeration` (`ContentAddressedMetadataStorage.h:685`) — "a read-only pool has no lease/observer to detect that erasure any other way". **Acceptable** | No |
| 8 | A scan that ran out of time | `partial=1 reason='…'`, and `FsckReport`'s own doc states `clean` must not be read as a claim about the unvisited part. **Correctly inconclusive** | No |

**Result: exactly one wrong-answer class (#4), and it is not caused by the absence of a mount claim.**
It must be fixed regardless of which contract wins — re-latch `gc/state` and retry the GC-labelling pass
on a vanished run, or report `gc_view=inconclusive` instead of a clean-looking number.

## The options {#options}

**(a) Observe-only, never claim.** Real cost: not "flip a flag" — the read-only contract has to move
from convention to enforcement (see `#read-only-gaps`). Leaves unsolved: nothing in the reader class;
`[F1-prod]` (a read-only shadow disk in the *server's* config breaking part discovery) is a separate
problem needing a `hidden`/`introspection_only` disk flag.

**(b) Keep claiming; make the refusal typed and let callers downgrade.** Cheap on the surface — retype
`claimOwnerOrThrow`'s `CORRUPTED_DATA`. Unworkable in substance:

- The tool has already **mutated** before it can be refused. On an owner-absent, provably-empty root the
  claim *succeeds*: `putIfAbsent(ownerKey, uuid=0000…)` (`CasServerRoot.cpp:142`), then epoch 1, then
  the mount. The pool is then permanently owned by the zero uuid and the real server can never claim it
  (`CasServerRoot.cpp:120-131`). A diagnostic tool bricking a fresh pool is strictly worse than the
  failure being fixed.
- Against a same-uuid live root the tool blocks for `mountObservationThresholdMs` times up to
  `kMaxObservationRestarts = 3` restarts (`CasServerRoot.cpp:454`) before refusing — having already
  burned a durable `writer_epoch`.
- A claiming tool joins the ack floor and can stall graduation pool-wide.
- It makes "downgrade to read-only" a per-caller decision. Every future caller is a fresh chance to get
  it wrong — the exact pattern behind `{#gc-observation-vacuous-2026-07-25}`.

**(c) CI carve-out.** Now known to be untested (see `#premise-corrections`); fixing the `sed` to also
patch `config.d` would probably make CI green. It should be done anyway as a one-liner, but it is a
config convention every future config must remember — the same "harness pins a name, mismatch degrades
to silence" shape that produced three separate silent under-reports this week.

**(d) — proposed fourth shape: role decides, not config.** `ContentAddressedMetadataStorage::startup`
already holds `context`; derive observe-only from `context->getApplicationType() != SERVER`
(`Context.h:1708-1715`) **or** the disk's `<readonly>`. Nothing in any config file has to be remembered,
patched, or `sed`-ed, and a tool physically cannot claim. Writable tool access becomes an explicit,
loud, per-invocation opt-in for the write-class applets that genuinely need it.

## Recommendation {#recommendation}

**Adopt (a) in shape (d), with the contract enforced below the facade.** Three items:

1. **Observe-only by process role, not by config.** A CA disk opened by a non-`SERVER` application never
   claims the mount, never mints, never schedules GC. This is already the de-facto contract for the CAS
   applets — `ca-fsck` (`CommandFsck.cpp:52`), `ca-gc-dryrun` (`CommandCaGcDryRun.cpp:38`) and
   `ca-gc-rebuild` (`CommandCaGcRebuild.cpp:54`) all *refuse* a writable disk today, and the stand has
   run fsck and dryrun against live pools this way for months. The change makes it unforgettable rather
   than something each config must opt into.
2. **A fail-closed read-only `Backend` decorator** so the contract is proved rather than conventional —
   every `put*` / `delete*` throws `READONLY`. Seven facade checks plus an unarmed fence is not a
   contract. `ca-gc-rebuild`'s single `gc/state` CAS becomes a named, explicit exception rather than a
   hole in the enforcement.
3. **Fix wrong-answer class #4** in `runFsck`, and make every tool emit an explicit
   `gc_view=inconclusive` instead of a clean-looking number when it could not latch a stable generation.

Separately and immediately, fix the CI `sed` to patch `config.d` — that is why the last attempt failed.
It is a one-liner and it is *not* the contract.

Reasoning, in one line: every protection the mount provides is a writer protection; the one thing a
non-claiming reader genuinely gets wrong is mount-independent; and claiming from a tool can brick a
fresh pool and stall a live one. Given this week's three silent under-reports, the deciding factor is
that (d) removes the human step — no config to remember, no per-caller downgrade to forget.

## What would have to be true for this to be wrong {#falsifiers}

- **A supported workflow writes a CA disk from `clickhouse-local` or `clickhouse-disks` without an
  explicit opt-in.** I found none — every CA applet already refuses a writable disk — but a sweep of
  generic `clickhouse disks write` / `copy` usage over CA disks is required before flipping the default.
- **Some read path depends on the mount runtime being armed.** `Pool::open` mints a random nonzero
  `process_epoch` specifically so the read-only path has one (`CasPool.cpp:452-457`), so this looks
  covered — but it is the load-bearing assumption of the whole recommendation.
- **`ApplicationType` is not reliably `SERVER` by the time `startup()` runs** in some embedded or
  test-harness configuration. If a real server can ever be seen as `LOCAL`, role-based defaulting
  silently disables GC and writes on a production disk — a fail-*open* failure and a hard blocker.
- **Wrong-answer class #4 turns out to be unfixable without a reader-visible GC pin.** If a diagnostic
  really needs GC to hold a generation for it, then a reader does need something ownership-shaped — a
  read pin, not a mount — and (a) alone is insufficient.
- **`[F1-prod]` is expected to be solved by this.** It is not. That needs a `hidden` /
  `introspection_only` disk flag so part discovery skips read-only same-pool disks.
