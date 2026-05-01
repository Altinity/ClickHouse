#!/usr/bin/env bash
# Tests that, after a restart, a refreshable local scalar
# resumes its schedule from the persisted last_successful_update_time
# instead of restarting the clock. Without the fix, an almost-due
# refresh would be postponed by nearly a full interval.

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh
CLICKHOUSE_CLIENT="$CLICKHOUSE_CLIENT --allow_experimental_named_scalars=1"
CLICKHOUSE_LOCAL="$CLICKHOUSE_LOCAL --allow_experimental_named_scalars=1"

TMP_DIR="${CLICKHOUSE_TMP}/named_scalars_cadence_${CLICKHOUSE_TEST_UNIQUE_NAME}"
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"
mkdir -p "$TMP_DIR/metadata"

${CLICKHOUSE_LOCAL} --path "$TMP_DIR" --multiquery --query "
CREATE TABLE src (x UInt64) ENGINE=Memory;
INSERT INTO src VALUES (1);
CREATE NAMED SCALAR cv_hourly REFRESH EVERY 1 HOUR AS SELECT max(x) FROM src;
"

DEFINITIONS_DIR="${TMP_DIR%/}/named_scalars"
VALUES_DIR="${TMP_DIR%/}/named_scalars_cache"
VALUE_FILE="$(
    python3 - "$DEFINITIONS_DIR" "$VALUES_DIR" <<'PY'
import re, sys

definitions_dir, values_dir = sys.argv[1], sys.argv[2]
definition = open(f"{definitions_dir}/named_scalar_cv_hourly.sql", "r").read()
match = re.search(r"UUID '([^']+)'", definition)
if not match:
    raise SystemExit("UUID not found for cv_hourly")
print(f"{values_dir}/{match.group(1).replace('-', '%2D')}.bin")
PY
)"

# Rewrite both timestamps in the persisted blob so the scalar looks like it
# was last refreshed 3570 seconds ago — the next scheduled tick under
# EVERY 1 HOUR should therefore be ~30 seconds from "now" on reload.
python3 - "$VALUE_FILE" <<'PY'
import re, sys, time

path = sys.argv[1]
text = open(path, "r").read()

target = int(time.time()) - 3570
text = re.sub(r"^last_update_time: \d+$", f"last_update_time: {target}", text, count=1, flags=re.MULTILINE)
text = re.sub(r"^last_successful_update_time: \d+$", f"last_successful_update_time: {target}", text, count=1, flags=re.MULTILINE)
open(path, "w").write(text)
PY

# After reload, next_refresh_time should point to roughly 30 seconds from now,
# NOT an hour away — the scheduler must consult the persisted success time.
next_in_seconds="$(${CLICKHOUSE_LOCAL} --path "$TMP_DIR" --query "
SELECT toInt64(next_refresh_time) - toInt64(now())
FROM system.named_scalars
WHERE name = 'cv_hourly' AND kind = 'local'")"

# Accept up to 5 minutes of skew for CI jitter; anything approaching 3600
# seconds means the schedule was reset to the restart moment.
if [ -z "$next_in_seconds" ]; then
    echo "no_row"
elif [ "$next_in_seconds" -gt 300 ]; then
    echo "schedule_reset"
else
    echo "schedule_preserved"
fi

${CLICKHOUSE_LOCAL} --path "$TMP_DIR" --query "DROP NAMED SCALAR cv_hourly" >/dev/null
rm -rf "$TMP_DIR"
