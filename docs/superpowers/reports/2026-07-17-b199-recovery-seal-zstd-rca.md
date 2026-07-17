# B199 RCA — `RefWriterRecoverySeal.SealPutConflictThrowPropagatesAndDoesNotWedgeRecovery` deterministic red

## Symptom {#symptom}

Deterministic gtest failure (the only red in the Ca* battery, 907/908):
the test's SECOND `listRefs` — the restarted recovery that must ADOPT the durable
foreign seal — threw `CORRUPTED_DATA`: `CAS cas_ref_snap: zstd decompression failed:
Src size is incorrect` out of `CasRefLedger::ensureRefTableRecovered` →
`decodeRefTableSnapshot`.

## Root cause {#root-cause}

Test-fixture assumption invalidated by the formats-v3 codec cutover — NOT a product bug.

- The `RefWriterTestBackend` corrupt hook fabricates the "foreign writer landed a
  DIFFERENT object" by landing `bytes + "\x01_FOREIGN_DIFFERENT"` — the attempt's own
  valid seal bytes with trailing garbage.
- Before formats v3 the snapshot object was raw text and `decodeRefTableSnapshot`
  tolerated the trailing garbage ("corrupted-but-decodable"); the test itself flagged this
  decode laxity as an open side-finding (F3-1a) and leaned on it: the adopted foreign seal
  decoded to the same two committed rows.
- `b44b7952ffc` (2026-07-16, formats v3 phase 3) cut `cas_ref_snap` over to zstd-framed
  text. A zstd frame with appended garbage fails one-shot decompression with
  `Src size is incorrect` — the fixture now fabricates a genuinely undecodable object, and
  the restarted recovery correctly fails closed on it.

Product behavior is CORRECT on both sides: an undecodable seal object must fail recovery
closed (fail-close on corruption), and the zstd frame check is precisely what CLOSED the
old F3-1a decode-laxity finding. Only the fixture's way of producing "a different object"
stopped matching its intent (a real cross-process writer lands a VALID seal, not garbage).

## Fix {#fix}

Test-only, two parts (`src/Disks/tests/gtest_cas_ref_writer.cpp`):

1. `RefWriterTestBackend` gains `corrupt_foreign_bytes`: when set, the corrupt hook lands
   these caller-provided bytes as the foreign object instead of `bytes + garbage` (the
   default stays garbage — right for tests pinning fail-closed corruption handling, e.g.
   the `_log/`-key corruption test).
2. The seal-conflict test now builds a VALID foreign seal (same ns, same seal id
   `(2, MAX)`, `sealed_from = (2,1)`) with THREE committed rows against the fixture's two,
   encodes it through the real codec, and asserts the restarted recovery adopts it:
   `listRefs().size() == 3`. Three-vs-two makes the adoption PROVABLE — the converged
   state is the durable foreign object, not a local recompute (the old assertion `== 2`
   could not distinguish those).

The twin concurrent test (`SealPutThrowsMidFlightSecondParkedCallerDoesNotHang`) never
relied on adoption decode (asserts no-hang only) and stayed green throughout.

## Validation {#validation}

- `RefWriterRecoverySeal.*`: 9/9 OK.
- Full Ca* battery (`Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*`):
  **907 ran / 907 passed / 0 failed** (2 pre-existing DISABLED). Zero reds.

## Follow-up closed by this RCA {#follow-up}

The F3-1a side-finding ("adoption-of-a-corrupt-seal tolerance") is MOOT since formats v3:
an undecodable foreign seal now fails recovery closed via the zstd frame check, which this
test run demonstrated live. The test comment now records this.
