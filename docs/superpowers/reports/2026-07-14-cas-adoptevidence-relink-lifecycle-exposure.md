---
description: 'Scoped reachability investigation of the adoptEvidence source-dropped-mid-relink data-loss exposure. Local-source relinks are refuted-unreachable (source held as a live DataPartPtr); only fetch-by-relink is a low-probability residual. §4 (8fe6331a431) widened it by removing the promote-time copyForwardFromCondemned token-displacement. Fix = a GC pre-CAS deleteExact liveness re-check that closes the whole deposed-leader same-token class.'
sidebar_label: 'Investigation: adoptEvidence relink lifecycle race'
sidebar_position: 22
slug: /superpowers/reports/adoptevidence-relink-lifecycle-exposure
title: 'CAS finding — adoptEvidence source-dropped-mid-relink lifecycle race (task #42)'
doc_type: 'reference'
---

<!-- Investigation report (task #42), 2026-07-14. Verbatim working-notes format preserved. -->

# Task #42 — adoptEvidence source-dropped-mid-relink exposure — INVESTIGATION REPORT

**Status:** IN PROGRESS (started 2026-07-14). READ-ONLY reachability investigation.
**Branch:** `cas-gc-rebuild`. **Method:** systematic-debugging, code-cited, no source changes.

## Verdict
**PLAUSIBLE-NEEDS-REPRO (scoped).** Splits by relink caller:
- ALL local-source relinks (mutation carry, projections, FREEZE/clone, ATTACH/REPLACE/MOVE PARTITION,
  rename/republishRef) → **REFUTED-UNREACHABLE**: the source is held as a live `DataPartPtr`;
  `grabOldParts` (`MergeTreeData.cpp:3372,3394`) drops a part's ref only when `Outdated` AND uniquely
  owned, so the held source pins `h` at in-degree ≥ 1 continuously — `h` is never condemnable in the
  adopt→precommit window.
- `fetch-by-relink` (`adoptPartFromManifest`→`publishEntries`, remote source) → **residual, NOT refuted**:
  window is structurally short (one synchronous `stageManifest` PUT), so the trace needs a pathological
  stack (that PUT stalling ≥2 GC folds + every replica dropping the part + a 2026-07-11 captured
  `deleteExact(t1)` firing post-promote). Low probability; not structurally impossible.
- **§4 (`8fe6331a431`) WIDENED it** (sub-Q4): it removed the pre-§4 promote-time `copyForwardFromCondemned`,
  which HEAD+loadMeta-probed each adopted leaf and, on condemned, displaced the body to a FRESH token
  `t2` — the exact defence that turns a captured `deleteExact(t1)` into a TokenMismatch no-op. Pre-§4 this
  path was safe; post-§4 it can lose the blob. DISTINCT from & OUTSIDE the D4 content-trust envelope
  (a temporal/lifecycle race that fails SILENT/fsck-only vs an ordinary byte-fetch's LOUD 404-retry).
- Fix (box I): a liveness re-check at the GC pre-CAS `deleteExact` site (`CasGc.cpp:377`) before deleting
  a `delete_pending` row closes the WHOLE deposed-leader class (this + 2026-07-11 + the §5 second victim)
  for all same-token recovery kinds. Needs its own TLA gate + a second independent consult before code.

_(Full grounding for each of the 5 sub-questions in the working notes below; identical verdict restated at bottom.)_

## The CRUX question
Can a relink's source blob lose its durable pool edge (in-degree contribution) in the window between
`adoptEvidence` (recorded in the transaction body at `createHardLink`) and `precommitAdd` (B's durable
protecting edge, appended only at commit)?

---

## Working notes (appended continuously)

### A. The two dep-recording moments and the durable-edge timeline (grounded)

`adoptEvidence` (`Core/CasBuild.cpp:681-696`): records a **tokenless, bodyless, edgeless** dep
`deps[entry.ref] = DepEntry{Blob, std::nullopt (no token), size, adopted=true}`. **No backend call
(no HEAD/GET/PUT).** This runs in the *transaction body*, at `createHardLink`
(`ContentAddressedTransaction.cpp:937` for committed-source, `:220` for staged-but-uploaded source).

The destination build B's **durable protecting edge** is `precommitAdd` (`CasBuild.cpp:822-895`): it
appends a `Precommit` `OwnerTransition` ref-op through `store->appendRefOps(... RootMutationKind::Precommit)`.
This runs inside `publishStaging` (`ContentAddressedTransaction.cpp:271-272`), which runs **at commit**
(`commit()` → `publishStaging`, `:339-343`).

Commit ordering (EDGE-BEFORE-OBSERVE, `ContentAddressedTransaction.cpp:260-272`):
`stageManifest` (writes manifest body) → `precommitAdd` (durable edge) → `putBlob` for each *pending*
blob → `promoteBuild`. **Crucially, an adopted (tokenless) leaf is NOT a pending_blob** — it is carried
as a manifest entry only (`:216-221`, `:938-941`) and is **never putBlob'd**. So B's ONLY durable edge
that can pin `h` is B's own precommit edge, first durable at commit.

**Therefore, between `createHardLink` (adoptEvidence, body) and `publishStaging`/`precommitAdd` (commit),
B contributes ZERO durable in-degree to `h`. In that window the only thing pinning `h`'s in-degree is
the SOURCE part `S`'s committed manifest edge.** This confirms the window described in the brief exists
structurally. The CRUX reduces to: can `S`'s edge be dropped in that window?

### B. What the §4 promote gate actually asserts (grounded, `CasBuild.cpp:942-999`)

The promote gate does NOT rest its argument on the source's edge. Its argument (comment `:975-983`) is:
(1) B's precommit edge was durably appended before promote (`precommitAdd`), and (2) the owner-liveness
check `:950` (`state.precommits.contains({final_ref_name, id.ref})`) re-proved B is the LIVE precommit
owner, so (3) "GC's fold pins every blob it names at in-degree >= 1 (the barrier-activated
create-precommit +1, Task 12). GC is the sole deleter and respects in-degree, so a trusted-promote leaf
cannot have been condemned/deleted."

This argument is about what a **future fold** will do (it will spare `h` because B's live precommit edge
is now visible). It is NOT an argument that a **captured pre-CAS `deleteExact(h,t1)`** — already queued
from an EARLIER fold's `delete_pending` row, keyed off the token — cannot fire. That is exactly the
2026-07-11 deposed-leader class. The gate's own comments concede the genuinely-absent case and defer to
fsck (`:964-967`, `:981-983`) — an fsck finding is *post-hoc data-loss detection*, not prevention.

**Key distinction from resurrect:** a resurrect (`uploadFromSource`) displaces the body to a FRESH token
`t2`, so a stale `deleteExact(t1)` finds `TokenMismatch`/absent → no-op (this is the whole safety
argument of Fix-4, 2026-07-11 report §fix-addendum). `adoptEvidence` does **NO body displacement** — it
re-references `h` at the SAME token `t1` (`CasBuild.cpp:694`, token = `std::nullopt`, no putOverwrite).
So a captured `deleteExact(h,t1)` finds the live body at `t1` and DELETES it. This is precisely the
"token-preserving recovery" the §5 report flagged (`2026-07-14 report §mechanism`, bullet on
`adoptEvidence`).

### C. GC redelete is pre-CAS and does NOT re-check recovered in-degree at delete time (sub-Q3, grounded)

`CasGc.cpp:368-370` labels the redelete loop **"R3: PRE-CAS deletes"** — it runs after `fold` (`:351`)
but BEFORE the round's single `gc/state` CAS. The loop (`:375-437`) fires
`backend.deleteExact(layout.blobKey(entry.ref), entry.token)` (`:377`) for every entry in
`folded.retired_merge[shard].redelete`, using the token captured by a PRIOR pass. It does **not** HEAD
or re-fold in-degree before deleting — the only re-check is the rustfs 412-on-absent quirk disambiguation
(`:390-398`), not a liveness re-proof. Comment `:368-370`: "safe at any leader staleness — Task-9
amendment" — precisely the assumption the deposed-leader class breaks.

Whether a `delete_pending(h,t1)` row goes to `redelete` or `spared` is decided per-fold by `settleEntry`
(`CasBlobInDegree.cpp:396-426`): `indeg > 0` ⇒ `spared` (`:404-408`, logged LOUD but no delete);
`delete_pending && indeg == 0 && !suppress_destructive` ⇒ `redelete` (`:410-415`). So a **stale/deposed**
leader whose fold snapshot does NOT yet contain B's precommit edge computes `indeg == 0` → schedules the
pre-CAS `deleteExact(h,t1)`; a **fresh** leader whose snapshot DOES contain B's edge computes `indeg > 0`
→ spares. Both can be in flight concurrently. The fresh leader's spare is ADD-ONLY (`CasGc.cpp:460-468`)
— it does NOT clear the tombstone and CANNOT cancel the stale leader's already-captured pre-CAS delete
("the final `gc/state` CAS fences adoption, not pre-CAS side effects"). This is structurally identical to
the 2026-07-11 witness, except the recovery edge is `adoptEvidence` (same-token) rather than a
`WriterStaleReuse` dedup (which also kept the same token). Either way the body is never displaced, so
`deleteExact(h,t1)` hits a live body.

**Confirmed for sub-Q2 and sub-Q3:** IF the source edge can drop long enough for `h` to be condemned +
graduated to `delete_pending(t1)` before B's precommit edge is durable, GC's captured pre-CAS
`deleteExact(h,t1)` fires against a still-`t1` live body that B's committed manifest names. The remaining
question is purely reachability of the source-drop window (sub-Q1).

### D. Sub-Q1 — enumerate `createHardLink`/`adoptEvidence` callers and whether the source is pinned

Two dep-recorders feed `adoptEvidence`: the in-transaction staged-carry (`ContentAddressedTransaction.cpp:220`)
and the committed-source carry (`:937`). Both fire from `createHardLink` (`:866`). The callers of
`createHardLink`/`createHardLinkFrom` reachable through a CA disk:

**Key structural fact (CA ref lifecycle) — the pin mechanism.** `MergeTreeData::grabOldParts`
(`MergeTreeData.cpp:3318`) is the ONLY path that removes a part (and on CA, a part removal is what drops
its ref). It removes a part only if BOTH: (a) the part is in state **`Outdated`** (`:3372`,
`getDataPartsStateRange(DataPartState::Outdated)`) — an `Active` part is never a candidate; and (b) the
part's shared_ptr is **unique** (`:3394`, `isSharedPtrUnique(part)` — else `NON_UNIQUE_OWNERSHIP`, skipped).
So **any relink that holds the source as a live `DataPartPtr` blocks that replica from dropping the source
ref for the whole time it holds it.**

**Key structural fact (per-replica namespaces).** `liveNamespace` (`ContentAddressedMetadataStorage.cpp:612-617`)
= `serverPrefix() + "/" + mirroredArchiveNamespace(table_uuid)`, and `serverPrefix()` = `server_root_id`
(`:584-589`), a persistent PER-SERVER layout identity. **Refs are per-replica; blobs are shared pool-wide.**
Therefore a blob `h`'s in-degree counts ONE edge per replica-manifest that names it. A part present on
N replicas contributes N independent edges to `h`. For `h` to hit in-degree 0, EVERY replica's manifest
naming it must be dropped.

Caller-by-caller (all now reachable on CA — the brief's "gated off" note is **STALE**; see box E):

| Relink caller | Source of the adopt | Source pinned through B's commit? |
|---|---|---|
| **Mutation unchanged-column relink** (`MutateTask.cpp:2121,2135`) | `ctx->source_part` (same-lineage predecessor) | YES. Held as a live `DataPartPtr` AND `Active` for the whole mutation; `grabOldParts` cannot drop it (non-unique + Active). It becomes `Outdated` only when the mutation result `S'` (= B) commits — an atomic edge hand-off. |
| **Projection build within mutation** (`MutateTask.cpp:2379,2415,2440`) | same `ctx->source_part` | YES (same as above). |
| **FREEZE / same-disk clone** (`DataPartStorageOnDiskBase::freeze`, `MergeTreeData.cpp:9476-9515`) | `src_part` | YES. Whole clone runs through ONE CA transaction; `src_part` held for its duration. |
| **REPLACE/ATTACH/MOVE PARTITION** (`MergeTreeData.cpp:9504`, `clonePartOnSameDiskWithHardlinks`) | source-table / detached `src_part`(s) held under lock | YES. Source parts collected and held as `DataPartPtr`s under a data-parts lock; detached parts are themselves `detached/<part>` refs (B181) that pin their blobs. |
| **rename / attach `republishRef`** (`CachedPartFolderAccess.cpp:275-276`, moveDirectory / RENAME) | the local source ref | YES — even stronger: it `publishEntries(dst)` (dst edge durable) BEFORE `dropRef(src)`, an explicit publish-before-drop hand-off (`:275` then `:276`). |
| **fetch-by-relink (cross-replica)** (`DataPartsExchange.cpp:1107` → `adoptPartFromManifest` → `publishEntries`) | the **remote sender's** committed part ref (a DIFFERENT replica's per-replica manifest) | **Not by a LOCAL hold.** The pinning edge lives on another node with an independent lifecycle. It is pinned only (i) by the sender holding the part `Active` while serving, and (ii) by the fact that the receiver's whole build is a SHORT synchronous `publishEntries` (see box F). |

**Conclusion for local-source relinks (mutation, projection, freeze, replace/attach/move):** the source
is a locally-held `DataPartPtr`, so `grabOldParts` provably cannot drop that replica's source ref while
the relink is in flight. That replica's edge pins `h` at in-degree ≥ 1 continuously from `getView`
through B's commit. `h` is therefore never condemnable in the window → the deposed-leader captured delete
cannot exist for `h` → **the trace is UNREACHABLE for every local-source relink.** The edge hand-off is
atomic for the same-lineage mutation (S stays `Active` until S' commits).

### E. Note: the brief's "gated off on CA" info is STALE (widens, not narrows, the surface)

`MergeTreeData.cpp:6549-6595` (this branch) lists `ATTACH_PARTITION`, `REPLACE_PARTITION`,
`MOVE_PARTITION`, `FETCH_PARTITION`, `FREEZE_*`, `UNFREEZE_*` as **SUPPORTED** on CA now. So the relink
surface is WIDER than the brief assumed. This does not change the verdict: every one of these enabled ops
takes a local held-`DataPartPtr` source (box D), so the pin guarantee still closes them. Only
fetch-by-relink has a non-local source.

### F. fetch-by-relink window is structurally SHORT (bounds the residual exposure)

`adoptPartFromManifest` (`ContentAddressedMetadataStorage.cpp:1285-1345`) runs a single synchronous
`publishEntries({receiver_ns, part_name}, decoded.entries, ..., ProvenanceOp::Attach)` (`:1326`). That is
the same build sequence as `publishStaging`: `stageManifest` → `precommitAdd` → (`putBlob` for pending —
here NONE, all leaves adopted) → `promote`. The vulnerable window is `[adoptEvidence (during decode/build),
precommitAdd]`, i.e. essentially the single `stageManifest` body PUT between them. For `h` to be condemned
+ graduated to `delete_pending(t1)` inside that window requires ≥ 2 GC fold rounds to elapse during that
one PUT — only possible if the receiver's `stageManifest` PUT stalls for multiple `gc_period`s (an
adversarial object-store pause) AND, concurrently, every replica holding the part drops its ref (e.g. a
`DROP PARTITION` racing the fetch) AND the deposed-leader capture fires. This is a deep stack of
independent low-probability conditions, not a clean reachable path — but it is NOT closed by any
structural guarantee (the pinning edge is on another node).

### G. Sub-Q4 — did §4 (`8fe6331a431`) INTRODUCE / WIDEN this? YES, for the fetch-by-relink path.

`git show 8fe6331a431 -- .../Core/CasBuild.cpp`: §4 **removed the promote-time probe** on tokenless
adopted leaves. The PRE-§4 promote loop, for each non-tokened leaf:
1. `head(blob_key)` — absent ⇒ fail closed (ABORTED);
2. `loadMeta` ⇒ if `MetaState::Condemned`:
3. `copyForwardFromCondemned(e.ref, blob_key, hr)` — reads the body IN FULL, re-verifies the payload
   hashes to the key, and re-wraps it under a **FRESH `incarnation_tag` + this build's `build_id`**,
   then `putOverwrite(key, bytes_out, hr.token)` — a **token-DISPLACING** rewrite to a fresh token `t2`,
   and flips the meta back to `Clean`.

That copy-forward is EXACTLY the token displacement that defeats a captured `deleteExact(h,t1)`: after it,
the body lives at `t2`, so a deposed leader's stale `deleteExact(h,t1)` finds `TokenMismatch` → no-op, and
the live copy-forwarded body survives (identical safety mechanism to Fix-4's resurrect-to-`t2`, 2026-07-11
report §fix-addendum). **§4 replaced this probe+copy-forward with pure trust (no HEAD, no `loadMeta`, no
displacement) — leaving the adopted body at the ORIGINAL token `t1`, exactly where the captured
`deleteExact(t1)` can still hit it.** So for any interleaving where the adopted blob IS condemned at B's
promote (the fetch-by-relink stall of box F), the pre-§4 code was SAFE (copy-forward → `t2`) and the
post-§4 code LOSES the blob. §4 did not create the deposed-leader delete mechanism (2026-07-11, pre-existing),
but it **removed the promote-time net that made the relink path safe against it**. The removed net was
per-leaf HEAD+`loadMeta` — the exact read cost §4 set out to eliminate — so re-adding a blanket probe
would undo §4; a targeted fix is needed (box I).

### H. Sub-Q5 — same as the accepted D4 relink trust model, or distinct? DISTINCT — outside the envelope.

D4 ([[feedback_cas_relink_trust_model]], commit `8fe6331`/D4 docs) = fetch-by-relink trusts that the
**source manifest names valid content**, exactly as an ordinary ReplicatedMergeTree interserver byte-fetch
trusts the sender's bytes. That is a **content-validity** trust ("is the source manifest correct?"),
asserted over the interserver channel. THIS exposure is a **lifecycle/temporal** race ("is the blob still
ALIVE at the instant the receiver commits and a stale exact-token delete fires?"). The two are orthogonal:
- Content-trust says nothing about a concurrent GC deleting a live blob out from under a just-committed ref.
- An ordinary RMT byte-fetch of a dropped source fails **LOUD** (source 404 → fetch retries → receiver
  re-materializes the ACTUAL bytes). This relink exposure fails **SILENT**: the receiver commits a ref,
  GC then deletes the body via a stale `deleteExact(t1)`, and the loss surfaces only later at fsck
  (`CasBuild.cpp:966-967,981-983` explicitly defer the genuinely-absent case to fsck). **Silent
  post-commit data loss is strictly worse than the loud-retry failure the D4 envelope assumes**, so this
  falls OUTSIDE the accepted D4 trust envelope.

### I. Fix direction (if the fetch-by-relink residual is judged worth closing)

Do NOT re-add a blanket promote-time HEAD+`loadMeta` (that reverts §4's whole read-cost win). Options,
each needing its own TLA gate + a second independent consult before any code change:
1. **Token-displace at promote for adopted leaves that are observed condemned** — i.e. keep §4's no-probe
   fast path, but if any cheap signal says the adopted leaf MIGHT be condemned, fall back to the removed
   `copyForwardFromCondemned` (rewrite to a fresh `t2`), restoring the exact-token-delete safety. The
   trick is a cheap "might be condemned" signal that does not cost a per-leaf probe on the hot path.
2. **Make the GC redelete non-destructive against recovered in-degree** — re-check (HEAD/meta) at the
   pre-CAS `deleteExact` site (`CasGc.cpp:377`) before deleting a `delete_pending` row, closing the
   deposed-leader class at its ROOT for ALL recovery kinds (adopt, dedup-reuse), not just relink. This is
   candidate-fix #2 from the 2026-07-11 report (§fixes) that was deferred; it also fixes the §5 second
   victim. Adds one op to the delete path only.
3. **Confine fetch-by-relink's window** — ensure the receiver's precommit edge is durable before any
   possibility of the sender's ref dropping (e.g. a receiver-side pin/lease on the adopted hashes for the
   duration of `publishEntries`). Most invasive; least in keeping with the shared-nothing model.

Option 2 is the smallest root-cause fix and is the natural closure of the whole deposed-leader class
(this report + the 2026-07-11 report + the §5 report all reduce to "a captured pre-CAS exact-token delete
can hit a same-token recovery"). It needs the CaRetiredInRunFoldAbortWitness gate re-run with a
token-preserving `adoptEvidence`-style recovery action added (the §5 report already prescribes exactly
this model extension).

---

## VERDICT: PLAUSIBLE-NEEDS-REPRO (scoped)

- **REFUTED-UNREACHABLE for every LOCAL-source relink** (mutation, projection, FREEZE/clone,
  REPLACE/ATTACH/MOVE PARTITION — the dominant and now-enabled CA relink surface). Guarantee:
  `grabOldParts` removes a part (⇒ drops its CA ref) only when it is `Outdated` AND its `DataPartPtr` is
  **unique** (`MergeTreeData.cpp:3372,3394`); every local relink holds the source as a live, `Active`
  `DataPartPtr` through B's commit; refs are per-replica (`ContentAddressedMetadataStorage.cpp:612-617`),
  so that held source contributes an edge that pins `h` at in-degree ≥ 1 continuously. `h` is never
  condemnable in the window ⇒ no captured `deleteExact(h,t1)` for `h` can exist ⇒ trace unreachable.

- **PLAUSIBLE-NEEDS-REPRO for fetch-by-relink only** (source = a remote replica's ref, not locally
  pinned; `adoptPartFromManifest` → `publishEntries`, `ContentAddressedMetadataStorage.cpp:1285-1345`).
  No structural guarantee closes it. It requires a pathological stack: the receiver's `stageManifest`
  PUT stalling across ≥ 2 GC fold rounds, every replica dropping the fetched part's ref in that window
  (e.g. a racing `DROP PARTITION`), and the 2026-07-11 deposed-leader capture firing `deleteExact(h,t1)`
  post-promote. The window is structurally SHORT (box F), so this is deeply improbable but not proven
  impossible. §4 (`8fe6331a431`) measurably WIDENED it by removing the `copyForwardFromCondemned`
  token-displacement net (box G) that made even this path safe pre-§4.

**Minimal candidate interleaving (fetch-by-relink), needs a repro to confirm reachability:** 3-replica RMT
on a shared CA pool; part P (blob `h`, in-degree = #replicas-with-P) fetched by R2 via relink. (1) R2
decodes P's manifest, `adoptEvidence(h)`; (2) R2's `stageManifest` PUT stalls under an object-store pause;
(3) a `DROP PARTITION` covering P lands, every replica drops its P-ref → `h` in-degree 0; (4) GC r1
condemns `h@t1`, GC r2 graduates → `delete_pending(t1)`; (5) deposed leader captures `deleteExact(h,t1)`,
pauses; (6) R2's stall clears, `precommitAdd(h)` + `promote` (trusts `h`, no probe) → R2 commits a ref
naming `h@t1`; (7) fresh leader folds R2's edge → spare (add-only, body still `t1`); (8) deposed leader
resumes → `deleteExact(h,t1)` deletes the LIVE body → R2's committed manifest dangles → fsck-only loss.

**What would settle it:** a gtest two-leader repro in the `gtest_cas_gc_ack_floor.cpp` family that (a) drives
a fetch-by-relink `publishEntries` whose `stageManifest` is held mid-flight, (b) condemns+graduates the
adopted hash via a source drop, (c) fires a deposed-leader `deleteExact(t1)` after promote, and asserts
`hr.exists == false` on the still-`t1` body — plus the `CaRetiredInRunFoldAbortWitness.tla` gate extended
with a token-preserving `adoptEvidence` recovery action (per the §5 report's prescription). Either goes
RED today and GREEN under fix-option 2 (box I). Per protocol: needs its own TLA gate + a second
independent consult before ANY code change.


