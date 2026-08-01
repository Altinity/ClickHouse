# Task 5 Step 8: perpetual namespace janitor

Implemented one bounded, leak-only `LIST(cas/ns/)` page per folding round in the existing
`namespace_cleanup` phase. `NamespaceJanitor` owns the page protocol: durable cursor read, bounded
physical listing, one immutable catalog cut after the page, fixed-family physical classification,
exact-token deletion under a fresh GC lease check, and one-shot cursor publication.

Current `Creating`, `Live`, and `Removing` life identifiers are retained through the catalog reverse
index. An absent identifier is a deletion candidate. Duplicate current identifiers suppress the whole
page; malformed keys are reported and skipped. Suppression and fence loss delete nothing. `_files` and
`_ckpt` debris are reclaimed without reconstructing a namespace name or consulting `_path`.

Corrupt or oversized progress performs no list or delete and is reset only against its exact token.
A backend-rejected cursor is likewise reset exactly before the error is surfaced to the outer
leak-only phase. Cursor CAS conflicts are ignored: they affect future maintenance progress only.
End-of-tree publishes the canonical empty cursor, so LIST omissions defer work to a later cycle rather
than producing permanent lifecycle coupling.

The phase publishes `janitor_pages`, `janitor_keys`, and `janitor_deleted`. No janitor state is stored
in `gc/state` or a fold seal, and there is no lifecycle-specific cleanup attempt.
The integration test drives a real authoritative `Gc::runRegularRound` with a non-vacuous proven
frontier and verifies that the `namespace_cleanup` phase deletes an absent-life `_ckpt` object and
reports the janitor metrics.

## Evidence

- Compile RED: `build_asan/build_step8_compile_red.log` failed on the absent janitor API.
- Batched mutation RED: `build_asan/test_step8_batched_red.log` failed 5/7 tests after breaking
  post-LIST catalog order, reversing absent/current classification, and discarding cursor progress.
- Final focused ASan GREEN: `build_asan/build_step8_final_green3.log` and
  `build_asan/test_step8_final_green3.log` (19/19 tests passed, including the real-GC integration).
