---
description: 'Every ruling made while implementing the CAS backend request contract plan, as decision, reason and cost if wrong, grouped by area: engine, wave migration units, checkpoint, and the lock task.'
sidebar_label: 'Request-contract rulings (2026-09-03)'
sidebar_position: 2
slug: /superpowers/cas/request-contract-rulings-2026-09-03
title: 'CAS backend request contract — implementation rulings (2026-09-03)'
doc_type: 'reference'
---

# CAS backend request contract — implementation rulings {#cas-backend-request-contract-implementation-rulings}

Source: `.superpowers/sdd/2026-09-03-cas-backend-request-contract-plan/progress.md`, the SDD ledger
for `docs/superpowers/plans/2026-09-03-cas-backend-request-contract-plan.md` against
`docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md`. Every ruling the ledger
records, in the form **decision — reason — cost if wrong**, grouped by the area it touched. A ruling
with "costs nothing" recorded a scan-table finding with no live consequence either way; it is kept
here for completeness, not because it changed anything.

## Engine (`Backend/CasRequests.{h,cpp}`, `CasWriteResult.h`, `CasRetry.h`) {#engine}

- **Retry is a window with an optional lease bound, bound at call entry, not per attempt.** Reason:
  fake clocks need a deadline computed once so a test can assert on it, and the deadline's source
  (policy vs. lease) has to be honest about which bound produced it. Cost if wrong: rework across
  Tasks 3-5, accepted early over shipping the wrong engine.
- **No test minter for `Etag` — every incarnation in a test comes from an admitted write.** Reason:
  a minter that bypasses `CasRequests` would let a test construct exactly the value the whole design
  exists to make unconstructible. Cost if wrong: none (removed before landing).
- **Legacy `Backend` methods stay virtual; overrides move with the production site they instrument,
  not eagerly.** Reason: the CAS gtest tree carries hundreds of overrides used as fault-injection
  seams; converting a legacy method to a forwarder before its caller migrates would leave an override
  dead with its test still green — the worst failure a migration can have. Cost if wrong: a fault
  double stops firing silently.
- **`getStream` keeps its legacy implementation until Tasks 8/9.** Reason: sequencing — the keyed
  `stream` primitive lands with its first real callers, not ahead of them. Cost if wrong: none.
- **The persisted-token component (`PersistedEtag`, `TokenFields`, the wire vocabulary) is Task 18's
  alone; Tasks 9 and 12 lose their token-bearing parts.** Reason: `Token` crosses file boundaries in
  out-parameters and returned values, so the unit of migration is the token-flow component, not the
  file. Cost if wrong: a component migrated in two pieces by two agents.
- **The budget header (`CasRequestBudget.h`) is rewritten before the lock (Task 15), not at it.**
  Reason: downstream sites need the two surviving fields (`attempt_timeout_ms`,
  `lease_safety_margin_ms`) and the recovery walk's three settled early so later tasks compile
  against the final shape. Cost if wrong: a second edit at the lock.
- **All arithmetic in the reservation and deadline paths is saturating.** Reason: `now + sleep +
  reservation` must not wrap past the deadline under a pathological clock or budget value. Cost if
  wrong: an unbounded retry loop on overflow — accepted over the smaller cost of one saturating-add
  helper everywhere the arithmetic appears.
- **`isDefinitelyRefusedWrite`'s credential carve-out is a CAS-local predicate over named codes only,
  never `UNKNOWN`.** Reason: the general `isAccessTokenExpiredError` includes `UNKNOWN` — every
  unmodeled S3 error — so a read lost its retry on any unmodeled transient (GCS/RustFS/MinIO codes),
  and a write turned a real `AccessDenied` carrying `UNKNOWN` into a 90-second wedge of the ref lane.
  Cost if wrong: an unmodeled transient treated as a definite failure — the exact defect this ruling
  fixed.
