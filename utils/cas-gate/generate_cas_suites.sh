#!/bin/bash
# Regenerates <build_dir>/cas_suites.txt for the CAS gate, and VERIFIES the invariant that makes the
# gate's coverage self-evident: every suite defined in a CAS test source is reachable by the plain name
# filter `CAS*`, in the spelling the binary exposes.
#
# The invariant exists because a curated suite list cannot be distinguished from a covering one by
# reading it -- a suite missing from the list looks exactly like a suite that has no tests, and a real
# defect once stayed red for over a day behind that ambiguity. With the invariant enforced here, the
# emitted list is a convenience for the per-suite runner, not the coverage mechanism: `CAS*` is.
#
# Three things are checked, and each one fails loud:
#   1. a non-excluded CAS suite whose name does not start with `CAS` -- rename it;
#   2. a parameterized suite whose `<Inst>/<Suite>` spelling does not start with `CAS`, i.e. a
#      non-`CAS` INSTANTIATE_TEST_SUITE_P prefix, which `CAS*` does not match even when the suite
#      itself is `CAS`-prefixed;
#   3. a CAS test file outside the CAS source glob, which means the glob no longer finds every CAS
#      test file and this script's own enumeration has a hole. CAS-ness is decided by what the file
#      includes, not by its suite names, because a file that is both misnamed and has non-`CAS` suites
#      is invisible to every name-based check -- that combination is how a suite once escaped the gate
#      entirely.
set -euo pipefail

# Resolve BUILD_DIR to an absolute path BEFORE the `cd` below -- a relative path (the default, and what
# the wrapper script passes) would otherwise be interpreted relative to the NEW cwd, not the caller's.
BUILD_DIR="${1:-../../build}"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"

SRC_ROOT="$(cd "$(dirname "$0")/../../src" && pwd)"
cd "$SRC_ROOT/Disks/tests"

BIN="$BUILD_DIR/src/unit_tests_dbms"
OUT="$BUILD_DIR/cas_suites.txt"

# CAS test files all live here under one name shape; check 3 below is what keeps that true.
CAS_SOURCE_GLOB='gtest_ca*.cpp'

if [ ! -x "$BIN" ]; then
    echo "error: $BIN not found or not executable -- build unit_tests_dbms first" >&2
    exit 1
fi

# Explicit exclude list: suites defined in a CAS-named source file that are generic backend/infra
# tests, not CAS ref/incarnation/catalog logic. Every entry needs a reason; an entry with no reason is
# not a valid exclusion. An excluded name must also not start with `CAS`, or `CAS*` would run it and
# the exclusion would be a fiction -- checked below.
declare -A EXCLUDE_REASONS=(
    [CountingBackendShape]="generic backend-wrapper shape test (gtest_cas_backend.cpp), no ref/incarnation logic"
    [MemoryWriteBuffer]="generic write-buffer infra test (gtest_cascade_and_memory_write_buffer.cpp), unrelated to CAS"
    [TestCascadeWriteBufferWithDisk]="generic cascade write-buffer infra test (gtest_cascade_and_memory_write_buffer.cpp), unrelated to CAS"
    [CascadeWriteBuffer]="generic cascade write-buffer infra test (gtest_cascade_and_memory_write_buffer.cpp), unrelated to CAS -- its leading 'Cas' is accidental and no longer matches CAS*"
)

for s in "${!EXCLUDE_REASONS[@]}"; do
    if [ -z "${EXCLUDE_REASONS[$s]}" ]; then
        echo "error: exclude entry '$s' has an empty reason, which is not a valid exclusion" >&2
        exit 1
    fi
    if [[ "$s" == CAS* ]]; then
        echo "error: excluded suite '$s' starts with 'CAS', so the plain 'CAS*' filter runs it anyway." >&2
        echo "Either drop the exclusion or rename the suite so its name does not claim to be CAS." >&2
        exit 1
    fi
done

# Every suite name that appears in a TEST(...)/TEST_F(...)/TEST_P(...) macro in any CAS test source
# file -- the SOURCE OF TRUTH for what must be accounted for.
mapfile -t source_suites < <(grep -ohP '^TEST(_F|_P)?\(\s*\w+' $CAS_SOURCE_GLOB 2>/dev/null \
    | sed -E 's/^TEST(_F|_P)?\(\s*//' | sort -u)

if [ "${#source_suites[@]}" -eq 0 ]; then
    echo "error: no suites found in $CAS_SOURCE_GLOB under $PWD -- the source glob matches nothing" >&2
    exit 1
fi

# Check 3: a CAS test file the glob does not reach is never verified at all, so the hole has to be
# reported from the file side. Including a CAS header is the signal that survives both a misnamed file
# and non-`CAS` suite names; a `CAS`-prefixed suite is taken as CAS-ness too, for a test that reaches
# CAS code without including its headers directly.
CAS_INCLUDE_RE='#include.*(MetadataStorages/ContentAddressed/|cas_test_helpers\.h)'
stray_files=()
while IFS= read -r f; do
    if grep -qE "$CAS_INCLUDE_RE" "$f" || grep -qE '^TEST(_F|_P)?\(\s*CAS' "$f"; then
        stray_files+=("$f")
    fi
