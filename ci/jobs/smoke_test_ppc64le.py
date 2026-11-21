#!/usr/bin/env python3
"""
ppc64le smoke test.

Runs the cross-built ppc64le binary inside a ppc64le Debian container via
qemu-user: unpacks the self-extracting binary, starts clickhouse-server with
the default server config (including SystemLogs), runs a MergeTree round-trip
and an exception-unwinding check, and verifies clean SIGTERM shutdown.
"""

import shutil
from pathlib import Path

from ci.praktika.result import Result
from ci.praktika.utils import Shell, Utils

REPO_DIR = Path(Utils.cwd())
TEMP_DIR = REPO_DIR / "ci" / "tmp"
BINARY_PATH = TEMP_DIR / "clickhouse"
SMOKE_DIR = TEMP_DIR / "smoke"
DEBIAN_IMAGE = (
    "debian:bookworm"
    "@sha256:268fb24c39bca92d326dd83ea3abebe4a3f7751c804ffef23bb6d857dd343ff8"
)
QEMU_IMAGE = (
    "tonistiigi/binfmt:qemu-v10.2.3"
    "@sha256:400a4873b838d1b89194d982c45e5fb3cda4593fbfd7e08a02e76b03b21166f0"
)

HTTP_PORT = 18123
TCP_PORT = 19000

SMOKE_CONFIG_OVERRIDE = f"""<clickhouse>
    <logger>
        <level>information</level>
        <log>/workspace/smoke/log/clickhouse-server.log</log>
        <errorlog>/workspace/smoke/log/clickhouse-server.err.log</errorlog>
        <console>0</console>
    </logger>
    <path>/workspace/smoke/data/</path>
    <tmp_path>/workspace/smoke/data/tmp/</tmp_path>
    <user_files_path>/workspace/smoke/data/user_files/</user_files_path>
    <format_schema_path>/workspace/smoke/data/format_schemas/</format_schema_path>
    <google_protos_path>/workspace/smoke/data/protos/</google_protos_path>
    <custom_cached_disks_base_directory>/workspace/smoke/data/caches/</custom_cached_disks_base_directory>
    <user_directories>
        <local_directory>
            <path>/workspace/smoke/data/access/</path>
        </local_directory>
    </user_directories>
    <http_port>{HTTP_PORT}</http_port>
    <tcp_port>{TCP_PORT}</tcp_port>
    <mysql_port remove="remove"/>
    <postgresql_port remove="remove"/>
    <interserver_http_port remove="remove"/>
    <listen_host>127.0.0.1</listen_host>
    <mlock_executable>false</mlock_executable>
</clickhouse>
"""