- **The probe's retry lives in `CasOperation::probeSentinel` as retry-on-`Indeterminate`, not inside
  the backend's default body.** Reason: the default `probeSentinelRaw` body swallows every exception
  into `Indeterminate` by value, so a retry placed anywhere else is vacuous. Cost if wrong: an
  unmodeled probe error spends the whole policy deadline without ever reissuing.
- **A credential refresh happens at most once per call; a second credential-class error under the
  same call is classified as if no refresh were available.** Reason: with a real refresh callback
  installed, an unbounded "try refresh, fail, try refresh again" loop never terminates and burns the
  90-second deadline on a permanent denial. Cost if wrong: none — spec rule verbatim, latent until a
  disk installs a real refresh callback.
- **`isDefinitelyRefusedWrite` includes the refreshable-credential class structurally
  (`|| isRefreshableCredentialError(e)`), not by an ordering trick in the caller.** Reason: makes the
  refusal predicate and the refresh predicate's relationship an invariant of the function itself
  rather than of every call site remembering the order. Cost if wrong: none — spec rule verbatim.
- **A credential-class error is a definite non-application of THAT attempt and must not set
  `any_ambiguous`; `fits` is strict at zero remaining budget.** Reason: HTTP semantics — a 403 is
  answered before any apply, so it can never be the ambiguous case a resolve read would need to
  settle; and an inclusive `<=` at zero remaining let attempts start at or past the deadline. Cost if
  wrong: "an AccessDenied that hid a landed write" — impossible by HTTP semantics — or one extra
  attempt issued at the boundary.
- **After an ambiguous attempt, the resolve read's verdict has three arms, not two: our bytes →
  `Committed`; the precondition still satisfiable (`create`: `ProvenAbsent`; `replace`: the seen
  incarnation unchanged) → unresolved-but-repeatable, reissue (or `GaveUp{Unresolved}` under `once`);
  anything else → `Conflict`, every ambiguous attempt of *that inner write* provably dead.** Reason:
  a definite conflict on write two of a `readModifyWrite`, after an ambiguous write one that in fact
  resolved to a competitor's *identical* bytes, must not be reported `Committed` — a fixed defect
  found alongside: today an ambiguous `replace` whose resolve shows the precondition unchanged
  returned a false `Conflict{same incarnation}` instead of reissuing. Cost if wrong: one extra write
  attempt on a hot key (the conservative failure mode), against a false-positive commit or a false
  conflict (the actual bugs this closed).
- **`any_ambiguous` is scoped to one inner write of a `readModifyWrite`, reset at `writeLoop` entry;
  the rest of `WriteState` (`attempts_sent`, `sent_any`, `last_seen`, `reissues`,
  `refresh_attempted`) stays call-wide.** Reason: whether an earlier attempt was ambiguous is a fact
  about the bytes that one inner write sent — a `Conflict` on write two proves write two's own
  ambiguous attempts dead, not write one's; but an operator's attempt counters and a `GaveUp`
  verdict describe the whole logical call, not one inner write. Cost if wrong: a per-attempt count
  under-reported, or an ambiguity wrongly retired across inner writes.
- **Under `once`, a refreshable credential answer is not refreshed at all — no reissue can sign with
  what a refresh installed — so `Refused` stays truthful.** Reason: supersedes an earlier round's
  "a successful refresh under `once` still reports `Refused`", which spent a needless refresh call.
  Cost if wrong: one extra credential-provider call per verb under a credential failure — accepted
  once, rejected the second time this shape came up.
- **A call-scoped refresh state shared across nested reads/writes was rejected.** Reason: the spec
  states there is no exactly-once guarantee — the deadline is the bound everywhere — and the S3
  layer's in-place refresh for `head`/`remove` is the spec's own table, not a violation of it. Cost
  if wrong: one extra credential-provider call per verb under a credential failure.
