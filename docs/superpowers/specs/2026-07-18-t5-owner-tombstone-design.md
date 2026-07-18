# T5 (finding #9): decommission tombstones the owner anchor instead of deleting it

Status: user-approved direction, minimal design note (not a full spec — small, targeted fix).

## Background

`CasDecommission.cpp`'s teardown sequence (mount lease, epoch counter, owner anchor)
already has three prior rounds of hardening: exact-token deletes, farewell/epoch
cross-validation, and a liveness recheck of mount+epoch before ever touching the owner
object. That closes the race for mount and epoch, because both carry naturally-changing
tokens/values between incarnations.

The owner anchor (`OwnerObject`, `CasServerRootFormats.h`) does not: it holds only
`server_uuid`, which is byte-identical whether written by the original (now-decommissioned)
server or by a legitimate successor reclaiming the same `srid`. An exact-token delete right
before the tail's very last step therefore cannot distinguish "stale debris from the run we
just tore down" from "a successor's freshly (re)written, live anchor" — there is no recheck
after the owner `get()` immediately preceding its delete, unlike mount/epoch.

## Decision (2026-07-18, user-confirmed)

Do not try to close this with more delete-side fencing. Instead, stop deleting the owner
object. Decommission's last step becomes a conditional **rewrite in place** (tombstone),
using the exact-token machinery already in the file (`putOverwrite` with an expected token,
mirroring the existing `deleteSlotObject`/`putIfAbsent` conditional-write style). If a
successor's own claim raced and changed the owner object first, the conditional write simply
fails — no bespoke protocol, ordinary CAS semantics settle it, exactly like every other
conditional write in this codebase.

This intentionally does not try to make concurrent decommission-vs-recreate airtight to the
microsecond — per the user, that scenario is not the priority. It removes the *destructive*
half of the race (a legitimate live owner anchor being deleted out from under a successor);
if a rewrite loses a race, the tail simply aborts, matching the triage's already-stated goal
("any successor reclaim fails the tail closed").

## Changes

1. `CasServerRootFormats.h`/`.cpp` (`OwnerObject`, `encodeOwner`/`decodeOwner`): add
   `std::optional<uint64_t> retired_at_ms;` to `OwnerObject`. Encoded key `"rt"` (ms
   timestamp), tolerant/absent when never retired — mirrors the existing `saw_*` +
   `std::optional` pattern already used by `decodeRefTableSnapshot` for genuinely optional
   fields, not the plain scalar style used for e.g. `ServerEpoch::next_writer_epoch`.

2. `CasDecommission.cpp`, final owner step: replace
   `deleteSlotObject(*pool_backend, owner_key, owner->token, report.warnings)` with a
   conditional overwrite of the SAME owner object (same `server_uuid`, `retired_at_ms = now`)
   via `Backend::putOverwrite(owner_key, encodeOwner(...), owner->token)`. Treat anything
   other than a successful write the same way a failed delete is treated today (abort the
   tail, push a warning, `report.slot_removed` reflects an in-place tombstone, not a
   deletion — rename/repurpose the flag or add a distinct one, whichever reads clearer in
   the actual diff). `report.slot_removed` semantics need a look: today it means "owner
   object gone"; after this change owner is never gone, so decide whether it should mean
   "owner tombstoned" or whether a new field is clearer for callers/tests that read it.

3. `CasServerRoot.cpp`, `claimOwnerOrThrow`: in the "owner present, `*owner_uuid ==
   our_uuid`" branch, ALSO check `retired_at_ms` — if set, throw `CORRUPTED_DATA` with a
   message distinct from the existing "different owner" case: this server-root identity was
   explicitly decommissioned by an operator; a normal restart must not silently resume it.
   Mirror the existing error message's style (it already gives manual-recovery guidance for
   the different-owner case) — tell the operator this requires deliberately clearing
   `retired_at_ms` (manual object surgery, same escape hatch already documented for the
   "owner anchor lost" case) if they intend to genuinely bring this `srid` back.

4. No new "un-decommission" tool/CLI flag in this fix — out of scope per the "no big
   complications" steer. The manual-recovery message is the only revival path for now.

## Tests

- `gtest_cas_decommission.cpp`: extend/add a case asserting decommission's owner step now
  produces a tombstoned owner object (still present, `retired_at_ms` set) rather than an
  absent key; and a case where a competing `putOverwrite` (simulating a successor's
  concurrent claim) races the tombstone write and the tail aborts closed.
- A `CasServerRoot`-level (or `CasPoolRemount`-family) gtest: `claimOwnerOrThrow` against a
  tombstoned owner object (same `server_uuid`) throws `CORRUPTED_DATA`, distinct from the
  existing foreign-owner throw.

## Scope check against the triage's original recommendation

The triage's originally suggested fix ("inspect every `DeleteOutcome`... fence the tail
deletes to the exact terminated objects... re-verify the mount token immediately before the
owner delete") is already implemented for mount+epoch. This design supersedes only the
*owner* half of that recommendation with the tombstone approach, which the mount/epoch code
doesn't need (their values naturally change between incarnations, so exact-token fencing
already works for them).