CONTAINER_SMOKE_SCRIPT = f"""#!/bin/bash
set -euo pipefail

CH=/workspace/clickhouse
CLIENT=("$CH" client --host 127.0.0.1 --port {TCP_PORT})

# Unpack the self-extracting binary before launching server + client processes.
"$CH" --version

CLICKHOUSE_WATCHDOG_ENABLE=0 "$CH" server \\
    --config-file=/workspace/smoke/conf/config.xml \\
    >/workspace/smoke/log/server.stderr.log 2>&1 &
SERVER_PID=$!

cleanup() {{
    if [[ -n "${{SERVER_PID:-}}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -KILL "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    chown -R "$(stat -c '%u:%g' /workspace)" /workspace 2>/dev/null || true
}}
trap cleanup EXIT

ready=0
for _ in $(seq 1 300); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        wait "$SERVER_PID" || rc=$?
        echo "clickhouse-server exited early (code ${{rc:-0}})" >&2
        tail -n 100 /workspace/smoke/log/server.stderr.log /workspace/smoke/log/clickhouse-server.err.log >&2 || true
        exit 1
    fi
    if [[ "$("${{CLIENT[@]}}" --query 'SELECT 1' 2>/dev/null || true)" == "1" ]]; then
        ready=1
        break
    fi
    sleep 1
done

if [[ "$ready" -ne 1 ]]; then
    echo "clickhouse-server did not become ready within 300s" >&2
    tail -n 100 /workspace/smoke/log/server.stderr.log /workspace/smoke/log/clickhouse-server.err.log >&2 || true
    exit 1
fi

"${{CLIENT[@]}}" --query "CREATE TABLE smoke (id UInt64, s String) ENGINE = MergeTree ORDER BY id"
"${{CLIENT[@]}}" --query "INSERT INTO smoke SELECT number, toString(number) FROM numbers(1000)"
got="$("${{CLIENT[@]}}" --query "SELECT count(), sum(id), max(s) FROM smoke")"
expected=$'1000\\t499500\\t999'
if [[ "$got" != "$expected" ]]; then
    echo "MergeTree round-trip mismatch: got [$got], expected [$expected]" >&2
    exit 1
fi

set +e
exc_out="$("${{CLIENT[@]}}" --query "SELECT throwIf(1, 'smoke_unwind_check')" 2>&1)"
exc_rc=$?
set -e
if [[ "$exc_rc" -eq 0 ]] || [[ "$exc_out" != *"smoke_unwind_check"* ]]; then
    echo "Exception unwind check unexpected result (rc=$exc_rc): $exc_out" >&2
    exit 1
fi

[[ "$("${{CLIENT[@]}}" --query 'SELECT 1')" == "1" ]]

kill -TERM "$SERVER_PID"
for _ in $(seq 1 120); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        wait "$SERVER_PID"
        SERVER_PID=""
        exit 0
    fi
    sleep 1
done

echo "clickhouse-server did not shut down cleanly within 120s" >&2
exit 1
"""


def prepare_workspace():
    if not BINARY_PATH.is_file():
        print(f"Missing binary {BINARY_PATH}")
        return False
    shutil.rmtree(SMOKE_DIR, ignore_errors=True)
    conf_dir = SMOKE_DIR / "conf"
    (conf_dir / "config.d").mkdir(parents=True)
    (SMOKE_DIR / "data" / "protos").mkdir(parents=True)
    (SMOKE_DIR / "log").mkdir(parents=True)

    server_cfg_dir = REPO_DIR / "programs" / "server"
    for name in ("config.xml", "users.xml"):
        shutil.copy(server_cfg_dir / name, conf_dir / name)
    (conf_dir / "config.d" / "zz_smoke.xml").write_text(SMOKE_CONFIG_OVERRIDE)

    script_path = SMOKE_DIR / "run_smoke.sh"
    script_path.write_text(CONTAINER_SMOKE_SCRIPT)
    script_path.chmod(0o755)

    return Shell.check(f"chmod +x {BINARY_PATH}", verbose=True)


def main():
    stopwatch = Utils.Stopwatch()
    test_results = []

    test_results.append(
        Result.from_commands_run(
            name="Prepare workspace",
            command=prepare_workspace,
        )
    )

    if test_results[-1].is_ok():
        test_results.append(
            Result.from_commands_run(
                name="Register qemu-ppc64le",
                command=f"docker run --rm --privileged {QEMU_IMAGE} --install ppc64le",
            )
        )

    if test_results[-1].is_ok():
        test_results.append(
            Result.from_commands_run(
                name="Server smoke test",
                command=(
                    "docker run --rm --platform linux/ppc64le "
                    f"--volume={TEMP_DIR}:/workspace "
                    f"{DEBIAN_IMAGE} "
                    "/workspace/smoke/run_smoke.sh"
                ),
                with_info=True,
            )
        )

    log_files = [str(p) for p in (SMOKE_DIR / "log").glob("*.log") if p.is_file()]
    result = Result.create_from(results=test_results, stopwatch=stopwatch, files=log_files)
    result.complete_job()


if __name__ == "__main__":
    main()
