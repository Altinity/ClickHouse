# CAS promote: resurrect a prematurely-condemned tokened blob from the writer's own bytes

**Date:** 2026-07-09
**Branch:** `cas-gc-rebuild`
**Status:** design (approved principle; mechanism validated by a fresh-model adversarial consult)

## Problem

Under a fast GC (`gc_interval_sec=5`), a plain `INSERT` on CA-over-S3 intermittently fails the whole
statement with:

```
promote: blob <key> condemned at commit revalidation — failing closed (INV-1). (ABORTED)
```

(`ErrorCodes::ABORTED`, Code 236). Reproduced deterministically-enough in the stateless CA-s3 lane
(`01156_pcg_deserialization`, `01710_projection_detach_part`,
`02346_exclude_materialize_skip_indexes_on_insert`). The soak passes only because it *retries* the
`ABORTED`; the design today treats this transient as retryable-by-caller. Stateless tests — and ordinary
production `INSERT`s — do not retry, so the transient surfaces as a user-visible failure.

## Root cause

The write path is `stageManifest → precommitAdd → putBlob (per blob) → promote`
(`ContentAddressedTransaction::publishStaging`, `ContentAddressedTransaction.cpp:227–255`).

- `Build::putBlob` (`CasBuild.cpp:130`) already implements INV-1 at *upload* time: if it finds the blob
  condemned, it re-uploads from the writer's own re-readable `BlobSource`, so post-`putBlob` the blob is
  live with a fresh incarnation.
- GC condemns a blob when its **folded** in-degree reaches 0 (`CasGc.cpp`). The `precommit→blob` edge that
  protects a freshly-inserted blob only raises in-degree once GC **folds** the create-precommit event with
  the manifest body present. Between `precommitAdd` and `promote` GC has usually not folded that event, so
  the blob's in-degree is still 0 from GC's view and it is condemnable.
- `Build::promote` performs a **fail-closed blob revalidation** (`CasBuild.cpp:886–899`): it HEADs every
  blob leaf and throws `ABORTED` if `isCondemnedToken` is true. When GC re-condemns the fresh incarnation
  in the tiny `putBlob→promote` window, this gate fires.

The gate's own comment already names the cure — *"A condemned blob is recreatable only from this build's
own source (INV-1)"* (`CasBuild.cpp:883–885`) — but the code only **aborts**; it never actually re-uploads.

There is already a **copy-forward pre-pass** in `promote` (`CasBuild.cpp:795–817`) that resurrects
condemned blobs *before* the shard CAS loop — but deliberately only for **tokenless W-EVIDENCE deps**
(`adoptEvidence`, always from a *committed* source manifest, no local source bytes → resurrect via
`copyForwardFromCondemned`, a documented INV-1 exception that GETs the condemned object). It explicitly
**skips tokened deps** (`CasBuild.cpp:809`: `dep->second.token.has_value() → continue`). Tokened deps are
exactly the `putBlob`'d blobs of a fresh `INSERT` — the ones that *do* have retained source bytes and hit
the fail-closed gate.

## Principle (user directive)

> At commit we must have the data in hand; the recovery must be invisible to the client.

Make the writer *retain the bytes it is committing* and, when promote finds one of its own tokened blobs
prematurely condemned, **re-upload from those bytes and continue** — the client sees a successful `INSERT`,
never the transient `ABORTED`.

## Design

Complete the resurrection symmetry that already exists for the tokenless case, applying it to the tokened
case in the correct, race-closing place. Four changes:

### 1. Retain the writer's `BlobSource` for every `putBlob`'d blob

Add to `Build` a `std::map<UInt128, BlobSource> retained_sources;` (a **parallel map**, not a `DepEntry`
field — `putBlob` assigns a fresh `DepEntry{...}` on its adopt/record paths, `CasBuild.cpp:294,369`, which
would clobber a source stored inside it). Populate it for **every** hash `putBlob` handles, including the
dedup-adopt case (`observeAndAdmit` adopt, `CasBuild.cpp:280–296`) — a later condemnation of an adopted
incarnation must also be re-uploadable, and content-addressing guarantees our temp bytes equal the adopted
content.

Cost is negligible: `BlobSource` is `{uint64_t size, std::function capturing one temp-path String}`
(`CasBuild.h:16–21`); it retains **no payload** — the bytes stay on disk in the pending-blob temp file.
Thousands of columns cost KBs of closures.

**Lifetime is safe:** the source closure captures `pb.temp_path`
(`ContentAddressedTransaction.cpp:245–250`); `cleanupPendingTempFiles` runs only at the very end of
`commit` (`:296`), *after* every `publishStaging`/`promote` (including in a multi-part commit). The temp
file is therefore guaranteed present throughout `promote`. `retained_sources` is scoped to the Build, which
dies with the transaction; it must not be used after `commit` returns.

### 2. Resurrect **inside the closure, after the owner-liveness check** — not in the pre-pass

The tokened resurrection goes **after** the owner-liveness check (`CasBuild.cpp:860–881`), fused with the
revalidation loop (`:886–899`) — **not** in the pre-pass at `:795–817`.

Why not the pre-pass: it runs *before* the owner check. A tokened blob's *only* protection is *this build's*
precommit. If that precommit was concurrently abandoned or GC-reclaimed, the owner check aborts at
`:876–881` — but a pre-pass re-upload would already have produced a genuinely orphaned incarnation
(in-degree 0, no owner): a consequential PUT on an aborting path (CLAUDE.md: *no consequential action on the
fallback path*), adding orphan pressure. Placing resurrection **after** the owner check means the live
precommit is *proof* the blob is legitimately protected and resurrection is warranted. (An abort at the
owner check then does no re-upload → no orphan.)

