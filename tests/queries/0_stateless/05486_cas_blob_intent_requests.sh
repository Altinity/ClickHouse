#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings, no-random-merge-tree-settings
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image. Randomized settings could
#   split the insert into several parts, and then blobs shared between parts would change the counts.

# Blob requests of one `INSERT` into a content-addressed disk, per blob, from `system.query_log`.
# A writer publishes a new body with `HEAD` (miss) + one `PUT` and reuses an existing body after `HEAD` + a `GET` of
# the deletion intent of that body: no intent means adopt, an intent means publish a fresh body.
#   1. new content:                    HEAD miss, PUT, no intent request
#   2. the same content again:         HEAD hit, intent GET (absent), no PUT
#   3. after DROP and the condemning GC round, the body is still there with its intent:
#                                      HEAD hit, intent GET (present), PUT
#   4. after DROP and the rounds that delete the body:
#                                      HEAD miss, PUT, no intent request
# N is the number of blobs the first insert creates. Every other count is stated in terms of N, so the test does
# not depend on how many files a part has. A mismatch prints the counters it saw.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_NAME="${CLICKHOUSE_DATABASE}_05486_cas"
POOL_DIR="${CLICKHOUSE_USER_FILES_UNIQUE}_05486"
# The interval is long enough that no background round runs: only `SYSTEM CAS GC RUN` advances the pool.
DISK="disk(type = object_storage, object_storage_type = local, metadata_type = cas, cas_unsafe_remount_no_delay = 1,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_05486',
    cas_gc_enabled = 1, cas_gc_interval_sec = 100000,
    name = '${DISK_NAME}',
    path = '${POOL_DIR}/')"

rm -rf "${POOL_DIR:?}"

CLIENT="${CLICKHOUSE_CLIENT} --send_logs_level=error"

for t in t1 t2 t3
do
    ${CLIENT} -q "DROP TABLE IF EXISTS ${t} SYNC"
    ${CLIENT} -q "CREATE TABLE ${t} (a UInt64, b UInt64) ENGINE = MergeTree ORDER BY a SETTINGS disk = ${DISK}"
    ${CLIENT} -q "SYSTEM STOP MERGES ${t}"
done

INSERT_SELECT="SELECT number, number * 7 FROM numbers(1000)"
Q_NEW="${CLICKHOUSE_DATABASE}_05486_new"
Q_DUPLICATE="${CLICKHOUSE_DATABASE}_05486_duplicate"
Q_CONDEMNED="${CLICKHOUSE_DATABASE}_05486_condemned"
Q_DELETED="${CLICKHOUSE_DATABASE}_05486_deleted"

# One round of `SYSTEM CAS GC RUN`: prints "<acquired_lease> <entries_condemned> <entries_intent_confirmed> <entries_bodies_deleted>
# <entries_graduated> <entries_redeleted> <entries_body_delete_failed> <entries_redelete_failed>"; the last three
# are the compatibility names of the second, third and fourth values.
gc_round()
{
    ${CLIENT} -q "SYSTEM CAS GC RUN '${DISK_NAME}'" --format TSVWithNames \
        | awk -F'\t' 'NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
                      { print $col["acquired_lease"], $col["entries_condemned"], $col["entries_intent_confirmed"], $col["entries_bodies_deleted"],
                                    $col["entries_graduated"], $col["entries_redeleted"], $col["entries_body_delete_failed"], $col["entries_redelete_failed"] }'
}