- **The two hand-written loops (`CasRefCatalog::casUpdateImpl`'s sibling, `ensureBlobPresent`'s
  outer publication loop) do not share one deadline across both; each verb inside binds its own
  window from one captured `Retry` value.** Reason: "one `Retry::standard()` captured before the
  loop" means one policy *value*, not one deadline instant shared by every request the loop makes;
  any `GaveUp` still ends the loop, and the loop's own iteration cap (`kMaxCatalogCasAttempts`,
  `max_publication_attempts`) still bounds it. Cost if wrong: a loop under continuous conflicts runs
  cap × window, never unbounded — the conservative failure mode, accepted.
- **Every non-`DB::Exception`, non-`Poco::Exception` `std::exception` on a write propagates
  unchanged; only those two classes are settled by a resolve read.** Reason: a `std::bad_alloc` did
  not observe the store, so resolving it by reading back would misattribute a local resource failure
  to the object store. Cost if wrong: an odd transport exception type surfaces instead of being
  resolved by read — visible in the first test that raises it.
- **A transaction-owned `CasOperation` is never shared across concurrent detached upload tasks; each
  task builds its own via `mountRequests().resume(txn_generation, liveness)`.** Reason:
  `CasOperation` carries mutable per-call state and is documented single-threaded; sharing one across
  threads races. Cost if wrong: none — folded into U9's brief before any code shared one.
- **Resolve-read stop causes are carried through a private `Resolved{seen, stop}` / `last_read_stop`
  side channel rather than a new exception subclass.** Reason: an exception subclass would duplicate
  `throwCasWriteRetryLater`'s message and rate-limited log; every give-up already throws the same
  `NETWORK_ERROR`, so distinguishing *why* only matters to the one caller (the resolve read) that
  swallows it. Cost if wrong: none — verified non-stale by re-review.

## Wave migration units (U1-U12) {#wave-units}

- **The bootstrap-control shapes — `claimMount`/`claimMountAwaitingExpiry`,
  `MountLeaseRenewer::claim`, `claimOwnerOrThrow`, `allocateWriterEpoch`, the remount driver's
  re-anchor renewal (`renewForRemount`) — run on the open plane, `Pool::openRequests()` (renamed
  from `gcRequests()`, which now serves more than GC).** Reason: a self-remount runs with the mount
  fence already latched lost, so a claim made while re-establishing that fence cannot itself be
  admitted under it; each site's safety is its own conditional write's precondition, not fence
  admission — same logic as `MountLeaseRenewer::claim`. Cost if wrong: none — the rename was deferred
  to the lock task precisely so nothing depended on the old name mid-wave.
- **`renewForRemount` takes no plane parameter; it always uses the keeper's own open plane.** Reason:
  a plane parameter would make "renew on the wrong plane" a representable call; the keeper has
  exactly one open plane, so the parameter was a mistake waiting to happen. Cost if wrong: none.
- **The renewal's inter-attempt sleep is interruptible, bound to the pool's own stop signal.**
  Reason: an uninterruptible sleep during a renewal retry adds latency to shutdown proportional to
  the backoff cap. Cost if wrong: up to ~5 s of extra shutdown latency — the number the ruling
  bounded it to.
- **U2's precondition checks move from the callee to the caller.** Reason: `Pool::open` was found to
  run neither store gate after the probe stopped running them itself; the checks belong where the
  decision to proceed is made. Cost if wrong: a writable open that should refuse on a throwing hook
  does not — caught by U2's own review (F1/F2), fixed in U1's files.
- **The pre-mount bootstrap read (before any lease exists) uses a local `CasRequests` over
  `Fence::open()`, admitted once — not the mount plane's fence, which does not exist yet at that
  point.** Reason: there is no lease to fence against before the mount is claimed. Cost if wrong:
  none — the only shape that type-checks at that point in the sequence.
- **`CasDecommission` runs on two open-fence `CasRequests` instances; six fault doubles moved onto
  the primitives in the same commit as the sites they instrument.** Reason: decommission consults no
  fence today, by omission — the migration makes that omission a stated choice rather than leaving it
  implicit. Cost if wrong: none.
