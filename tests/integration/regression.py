import os
import uuid
import json
import tempfile

import testflows.settings

from testflows.core import *
from testflows.connect import Shell

# FIXME: add pre-pull
# FIXME: add docker image rebuild

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
        default=None
    )


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
    container_clickhouse_binary_path="/usr/bin/clickhouse",
    host_clickhouse_binary_path=None,
):
    """Get clickhouse-server binary from some ClickHouse docker container."""
    docker_image = docker_image.split("docker://", 1)[-1]
    docker_container_name = str(uuid.uuid1())

    if host_clickhouse_binary_path is None:
        host_clickhouse_binary_path = os.path.join(
            tempfile.gettempdir(),
            f"{docker_image.rsplit('/', 1)[-1].replace(':', '_')}",
        )

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
                f'docker cp "{docker_container_name}:{container_clickhouse_binary_path}" "{host_clickhouse_binary_path}"'
            )
            bash(f'docker stop "{docker_container_name}"')

    with And("debug"):
        with Shell() as bash:
            bash(f"ls -la {host_clickhouse_binary_path}", timeout=300)

    return host_clickhouse_binary_path


@TestStep(Given)
def download_clickhouse_binary(self, clickhouse_binary_path):
    """I download ClickHouse server binary using wget"""
    filename = f"{short_hash(clickhouse_binary_path)}-{clickhouse_binary_path.rsplit('/', 1)[-1]}"

    if not os.path.exists(f"./{filename}"):
        with Shell() as bash:
            bash.timeout = 300
            try:
                cmd = bash(
                    f'wget --progress dot "{clickhouse_binary_path}" -O {filename}'
                )
                assert cmd.exitcode == 0
            except BaseException:
                if os.path.exists(filename):
                    os.remove(filename)
                raise

    return f"./{filename}"


@TestStep(Given)
def get_clickhouse_binary_from_deb(self, clickhouse_binary_path):
    """Get clickhouse binary from deb package."""

    deb_binary_dir = clickhouse_binary_path.rsplit(".deb", 1)[0]
    os.makedirs(deb_binary_dir, exist_ok=True)

    with Shell() as bash:
        bash.timeout = 300
        if not os.path.exists(
            f"{deb_binary_dir}/clickhouse"
        ) or not os.path.exists(
            f"{deb_binary_dir}/clickhouse-odbc-bridge"
        ):
            bash(
                f'ar x "{clickhouse_binary_path}" --output "{deb_binary_dir}"'
            )
            bash(
                f'tar -vxzf "{deb_binary_dir}/data.tar.gz" ./usr/bin/clickhouse -O > "{deb_binary_dir}/clickhouse"'
            )
            bash(f'chmod +x "{deb_binary_dir}/clickhouse"')
            bash(
                f'tar -vxzf "{deb_binary_dir}/data.tar.gz" ./usr/bin/clickhouse-odbc-bridge -O > "{deb_binary_dir}/clickhouse-odbc-bridge"'
            )
            bash(f'chmod +x "{deb_binary_dir}/clickhouse-odbc-bridge"')

    return f"./{deb_binary_dir}/clickhouse"


@TestStep(Given)
def clickhouse_binary(self, clickhouse_binary_path):
    """Extract clickhouse binary from --clickhouse_binary_path."""

    if clickhouse_binary_path.startswith(("http://", "https://")):
        clickhouse_binary_path = download_clickhouse_binary(clickhouse_binary_path=clickhouse_binary_path)

    elif clickhouse_binary_path.startswith("docker://"):
        clickhouse_binary_path = get_clickhouse_binary_from_docker_container(
            clickhouse_binary_path=clickhouse_binary_path
        )

    if clickhouse_binary_path.endswith(".deb"):
        clickhouse_binary_path = get_clickhouse_binary_from_deb(clickhouse_binary_path=clickhouse_binary_path)

    clickhouse_binary_path = os.path.abspath(clickhouse_binary_path)

    with Shell() as bash:
        bash(f"chmod +x {clickhouse_binary_path}", timeout=300)

    return clickhouse_binary_path


@TestStep(Given)
def temporary_file(self, mode, dir=None, prefix=None, suffix=None):
    """Create temporary named file."""
    with tempfile.NamedTemporaryFile(mode, dir=dir, prefix=prefix, suffix=suffix, delete=(not testflows.settings.debug)) as log:
        yield log


@TestStep(Given)
def shell(self):
    """Create iteractive system bash shell."""
    with Shell() as bash:
        yield bash


@TestStep(Given)
def launch_runner(self, clickhouse_binary_path, run_in_parallel=None):
    report_timeout = 60

    with By("creating temporary report log file"):
        log = temporary_file(mode="r", suffix=".pytest.jsonl", dir=current_dir())

    command = define("command",
        "./runner" +
        (f" -n {run_in_parallel}" if run_in_parallel is not None else "") +
        f" --binary {clickhouse_binary_path}" +
        " 'test_ssl_cert_authentication'" +
        " --" +
        f" --report-log={os.path.basename(log.name)}"
    )

    with And("execute runner"):
        bash = shell()

        with bash(command, asyncronous=True, name="runner") as runner:
            runner.readlines()

            if runner.exitcode is not None:
                report_timeout = 1

            yield runner, log

            runner.readlines()


@TestModule
@ArgumentParser(argparser)
def regression(self, clickhouse_binary_path, run_in_parallel=None):
    """Run ClickHouse pytest integration tests."""
    lineno = 0

    with Given("clickhouse binary path"):
        clickhouse_binary_path = clickhouse_binary(clickhouse_binary_path=clickhouse_binary_path)

    with And("launch runner"):
        runner, log = launch_runner(
            clickhouse_binary_path=clickhouse_binary_path,
            run_in_parallel=run_in_parallel
        )

    while True:
        runner.readlines(timeout=1)

        for line in log.readlines():
            with By(f"parsing json line: {lineno}") if not testflows.settings.debug else NullStep():
                if testflows.settings.debug:
                    debug(line)
                entry = json.loads(line)
                lineno +=1

            if entry["$report_type"] == "TestReport":
                # skip setup and teardown entries unless they have non-passing outcome
                if entry["when"] != "call" and entry["outcome"] == "passed":
                    continue

                # create scenario for each test call or non-passing setup or teardown outcome
                with Scenario(
                    name=entry["nodeid"] + ((":" + entry["when"]) if entry["when"] != "call" else "") ,
                    description=f"location: {entry['location']}",
                    attributes=Attributes(entry["keywords"]),
                    start_time=entry["start"],
                    test_time=(entry["start"] - entry["stop"])
                ):
                    for section in entry["sections"]:
                        if section and section[0] == "Captured log call":
                            message(section[1])

                    if entry["outcome"].lower() == "passed":
                        ok("success")

                    fail(entry["outcome"])

        if runner.exitcode is not None:
            break

    debug(f"runner exitcode: {runner.exitcode}")


if main():
    regression()
