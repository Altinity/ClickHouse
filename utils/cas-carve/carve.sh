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
    '.claude/*'
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

# ============================ Phase 1: upstream fixes ========================
group upstream-b115-fileview \
    'src/IO/ReadBufferFromFileView.cpp' \
    'src/IO/ReadBufferFromMemory.cpp' \
    'src/IO/tests/gtest_read_buffer_from_file_view.cpp' \
    'src/IO/tests/gtest_read_buffer_from_memory.cpp' <<'MSG'
Fix ReadBufferFromFileView position tracking (B115)

ReadBufferFromFileView assumed the inner buffer keeps its working buffer
across setReadUntilPosition; ReadBufferFromS3 resets it, so getPosition
lied and seek logic could re-read a stale decompressed block. Recompute
the offset from the inner buffer after every right-bound change.
Includes the ReadBufferFromMemory counterpart and gtest batteries.
MSG

group upstream-b117-s3-read-cancel \
    'src/IO/ReadBufferFromS3.cpp' <<'MSG'
Stop retrying S3 reads after query cancellation (B117)

processException kept retrying transient errors after KILL QUERY; check
CurrentThread::get().isQueryCanceled() in the outer retry loop.
MSG

group upstream-b90-threadgroup-lifetime \
    'src/Interpreters/ThreadStatusExt.cpp' \
    'src/Common/ThreadStatus.h' <<'MSG'
Retain parent ThreadGroup from borrowed children (B90)

A borrowed child ThreadGroup parents its trackers at the parent group via
raw pointers; background work outliving the query produced a use-after-free
in parent counters. The child now holds a shared_ptr to the parent group.
MSG

group upstream-b37-dedup-log \
    'src/Storages/MergeTree/MergeTreeDeduplicationLog.cpp' \
    'src/Storages/MergeTree/tests/gtest_deduplication_log_null_writer.cpp' <<'MSG'
Fail closed on a null MergeTreeDeduplicationLog writer (B37)

addPart/dropPart guarded current_writer only with chassert (a release
no-op), so a missing writer meant a null dereference; throw LOGICAL_ERROR
instead. Also treats a missing logs_dir as normal for storages that do
not materialize empty directories (carries a CAS-related hunk; wired later).
MSG

group upstream-s3-conditional-core \
    'src/IO/S3/Client.cpp' 'src/IO/S3/Client.h' \
    'src/IO/S3Common.cpp' 'src/IO/S3Common.h' 'src/IO/S3Defines.h' \
    'src/IO/S3AuthSettings.cpp' 'src/IO/S3/Requests.h' \
    'src/IO/S3/copyS3File.cpp' 'src/IO/S3/copyS3File.h' \
    'src/IO/WriteBufferFromS3.cpp' 'src/IO/WriteBufferFromS3.h' \
    'src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp' \
    'src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h' \
    'src/Disks/DiskObjectStorage/ObjectStorages/S3/diskSettings.cpp' \
    'src/IO/S3/tests/gtest_aws_s3_client.cpp' \
    'src/IO/tests/gtest_writebuffer_s3.cpp' <<'MSG'
S3 conditional-write support and 412 no-retry policy

HTTP 412 on a conditional request is deterministic: never retry it
(S3Exception::isPreconditionFailed, RetryStrategy). Adds conditional
PUT/COPY (If-Match / If-None-Match) through the client, WriteBufferFromS3
and copyS3File, and the token-conditional operations on S3ObjectStorage.
Also carries the copyS3File message_format_string fix (PreformattedMessage
instead of a preformatted string) and CAS-facing write-settings plumbing
(wired later).
MSG

group upstream-expect-100-continue \
    'src/IO/S3/PocoHTTPClient.cpp' 'src/IO/S3/PocoHTTPClient.h' \
    'src/IO/S3/PocoHTTPClientFactory.cpp' <<'MSG'
Use Expect: 100-continue for large conditional S3 uploads

A conditional PUT that is doomed to 412 should not stream its whole body;
send Expect: 100-continue above a size threshold and peek the response.
Prevents mid-upload connection resets and retry storms on S3-compatible
stores. Carries the GCS dialect integration points (wired later).
MSG

group upstream-gcs-dialect \
    'src/IO/S3/GOOG4Signer.cpp' 'src/IO/S3/GOOG4Signer.h' \
    'src/IO/S3/GCSConditionalDialect.cpp' 'src/IO/S3/GCSConditionalDialect.h' \
    'src/IO/S3/tests/gtest_goog4_signer.cpp' \
    'src/IO/S3/tests/gtest_gcs_conditional_dialect.cpp' <<'MSG'