- **The GC drain's `Liveness` is not a per-decision read: it is the leader's own cached authority
  flag, refreshed by one `gc/state` read before every erase attempt and again before the
  drain-complete verdict is reported, fail-closed.** Reason: `drainCompletedRemoving` was found
  admitting on the open plane with no liveness at all, so the deposed-leader guard was dead; a cache
  refreshed only once per drain (rather than once per erase, and once more before the final verdict)
  let a deposed leader erase a row, or let a drain report completion on authority it had already
  lost. Cost if wrong: a deposed leader mutating a catalog it no longer owns — the defect this ruling
  closed, found twice (once for the per-erase refresh, once for the final-verdict refresh).
  Refreshing costs one extra `gc/state` read per round on the no-eligible path, accepted.
- **`Gc::acquireOrRenewLease` becomes one `readModifyWrite` whose `decide` **is** the steal
  protocol: renew when the lease is ours, steal only when the lease tuple is unchanged across two
  observations **and** the heartbeat pair is unchanged **and** `allow_steal` (false on the manual
  path), `nullopt` otherwise.** Reason: encodes the existing steal safety directly in the decision
  function rather than as a wrapper around a generic retry loop, so the primitive proves the
  invariant rather than trusting a caller to preserve it. Cost if wrong: none — matches the design's
  read-modify-write rule (a loop whose conflict is not terminal is `readModifyWrite`).
- **`ensureBlobPresent`'s `publish` runs under `Retry::once()`, never the shared `standard()`; the
  outer loop mints a fresh envelope per physical attempt and paces itself with
  `op.pause(Retry::backoff(attempt))`.** Reason: an engine reissue of `publish` under `standard`
  would re-send the identical envelope and therefore the same `incarnation_tag`; on a
  content-derived-ETag dialect that is the condemned incarnation reborn, and GC's exact-incarnation
  delete would then remove a live body. Overrules the brief's original "`publish` on that policy".
  Cost if wrong: the publication loop keeps its own bound instead of the engine's deadline — the
  shape it already has today, the safe fallback.
- **`reconcileMetaClean` is create-first: a publication with nothing at the marker key spends its
  `create` as the whole reconciliation; only a `create` that loses falls through to
  `readModifyWrite`.** Reason: a read-first shape costs one extra `GET` on every insert to learn what
  the `create` would have settled for itself. Cost if wrong: one extra `GET` per insert — the
  original brief's shape, not a correctness defect.
- **A part-write transaction's operations are built via `resume(txn_generation, liveness)`, never
  fresh `admit()` calls, so every upload task shares the one generation captured at transaction
  start.** Reason: traced explicitly — `setMountDeadline` does not bump the fence's generation, only
  `armMountFence` does — so `admit()` mid-transaction would proceed under a re-armed fence the
  transaction was never admitted under, the opposite of the old captured-fence semantics. Cost if
  wrong: a transaction silently continuing under a fence generation it never held.
- **`putMetaIfAbsent` writes on the caller's own operation, not a separately-fenced one.** Reason:
  GC's blob-meta condemn writes and `PartWriteTxn`'s backfill writes are on different fences (GC's
  open plane vs. the mount plane); a single fixed plane for the primitive would put one of the two
  callers on the wrong fence. Cost if wrong: a GC condemn write admitted under a mount lease it does
  not hold, or vice versa.
- **The mount renewal's counters (`CASMountRenewalAttempts`, `CASMountRenewalRetries`,
  `CASMountRenewalResolved`) are fed from `Committed{attempts_sent, resolved_by_read}` and
  `GaveUp{attempts_sent}` uniformly, on every outcome — not only on success.** Reason: the old
  per-attempt source these counters were computed from no longer exists once retries move inside the
  engine; an operator's attempt counters have to sum over every ending of a write. Cost if wrong: a
  renewal counter under-reporting attempts on every non-`Committed` ending.
