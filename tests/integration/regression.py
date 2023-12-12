import os
import uuid
import json
import time
import shlex
import tempfile
import contextlib
import subprocess
import testflows.settings

from testflows.core import *
from testflows.connect import Shell

# FIXME: handler analyzer and analyzer broken tests
# FIXME: clear ip tables and restart docker between each interation of runner?
# FIXME: run parallel_skip.json tests sequentially
# FIXME: add pre-pull
# FIXME: add docker image rebuild
# FIXME: add test_times.json that contains approximate test times for each test
#        to devide tests by approximate total test time instead of number of tests


def argparser(parser):
    parser.add_argument(
        "--clickhouse-binary-path",
        type=str,
        dest="clickhouse_binary_path",
        help=(
            "path to ClickHouse binary, default: $CLICKHOUSE_TESTS_SERVER_BIN_PATH or /usr/bin/clickhouse.\n"
            "The path can be either:\n"
            "  relative or absolute local file path"
            "  http[s]://<url_to_binary_or_deb_package>"
            "  docker://<clickhouse/docker_image:tag>"
        ),
        metavar="path",
        default=os.getenv("CLICKHOUSE_TESTS_SERVER_BIN_PATH", "/usr/bin/clickhouse"),
    )

    parser.add_argument(
        "--run-in-parallel",
        type=int,
        dest="run_in_parallel",
        help="set runner parallelism, default: not set",
        default=10,
    )


@contextlib.contextmanager
def catch(exceptions, raising):
    try:
        yield
    except exceptions as e:
        raise raising from e


@TestStep(Given)
def sysprocess(self, command):
    """Run system command"""

    proc = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        encoding="utf-8",
        shell=True,
    )
    try:
        yield proc
    finally:
        if proc.poll() is None:
            proc.kill()

        while proc.poll() is None:
            debug(f"waiting for {proc} to exit")
            time.sleep(1)


@TestStep
def getuid(self):
    """Return unique id."""
    return str(uuid.uuid1()).replace("-", "_")


@TestStep
def short_hash(s):
    """Return good enough short hash of a string."""
    return hashlib.sha1(s.encode("utf-8")).hexdigest()[:10]


@TestStep
def get_clickhouse_binary_from_docker_container(
    self,
    docker_image,
    container_binary="/usr/bin/clickhouse",
    container_odbc_bridge_binary="/usr/bin/clickhouse-odbc-bridge",
    container_library_bridge_binary="/usr/bin/clickhouse-library-bridge",
    host_binary=None,
    host_odbc_bridge_binary=None,
    host_library_bridge_binary=None,
):
    """Get clickhouse binaries from some ClickHouse docker container."""
    docker_image = docker_image.split("docker://", 1)[-1]
    docker_container_name = str(uuid.uuid1())

    if host_binary is None:
        host_binary = os.path.join(
            tempfile.gettempdir(),
            f"{docker_image.rsplit('/', 1)[-1].replace(':', '_')}",
        )

    if host_odbc_bridge_binary is None:
        host_odbc_bridge_binary = host_binary + "_odbc_bridge"

    if host_library_bridge_binary is None:
        host_library_bridge_binary = host_binary + "_library_bridge"

    with Given(
        "I get ClickHouse server binary from docker container",
        description=f"{docker_image}",
    ):
        with Shell() as bash:
            bash.timeout = 300
            bash(
                f'set -o pipefail && docker run -d --name "{docker_container_name}" {docker_image} | tee'
            )
            bash(
                f'docker cp "{docker_container_name}:{container_binary}" "{host_binary}"'
            )
            bash(
                f'docker cp "{docker_container_name}:{container_odbc_bridge_binary}" "{host_odbc_bridge_binary}"'
            )
            bash(
                f'docker cp "{docker_container_name}:{container_library_bridge_binary}" "{host_library_bridge_binary}"'
            )
            bash(f'docker stop "{docker_container_name}"')

    with And("debug"):
        with Shell() as bash:
            bash(f"ls -la {host_binary}", timeout=300)
            bash(f"ls -la {host_odbc_bridge_binary}", timeout=300)
            bash(f"ls -la {host_library_bridge_binary}", timeout=300)

    return host_binary, host_odbc_bridge_binary, host_library_bridge_binary


