"""Integration tests high-level runner using TestFlows framework.
"""
import os
import json
import time
import shlex
import testflows.settings

from testflows.core import *
from testflows.connect import Shell

from steps import *

# FIXME: only clickhouse/integration-test-runner:latest is used
#        but inside the runner image, locally build images are not being used
# FIXME: add support for --network which should be "" by default (instead of host by default)
#        in general we need to support all ./runner options, --disable-net-host
# FIXME: docker-compose-images-tags?
# FIXME: handler analyzer and analyzer broken tests
# FIXME: clear ip tables and restart docker between each interation of runner?
# FIXME: add pre-pull


def argparser(parser):
    parser.add_argument(
        "--binary",
        type=str,
        help=(
            "path to ClickHouse binary, default: /usr/bin/clickhouse.\n"
            "The path can be either:"
            "relative or absolute file path, "
            "http[s]://<url_to_binary_or_deb_package>, or "
            "docker://<clickhouse/docker_image:tag>\n"
        ),
        default="/usr/bin/clickhouse",
    )

    parser.add_argument(
        "--slice",
        type=int,
        dest="tests_slice",
        nargs=2,
        help="run specific slice of tests specified as '<start> <end>', default: 0 -1",
        default=[0, -1],
    )

    parser.add_argument(
        "--part",
        type=count,
        nargs=2,
        help="run specific part of tests specified as '<part number> <total number of parts>', default: 0 1",
        default=[0, 1],
    )

    parser.add_argument(
        "--tests",
        action="store",
        nargs="+",
        default=[],
        help="list of tests to run, default: collect all tests automatically",
    )

    parser.add_argument(
        "--deselect",
        action="store",
        nargs="+",
        default=[],
        help="list of tests to exclude from the tests list",
    )

    parser.add_argument(
        "--retry-attempts",
        type=count,
        help="number of times to retry failed tests, default: 2",
        default=2,
    )

    parser.add_argument(
        "--max-failed-tests-to-retry",
        type=count,
        help="maximum number of failed tests to retry, default: 100",
        default=100,
    )

    parser.add_argument(
        "--in-parallel",
        type=count,
        dest="in_parallel",
        help="number of tests to be executed in parallel, default: 10",
        default=10,
    )

    # runner options
    parser.add_argument(
        "--analyzer",
        action="store_true",
        default=False,
        dest="analyzer",
        help="run tests with analyzer enabled",
    )

    parser.add_argument(
        "--tmpfs",
        action="store_true",
        default=False,
        dest="tmpfs",
        help="use tmpfs for /var/lib/docker that is used inside the runner",
    )

    parser.add_argument(
        "--cleanup-containers",
        action="store_true",
        default=False,
        dest="cleanup_containers",
        help="remove all running containers on runner's test session start",
    )

    parser.add_argument(
        "--dockerd-volume-dir",
        action="store",
        dest="dockerd_volume_dir",
        help="bind volume to this dir to use for runner's dockerd files",
    )

    parser.add_argument(
        "--build-images",
        action="store_true",
        help=(
            "build all docker images inside the ClickHouse/docker folder including the ones used\n"
            "by integration tests. Use --build-images-tag to specify custom tag, default: latest."
        ),
    )

    parser.add_argument(
        "--images-tag",
        type=str,
        help="tag to be used for all docker images or when building them, default: latest",
        default="latest",
    )

    parser.add_argument(
        "--timeout",
        type=count,
        help="timeout in sec to wait for tests to complete, default: none",
    )

    parser.add_argument(
        "--group-timeout",
        type=count,
        help="timeout in sec to wait for a group of tests to complete, default: none",
    )

    parser.add_argument(
        "--group-size",
        type=count,
        help="size of test group, default: 100",
        default=100,
    )


@TestStep
def runner_opts(self):
    """Return runner script options."""
    return (
        f" --binary {self.context.binary}"
        + f" --odbc-bridge-binary {self.context.odbc_bridge_binary}"
        + f" --library-bridge-binary {self.context.library_bridge_binary}"
    )


