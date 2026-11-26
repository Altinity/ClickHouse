#!/usr/bin/env bash
# Tags: no-fasttest, no-replicated-database
# no-fasttest: cas is an object-storage metadata type, absent from the fasttest image.
# no-replicated-database: replicas share `user_files`, and `FORGET` certifies the teardown of this server only.

# `cas-fsck` lists a `Creating` row of a creator whose mount slot is live: the line says the GC leader keeps
# it and that the creating server cleans up its own idle rows. The row is added to a copy of the pool, under
# this server's root and live writer epoch, as an interrupted `CREATE` would leave it.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_NAME="ca_own_creating_${CLICKHOUSE_TEST_UNIQUE_NAME}_${RANDOM}"
ROOT_ID="${CLICKHOUSE_DATABASE}_05489"
# Fresh directories: a leftover pool of an earlier run may still be mounted by the server and is never touched.
POOL_DIR=$(mktemp -d "${CLICKHOUSE_USER_FILES_UNIQUE}_own_creating_XXXXXX") || { echo "mktemp failed"; exit 1; }
COPY_DIR=$(mktemp -d "${CLICKHOUSE_USER_FILES_UNIQUE}_own_creating_copy_XXXXXX") || { echo "mktemp failed"; exit 1; }
chmod 755 "${POOL_DIR}" "${COPY_DIR}"
OUT="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}_fsck.out"
trap 'rm -rf "$OUT" "${COPY_DIR:?}"' EXIT
export TEST_DISK_05489_POOL="${COPY_DIR}/"
config="$CUR_DIR/05489_cas_fsck_own_creating_row.xml"

${CLICKHOUSE_CLIENT} --multiquery <<SQL
DROP TABLE IF EXISTS t_own_creating SYNC;
CREATE TABLE t_own_creating (k UInt64)
ENGINE = MergeTree ORDER BY k
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    cas_unsafe_remount_no_delay = 1,
    cas_server_root_id = '${ROOT_ID}',
    name = '${DISK_NAME}',
    path = '${POOL_DIR}/',
    cas_gc_enabled = 0);
SYSTEM STOP MERGES t_own_creating;
INSERT INTO t_own_creating SELECT number FROM numbers(10);
SQL
[ $? -eq 0 ] || { echo "fixture setup failed: leaving the pool directory in place"; exit 1; }

EPOCH=$(${CLICKHOUSE_CLIENT} --query "SELECT writer_epoch FROM system.cas_mounts WHERE disk = '${DISK_NAME}' AND server_root_id = '${ROOT_ID}'")
[ $? -eq 0 ] && [ -n "${EPOCH}" ] || { echo "writer epoch query failed: leaving the pool directory in place"; exit 1; }
cp -r "${POOL_DIR}/." "${COPY_DIR}/" || { echo "pool copy failed: leaving the pool directory in place"; exit 1; }
CATALOG=$(find "${COPY_DIR}" -type f -path '*/cas/ref_catalog*' | head -1)
[ -n "${CATALOG}" ] || { echo "no catalog in the pool copy: leaving the pool directory in place"; exit 1; }

# Adds a `Creating` row beside the table's own row: same namespace with the table UUID replaced by `f`s,
# a fresh life, this server's root and live writer epoch. Rows stay in ascending namespace order.
python3 - "${CATALOG}" "${ROOT_ID}" "${EPOCH}" <<'PY'
import json, re, sys
path, root, epoch = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(path).read().split('\n')
header, rows, trailer = lines[0], [json.loads(l) for l in lines[1:-2]], json.loads(lines[-2])
assert lines[-1] == '' and list(trailer) == ['n'] and trailer['n'] == len(rows) == 1, lines
base = rows[0]
ns = re.sub(r'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}', 'ffffffff-ffff-ffff-ffff-ffffffffffff', base['ns'])
assert ns != base['ns'], base
row = {'kind': base['kind'], 'ns': ns, 'state': 'creating', 'life': 'f' * len(base['life']),
       'creator': root, 'creator_epoch': epoch, 'creator_fence': '1'}
rows = sorted(rows + [row], key=lambda r: r['ns'].encode())
body = [header] + [json.dumps(r, separators=(',', ':')) for r in rows] + [json.dumps({'n': len(rows)}, separators=(',', ':')), '']
open(path, 'w').write('\n'.join(body))
PY
[ $? -eq 0 ] || { echo "catalog edit failed: leaving the pool directory in place"; exit 1; }

clickhouse-disks -C "$config" --disk cas_ro_05489 --query "cas-fsck" > "$OUT" 2>&1
echo "exit $?"
grep -o 'stalled_creating=[0-9]* creating_unproven=[0-9]*' "$OUT"
grep "is \`Creating\` under creator '${ROOT_ID}' at writer epoch ${EPOCH}; its mount slot is live" "$OUT" \
    | grep -c 'the GC leader cancels the row once that server remounts.*a writable creator with its GC scheduler running also cancels its own idle rows of its current writer epoch'
rm -rf "$OUT" "${COPY_DIR:?}"

# The pool directory is removed only after the disk is forgotten; any failure leaves it in place.
${CLICKHOUSE_CLIENT} --query "DROP TABLE t_own_creating SYNC" || {
    echo "DROP failed: leaving the pool directory in place"; exit 1; }
# FORGET logs an operator WARNING that the harness would flag; silence it for this call only.
${CLICKHOUSE_CLIENT} --allow_repeated_settings --send_logs_level=fatal \
    --query "SYSTEM CAS FORGET '${DISK_NAME}'" || {
    echo "FORGET failed: leaving the pool directory in place"; exit 1; }
LIFECYCLE=$(${CLICKHOUSE_CLIENT} --query "
    SELECT lifecycle || '(' || lifecycle_reason || ')' FROM system.cas_mounts
    WHERE disk = '${DISK_NAME}'") || {
    echo "lifecycle query failed: leaving the pool directory in place"; exit 1; }
[ "${LIFECYCLE}" = "vanished(forgotten)" ] || {
    echo "unexpected lifecycle after FORGET: ${LIFECYCLE}"; exit 1; }
rm -rf "${POOL_DIR:?}"