@TestStep(Given)
def download_clickhouse_binary(self, path):
    """I download ClickHouse server binary using wget"""
    filename = f"{short_hash(path)}-{path.rsplit('/', 1)[-1]}"

    if not os.path.exists(f"./{filename}"):
        with Shell() as bash:
            bash.timeout = 300
            try:
                cmd = bash(f'wget --progress dot "{path}" -O {filename}')
                assert cmd.exitcode == 0
            except BaseException:
                if os.path.exists(filename):
                    os.remove(filename)
                raise

    return f"./{filename}"


@TestStep(Given)
def get_clickhouse_binary_from_deb(self, path):
    """Get clickhouse binary from deb package."""

    deb_binary_dir = path.rsplit(".deb", 1)[0]
    os.makedirs(deb_binary_dir, exist_ok=True)

    with Shell() as bash:
        bash.timeout = 300
        if not os.path.exists(f"{deb_binary_dir}/clickhouse") or not os.path.exists(
            f"{deb_binary_dir}/clickhouse-odbc-bridge"
        ):
            bash(f'ar x "{clickhouse_binary_path}" --output "{deb_binary_dir}"')
            bash(
                f'tar -vxzf "{deb_binary_dir}/data.tar.gz" ./usr/bin/clickhouse -O > "{deb_binary_dir}/clickhouse"'
            )
            bash(f'chmod +x "{deb_binary_dir}/clickhouse"')
            bash(
                f'tar -vxzf "{deb_binary_dir}/data.tar.gz" ./usr/bin/clickhouse-odbc-bridge -O > "{deb_binary_dir}/clickhouse-odbc-bridge"'
            )
            bash(f'chmod +x "{deb_binary_dir}/clickhouse-odbc-bridge"')
            bash(
                f'tar -vxzf "{deb_binary_dir}/data.tar.gz" ./usr/bin/clickhouse-library-bridge -O > "{deb_binary_dir}/clickhouse-library-bridge"'
            )
            bash(f'chmod +x "{deb_binary_dir}/clickhouse-library-bridge"')

    return (
        f"./{deb_binary_dir}/clickhouse",
        f"{deb_binary_dir}/clickhouse-odbc-bridge",
        f"{deb_binary_dir}/clickhouse-library-bridge",
    )


@TestStep(Given)
def clickhouse_binaries(self, path, odbc_bridge_path=None, library_bridge_path=None):
    """Extract clickhouse, clickhouse-odbc-bridge, clickhouse-library-bridge
    binaries from --clickhouse_binary_path."""

    if path.startswith(("http://", "https://")):
        path = download_clickhouse_binary(clickhouse_binary_path=path)

    elif path.startswith("docker://"):
        (
            path,
            odbc_bridge_path,
            library_bridge_path,
        ) = get_clickhouse_binary_from_docker_container(docker_image=path)

    if path.endswith(".deb"):
        path, odbc_bridge_path, library_bridge_path = get_clickhouse_binary_from_deb(
            path=path
        )

    if odbc_bridge_path is None:
        odbc_bridge_path = path + "-odbc-bridge"

    if library_bridge_path is None:
        library_bridge_path = path + "-library-bridge"

    path = os.path.abspath(path)
    odbc_bridge_path = os.path.abspath(odbc_bridge_path)
    library_bridge_path = os.path.abspath(library_bridge_path)

    with Shell() as bash:
        bash(f"chmod +x {path}", timeout=300)
        bash(f"chmod +x {odbc_bridge_path}", timeout=300)
        bash(f"chmod +x {library_bridge_path}", timeout=300)

    return path, odbc_bridge_path, library_bridge_path


@TestStep(Given)
def temporary_file(self, mode, dir=None, prefix=None, suffix=None):
    """Create temporary named file."""
    with tempfile.NamedTemporaryFile(
        mode,
        dir=dir,
        prefix=prefix,
        suffix=suffix,
        delete=(not testflows.settings.debug),
    ) as log:
        yield log


@TestStep(Given)
def shell(self):
    """Create iteractive system bash shell."""
    with Shell() as bash:
        yield bash


@TestStep
def runner_opts(self):
    """Return runner script options."""
    return (
        f" --binary {self.context.binary}"
        + f" --odbc-bridge-binary {self.context.odbc_bridge_binary}"
        + f" --library-bridge-binary {self.context.library_bridge_binary}"
    )


