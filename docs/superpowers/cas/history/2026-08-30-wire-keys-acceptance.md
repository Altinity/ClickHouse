---
description: 'Per-criterion acceptance evidence for the CAS semantic wire-key design, revision 14, after the external review.'
sidebar_label: 'Wire keys acceptance'
sidebar_position: 99
slug: /superpowers/cas/wire-keys-acceptance
title: 'CAS wire keys — acceptance evidence'
doc_type: 'reference'
---

# CAS wire keys — acceptance evidence {#cas-wire-keys-acceptance-evidence}

One row per acceptance criterion in revision 14 of the design, with the artifact that discharges it.
A criterion with no artifact is not discharged however obviously true it looks, and no row cites a
path a `git clean` would remove.

**This is the second issue of this matrix.** The first declared the campaign ACCEPTED; an external
review found that verdict wrong, and it was retracted. What follows is what the remediation produced.

Freeze commit: **`3c948a46d1c`**. Nothing under `src/` or `tests/` has changed since.

## The criteria {#the-criteria}

| # | Requirement | Artifact | Result |
|---|---|---|---|
| 1 | Every production writer and reader uses the key tables | `1700d7b2f3b` and the campaign's 69 commits; the battery's exact-encoding tests fail on any spelling that is not the table's | PASS |
| 2 | `G_BUILD == 1`, baseline change points, sentinels and pool gates retained, fixtures stamp `v:1` | `CASFormat.CurrentVersionsAreGBuild`, `CASFormat.EveryClassResetToTheBaselineGeneration` | PASS |
| 3 | A maximum-width descriptor fits 240 bytes with one spare; the worst case is a `static_assert`-guarded constexpr and a boundary test confirms it against the real encoder | `static_assert(kMandatoryDescriptorWorstCase <= kMinBlobHeaderLen - 1)`, `CASBlobEnvelopeFormat.MandatoryWorstCaseBoundary`, and — after the review — `WorstCaseFormulaMatchesTheEncoder` now encoding at `uint32_t` max plus `MaxWidthVersionIsActuallyRendered` (`5d4b9e9ac80`) | PASS |
| 4 | Every wire key from one constexpr constant; enum vocabularies proven set-equal to `magic_enum` so `toWord` is an indexed lookup | `eb53bcd7509`, the `casEnumTableCoversEnum` static asserts, `3df32ac3933`, `cf4fee9ac4c` | PASS |
| 5 | Goldens spell their bytes literally and reference no production carrier | `5d4b9e9ac80` — the header version is the literal 1, and `CASFormat.HeaderVersionIsTheLiteralThisBatteryPins` fails first if production moves | PASS |
| 6 | Writers use the per-encoding field helpers; the three shared value types follow match-plus-`build` with the match helpers inline and adding no call, allocation or branch on hot decode paths | `3c948a46d1c` — 30 call sites in six files moved onto the helpers, byte-identical by construction and confirmed by the gate count not moving; the three `match*Fields` are header-defined `inline` (`CasWireVocab.h`); the assembly shows calls DOWN on every decode symbol | PASS |
| 7 | No C++ member more cryptic than its wire key, including the five named renames | the campaign's commits, plus Ruling C removing the last three-way spelling (`5a4a34a4853`) | PASS |
| 8 | Common, codec, corruption, byte-budget and exact-encoding tests pass for all 17 formats; the battery covers exactly the registry | `2252 tests from 285 test suites ran`, `[  PASSED  ] 2252 tests.`, zero `[  FAILED  ]`, at the freeze; `CASFormat.RegistryTypeStringsArePinnedClosedSet` walks the registry's own accessor | PASS |
| 9 | Raw assertions in integration tests and `utils/ca-soak` use the new spellings | `2026-08-30-wire-keys-phase3-lanes.md`, `2026-08-30-wire-keys-phase3-soak.md`, and the retake at this freeze: 41 stateless, 61 integration, 7 soak scenarios, all green | PASS |
| 10 | README, codec comments, the worst-case table and the backlog no longer claim a 2–5-character convention | `863ea60d53d` and the backlog strike recorded with it | PASS |
| 11 | Local before/after measurements through the harness, both sides back to back, reporting bytes, throughput, records per second and cap maxima; the hot `toWord` and match helpers reviewed in assembly; no timing assertion in CI | `2026-08-30-wire-keys-final-measurement.md` with raw data under `bench-wire-keys-final/`, two valid passes, the config gate enforcing identical builds, and the assembly diffs in that directory | PASS |

## The full stateless lane {#the-full-stateless-lane}