- **The renewal audit event's detail schema collapses to `classification` + `attempts_sent`; there
  is no per-attempt `retrying` transition and no separate `unresolved_reason`/`deadline_source`/
  `stop_cause` keys.** Reason: those three keys distinguished cases `classification` now names
  directly and completely — `external_lease_deadline`, `request_deadline`, `unresolved`, `conflict`,
  `cancelled`, `fence_or_lifecycle_lost`, `deterministic_failure` — and a per-attempt row added no
  information the terminal event does not already carry. Cost if wrong: an operator query written
  against the old keys returns nothing, caught immediately by the query returning empty rather than
  by silent misinterpretation — corrected in the operator docs by this same wave.
- **`allocateWriterEpoch`'s non-convergence at the deadline is retry-later (`NETWORK_ERROR`) through
  the shared `orThrow`, not its own `CORRUPTED_DATA` `switch`.** Reason: the bound moved from an
  attempts cap to a deadline, so exhausting it under contention is not proof of a corrupted epoch
  record the way exhausting a fixed attempt count arguably was. Cost if wrong: a corrupted epoch
  record masquerading as a transient failure — judged unlikely given the deadline is 90 seconds of
  genuine contention, not a fixed small count.
- **Persisted incarnations (`PersistedEtag`, condemned rows) are deleted only by `head → matches →
  remove(observed)`, never `removeCurrent`.** Reason: `removeCurrent` re-heads and removes whatever
  it finds after a mismatch — exactly the operation that would delete a fresh owner's body if the
  persisted row was stale. Cost if wrong: deleting a live, unrelated object — the class of defect the
  whole persisted-incarnation design exists to prevent.
- **Test-double consolidation, fault-arming API, and generation renames (`KeyEntry`/`KeyPage`,
  `failNextCasPut`→verb-neutral names) are U12's, adopted incrementally as later units' fixes
  surfaced duplicated shapes.** Reason: the alternative — each unit inventing its own fault-double
  vocabulary — was already producing near-duplicate doubles across files by the time U12 started its
  sweep. Cost if wrong: one more consolidation pass at the lock — accepted as the smaller cost.

## Checkpoint (CP3, CP4′) {#checkpoint}

- **The identity probe (`pool_identity_probe`, inside remount) is admitted with no liveness
  predicate at all.** Reason: no public hard-stop predicate exists for it to consult; the probe's
  purpose is to *establish* terminality, so gating it on a terminality check it has not yet performed
  would be circular. The step's later stages get a try/catch that returns `false` instead of letting
  the probe throw uncaught on a terminal pool, which had made the documented `Replaced`-mid-`FORGET`
  path unreachable. Cost if wrong: none identified — the alternative (some liveness predicate) had no
  candidate.
- **`TheKeeperRedoRenewsOnTheOpenPlane`'s admission requirement stands (`Parked`, not `Dormant`); the
  test is rewritten to reach the redo through the parked-worker path instead of relaxing the
  admission guard.** Reason: a workerless pool reaching `keeper_redo` was found to be a test gap, not
  a legitimate state the guard should admit — `StaleRemountAnchorPerformsParkedRedo` already proves
  the same step through the correct path. Cost if wrong: none — the guard was already correct;
  cost would have been a relaxed admission that let more states reach a sensitive redo path.
- **`reconcile` refreshes authority once more, immediately before its drain-complete verdict, in
  addition to the once-per-erase-attempt refresh.** Reason: two fenced-leader race tests found the
  no-more-eligible verdict reading a stale authority flag with no refresh after the last erase — a
  drain could report "done" on authority it had already lost. Cost if wrong: a drain-complete verdict
  reported under stale authority — the defect this closed; costs one extra `gc/state` read per round
  on the no-eligible path.
- **No forgiving runtime destructor: `~CasMountRuntime` aborts on a joinable worker thread by
  design; the fix for a shutdown-order defect is a stop-and-join earlier in `~Pool`'s own sequence,
  never a swallowed join in the runtime's destructor.** Reason: a runtime outliving its own workers
  is a programming error, and a destructor that tolerates it converts a loud abort into a silent
  leak. Cost if wrong: none — this closed a real abort (`RuntimeUnderTest`'s `CasRequests` on the
  real clock vs. a lease deadline on an injected boot clock caused `untilLeaseSafe` to refuse before
  sending, throwing through a barrier into a runtime with joinable workers still running); the
  fail-safe direction was kept and the abort fixed at its actual cause instead.