done < <(find "$SRC_ROOT" -name 'gtest_*.cpp' -not -path "$SRC_ROOT/Disks/tests/gtest_ca*")

if [ "${#stray_files[@]}" -gt 0 ]; then
    echo "error: ${#stray_files[@]} CAS test file(s) are not matched by $CAS_SOURCE_GLOB in src/Disks/tests:" >&2
    printf '  %s\n' "${stray_files[@]}" >&2
    echo "Rename each to gtest_ca*.cpp under src/Disks/tests so this script's enumeration sees it." >&2
    exit 1
fi

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
    CASBlobDigestDeathTest CASBlobUploadPoolDeathTest CASFoldSealFormatDeathTest
    CASFormatTraitsDeathTest CASGCHoldGrammarDeathTest CASGCStateFormatDeathTest
    CASNamespaceLifeIdDeathTest CASNsCreationLifecycleDeathTest CASPartFolderAccessDeathTest
    CASPromoteRepublishDeathTest CASRefCatalogDeathTest CASRefCatalogFormatDeathTest
    CASRefCatalogRemovalDeathTest CASRefInstallSafetyDeathTest CASRequestControllerCreateDeathTest
    CASUploadDetachedDeathTest CASUploadFanoutDeathTest CASWiringExchangeDeathTest
    CASWiringOpsDeathTest
)
declare -A known_guarded
for s in "${KNOWN_COMPILE_GUARDED[@]}"; do known_guarded["$s"]=1; done

included=()
unclaimed=()
unprefixed=()
for s in "${source_suites[@]}"; do
    if [ -n "${EXCLUDE_REASONS[$s]-}" ]; then
        continue   # explicitly excluded, with a non-empty reason
    fi
    # Check 1: the suite's own name. Applied before the binary lookup so a compile-guarded suite that
    # this build does not contain is still held to the invariant.
    if [[ "$s" != CAS* ]]; then
        unprefixed+=("$s (suite name does not start with 'CAS')")
        continue
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
        # Check 2: `CAS*` matches the whole exposed spelling, so the instantiation prefix decides.
        for b in "${param_spellings[@]}"; do
            if [[ "$b" == CAS* ]]; then
                included+=("$b")
            else
                unprefixed+=("$b (INSTANTIATE_TEST_SUITE_P prefix does not start with 'CAS')")
            fi
        done
        continue
    fi
    if [ -n "${known_guarded[$s]+x}" ]; then
        continue   # known compile-time-guarded death-test suite, absent from this build type
    fi
    # Named in source but not in the built binary: could be a stale build, dead code behind an
    # #ifdef, or a new compile-guarded suite not yet in the explicit list. Either way it needs a
    # human decision, not a silent drop.
    unclaimed+=("$s (in source, NOT in built binary -- stale build? dead #ifdef? new guarded suite?)")
done

if [ "${#unprefixed[@]}" -gt 0 ]; then
    echo "error: ${#unprefixed[@]} CAS suite spelling(s) are not reachable by the plain 'CAS*' filter:" >&2
    printf '  %s\n' "${unprefixed[@]}" >&2
    echo "Rename each so the spelling the binary exposes starts with 'CAS' -- that filter is the gate." >&2
    exit 1
fi

if [ "${#unclaimed[@]}" -gt 0 ]; then
    echo "error: ${#unclaimed[@]} suite(s) found in CAS test sources are unclaimed (neither included nor excluded-with-reason):" >&2
    printf '  %s\n' "${unclaimed[@]}" >&2
    echo "Add each to the CAS set (it will be next time this script runs) or to EXCLUDE_REASONS with a reason." >&2
    exit 1
fi

printf '%s\n' "${included[@]}" | sort -u > "$OUT"

# The payoff, asserted rather than assumed: what `CAS*` selects in this binary must be exactly what
# was emitted. A difference means a CAS suite is being gated but not covered by the simple filter, or
# a non-CAS suite has taken a `CAS` name.
mapfile -t cas_filtered < <(printf '%s\n' "${binary_suites[@]}" | grep '^CAS' | sort -u)
if ! diff -q <(printf '%s\n' "${cas_filtered[@]}") "$OUT" > /dev/null; then
    echo "error: the emitted suite list and the plain 'CAS*' filter disagree in this binary:" >&2
    diff <(printf '%s\n' "${cas_filtered[@]}") "$OUT" | sed 's/^/  /' >&2
    echo "'<' is selected by CAS* only; '>' is emitted only. The gate and the filter must be the same set." >&2
    exit 1
fi

echo "wrote $(wc -l < "$OUT") suites to $OUT (equals the 'CAS*' filter set; ${#EXCLUDE_REASONS[@]} excluded, 0 unclaimed)"