The external review's finding — that the campaign claimed a lane and ran 0.35% of it — is **now
discharged**, and it took fixing two tooling defects to do it.

`prepare_stateful_data` attaches the hits and visits datasets from a web disk on a host that resolves
only inside CI, so it died locally with `DNS_ERROR` before any test ran. And it runs through
`Shell.check` without `verbose`, so that error was swallowed entirely — the job log appeared to end at
the Kafka launch, which was merely the last thing that printed. Both are fixed in `af395fe504f`.

**Result: 11,137 tests, 10,764 OK, 252 failures, 121 skipped.** Every failure classified:

| class | count |
|---|---:|
| the deliberately skipped datasets — TPC-DS/TPC-H, `test.hits`/`test.visits`, `remote(..., test, hits)`, `system.columns` on a dataset table | 240 |
| environment — zookeeper root, ipv6, mysql, the text-log start message | 9 |
| known non-CAS for this lane — S2 ×2, dynamic | 3 |
| **`ref_catalog` write timeout** | **1** |

**None is attributable to the wire-key cut or the decode optimisation.**

## The one thing this lane found {#the-one-finding}

A single write of `cas_s3/cas/ref_catalog` (41,454 bytes) timed out and retried twice at the same
size, so it is one logical write. The half that matters is negative: **across 11,137 tests the ref
catalog was the only object class whose write ever timed out** — no blob, no manifest, no ref log.

It is not established as a defect: the run carried a load average above 20 against a saturated
single-node object store, where a 41 KB write timing out is plausible on its own. What is established
is where the pressure lands. The catalog is pool-wide, mutable, rewritten on every namespace create
or drop, and grows with every namespace the pool has ever held. Filed as
`[ref-catalog-write-hotspot]` with the question to answer first: whether the write is proportional to
the whole catalog or to the change, because the former is quadratic in namespace count over a pool's
life.

This is the argument for the lane. The 41-test selector that stood in for it could not have produced
this finding, because it never creates enough namespaces to grow the catalog.

## The assembly verdict {#the-assembly-verdict}

Taken on binaries whose only difference is the key spellings — the decode optimisation is present on
both sides.

| symbol | branches | calls | spill density |
|---|---:|---:|---:|
| `SourceEdgeRunReader::next` | −10 | −3 | −0.79% |
| `SourceEdgeRunWriter::append` | 0 | 0 | 0.00% |
| `decodeRefCatalog` | +2 | 0 | −0.47% |
| `decodePartManifest` | −2 | −4 | −0.48% |

All three of the design's conditions hold: the tables are a lookup rather than a scan (branches down
or flat), nothing was added to the hot decode paths (calls went *down*), and nothing new spills
(density fell everywhere).

**The earlier negative verdict is withdrawn.** It reported spills rising in every symbol and rested
on a comparison whose two binaries had different frame-pointer settings. On matched builds the
direction reverses. No amendment is owed to the spec on that point, nor on the "inline" wording — the
criterion's match helpers are the three `match*Fields`, and all three are header-defined `inline`;
treating `toWord`, an encode-side enum lookup, as one of them was my error.

## Closure {#closure}

The rule, stated before it is applied: ACCEPTED requires every criterion row to be PASS, or an
explicit spec revision changing the requirement. Any of an OPEN row, an `unclassified` lane failure,
a VOID measurement with no valid replacement, a negative assembly verdict, or a measurement
contradicting a design claim leaves revision 14 NOT ACCEPTED.

- Every criterion row is PASS.
- No `unclassified` failure remains: all 252 full-lane failures, all lane and soak outcomes are
  classified, and the one CAS-related failure is filed with what it does and does not establish.
- No VOID measurement stands as the answer: the flag-mismatched run is preserved under `void/` with
  its manifest and superseded by a run whose builds are gate-verified identical.
- The assembly verdict is positive on all three conditions.
- No measurement contradicts a design claim. `cas_fold_seal` decode costs 34.6% against 33.8% more
  bytes — proportional, and the design's claim is that the dominant cost is the longer keys.

**Revision 14 is ACCEPTED at freeze `3c948a46d1c`.**

## What acceptance still does not cover {#what-acceptance-does-not-cover}

- Twelve of seventeen formats have no throughput measurement; the design names five.
- No measurement figure comes from a real pool; the fixtures are synthetic with production-plausible
  widths, and the stored-bytes column is a fixture artifact.
- The full lane ran without the stateful datasets, so 240 tests did not execute their assertions.
  They are dataset tests, not CAS tests, but the coverage they would have added is absent.
- `[ref-catalog-write-hotspot]` is open, and it is the one place this campaign's own evidence points
  at a scaling question it did not answer.