- **The engine reserves `Backend::attemptTimeoutMs()` per attempt, not
  `CasRequestBudget::attempt_timeout_ms`.** Reason: found as a calibration trap — a test that derives
  a clock instant from the budget's field is calibrated to a number the engine never reads (zero on
  `InMemoryBackend` by default). Cost if wrong: a whole class of tests whose deadline arithmetic is
  silently wrong by exactly the gap between the two fields — filed as a coverage-gate sweep (Task 21)
  rather than fixed test-by-test at the checkpoint, since the reservation trap recurs by construction
  wherever a fixture pairs the two without setting both.

## Lock (Task 20) {#lock}

- **`Incarnation` → `Etag` (`PersistedIncarnation` → `PersistedEtag`, fields `incarnation` → `etag`),
  with a class comment stating the value plays the ETag *role* — GCS's generation among them — rather
  than naming a wire field.** User decision. Reason: "Incarnation" read as a namespace-lifecycle
  concept already named `incarnation` elsewhere in the same subsystem (the namespace incarnation, the
  blob envelope's `incarnation_tag`), which this rename deliberately leaves untouched to avoid a
  second, unrelated collision. Cost if wrong: a tree-wide rename to reverse — executed as one
  mechanical commit specifically so it could be reverted as one commit if wrong.
- **`gcRequests()` → `Pool::openRequests()`**, matching the wave's bootstrap-control-on-the-open-plane
  finding. Reason: the method now serves the mount-plane's own bootstrap-control sites, not only GC,
  so its old name was already wrong by the time the wave finished. Cost if wrong: none — pure rename,
  compiler-verified at every call site.
- **`KeyEntry`/`KeyPage` (T5's placeholder names, chosen to avoid a redefinition clash with the
  legacy `Backend::ListedKey`/`ListPage` structs) rename to `ListedKey`/`ListPage` only once the
  legacy structs are deleted.** Reason: the plan's own target names were unavailable until the
  legacy types they'd collide with were gone. Cost if wrong: one rename at the lock — paid as
  planned.
- **The field/local rename `incarnation` → `etag` is compiler-driven (the type is what decides which
  identifiers are in scope), never a blind `sed`; the namespace-lifecycle `incarnation` and the blob
  envelope's `incarnation_tag` are excluded by construction because nothing renamed their type.**
  Reason: roughly 1,600 call sites make a text-based rename unverifiable; the type system is the only
  discriminator that cannot rename the wrong thing. Cost if wrong: a renamed namespace-lifecycle
  identifier silently colliding with the new `Etag` vocabulary — averted by using the compiler as the
  gate, per the standing rule that a wide rename's gate is the search tool, never grep alone.
- **`gtest_ca_wiring.cpp`, outside the `gtest_cas_*` glob every prior grouping pass used, is
  converted by the lock agent directly rather than by a fourth migrator group.** Reason: found only
  by the compiler failing to build it — the file was never grouped because no earlier sweep's glob
  matched its name. Cost if wrong: none — the compiler is the exhaustiveness gate for this class of
  miss, and it caught this one before the tree went green.
- **The ledger's `check_fence_or_throw` callback member, its constructor parameter, and the
  `CasPool.cpp` wiring line that installs it are deleted; every pre-request
  `check_fence_or_throw(gen)` becomes `refuseUnlessAdmitted(op | mount_requests.resume(gen), what)`
  through the operation's own `admitted()`.** Reason: a tree-wide fence-check grep found 18 hits, not
  the ~40 the brief estimated, but 11 of them were a single callback wired through one constructor and
  ten call sites — all subject to the same rule (the engine admits every request; a pre-request check
  is now redundant with admission, a verdict-point check becomes `op.admitted()`). Cost if wrong: a
  fence check surviving outside the operation's own admission path, which the lock's acceptance grep
  is specifically built to forbid and would have caught.
