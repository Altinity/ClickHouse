#!/bin/bash
# Regenerates <build_dir>/cas_suites.txt for the CAS gate, and FAILS LOUD if any suite defined in a
# CAS test source file is not accounted for -- either included in the CAS set or named in the exclude
# list below with a reason. This replaces the old `Cas*:CA*` name-prefix filter, which silently missed
# every suite whose name doesn't start with those two prefixes (RefWriter*, RefTableCacheEviction,
# CaWiring, CaLifecycle, ContentAddressedLog -- found 2026-07-30, the THIRD recurrence of this exact gap;
# the first two "fixes" only added the missing suites back, which is why it kept coming back once the
# list was next regenerated). A silently-omitting list is indistinguishable from a covering one, so the
# fix here is the cross-check, not the list.
set -euo pipefail

# Resolve BUILD_DIR to an absolute path BEFORE the `cd` below -- a relative path (the default, and what
# the wrapper script passes) would otherwise be interpreted relative to the NEW cwd, not the caller's.
BUILD_DIR="${1:-../../build}"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"

cd "$(dirname "$0")/../../src/Disks/tests"

BIN="$BUILD_DIR/src/unit_tests_dbms"
OUT="$BUILD_DIR/cas_suites.txt"

if [ ! -x "$BIN" ]; then
    echo "error: $BIN not found or not executable -- build unit_tests_dbms first" >&2
    exit 1
fi

# Explicit exclude list: suites defined in a `gtest_ca*.cpp`/`gtest_cas*.cpp` file that are generic
# backend/infra tests, not CAS ref/incarnation/catalog logic. Every entry needs a reason; an entry with
# no reason is not a valid exclusion.
declare -A EXCLUDE_REASONS=(
    [CountingBackendShape]="generic backend-wrapper shape test (gtest_cas_backend.cpp), no ref/incarnation logic"
    [MemoryWriteBuffer]="generic write-buffer infra test (gtest_cascade_and_memory_write_buffer.cpp), unrelated to CAS"
    [TestCascadeWriteBufferWithDisk]="generic cascade write-buffer infra test (gtest_cascade_and_memory_write_buffer.cpp), unrelated to CAS"
)

# Every suite name that appears in a TEST(...)/TEST_F(...)/TEST_P(...) macro in any CAS test source
# file -- the SOURCE OF TRUTH for what must be accounted for, independent of naming convention.
# TEST_P suites appear in the binary only under their instantiation prefixes (`<Inst>/<Suite>`), which
# the matching below resolves.
mapfile -t source_suites < <(grep -ohP '^TEST(_F|_P)?\(\s*\w+' gtest_ca*.cpp gtest_cas*.cpp 2>/dev/null \
    | sed -E 's/^TEST(_F|_P)?\(\s*//' | sort -u)

# Every suite name the BUILT BINARY actually contains (catches source-vs-binary drift: a stale build,
# or a suite whose only test is behind a compile-time #ifdef).
mapfile -t binary_suites < <("$BIN" --gtest_list_tests 2>/dev/null \
    | grep -v '^  ' | sed 's/\.$//' | sort -u)

declare -A in_binary
for s in "${binary_suites[@]}"; do in_binary["$s"]=1; done

# Suites compile-time guarded by the LOGICAL_ERROR death-test split: they exist only in
# debug/sanitizer builds, so absence from a release binary is the EXPECTED state, not drift. This is
# an EXPLICIT list on purpose -- a name-pattern skip would silently drop the next new suite; a new
# guarded suite must be added here deliberately, with the build that contains it verified once.
KNOWN_COMPILE_GUARDED=(
    CasBlobDigestDeathTest CasBlobUploadPoolDeathTest CasFoldSealFormatDeathTest
    CasFormatTraitsDeathTest CasGcHoldGrammarDeathTest CasGcStateFormatDeathTest
    CasNamespaceLifeIdDeathTest CasNsCreationLifecycleDeathTest CasPartFolderAccessDeathTest
    CasPromoteRepublishDeathTest CasRefCatalogDeathTest CasRefCatalogFormatDeathTest
    CasRefCatalogRemovalDeathTest CasRefInstallSafetyDeathTest CasRequestControllerCreateDeathTest
    CasUploadDetachedDeathTest CasUploadFanoutDeathTest CasWiringExchangeDeathTest
    CasWiringOpsDeathTest
)
declare -A known_guarded
for s in "${KNOWN_COMPILE_GUARDED[@]}"; do known_guarded["$s"]=1; done

included=()
unclaimed=()
for s in "${source_suites[@]}"; do
    if [ -n "${EXCLUDE_REASONS[$s]-}" ]; then
        continue   # explicitly excluded, with a non-empty reason
    fi
    if [ -n "${in_binary[$s]+x}" ]; then
        included+=("$s")
        continue
    fi
    # TEST_P suites live in the binary only under instantiation prefixes: include every `<Inst>/<s>`
    # spelling, so the emitted filter names what is actually runnable.
    param_spellings=()
    for b in "${binary_suites[@]}"; do
        if [[ "$b" == */"$s" ]]; then
            param_spellings+=("$b")
        fi
    done
    if [ "${#param_spellings[@]}" -gt 0 ]; then
        included+=("${param_spellings[@]}")
        continue
    fi
    if [ -n "${known_guarded[$s]+x}" ]; then
        continue   # known compile-time-guarded death-test suite, absent from this build type
    fi
    # Named in source but not in the built binary: could be a stale build, dead code behind an
    # #ifdef, or a new compile-guarded suite not yet in the explicit list. Either way it needs a
    # human decision, not a silent drop.
    unclaimed+=("$s (in source, NOT in built binary -- stale build? dead #ifdef? new guarded suite?)")
    continue
done

if [ "${#unclaimed[@]}" -gt 0 ]; then
    echo "error: ${#unclaimed[@]} suite(s) found in CAS test sources are unclaimed (neither included nor excluded-with-reason):" >&2
    printf '  %s\n' "${unclaimed[@]}" >&2
    echo "Add each to the CAS set (it will be next time this script runs) or to EXCLUDE_REASONS with a reason." >&2
    exit 1
fi

printf '%s\n' "${included[@]}" | sort -u > "$OUT"
echo "wrote $(wc -l < "$OUT") suites to $OUT ($(( ${#source_suites[@]} - ${#included[@]} )) excluded, 0 unclaimed)"
