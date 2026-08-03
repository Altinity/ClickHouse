#!/usr/bin/env bash
# carve.sh — reconstruct the essential CAS branch fragment as an ordered commit
# series on a fresh branch from the merge-base with altinity/antalya-26.6.
#
# Usage: utils/cas-carve/carve.sh [--src <ref>] [--base <ref>] [--branch <name>] [--check] [--force]
#
# Design: docs/superpowers/specs/2026-08-03-cas-branch-reconstruction-script-design.md
#
# Maintenance when the WORKING branch grows: new files inside known directory
# globs are absorbed automatically; anything else fails the run with an
# unmatched-file list — add each file to the right group (or to EXCLUDES).
#
# Maintenance when the BASE branch advances: nothing here breaks, because the
# new branch starts at merge-base(BASE, SRC), not at BASE's tip. Procedure:
#   1. merge the advanced base into the working branch as usual;
#   2. rerun with --check: the merge-base moves forward by itself;
#   3. groups whose fixes got absorbed upstream become empty -> the empty-group
#      warning is the signal to delete them from the manifest.
set -euo pipefail

SRC=HEAD
BASE=altinity/antalya-26.6
BRANCH=cas-carved
CHECK=0
FORCE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --src) SRC=$2; shift 2 ;;
        --base) BASE=$2; shift 2 ;;
        --branch) BRANCH=$2; shift 2 ;;
        --check) CHECK=1; shift ;;
        --force) FORCE=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

cd "$(git rev-parse --show-toplevel)"
SRC_SHA=$(git rev-parse --verify "$SRC^{commit}")
MB=$(git merge-base "$BASE" "$SRC_SHA")
echo "src=$SRC_SHA"
echo "merge-base($BASE, src)=$MB"

# ----------------------------------------------------------------------------
# Data: what to discard.
# ----------------------------------------------------------------------------
EXCLUDES=(
    'docs/superpowers/*'
    '.superpowers/*'
    'utils/ca-soak/*'
    'utils/cas-gate/*'
    'utils/cas-carve/*'
    'tmp/*'
    '.gitignore'
    'tests/broken_tests.yaml'
)

# ----------------------------------------------------------------------------
# Data: ordered commit groups.
# group <name> <pattern>... <<'MSG'
#   <commit message>
# MSG
# Patterns are bash [[ == ]] globs against repo-relative paths (* crosses /).
# Optional prefix: "A:<glob>" matches only files added vs merge-base,
# "M:<glob>" only files modified vs merge-base.
# Classification is first-match-wins in declaration order.
# ----------------------------------------------------------------------------
GROUP_ORDER=()
declare -A GROUP_PATTERNS GROUP_MESSAGES
group()
{
    local name=$1; shift
    GROUP_ORDER+=("$name")
    GROUP_PATTERNS[$name]=$(printf '%s\n' "$@")
    GROUP_MESSAGES[$name]=$(cat)
}

# Placeholder manifest — replaced by the full one in the next task.
group upstream-b115 \
    'src/IO/ReadBufferFromFileView.cpp' \
    'src/IO/tests/gtest_read_buffer_from_file_view.cpp' <<'MSG'
Fix ReadBufferFromFileView position tracking after setReadUntilPosition (B115)
MSG

# ----------------------------------------------------------------------------
# Engine: classification.
# ----------------------------------------------------------------------------
DIFF_FILES=()
declare -A FILE_STATUS CLAIM
while IFS=$'\t' read -r st file; do
    st=${st:0:1}
    [[ $st == T ]] && st=M
    FILE_STATUS[$file]=$st
    DIFF_FILES+=("$file")
done < <(git diff --name-status --no-renames "$MB" "$SRC_SHA")

pattern_matches()  # <file> <status> <pattern-with-optional-prefix>
{
    local file=$1 st=$2 pat=$3 want=""
    case "$pat" in
        A:*|M:*) want=${pat%%:*}; pat=${pat#*:} ;;
    esac
    [[ -n $want && $want != "$st" ]] && return 1
    # shellcheck disable=SC2053
    [[ $file == $pat ]]
}

UNMATCHED=()
for file in "${DIFF_FILES[@]}"; do
    st=${FILE_STATUS[$file]}
    claimed=""
    for pat in "${EXCLUDES[@]}"; do
        if pattern_matches "$file" "$st" "$pat"; then claimed=__excluded__; break; fi
    done
    if [[ -z $claimed ]]; then
        for g in "${GROUP_ORDER[@]}"; do
            while IFS= read -r pat; do
                if pattern_matches "$file" "$st" "$pat"; then claimed=$g; break; fi
            done <<< "${GROUP_PATTERNS[$g]}"
            [[ -n $claimed ]] && break
        done
    fi
    if [[ -n $claimed ]]; then
        CLAIM[$file]=$claimed
    else
        UNMATCHED+=("$file")
    fi
done

# ----------------------------------------------------------------------------
# Engine: classification report.
# ----------------------------------------------------------------------------
echo
echo "== classification =="
total=0
for g in "${GROUP_ORDER[@]}"; do
    n=0
    for file in "${DIFF_FILES[@]}"; do
        [[ ${CLAIM[$file]:-} == "$g" ]] && n=$((n + 1))
    done
    total=$((total + n))
    printf '%6d  %s\n' "$n" "$g"
    [[ $n -eq 0 ]] && echo "WARNING: group '$g' matches no files (manifest rot? fix absorbed by base?)" >&2
done
n_excluded=0
for file in "${DIFF_FILES[@]}"; do
    [[ ${CLAIM[$file]:-} == __excluded__ ]] && n_excluded=$((n_excluded + 1))
done
printf '%6d  (excluded junk)\n' "$n_excluded"
printf '%6d  total diff files\n' "${#DIFF_FILES[@]}"

if [[ ${#UNMATCHED[@]} -gt 0 ]]; then
    echo
    echo "ERROR: ${#UNMATCHED[@]} files match neither EXCLUDES nor any group:" >&2
    printf '  %s\n' "${UNMATCHED[@]}" >&2
    exit 1
fi

if [[ $CHECK -eq 1 ]]; then
    echo
    echo "--check OK: every diff file is classified."
    exit 0
fi

echo "apply phase not implemented yet" >&2
exit 3