GCS conditional-write dialect and GOOG4 signer

GCS XML API needs native GOOG4 signing and x-goog-if-generation-match for
generation-safe conditional writes; AWS SigV4 If-Match semantics are not
enough, and conditional multipart complete is silently ignored. Adds the
signer, the header dialect, and fixed-vector tests.
MSG

group upstream-local-object-storage \
    'src/Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.cpp' <<'MSG'
LocalObjectStorage: snapshot listing semantics and hardening

Emulate object-store behavior under concurrent removal: a file vanishing
between listing and stat is skipped, not a filesystem_error; non-recursive
explicit-stack walk with error_code overloads and a symlink guard; fail
closed on an embedded-NUL path (AST fuzzer).
MSG

group upstream-disk-txn-contract \
    'src/Disks/IDiskTransaction.h' \
    'src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp' \
    'src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.h' \
    'src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp' \
    'src/Storages/MergeTree/DataPartStorageOnDiskBase.h' \
    'src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp' \
    'src/Storages/MergeTree/IDataPartStorage.h' \
    'src/Storages/MergeTree/IMergeTreeDataPart.cpp' \
    'src/Storages/MergeTree/MergeTask.cpp' 'src/Storages/MergeTree/MergeTask.h' \
    'src/Storages/MergeTree/MutateTask.cpp' \
    'src/Storages/MergeTree/MergeProjectionPartsTask.cpp' \
    'src/Storages/MergeTree/MergeTreeDataWriter.cpp' \
    'src/Storages/MergeTree/MergeTreeData.cpp' 'src/Storages/MergeTree/MergeTreeData.h' \
    'src/Storages/MergeTree/tests/gtest_projection_borrowed_transaction.cpp' <<'MSG'
Disk-transaction contract: one logical part = one transaction

Projection sub-parts ride the parent whole-part transaction instead of
committing early; read-your-writes (in-flight resolve) becomes part of the
IDiskTransaction contract so staged state is visible before commit;
clone/freeze/restore wrap the whole part in one transaction; staged
operation order is explicit. Mixed-file note: these files also carry the
content-addressed capability surface and eager-dispatch branches that are
wired by the later CAS integration commits.
MSG

group upstream-system-proxy-unwrap \
    'src/Storages/StorageProxy.h' \
    'src/Storages/StorageTableProxy.h' <<'MSG'
Unwrap table proxies in single-table SYSTEM commands

Single-table SYSTEM commands (SYNC/RESTORE/RESTART/DROP REPLICA, WAIT
LOADING PARTS, PREWARM, ...) cast the storage directly and missed tables
behind a proxy; unwrap the proxy before the cast.
MSG

# ==================== Phase 2: CAS subsystem, bottom-up ======================
CA=src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed

group cas-primitives "$CA/Primitives/*" <<'MSG'
CAS subsystem: Primitives layer

Core value types of the content-addressed storage subsystem: identifiers,
hashes, tokens, namespaces. No dependencies on other CAS layers.
MSG

group cas-formats "$CA/Formats/*" <<'MSG'
CAS subsystem: Formats layer

On-wire/on-disk encodings: manifests, ref-log records, GC state, seals,
codecs. Depends only on Primitives.
MSG

group cas-backend "$CA/Backend/*" <<'MSG'
CAS subsystem: Backend layer

Object-storage access layer: the backend interface, the object-storage
adapter, the in-memory backend for tests, capability probing and request
control.
MSG

group cas-pool "$CA/Pool/*" <<'MSG'
CAS subsystem: Pool layer

Pool identity and runtime: server root, mount lifecycle, pool metadata,
the ref ledger and ref protocol (publish/confirm, recovery, snapshots),
the part-write transaction (dedup gate, conditional create, promote),
staging and plain objects.
MSG

group cas-parts "$CA/Parts/*" <<'MSG'
CAS subsystem: Parts layer

Part-path parsing and the part-folder access facade over manifests.
MSG

group cas-gc "$CA/Gc/*" <<'MSG'
CAS subsystem: GC layer

The garbage-collection round: fold, in-degree settlement, lease and
heartbeat, prune, baseline rebuild, ack-floor fencing.
MSG

group cas-tools "$CA/Tools/*" <<'MSG'
CAS subsystem: Tools layer

fsck (integrity checking), inspect, and pool-member decommission.
MSG

group cas-benchmarks "$CA/benchmarks/*" <<'MSG'
CAS subsystem: microbenchmarks
MSG