# 1. New content.
${CLIENT} --query_id "${Q_NEW}" -q "INSERT INTO t1 ${INSERT_SELECT}"
${CLIENT} -q "SYSTEM FLUSH LOGS query_log"
N=$(${CLIENT} -q "
    SELECT ProfileEvents['CASBlobPut'] FROM system.query_log
    WHERE current_database = currentDatabase() AND query_id = '${Q_NEW}' AND type = 'QueryFinish'")
if ! [[ "${N}" =~ ^[0-9]+$ ]] || [ "${N}" -eq 0 ]
then
    echo "FAIL: the first insert created no blob (N='${N}')"
    exit 1
fi

# 2. The same content again, into the same table: every body exists and has no intent.
${CLIENT} --query_id "${Q_DUPLICATE}" -q "INSERT INTO t1 ${INSERT_SELECT}"

# 3. Drop every reference, then stop at the first round that condemns. Its intents exist and no body is deleted yet.
${CLIENT} -q "DROP TABLE t1 SYNC"
CONDEMN_ROUND=""
for _ in $(seq 1 40)
do
    read -r ACQUIRED CONDEMNED GRADUATED REDELETED OLD_GRADUATED OLD_REDELETED _ _ <<< "$(gc_round)"
    if [ "${CONDEMNED:-0}" -gt 0 ]
    then
        COMPAT_CONFIRMED="${GRADUATED} ${OLD_GRADUATED}"
        CONDEMN_ROUND="${ACQUIRED} ${CONDEMNED} ${GRADUATED} ${REDELETED}"
        break
    fi
done
echo "condemning round: acquired_lease condemned intent_confirmed bodies_deleted = $([ "${CONDEMN_ROUND}" = "1 ${N} ${N} 0" ] && echo ok || echo "${CONDEMN_ROUND:-none}")"
${CLIENT} --query_id "${Q_CONDEMNED}" -q "INSERT INTO t2 ${INSERT_SELECT}"

# 4. Drop again and run rounds until the bodies are deleted: the round that moves the intents to the new bodies,
#    then the round that deletes.
${CLIENT} -q "DROP TABLE t2 SYNC"
DELETED_TOTAL=0
COMPAT_DELETED=""
for _ in $(seq 1 40)
do
    read -r _ _ _ REDELETED _ OLD_REDELETED FAILED OLD_FAILED <<< "$(gc_round)"
    if [ "${REDELETED:-0}" -gt 0 ] && [ -z "${COMPAT_DELETED}" ]
    then
        COMPAT_DELETED="${REDELETED} ${OLD_REDELETED} ${FAILED} ${OLD_FAILED}"
    fi
    DELETED_TOTAL=$((DELETED_TOTAL + ${REDELETED:-0}))
    if [ "${DELETED_TOTAL}" -ge "${N}" ]
    then
        break
    fi
done
echo "bodies deleted by the rounds: $([ "${DELETED_TOTAL}" -ge "${N}" ] && echo ok || echo "${DELETED_TOTAL} of ${N}")"
# The compatibility names equal the new ones in rounds that confirmed and deleted something (non-zero, not 0 = 0).
echo "compatibility names: $([ "${COMPAT_CONFIRMED}" = "${N} ${N}" ] && [ "${COMPAT_DELETED}" = "${N} ${N} 0 0" ] && echo ok || echo "confirmed '${COMPAT_CONFIRMED}' deleted '${COMPAT_DELETED}'")"
${CLIENT} --query_id "${Q_DELETED}" -q "INSERT INTO t3 ${INSERT_SELECT}"

${CLIENT} -q "SYSTEM FLUSH LOGS query_log"

# Prints "ok" when the counters equal the expectation, else the counters seen:
# (HEAD miss, HEAD hit, intent GET, PUT, reused over an intent, body PUT avoided).
check_requests()
{
    local label=$1 query_id=$2 expected=$3
    ${CLIENT} -q "
        SELECT '${label}', if(seen = ${expected}, 'ok', toString(seen))
        FROM
        (
            SELECT (ProfileEvents['CASBlobHeadMiss'], ProfileEvents['CASBlobHead'], ProfileEvents['CASBlobIntentGet'],
                    ProfileEvents['CASBlobPut'], ProfileEvents['CASBlobReuseIntentPresent'], ProfileEvents['CASBlobBodyPutAvoided']) AS seen
            FROM system.query_log
            WHERE current_database = currentDatabase() AND query_id = '${query_id}' AND type = 'QueryFinish'
        )"
}

check_requests "new content" "${Q_NEW}" "(${N}, 0, 0, ${N}, 0, 0)"
check_requests "duplicate" "${Q_DUPLICATE}" "(0, ${N}, ${N}, 0, 0, ${N})"
check_requests "duplicate after the condemning round" "${Q_CONDEMNED}" "(0, ${N}, ${N}, ${N}, ${N}, 0)"
check_requests "duplicate after the body delete" "${Q_DELETED}" "(${N}, 0, 0, ${N}, 0, 0)"

${CLIENT} -q "DROP TABLE t3 SYNC"
# FORGET logs an operator WARNING; keep it off the client's stderr.
${CLICKHOUSE_CLIENT} --send_logs_level=fatal -q "SYSTEM CAS FORGET '${DISK_NAME}'"
rm -rf "${POOL_DIR:?}"