@TestStep(Given)
def get_all_tests(self):
    """Collect a list of tests using pytest --setup-plan command."""

    with tempfile.NamedTemporaryFile(
        "r", dir=current_dir(), delete=(not testflows.settings.debug)
    ) as out:
        command = (
            f"set -o pipefail && ./runner {runner_opts()} -- --setup-plan "
            "| grep -F '::' | sed -r 's/ \(fixtures used:.*//g; s/^ *//g; s/ *$//g' "
            f"| grep -v -F 'SKIPPED' | sort --unique > {os.path.basename(out.name)}"
        )

        with shell() as bash:
            cmd = bash(command, timeout=100)
            assert (
                cmd.exitcode == 0
            ), f"non-zero exitcode {cmd.exitcode} when trying to collect all tests"

        tests = []
        for line in out.readlines():
            if not line.startswith("test_"):
                continue
            tests.append(line.strip())

    assert tests, "no tests found"
    return sorted(tests)


@TestStep(Given)
def get_all_parallel_skip_tests(self):
    """Get all tests that can't be run in parallel defined in parallel_skip.json file."""
    tests = []

    with open(os.path.join(current_dir(), "parallel_skip.json"), "r") as fd:
        for test in json.load(fd):
            tests.append(test)

    return sorted(tests)


@TestStep(Given)
def launch_runner(self, run_id, tests, run_in_parallel=None):
    report_timeout = 60

    with By("creating temporary report log file"):
        log = temporary_file(mode="r", suffix=".pytest.jsonl", dir=current_dir())

    tests = " ".join([shlex.quote(test) for test in sorted(tests)])

    command = define(
        "command",
        "./runner"
        + runner_opts()
        + f" -t {tests}"
        + (f" --parallel {run_in_parallel}" if run_in_parallel is not None else "")
        + " --"
        + " -rfEps"
        + f" --run-id={run_id} --color=no --durations=0"
        + f" --report-log={os.path.basename(log.name)} > runner.log 2>&1",
    )

    with And("launch runner"):
        proc = sysprocess(command=command)

        if proc.poll() is not None:
            if proc.returncode != 0:
                fail(f"failed to start, exitcode: {proc.returncode}")

    yield proc, log


@TestModule
@ArgumentParser(argparser)
def regression(self, clickhouse_binary_path, run_in_parallel=5):
    """Run ClickHouse pytest integration tests."""

    with Given("I get clickhouse binaries"):
        (
            self.context.binary,
            self.context.odbc_bridge_binary,
            self.context.library_bridge_binary,
        ) = clickhouse_binaries(path=clickhouse_binary_path)

    with And("I collect all tests"):
        self.context.all_tests = get_all_tests()

    with And("I collect all parallel skip tests"):
        self.context.parallel_skip_tests = get_all_parallel_skip_tests()

    with And("I create parallel and non parallel tests list"):
        self.context.non_parallel_tests = [
            test
            for test in self.context.parallel_skip_tests
            if test in self.context.all_tests
        ]
        self.context.parallel_tests = [
            test
            for test in self.context.all_tests
            if not (test in self.context.parallel_skip_tests)
        ]

    with And("I launch the runner"):
        runner, log = launch_runner(
            run_id=0,
            tests=self.context.parallel_tests[:100],
            run_in_parallel=run_in_parallel,
        )

    try:
        while True:
            pos = log.tell()
            line = log.readline()

            if line:
                if not line.endswith("\n"):
                    log.seek(pos)
                    time.sleep(1)
                    continue

                with catch(
                    Exception, raising=ValueError(f"failed to parse line: {line}")
                ):
                    entry = json.loads(line)

                if entry["$report_type"] == "TestReport":
                    # skip setup and teardown entries unless they have non-passing outcome
                    if entry["when"] != "call" and entry["outcome"] == "passed":
                        continue

                    # create scenario for each test call or non-passing setup or teardown outcome
                    with Scenario(
                        name=entry["nodeid"]
                        + ((":" + entry["when"]) if entry["when"] != "call" else ""),
                        description=f"location: {entry['location']}",
                        # attributes=Attributes(entry["keywords"]), # FIXME
                        start_time=entry["start"],
                        test_time=(entry["start"] - entry["stop"]),
                    ):
                        for section in entry["sections"]:
                            if section and section[0] == "Captured log call":
                                message(section[1])

                        if entry["outcome"].lower() == "passed":
                            ok("success")

                        fail(entry["outcome"])

            if runner.poll() is not None:
                break

            time.sleep(1)

        if runner.returncode != 0:
            fail(f"runner exited with non-zero exitcode: {runner.returncode}")

    finally:
        with Finally("dump runner stdout/stderr"):
            for line in runner.stdout.readlines():
                message(line, stream="runner")


if main():
    regression()