# ==================== Phase 3: integration and wiring ========================
group cas-wiring-disks \
    "$CA/*" \
    'src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp' \
    'src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp' \
    'src/Disks/DiskObjectStorage/DiskObjectStorage.cpp' \
    'src/Disks/DiskObjectStorage/DiskObjectStorage.h' \
    'src/Disks/DiskObjectStorage/DiskObjectStorageCache.cpp' \
    'src/Disks/DiskObjectStorage/MetadataStorages/Cache/MetadataStorageFromCacheObjectStorage.cpp' \
    'src/Disks/DiskObjectStorage/MetadataStorages/Cache/MetadataStorageFromCacheObjectStorage.h' \
    'src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h' \
    'src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h' \
    'src/Disks/DiskType.cpp' 'src/Disks/DiskType.h' \
    'src/Disks/IDisk.h' \
    'src/Disks/ReadOnlyDiskWrapper.h' \
    'src/IO/ReadPipeline.cpp' 'src/IO/ReadPipeline.h' \
    'src/IO/WriteBufferFromFileBase.h' \
    'src/IO/WriteBufferFromFileDecorator.h' \
    'src/IO/WriteSettings.h' \
    'src/Storages/StorageMergeTree.cpp' \
    'src/Storages/StorageReplicatedMergeTree.cpp' \
    'src/Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp' <<'MSG'
CAS integration: disk surface and registration

The content-addressed metadata storage, its disk transaction and part
staging (the top-level subsystem glue), registration of the
content_addressed metadata storage type, capability predicates on
IDisk/DiskObjectStorage/IMetadataStorage, the conditional-object-storage
API on IObjectStorage, the FileView read-pipeline stage, write-ETag
surfacing, cache-over-CA, and the atomic-file-write short-circuit for
txn_version.txt.
MSG

group cas-wiring-system-logs \
    'src/Interpreters/ContentAddressedLog.cpp' 'src/Interpreters/ContentAddressedLog.h' \
    'src/Interpreters/ContentAddressedGarbageCollectionLog.cpp' \
    'src/Interpreters/ContentAddressedGarbageCollectionLog.h' \
    'src/Interpreters/SystemLog.cpp' 'src/Interpreters/SystemLog.h' \
    'src/Common/SystemLogBase.cpp' 'src/Common/SystemLogBase.h' \
    'src/Interpreters/Context.cpp' 'src/Interpreters/Context.h' \
    'src/Interpreters/ServerAsynchronousMetrics.cpp' \
    'src/Storages/System/*' <<'MSG'
CAS integration: system logs and introspection

system.content_addressed_log and system.content_addressed_garbage_collection_log
(definitions, SystemLog registration, Context getters), the
system.content_addressed_mounts table, and asynchronous metrics.
MSG

group cas-wiring-system-commands \
    'src/Parsers/ASTSystemQuery.cpp' 'src/Parsers/ASTSystemQuery.h' \
    'src/Parsers/ParserSystemQuery.cpp' \
    'src/Parsers/tests/gtest_Parser.cpp' \
    'src/Interpreters/InterpreterSystemQuery.cpp' \
    'src/Interpreters/InterpreterSystemQuery.h' <<'MSG'
CAS integration: SYSTEM commands

SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION / GC REBUILD / DROP POOL
MEMBER: grammar, AST, interpreter handlers, access checks, and the parser
round-trip test. Includes the magic_enum range widening required once
ASTSystemQuery::Type outgrew the default range.
MSG

group cas-wiring-registries-and-entrypoints \
    'src/Common/ProfileEvents.cpp' \
    'src/Common/CurrentMetrics.cpp' \
    'src/Access/Common/AccessType.h' \
    'src/Common/FailPoint.cpp' \
    'src/Core/ServerSettings.cpp' \
    'src/Common/setThreadName.h' \
    'src/CMakeLists.txt' \
    'contrib/*' \
    'src/Storages/MergeTree/DataPartsExchange.cpp' \
    'src/Storages/MergeTree/DataPartsExchange.h' \
    'programs/*' <<'MSG'
CAS integration: registries, fetch-by-relink, entry points

ProfileEvents/CurrentMetrics/AccessType/FailPoint/ServerSettings entries,
build wiring, the fetch-by-relink replication protocol extension in
DataPartsExchange (same-pool fetch publishes a local ref over shared
blobs; gated by pool_uuid, non-CA fetches unchanged), the clickhouse-disks
CA commands (inspect, fsck, gc-dryrun, gc-rebuild, drop-member), and
server/local entry-point wiring.
MSG

