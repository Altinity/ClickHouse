#!/usr/bin/env bash
# Tags: no-parallel

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# Intent (mirrors 05011_cas_gc_rebuild_access.sh's GC REBUILD access test):
#  1) A zero-grant user is denied on both new verbs before the disk is even resolved -- naming a
#     disk that does not exist still yields ACCESS_DENIED, not UNKNOWN_DISK.
#  2) Granting "SYSTEM CONTENT ADDRESSED UNMOUNT" permits UNMOUNT but not MOUNT (still denied);
#     granting "SYSTEM CONTENT ADDRESSED MOUNT" then permits MOUNT too.
# (No CA disk needs to exist: the privilege check fires before any disk I/O. The `default` disk
#  always exists and is never content-addressed, so once the grant check passes the query
#  deterministically fails with BAD_ARGUMENTS instead.)

${CLICKHOUSE_CLIENT} --multiline -q """
DROP USER IF EXISTS user_test_05019;
CREATE USER user_test_05019 IDENTIFIED WITH plaintext_password BY 'user_test_05019';
REVOKE ALL ON *.* FROM user_test_05019;
"""

# Zero grants: both verbs are denied before the disk is resolved.
${CLICKHOUSE_CLIENT} --multiline --user user_test_05019 --password user_test_05019 -q """
SYSTEM CONTENT ADDRESSED UNMOUNT 'no_such_disk'; -- { serverError ACCESS_DENIED }
SYSTEM CONTENT ADDRESSED MOUNT 'no_such_disk'; -- { serverError ACCESS_DENIED }
"""

${CLICKHOUSE_CLIENT} --multiline -q """
GRANT SYSTEM CONTENT ADDRESSED UNMOUNT ON *.* TO user_test_05019;
"""

# UNMOUNT-only role: UNMOUNT is allowed (fails later, on the disk-type check); MOUNT stays denied.
${CLICKHOUSE_CLIENT} --multiline --user user_test_05019 --password user_test_05019 -q """
SYSTEM CONTENT ADDRESSED UNMOUNT default; -- { serverError BAD_ARGUMENTS }
SYSTEM CONTENT ADDRESSED MOUNT default; -- { serverError ACCESS_DENIED }
"""

${CLICKHOUSE_CLIENT} --multiline -q """
GRANT SYSTEM CONTENT ADDRESSED MOUNT ON *.* TO user_test_05019;
"""

# Granting the new right permits MOUNT too (fails later, on the disk-type check).
${CLICKHOUSE_CLIENT} --multiline --user user_test_05019 --password user_test_05019 -q """
SYSTEM CONTENT ADDRESSED MOUNT default; -- { serverError BAD_ARGUMENTS }
"""

# Both verbs require an explicit disk (syntax error).
${CLICKHOUSE_CLIENT} --multiline -q """
SYSTEM CONTENT ADDRESSED UNMOUNT; -- { clientError SYNTAX_ERROR }
SYSTEM CONTENT ADDRESSED MOUNT; -- { clientError SYNTAX_ERROR }
"""

${CLICKHOUSE_CLIENT} --multiline -q """
DROP USER IF EXISTS user_test_05019;
"""
