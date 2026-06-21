# CA adopt: defer presence to the gate (precommit-first for adopted blobs) — design (B188)

**Status:** design for review
**Date:** 2026-06-21
**Backlog:** B188. Model: `docs/superpowers/models/CaBuildRootPrecommit.tla` (B171).

## Problem

The 24h trust-soak failed at T+228min (steady-state, no chaos) with a fatal
`Code 107 FILE_DOESNT_EXIST: Build: object blobs/e9/e9be… absent — cannot reuse (caller must
upload it)`. A staged part operation (`createHardLink`/`moveFile`/`clonePart`) adopts a content blob
by reference via `reuseBlob`, which **eagerly HEADs** the blob (`observeAndAdmit`, `CasBuild.cpp:316`)
**during staging — before any precommit**. Under sustained churn a hot duplicate-content blob whose
in-degree legitimately hit 0 is GC-deleted; the eager HEAD finds it absent and, for a tokenless
(`body_recreatable=false`) adopt, throws a **fatal** `FILE_DOESNT_EXIST` instead of a retryable abort.
No data is lost (the INSERT is never acknowledged), but the query fails spuriously.

## Why this diverges from the model

`CaBuildRootPrecommit.tla` (exhaustively clean, TRUE/TRUE, 24205 states) establishes two protections:

- **`INV_BUILDROOT_PROTECTS`** — `Precommit(bld,t)` references the full manifest tree by hash and
  protects the whole closure by reachability **from Precommit onward**. It does NOT require
  protection from adopt onward.
- **`INV_COMMIT_FAILCLOSED`** — a blob missing at commit takes the retryable `CommitAbort` path,
  never a fatal dangle.

In the model, `AdoptBlob` simply records the blob in the dep set; the adopt→precommit window is
**tolerated** because a deletion there surfaces as `CommitAbort` (retry). The implementation diverges
only because `reuseBlob`'s eager HEAD turns that window's deletion into a **fatal** error at staging
time, before the precommit and before the gate.

## The implementation already has the right pieces

- `adoptFromTree` records a **tokenless W-EVIDENCE dep with no HEAD**
  (`deps[{Blob,hash}] = DepEntry{Blob, std::nullopt, view_round, size}`). This is the model's
  `AdoptBlob`.
- `precommit(tree)` runs at commit, before publish (`publishStaging`, `ContentAddressedTransaction.cpp:177-192`)
  → `INV_BUILDROOT_PROTECTS`.
- The publish gate (`revalidateDeps`/`gateCheckDeps`) resolves a tokenless dep post-precommit:
  present ⇒ admit, condemned ⇒ resurrect, **absent ⇒ retryable `ABORTED`** (`CasBuild.cpp:839`).
  This is `INV_COMMIT_FAILCLOSED`.

The only offender is the eager HEAD in the three `reuseBlob` call sites.

## Design

### 1. `Build::adoptEvidence(const TreeEntry & entry)` — new method
Factor the tokenless-dep recording out of `adoptFromTree` into a method that takes a `TreeEntry`
(which already carries `placement`, `file_hash`/`pack_hash`, `file_size`/`pack_length`) and records
the W-EVIDENCE dep — **no backend call, no HEAD**:

```cpp
void Build::adoptEvidence(const TreeEntry & entry)
{
    requireAlive();
    const uint64_t view_round = store->retireView().round();
    switch (entry.placement)
    {
        case Placement::Blob:
            deps[{static_cast<uint8_t>(ObjectKind::Blob), entry.file_hash}] =
                DepEntry{ObjectKind::Blob, std::nullopt, view_round, entry.file_size};
            break;
        case Placement::Subtree:
            deps[{static_cast<uint8_t>(ObjectKind::Tree), entry.file_hash}] =
                DepEntry{ObjectKind::Tree, std::nullopt, view_round, entry.file_size};
            break;
        case Placement::PackSlice:
            deps[{static_cast<uint8_t>(ObjectKind::Pack), entry.pack_hash}] =
                DepEntry{ObjectKind::Pack, std::nullopt, view_round, entry.pack_length};
            break;
        case Placement::Inline:
            break;
    }
}
```
`adoptFromTree` keeps its current behavior by calling `adoptEvidence(entry)` on the found entry.

### 2. Replace the three eager-adopt sites
`ContentAddressedTransaction.cpp`, the staged-source content-blob branches in `createHardLink`
(~615), `moveFile` (~802), `clonePart`/`replaceFile` (~943): replace

```cpp
const bool body_recreatable = ... depIsTokened(entry.file_hash) ...;
dst_build.reuseBlob(BlobId(u128ToHex(entry.file_hash)), body_recreatable);
```
with
```cpp
dst_build.adoptEvidence(entry);   // record W-EVIDENCE dep by hash; gate verifies post-precommit
```
Delete the now-unused `body_recreatable` computations at these sites.

### 3. Gate, precommit, putTree — unchanged
- `putTree`'s W-TREE-BUILD invariant ("every child in the dep set") is satisfied: `adoptEvidence`
  records the dep.
- `precommit` then protects the closure; the gate observes/resurrects/aborts-retryable.

### 4. `reuseBlob`
Becomes unused by these sites. Leave it in place (still part of the `Build` API / referenced by
tests) unless a follow-up confirms it is fully dead; no behavior change to it.

## Why this is precommit-first for adopted blobs (your expectation)
The precommit references the assembled tree — which now includes every adopted hash via the
W-EVIDENCE deps — and protects the closure from precommit through publish across the intervening GC
rounds. The adopt no longer asserts presence before that protection exists; the residual
adopt→precommit window is, per the model, a retryable abort, not a fatal error. Written blobs are
unchanged (already heartbeat-protected via tokened deps — the model's owner protection).

## Out of scope
- Upload-after-precommit / S3 private-staging + server-side copy (B172) — a perf/footprint
  optimization, not required for this correctness fix.
- Whether `reuseBlob` can be deleted entirely — a later cleanup.

## Testing
- A gtest reproducing the race deterministically: a build adopts a blob by reference; GC deletes
  the blob (in-degree 0) before commit; assert the commit takes a **retryable `ABORTED`** (not fatal
  `FILE_DOESNT_EXIST`), and that with the blob present-but-condemned it resurrects and commits.
- A gtest that `adoptEvidence` records the dep without any backend HEAD (use a counting backend:
  zero `head` calls at adopt time).
- The existing CA gtest suite + a no-chaos soak segment over the hot-duplicate workload to confirm
  no fatal `FILE_DOESNT_EXIST` from adopts.

## Files
- `Core/CasBuild.h` / `Core/CasBuild.cpp` — `adoptEvidence`; `adoptFromTree` delegates to it.
- `ContentAddressedTransaction.cpp` — three call-site swaps; drop `body_recreatable` there.
- `Disks/tests/gtest_cas_build.cpp` (or `gtest_ca_transaction.cpp`) — the regression tests.