# ============================ Phase 4: tests =================================
group cas-tests-unit 'src/Disks/tests/*' <<'MSG'
CAS tests: unit (gtest)

The full CAS gtest battery: per-layer unit tests, protocol state-machine
tests, wiring/assembly tests, and the shared test helpers.
MSG

group cas-tests-stateless 'A:tests/queries/*' <<'MSG'
CAS tests: stateless

New stateless tests for content-addressed storage.
MSG

group cas-tests-integration 'tests/integration/*' <<'MSG'
CAS tests: integration

Integration suites: shared pool, replicated relink, GC (sharded and S3),
insert fault recovery, lazy-load recovery, file cache, drop pool member,
ref snaplog, disks-app; plus shared helpers and compose files.
MSG

# =================== Phase 5: CI/CD and old-test tags ========================
group cas-ci-wiring \
    'ci/*' \
    '.github/*' \
    'tests/config/*' \
    'tests/clickhouse-test' \
    'M:tests/queries/*' <<'MSG'
CAS CI wiring and test tags

CA-default stateless/integration lanes in praktika and workflows, the
content-addressed default-disk test configs, clickhouse-test support, and
tag edits to pre-existing tests (no-content-addressed-storage and
friends).
MSG

# ========================== Phase 6: documentation ===========================
group cas-docs 'docs/en/*' <<'MSG'
CAS documentation

User-facing documentation: content-addressed storage in storing-data, the
SYSTEM statements, and the three system-table pages.
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

# ----------------------------------------------------------------------------
# Engine: apply — build the branch in a temporary worktree.
# ----------------------------------------------------------------------------
WT=tmp/carve-worktree
mkdir -p tmp

if git worktree list --porcelain | grep -qx "worktree $(pwd)/$WT"; then
    git worktree remove --force "$WT"
fi
if git show-ref --verify --quiet "refs/heads/$BRANCH"; then
    if [[ $FORCE -eq 1 ]]; then
        git branch -D "$BRANCH"
    else
        echo "ERROR: branch '$BRANCH' already exists (use --force to recreate)" >&2
        exit 1
    fi
fi

git worktree add -b "$BRANCH" "$WT" "$MB"
trap 'git worktree remove --force "$WT" 2>/dev/null || true' EXIT

echo
echo "== building $BRANCH from $MB =="
declare -a SUMMARY
pushd "$WT" > /dev/null
for g in "${GROUP_ORDER[@]}"; do
    files=()
    for file in "${DIFF_FILES[@]}"; do
        [[ ${CLAIM[$file]:-} == "$g" ]] && files+=("$file")
    done
    if [[ ${#files[@]} -eq 0 ]]; then
        echo "skipping empty group '$g'"
        continue
    fi
    for file in "${files[@]}"; do
        if [[ ${FILE_STATUS[$file]} == D ]]; then
            git rm -q --ignore-unmatch -- "$file"
        else
            git checkout -q "$SRC_SHA" -- "$file"
        fi
    done
    git commit -q -s -F - <<< "${GROUP_MESSAGES[$g]}

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
    SUMMARY+=("$(git rev-parse --short HEAD)  ${#files[@]}  $g")
done

# ----------------------------------------------------------------------------
# Engine: completeness invariant — nothing essential may be lost.
# ----------------------------------------------------------------------------
LOST=()
declare -A DISCARD_COUNT
while IFS= read -r file; do
    [[ -z $file ]] && continue
    excluded=0
    for pat in "${EXCLUDES[@]}"; do
        # status is irrelevant for excludes; pass M as a dummy
        if pattern_matches "$file" M "$pat"; then
            excluded=1
            DISCARD_COUNT[$pat]=$(( ${DISCARD_COUNT[$pat]:-0} + 1 ))
            break
        fi
    done
    [[ $excluded -eq 0 ]] && LOST+=("$file")
done < <(git diff --name-only --no-renames HEAD "$SRC_SHA")

echo
echo "== summary =="
printf '%s\n' "${SUMMARY[@]}"
echo
echo "== discarded (by exclude) =="
for pat in "${EXCLUDES[@]}"; do
    printf '%6d  %s\n' "${DISCARD_COUNT[$pat]:-0}" "$pat"
done

popd > /dev/null

if [[ ${#LOST[@]} -gt 0 ]]; then
    echo
    echo "ERROR: completeness invariant violated — ${#LOST[@]} non-excluded files differ between $BRANCH and src:" >&2
    printf '  %s\n' "${LOST[@]}" >&2
    exit 1
fi

echo
echo "OK: completeness invariant holds. Branch '$BRANCH' is ready (worktree removed, branch kept)."