The tokenless copy-forward pre-pass at `:795–817` stays **untouched**: its `adoptEvidence` source is a
*committed* manifest, so its blob has an independent live owner regardless of this build's fate, and the
early placement is safe.

### 3. Bounded resurrect-then-recheck loop (close the liveness race)

A single re-upload does not close the race: GC can complete a fold round between the re-upload and the
revalidation HEAD and re-condemn the still-unfolded fresh incarnation. Since the user's bar is *invisible*
(zero flake), fuse resurrect + revalidate into a **bounded loop** per blob leaf:

```
for attempt in 0..MAX (MAX = 8, mirroring CasBuild.cpp:182 / :501):
    hr = backend().head(blob_key)
    if !hr.exists:
        if have retained source: uploadFromSource(...); continue   // absent → re-upload, recheck
        else: ABORTED "absent at commit revalidation"              // backstop (unchanged)
    if isCondemnedToken(...):
        if have retained source: uploadFromSource(...); continue   // condemned → re-upload, recheck
        else: ABORTED "condemned at commit revalidation (INV-1)"   // backstop (unchanged)
    break   // present & live → leaf validated
after loop without break: ABORTED (exhausted attempts)
```

Exhausting 8 re-condemnations within a single `promote` closure is not physically reachable at a 5 s GC
interval, so this is effectively deterministic while staying honestly bounded. `uploadFromSource`
(`CasBuild.cpp:299–492`) is confirmed to touch the backend only via `head`/`putIfAbsentStream`/
`putOverwrite` — **never `backend().get`** — so it re-uploads our own bytes for our own content hash without
ever reading the dying object (INV-1 preserved; strictly safer than the tokenless copy-forward, which must
GET and hash-verify). A racing *live* displacement makes its If-Match fail → re-observe → adopt
(`:488–491`), so it cannot clobber another writer's live incarnation.

**Backstop preserved:** a tokened dep with **no** retained source (should not occur for a `putBlob`'d blob,
but defensively) keeps the fail-closed `ABORTED` — retryable-by-caller, exactly as today.

### 4. (Follow-up, out of scope) fold-barrier at promote

The fully-invisible, zero-re-upload root-cause fix is a **writer-triggerable synchronous fold** that
activates the precommit edge before revalidation, so the blob is never condemnable. The concepts exist
(`minLivePrecommit`/`has_live_precommit`, fold-barrier control) but there is no writer↔GC synchronous-fold
API today (`foldManifestEdgesForTest` is test-only), so a true barrier is a larger coupling change. Recorded
as the ideal follow-up; the bounded in-closure resurrect is the pragmatic fix that makes the failure
disappear now.

## Invariants / correctness

- **INV-1 (never revive by reading the dying object):** preserved — resurrection is `uploadFromSource`
  (no GET). The tokenless copy-forward's GET remains the sole, documented exception, unchanged.
- **INV_NO_DANGLE (no committed manifest over to-be-deleted blobs):** preserved and strengthened — a
  committed ref now names blobs that were re-uploaded live at commit, rather than aborting.
- **No consequential action on the fallback path:** resurrection is gated behind the owner-liveness check,
  so an aborting promote never re-uploads.
- **"Accept condemned without resurrect" is UNSOUND** and rejected: the condemn→delete pipeline is exact-
  token two-phase; an accepted-but-condemned token can graduate to delete, dangling the manifest
  (`CasGc.cpp` treats in-degree recovery on a `delete_pending` blob as structurally impossible). Only a
  fresh incarnation (new token) is safe — which is what `uploadFromSource` produces.

## TLA+

The abstract `WPromote` action's blob revalidation currently models the condemned leaf as a fail-closed
stutter/abort. This change lets `WPromote` re-upload a condemned leaf from the writer's retained source (an
already-modeled INV-1 revival primitive) and re-check, bounded. The plan's first task is the TLA+ gate:
extend `WPromote` (or its blob-revalidation sub-step) so a condemned, source-backed, owner-live leaf
transitions via revival rather than abort, and confirm `INV_NO_DANGLE` / the liveness properties still hold
(and that an owner-dead leaf still aborts with no revival). No new constant.

## Testing

- **gtest (unit, RED→GREEN)** in the CA core/wiring gtests: build a Build, `putBlob` a blob, condemn its
  token via the retire view (mirror the `gtest_cas_gc_leak.cpp` condemn helpers), then `promote` and assert
  the commit **succeeds** (the blob is resurrected) — RED against current code (ABORTED), GREEN with the
  fix. A second case: a tokened dep whose precommit is abandoned before promote still aborts **and** leaves
  no re-uploaded orphan (owner-check-first placement).
- **Stateless:** the three CA-s3 lane tests (`01156`, `01710`, `02346`) pass without retry.
- No `sleep`-based synchronization anywhere (CLAUDE.md).

## Files

- `Core/CasBuild.h` — `retained_sources` map + method-doc updates.
- `Core/CasBuild.cpp` — populate `retained_sources` in `putBlob`; replace the fail-closed revalidation
  (`:886–899`) with the bounded resurrect-then-recheck loop, after the owner-liveness check.
- TLA+ spec (the CAS GC/promote model) — `WPromote` revival extension.
- CA core/wiring gtests — the two new cases above.