@TestStep(Given)
def collect_tests(self, timeout=300):
    """Collect a list of all tests using pytest --setup-plan command."""
    tests = []
    command = (
        f"set -o pipefail && {os.path.join(current_dir(), 'runner')} {runner_opts()} -- --setup-plan "
        "| grep -F '::' | sed -r 's/ \(fixtures used:.*//g; s/^ *//g; s/ *$//g' "
        f"| grep -v -F 'SKIPPED' | sort --unique"
    )

    with Shell() as bash:
        cmd = bash(command, timeout=timeout)
        assert (
            cmd.exitcode == 0
        ), f"non-zero exitcode {cmd.exitcode} when trying to collect all tests"

        for line in cmd.output.splitlines():
            if not line.startswith("test_"):
                continue
            tests.append(line.strip())

    assert tests, "no tests found"
    return sorted(tests)


@TestStep(Given)
def launch_runner(self, run_id, tests, in_parallel=None):
    """Launch integration tests runner script."""

    with By("creating temporary file for the report"):
        log = temporary_file(mode="r", suffix=".pytest.jsonl", dir=current_dir())

    command = define(
        "command",
        f"{os.path.join(current_dir(), 'runner')}"
        + runner_opts()
        + f" -t {' '.join([shlex.quote(test) for test in sorted(tests)])}"
        + f" --docker-image-version {self.context.docker_image_version}"
        + (f" --parallel {in_parallel}" if in_parallel is not None else "")
        + (" --analyzer" if self.context.analyzer else "")
        + (" --tmpfs" if self.context.tmpfs else "")
        + (" --cleanup-containers" if self.context.cleanup_containers else "")
        + (" --dockerd-volume-dir" if self.context.dockerd_volume_dir else "")
        + " --"
        + " -rfEps"
        + f" --run-id={run_id} --color=no --durations=0"
        + f" --report-log={os.path.basename(log.name)}",
    )

    with And("launching command"):
        proc = sysprocess(command=command)

        if proc.poll() is not None:
            if proc.returncode != 0:
                fail(f"failed to start, exitcode: {proc.returncode}")

    yield proc, log


@TestOutline(Feature)
def execute_group(
    self, group_id, tests, in_parallel=None, retry_tests=None, timeout=None
):
    """Execute a group of tests."""
    executed = 0

    with Given(f"launch runner for {group_id}"):
        runner, log = launch_runner(
            run_id=group_id,
            tests=tests,
            in_parallel=in_parallel,
        )

    while True:
        with timer(
            timeout, f"timed out while running {len(tests)} tests in group {group_id}"
        ):
            pass

        line = readline(log)

        if not line:
            if runner.poll() is not None:
                # runner has exited, try reading one final time
                line = readline(log)
                if not line:
                    break
            time.sleep(1)
            continue

        with catch(Exception, raising=ValueError(f"failed to parse line: {line}")):
            entry = json.loads(line)

        if entry["$report_type"] == "TestReport":
            # skip setup and teardown entries unless they have non-passing outcome
            if entry["when"] != "call" and entry["outcome"] == "passed":
                continue

            # create scenario for each test call or non-passing setup or teardown outcome
            with Scenario(
                name=entry["nodeid"]
                + ((":" + entry["when"]) if entry["when"] != "call" else ""),
                description=f"Test location: {':'.join([str(e) for e in entry['location']])}",
                attributes=Attributes(*entry["keywords"].items()),
                start_time=entry["start"],
                test_time=(entry["start"] - entry["stop"]),
                flags=TE,
            ):
                executed += 1
                for section in entry["sections"]:
                    # process captured log
                    if section and section[0] == "Captured log call":
                        message("Captured log call\n" + section[1])

                # process trackback entries if any
                longrepr = entry.get("longrepr")

                reprcrash = longrepr.get("reprcrash") if longrepr is not None else None
                if reprcrash:
                    reprcrash = f"{reprcrash['path']}:{reprcrash['lineno']} {reprcrash['message']}"
                    message(reprcrash)

                reprtraceback = (
                    longrepr.get("reprtraceback") if longrepr is not None else None
                )
                reprentries = (
                    reprtraceback.get("reprentries", [])
                    if reprtraceback is not None
                    else []
                )

                for reprentry in reprentries:
                    if reprentry["type"] == "ReprEntry":
                        reprfuncargs = reprentry["data"].get("reprfuncargs")
                        args = (
                            reprfuncargs.get("args", [])
                            if reprfuncargs is not None
                            else []
                        )
                        for arg in args:
                            message(" = ".join(arg))
                        if reprentry["data"].get("lines"):
                            message("\n".join(reprentry["data"]["lines"]))
                        if reprentry["data"].get("reprfileloc"):
                            fileloc = reprentry["data"]["reprfileloc"]
                            message(
                                f"{fileloc['path']}:{fileloc['lineno']} {fileloc['message']}"
                            )

                if entry["outcome"].lower() == "passed":
                    ok("success")

                fail_message = (
                    f"{entry['outcome']}{(' ' + reprcrash) if reprcrash else ''}"
                )

                if retry_tests is not None:
                    retry_tests.append(entry["nodeid"])
                    xfail(fail_message, reason="will be retried")
                else:
                    fail(fail_message)

    assert executed == len(
        tests
    ), f"failed to execute all tests: executed {executed} out of {len(tests)}"


