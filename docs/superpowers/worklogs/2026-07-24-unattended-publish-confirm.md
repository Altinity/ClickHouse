# Unattended round — publish-confirm + ref-lane exception safety (2026-07-24)

Spec: `docs/superpowers/specs/2026-07-23-cas-fetch-handoff-publish-confirm-design.md` (rev.5)
Branch: `cas-gc-rebuild`. Start HEAD: (filled below.)

**User-set program:**
1. writing-plans for the spec.
2. Subagent-driven implementation (Opus 5 for hard tasks, Sonnet 5 for simple/medium; codex
   double-check on critically hard ones).
3. Run the new fault-injection scenario (S42).
4. 4-hour soak.

**Standing rules for this round:** no handwaving on suspected bugs — systematic debugging; fix only
when the fix is obvious AND does not touch protocol/guarantees, else BACKLOG. Watch correctness,
S3 budget, CPU/memory/disk. Long tasks (>20 min) via nohup + log file + monitor; never a
self-matching pgrep. Watchdog every 20 min. Commit with pathspec ONLY (shared checkout).

## Timeline

- start HEAD: 3ddad3c6009
- 00:01 UTC — worklog created; unattended round begins
- 00:05 UTC — writing-plans started. Two read-only explore agents dispatched (exchange/fetcher
  surfaces; CAS test+bench infra). Read myself: `RefTableRuntime` layout, `commitRefChunk` +
  chunk-boundary call sites, `RefTableState` members, TLA runner conventions
  (`docs/superpowers/models/`, `run_*.sh`, jar symlink present), ca-soak `Scenario` contract.
- 00:12 UTC — SPEC REFINEMENT found while planning (committed with the plan): do NOT materialize
  the A1 candidate before the PUT. A candidate sharing its COW base with the live state cannot fold
  in place, so pre-PUT materialization would force an O(n) base rebuild per chunk; the install
  needs no materialization (moving base+overlay is equally noexcept). Correct order: COW-copy →
  apply(real id) → PUT → noexcept move install (+atomic bumps) under DENY → today's in-place
  O(overlay) fold outside the deny region. Net cost vs today ≈ one cheap COW copy; the apply merely
  MOVES to before the PUT (where a throw is a clean pre-durability failure).
- 00:14 UTC — exchange/fetcher surfaces mapped: `REPLICATION_PROTOCOL_VERSION_WITH_CA_RELINK=10`,
  `CA_POOL_UUID_PARAM`/`CA_RELINK_COOKIE`/`CA_RELINK_COOKIE_VALUE`, `tryGetContentAddressedExchange`
  (dynamic_cast on the disk's metadata storage), sender branch `DataPartsExchange.cpp:249-280`
  (releases the part by scope exit at `:276`), receiver branch `:728-771` with the
  `fall_back_to_byte_fetch` lambda, `relinkPartToDisk` `:1107-1169`, `publishEntries`
  `PartFolderAccess.cpp:338-372` (beginPartWrite → adoptEvidence* → stageManifest → precommitAdd →
  promoteBuild; catch → abandon → rethrow).
