# Task 2 prose-sweep proof

Branch: `cas-gc-rebuild`

## 1. Tree-wide provenance sweep

Files: 19 `src/Disks` source/test files containing non-historical matches.

Old → new: task/cutover history such as “before this task” and “this task adds” → the current invariant or reason, such as successor-only ref transactions, durable hold evidence, and a refused pre-attempt leaving no wedge.

Verification: `rg -n 'this task|this same cut|pre-cut' src/Disks/` now returns only the `CASWireCutDeltas` old-side baselines and the marked old-wire rejection fixture. Each retained `pre-cut` occurrence labels bytes used as the deliberate historical comparison side; none describes a change as its rationale. The edited comments were checked against their adjacent assertions and the called code before rewording.

Fully discharged Inbox bullet: the tree-wide comment sweep. The scan no longer has provenance matches outside the allowed historical fixtures.

## 2. Internal-reference comments

Files: `gtest_cas_ref_catalog.cpp`, `CasRefCatalogFormat.h`, `gtest_cas_gc_round.cpp`, `gtest_cas_gc_round_defer.cpp`, `cas_test_helpers.h`, `gtest_cas_encoding_pins.cpp`, `gtest_cas_ref_epoch_seal_format.cpp`, `gtest_cas_part_manifest_format.cpp`, `CasRefLogFormat.cpp`, `gtest_cas_ref_log_format.cpp`, `gtest_cas_ref_snapshot_format.cpp`, `CasPoolMetaFormat.h`, `Pool/CasPoolMeta.cpp`, and `utils/ca-soak/scenarios/cards/s28_s33_corner.py`.

Old → new: test/file/review/spec/plan citations and `Retired-in-snapshot (T4)` labels → direct statements of the encoded grammar, raw-storage rule, zero-I/O adopted-seal summary, and same-instant dry-run contract.

Verification: the catalog codec and its registry assertions establish raw `CompressionPolicy::Never` storage; the GC helpers/tests read condemned rows and summaries from the adopted fold seal; the pool-meta calls use `allow_mint` only on the verified bootstrap path; and the soak card compares the preview with the same-instant condemned set. Searches for the listed citations return no matching edited citation.

Fully discharged Inbox bullets: the part-manifest provenance, ref-codec provenance, pool-meta/soak citation provenance, and epoch-seal citation provenance. Each cited comment now retains its code-derived reason without an internal reference.

## 3. Opaque namespace-life vocabulary

Files: `gtest_cas_namespace_life_id.cpp`.

Old → new: “Generation-5/6 namespace-bearing keys” → keys inside or outside the opaque-life layout.

Verification: the tested parser entry points accept only keys under the life-scoped layout, while malformed life identifiers are rejected as corruption and report the offending key. The new wording describes that stable layout rule rather than parser-generation history.

Fully discharged Inbox bullet: stale-generation narrative; both identified comments were reworded.

## Gates

- `ninja -C build unit_tests_dbms > build/build_p3_prose.log 2>&1`: passed; `unit_tests_dbms` linked, with no warnings or errors in the log.
- `build/src/unit_tests_dbms --gtest_filter='CAS*' > build/test_cas_p3_prose.log 2>&1`: passed; 2249 tests ran, 2249 passed, and 0 failed.