@TestFeature
def execute(
    self,
    tests,
    group_size,
    in_parallel,
    retry_tests=None,
    timeout=None,
    group_timeout=None,
):
    """Execute tests in groups."""

    for group_id, i in enumerate(range(0, len(tests), group_size)):
        with timer(timeout, f"timed out while executing {len(tests)} tests in groups"):
            pass

        group_tests = tests[i : i + group_size]

        note(f"running group {i, i + group_size}")

        Feature(name=f"{group_id}", test=execute_group, flags=TE)(
            group_id=group_id,
            tests=group_tests,
            in_parallel=in_parallel,
            retry_tests=retry_tests,
            timeout=next_group_timeout(group_timeout, timeout),
        )


@TestModule
@ArgumentParser(argparser)
def regression(
    self,
    binary,
    tests=None,
    tests_slice=None,
    part=None,
    subset=None,
    deselect=None,
    in_parallel=5,
    build_images=False,
    images_tag="latest",
    max_failed_tests_to_retry=100,
    retry_attempts=2,
    group_size=100,
    timeout=None,
    group_timeout=None,
    analyzer=False,
    tmpfs=False,
    cleanup_containers=False,
    dockerd_volume_dir=None,
):
    """Execute ClickHouse pytest integration tests."""
    # propagate runner options
    self.context.docker_image_version = images_tag
    self.context.analyzer = analyzer
    self.context.tmpfs = tmpfs
    self.context.cleanup_containers = cleanup_containers
    self.context.dockerd_volume_dir = dockerd_volume_dir

    retry_tests = None
    part_num, max_parts = part

    if retry_attempts > 1:
        # create a list to collect failed tests to be retried
        retry_tests = []

    with Given("clickhouse binaries"):
        (
            self.context.binary,
            self.context.odbc_bridge_binary,
            self.context.library_bridge_binary,
        ) = clickhouse_binaries(path=binary)

    if not tests:
        with And("automatically collect all tests"):
            all_tests = collect_tests()
    else:
        all_tests = tests

    if deselect:
        with And(f"deselect {len(deselect)} tests"):
            all_tests = [test for test in all_tests if test not in deselect]

    with And("select slice of tests"):
        all_tests = all_tests[slice(*tests_slice)]

    with And("select tests for the choose part"):
        tests_per_part = max(int(len(all_tests) / max_parts), 1)
        tests_offset = part_num * tests_per_part
        tests = all_tests[tests_offset : tests_offset + tests_per_part]

    note(
        f"number of tests to be executed {len(tests)}/{len(all_tests)}, part {part_num}/{max_parts}"
    )

    if not tests:
        fail("no tests")

    if build_images:
        with Feature("build images"):
            build_images = load_module(
                "buildimages",
                path=os.path.join(current_dir(), "..", "..", "docker", "build"),
            ).build_images
            build_images(tag=images_tag, timeout=300)

    Feature("group", description="execute tests in groups", test=execute)(
        tests=tests,
        group_size=group_size,
        in_parallel=in_parallel,
        retry_tests=retry_tests,
        timeout=timeout,
        group_timeout=group_timeout,
    )

    for attempt in range(retry_attempts):
        if retry_tests:
            with Feature(
                f"retry #{attempt}",
                description="Retry failed tests by running them without any parallelism.",
                flags=TE,
            ):
                if len(retry_tests) > max_failed_tests_to_retry:
                    debug(retry_tests)
                    fail(f"too many tests to retry: {len(retry_tests)}")

                tests = retry_tests
                retry_tests = [] if attempt + 1 < retry_attempts else None
                execute_group(
                    group_id=f"retry-{attempt}",
                    tests=tests,
                    retry_tests=retry_tests,
                    timeout=next_group_timeout(group_timeout, timeout),
                )


if main():
    regression()
