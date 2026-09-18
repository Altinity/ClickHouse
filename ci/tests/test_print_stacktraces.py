"""
End-to-end tests for the stacktrace helpers in tests/clickhouse-test.

Background
----------
``clickhouse-test`` assigns ``args = parse_args()`` only inside
``if __name__ == "__main__":``.  On macOS, Python's default
multiprocessing start method is ``spawn``, which re-imports the module
in each worker without executing ``__main__`` — so module-level
``args`` is undefined, and any helper that closed over it crashed with
``NameError``.  See the fast_test_arm_darwin failure where the
hung-check path raised ``NameError: name 'args' is not defined`` inside
``get_server_pid``.

These tests reproduce the same import condition by loading
``clickhouse-test`` via ``runpy.run_path`` (which, like spawn, does not
run ``__main__``) and then invoke each public stacktrace helper against
the live ClickHouse server provided by the ``ClickHouseService``
fixture in ``ci/jobs/ci_tests_job.py``.

Pre-fix: NameError inside the fresh import.
Post-fix: the helpers run to completion against a live server.
"""

import argparse
import io
import os
import re
import runpy
from contextlib import redirect_stdout
from pathlib import Path
from time import sleep, time

_REPO_ROOT = Path(__file__).resolve().parent.parent.parent
_CLICKHOUSE_TEST = str(_REPO_ROOT / "tests" / "clickhouse-test")


def _load_clickhouse_test(require_server=True):
    # Mimic a spawn worker: load clickhouse-test without running __main__,
    # so module-level `args` is absent.
    ct = runpy.run_path(_CLICKHOUSE_TEST)
    assert "args" not in ct, (
        "module-level 'args' must not be defined outside __main__; otherwise "
        "the spawn-worker scenario this test reproduces does not apply"
    )

    if require_server:
        # Sanity-check the precondition: the CI tests job started a server.
        assert ct["pgrep"](command="clickhouse-server"), (
            "no clickhouse-server process found — this test expects ClickHouseService "
            "(see ci/jobs/ci_tests_job.py) to be running on localhost:9000"
        )
    return ct


def _make_args():
    # Minimal args namespace: only the fields the helpers and their
    # transitive callees actually read.  Mirrors what __main__ assigns
    # after parse_args() for a local-server, plaintext-TCP,
    # default-database run.
    return argparse.Namespace(
        client="clickhouse-client --port=9000",
        client_option=None,
        secure=False,
        tcp_host="localhost",
        http_port=8123,
        client_options_query_str="",
        replicated_database=False,
        shared_catalog=False,
        force_color=False,
        binary=os.environ.get("CLICKHOUSE_BINARY", "clickhouse"),
        # A reachable server means __main__ collected build flags at startup;
        # a non-ASan set keeps print_c_stacktraces on its lldb path.
        build_flags=set(),
    )


def _capture_lldb_budgets(ct, args, pids, elapsed=0.0, dump=None, **kwargs):
    """Run print_c_stacktraces with the collector stubbed, returning the
    per-PID timeouts it was called with (and its stdout)."""
    budgets = []

    def collector(pid, timeout=None):
        budgets.append(timeout)
        if elapsed:
            sleep(elapsed)
        return "x" * 2000 if dump is None else dump

    globals_ = ct["print_c_stacktraces"].__globals__
    saved = (
        globals_["get_stacktraces_from_lldb"],
        globals_["get_all_server_pids"],
        globals_["get_server_pid"],
        globals_["is_asan_build"],
    )
    globals_["get_stacktraces_from_lldb"] = collector
    globals_["get_all_server_pids"] = lambda: list(pids)
    globals_["get_server_pid"] = lambda: pids[0]
    globals_["is_asan_build"] = lambda _args: False
    captured = io.StringIO()
    try:
        with redirect_stdout(captured):
            ct["print_c_stacktraces"](args, **kwargs)
    finally:
        (
            globals_["get_stacktraces_from_lldb"],
            globals_["get_all_server_pids"],
            globals_["get_server_pid"],
            globals_["is_asan_build"],
        ) = saved
    return budgets, captured.getvalue()


def test_print_c_stacktraces_against_live_server():
    ct = _load_clickhouse_test()
    args = _make_args()

    captured = io.StringIO()
    with redirect_stdout(captured):
        ct["print_c_stacktraces"](args)
    output = captured.getvalue()

    # The function must have located the server PID and reached gdb.
    # Whether the attach itself succeeds depends on the host's
    # `kernel.yama.ptrace_scope` and is not asserted.
    assert "Collecting C stacktraces from main server process" in output, output


def test_print_sql_stacktraces_against_live_server():
    ct = _load_clickhouse_test()
    args = _make_args()

    captured = io.StringIO()
    with redirect_stdout(captured):
        ct["print_sql_stacktraces"](args)
    output = captured.getvalue()

    # The function must have queried system.stack_trace and printed
    # traces.  We don't require a specific thread name — any non-trivial
    # output confirms the round-trip succeeded.
    assert "Collecting stacktraces from system.stack_trace table" in output, output
    assert "trace_str" in output or "thread_name" in output, output


