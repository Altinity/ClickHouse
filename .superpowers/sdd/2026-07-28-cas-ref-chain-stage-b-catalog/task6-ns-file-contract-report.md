# Task 6 namespace-file contract report

## Outcome

Added `src/Disks/tests/gtest_cas_ns_file_read_contract.cpp` with two real disk-path contract tests:

- `StaleReaderAfterSameNameRebirthNeverSeesSuccessorBytes` keeps the original storage/runtime alive across exact catalog removal and same-name rebirth. The successor object is positively shown to exist through `nativeKeyUnder` and to contain distinct bytes, while the stale disk read may return only predecessor bytes or absence.
- `DelayedInlineFinalizeCannotChangeSuccessorTokenOrBytes` opens a real `CaInlineWriteBuffer` under life 1, makes life 2 live with the same relative file name, and finalizes afterward. Life 2's backend token and bytes must remain unchanged; a successful finalize must land under life 1, while the existing typed stale-write exception is also accepted.

The fixture uses the production catalog lifecycle: life 1 is moved to `Removing`, deleted through `CasRefCatalog::deleteCompletedRemoving` with cleanup evidence and a held fence, and life 2 is admitted as `Live`. No production seam or CMake change was needed; the existing recursive `gtest*.cpp` source glob discovers the new translation unit.

## Controlled RED proof

Two temporary, uncommitted mutations modeled the exact forbidden behavior:

1. `ContentAddressedMetadataStorage::readableNamespaceFilesLife` re-resolved the namespace from the current catalog on every read.
2. The table-file `CaInlineWriteBuffer` callback captured the namespace name and called `CasRefCatalog::resolveLifeOrSentinel` during finalization instead of retaining its open-time `NamespaceLifeId`.

ASan build log: `build_asan/build_task6_ns_file_contract_red.log`.

Focused RED log: `build_asan/test_task6_ns_file_contract_red.log`.

Result: both tests failed non-vacuously. The stale reader returned `life-2\n`; delayed finalization changed life 2's token and replaced `life-2-stable\n` with `life-1-delayed\n`.

The mutations were then removed explicitly. The restored production files matched their recorded original object hashes:

- `ContentAddressedMetadataStorage.cpp`: `73550b6b20f6399603f7b869d6e5ea8a814f1aed`
- `ContentAddressedTransaction.cpp`: `a98d81dd65341e40177e3872cba5764bd677a76b`

Their scoped `git diff --quiet` check returned success before the GREEN build.

## GREEN verification

ASan build:

```text
CCACHE_TEMPDIR=/home/mfilimonov/.ccache/tmp ninja unit_tests_dbms
```

Final log: `build_asan/build_task6_ns_file_contract_final.log`; exit status 0 with no compiler or linker errors.

Focused ASan test:

```text
./src/unit_tests_dbms --gtest_filter='CasNamespaceFileReadContract.*'
```

Final log: `build_asan/test_task6_ns_file_contract_final.log`; both tests passed, with no AddressSanitizer, LeakSanitizer, or runtime-error diagnostic.

The existing request-profile suite was intentionally not duplicated; `CasNamespaceFileDiskProfile.SteadyStateFileOperationsTouchNoCatalogRefBlobOrManifestKey` already covers the steady namespace-file request budget at the disk boundary.