def test_lldb_budget_scales_with_build_flavor():
    # A debug or sanitizer or coverage server needs far longer than 30s to walk;
    # a release server does not, and must keep the tight budget that bounds a
    # genuinely wedged lldb.
    ct = _load_clickhouse_test(require_server=False)
    flags = ct["BuildFlags"]
    release, slow = ct["LLDB_TIMEOUT"], ct["LLDB_SLOW_BUILD_TIMEOUT"]
    assert slow > release, (slow, release)

    args = _make_args()
    for build_flags, expected in (
        ({flags.RELEASE}, release),
        ({flags.DEBUG}, slow),
        ({flags.THREAD}, slow),
        ({flags.MEMORY}, slow),
        ({flags.UNDEFINED}, slow),
        ({flags.WITH_COVERAGE}, slow),
    ):
        args.build_flags = build_flags
        assert ct["lldb_timeout_for_build"](args) == expected, build_flags
        # And the loop passes that value through. It is clamped to what is left
        # of the aggregate ceiling, so compare with a tolerance rather than
        # exactly.
        budgets, _ = _capture_lldb_budgets(ct, args, [4242])
        assert len(budgets) == 1 and abs(budgets[0] - expected) < 1, (
            build_flags,
            budgets,
            expected,
        )


def test_lldb_budget_survives_the_spawn_start_method():
    # A spawned worker re-imports the module with RELEASE_NON_SANITIZED /
    # SANITIZED back at their defaults while `args` is transferred intact, so
    # the budget must come from args.build_flags, not from those globals.
    ct = _load_clickhouse_test(require_server=False)
    globals_ = ct["print_c_stacktraces"].__globals__
    saved = (globals_["RELEASE_NON_SANITIZED"], globals_["SANITIZED"])
    globals_["RELEASE_NON_SANITIZED"] = False
    globals_["SANITIZED"] = False
    try:
        args = _make_args()
        args.build_flags = {ct["BuildFlags"].DEBUG}
        budgets, _ = _capture_lldb_budgets(ct, args, [4242])
    finally:
        globals_["RELEASE_NON_SANITIZED"], globals_["SANITIZED"] = saved

    assert len(budgets) == 1, budgets
    assert abs(budgets[0] - ct["LLDB_SLOW_BUILD_TIMEOUT"]) < 1, budgets


def test_lldb_budget_reads_the_binary_when_build_flags_are_missing():
    # The startup-failure caller (main -> check_server_started) runs before
    # `args.build_flags` is assigned, so the flavor comes from the binary via
    # `clickhouse local` rather than falling back to the release budget.
    ct = _load_clickhouse_test(require_server=False)
    args = _make_args()
    delattr(args, "build_flags")

    for slow, expected_key in ((True, "LLDB_SLOW_BUILD_TIMEOUT"), (False, "LLDB_TIMEOUT")):
        globals_ = ct["lldb_timeout_for_build"].__globals__
        saved = globals_["is_slow_build_binary"]
        globals_["is_slow_build_binary"] = lambda _args, _s=slow: _s
        try:
            budgets, _ = _capture_lldb_budgets(ct, args, [4242])
        finally:
            globals_["is_slow_build_binary"] = saved

        assert len(budgets) == 1, (slow, budgets)
        assert abs(budgets[0] - ct[expected_key]) < 1, (slow, budgets)


def test_slow_build_binary_probe_is_server_independent_and_fails_closed():
    # The probe must not need a live server (it exists for the path where the
    # server never came up), and an unreadable binary must yield the tighter
    # budget rather than raising on an already-failing run.
    ct = _load_clickhouse_test(require_server=False)

    args = _make_args()
    args.binary = "/nonexistent/clickhouse-does-not-exist"
    assert ct["is_slow_build_binary"](args) is False

    # And the query it issues names every flag collect_build_flags derives, so
    # the two cannot drift into disagreeing about what "slow" means.
    source = Path(_CLICKHOUSE_TEST).read_text(encoding="utf-8")
    probe = source.split("def is_slow_build_binary")[1].split("\ndef ")[0]
    for token in ("BUILD_TYPE", "Debug", "WITH_COVERAGE", "-fsanitize="):
        assert token in probe, token


def test_lldb_pid_loop_honours_the_aggregate_deadline():
    # The loop spans every server process, so a per-PID budget alone does not
    # bound it. Exhausting the total must stop the loop and say how many
    # processes went undumped.
    ct = _load_clickhouse_test(require_server=False)
    args = _make_args()
    args.build_flags = {ct["BuildFlags"].DEBUG}
    pids = [111, 222, 333, 444]

    started = time()
    budgets, output = _capture_lldb_budgets(
        ct, args, pids, elapsed=1.0, per_pid_timeout=30, total_timeout=2
    )
    took = time() - started

    assert len(budgets) < len(pids), budgets
    assert took < 30, took
    # Each call is clamped to what is left, so no single attach can overrun the
    # ceiling on its own.
    assert all(b <= 2 for b in budgets), budgets
    skipped = len(pids) - len(budgets)
    assert f"skipping {skipped} of {len(pids)} processes" in output, output
    assert str(pids[-1]) in output.split("skipping")[1], output


def test_timeout_handler_keeps_the_tight_lldb_pair():
    # The per-test timeout handler runs after its one-shot alarm has fired, so
    # the alarm cannot bound it and only the job's outer timeout is left. That
    # site therefore keeps the 30s per-PID value; the abort paths, where no
    # alarm is pending, take the flavor budget. Asserted on the source so
    # neither the outer-deadline risk nor the hung-check coverage can regress.
    source = " ".join(Path(_CLICKHOUSE_TEST).read_text(encoding="utf-8").split())
    calls = re.findall(r"(?<!def )print_c_stacktraces\((.*?)\)", source)
    tight = [c for c in calls if "per_pid_timeout" in c or "total_timeout" in c]
    assert len(tight) == 1, calls
    assert "per_pid_timeout=LLDB_TIMEOUT" in tight[0], tight
    assert "total_timeout=60" in tight[0], tight
    # Every other call site takes the defaults, i.e. the flavor budget.
    plain = [c for c in calls if c.strip() == "args"]
    assert len(plain) == 4, calls
