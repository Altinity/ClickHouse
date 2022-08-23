#!/usr/bin/env python3

from collections import defaultdict
import csv
import json
import logging
import os
import shutil
import subprocess
import time


CLICKHOUSE_BINARY_PATH = "usr/bin/clickhouse"
CLICKHOUSE_ODBC_BRIDGE_BINARY_PATH = "usr/bin/clickhouse-odbc-bridge"
CLICKHOUSE_LIBRARY_BRIDGE_BINARY_PATH = "usr/bin/clickhouse-library-bridge"

MAX_TIME_IN_SANDBOX = 20 * 60  # 20 minutes
TASK_TIMEOUT = 8 * 60 * 60  # 8 hours


def clear_ip_tables_and_restart_daemons():
    logging.info(
        "Dump iptables after run %s",
        subprocess.check_output("sudo iptables -L", shell=True),
    )
    try:
        logging.info("Killing all alive docker containers")
        subprocess.check_output(
            "timeout -s 9 10m docker kill $(docker ps -q)", shell=True
        )
    except subprocess.CalledProcessError as err:
        logging.info("docker kill excepted: " + str(err))

    try:
        logging.info("Removing all docker containers")
        subprocess.check_output(
            "timeout -s 9 10m docker rm $(docker ps -a -q) --force", shell=True
        )
    except subprocess.CalledProcessError as err:
        logging.info("docker rm excepted: " + str(err))

    # don't restart docker if it's disabled
    if os.environ.get("CLICKHOUSE_TESTS_RUNNER_RESTART_DOCKER", "1") == "1":
        try:
            logging.info("Stopping docker daemon")
            subprocess.check_output("service docker stop", shell=True)
        except subprocess.CalledProcessError as err:
            logging.info("docker stop excepted: " + str(err))

        try:
            for i in range(200):
                try:
                    logging.info("Restarting docker %s", i)
                    subprocess.check_output("service docker start", shell=True)
                    subprocess.check_output("docker ps", shell=True)
                    break
                except subprocess.CalledProcessError as err:
                    time.sleep(0.5)
                    logging.info("Waiting docker to start, current %s", str(err))
            else:
                raise Exception("Docker daemon doesn't responding")
        except subprocess.CalledProcessError as err:
            logging.info("Can't reload docker: " + str(err))

    iptables_iter = 0
    try:
        for i in range(1000):
            iptables_iter = i
            # when rules will be empty, it will raise exception
            subprocess.check_output("sudo iptables -D DOCKER-USER 1", shell=True)
    except subprocess.CalledProcessError as err:
        logging.info(
            "All iptables rules cleared, "
            + str(iptables_iter)
            + "iterations, last error: "
            + str(err)
        )


class ClickhouseTestFlowsTestsRunner:
    def __init__(self, result_path, params):
        self.result_path = result_path
        self.params = params
        self.start_time = time.time()
        self.disable_net_host = (
            "disable_net_host" in self.params and self.params["disable_net_host"]
        )
        self.image_versions = self.params["docker_images_with_versions"]

    def path(self):
        return self.result_path

    def base_path(self):
        return os.path.join(str(self.result_path), "../")

    def _can_run_with(self, path, opt):
        with open(path, "r") as script:
            for line in script:
                if opt in line:
                    return True
        return False

    def _install_clickhouse(self, debs_path):
        for package in (
            "clickhouse-common-static_",
            "clickhouse-server_",
            "clickhouse-client",
            "clickhouse-common-static-dbg_",
        ):  # order matters
            logging.info("Installing package %s", package)
            for f in os.listdir(debs_path):
                if package in f:
                    full_path = os.path.join(debs_path, f)
                    logging.info("Package found in %s", full_path)
                    log_name = "install_" + f + ".log"
                    log_path = os.path.join(str(self.path()), log_name)
                    with open(log_path, "w") as log:
                        cmd = "dpkg -x {} .".format(full_path)
                        logging.info("Executing installation cmd %s", cmd)
                        retcode = subprocess.Popen(
                            cmd, shell=True, stderr=log, stdout=log
                        ).wait()
                        if retcode == 0:
                            logging.info("Installation of %s successful", full_path)
                        else:
                            raise Exception("Installation of %s failed", full_path)
                    break
            else:
                raise Exception("Package with {} not found".format(package))

        logging.info("All packages installed")
        os.chmod(CLICKHOUSE_BINARY_PATH, 0o777)
        os.chmod(CLICKHOUSE_ODBC_BRIDGE_BINARY_PATH, 0o777)
        os.chmod(CLICKHOUSE_LIBRARY_BRIDGE_BINARY_PATH, 0o777)
        shutil.copy(
            CLICKHOUSE_BINARY_PATH, os.getenv("CLICKHOUSE_TESTS_SERVER_BIN_PATH")
        )
        shutil.copy(
            CLICKHOUSE_ODBC_BRIDGE_BINARY_PATH,
            os.getenv("CLICKHOUSE_TESTS_ODBC_BRIDGE_BIN_PATH"),
        )
        shutil.copy(
            CLICKHOUSE_LIBRARY_BRIDGE_BINARY_PATH,
            os.getenv("CLICKHOUSE_TESTS_LIBRARY_BRIDGE_BIN_PATH"),
        )

    def _compress_logs(self, dir, relpaths, result_path):
        subprocess.check_call(  # STYLE_CHECK_ALLOW_SUBPROCESS_CHECK_CALL
            "tar czf {} -C {} {}".format(result_path, dir, " ".join(relpaths)),
            shell=True,
        )

    def _get_runner_opts(self):
        result = []
        if self.disable_net_host:
            result.append("--disable-net-host")
        return " ".join(result)

    @staticmethod
    def get_images_names():
        return [
            "altinityinfra/testflows-runner",
        ]

    def get_image_version(self, name: str):
        if name in self.image_versions:
            return self.image_versions[name]
        logging.warn(
            "Cannot find image %s in params list %s", name, self.image_versions
        )
        return "latest"

    def get_image_with_version(self, name):
        if name in self.image_versions:
            return name + ":" + self.image_versions[name]
        logging.warn(
            "Cannot find image %s in params list %s", name, self.image_versions
        )
        if ":" not in name:
            return name + ":latest"
        return name

    def _get_runner_image_cmd(self, repo_path):
        image_cmd = ""
        if self._can_run_with(
            os.path.join(repo_path, "tests/testflows", "runner"),
            "--docker-image-version",
        ):
            for img in self.get_images_names():
                if img == "altinityinfra/testflows-runner":
                    runner_version = self.get_image_version(img)
                    logging.info(
                        "Can run with custom docker image version %s", runner_version
                    )
                    image_cmd += " --docker-image-version={} ".format(runner_version)
                else:
                    logging.info(
                        f"Cannot run with custom docker compose image version :( for {img}"
                    )
        else:
            image_cmd = ""
            logging.info("Cannot run with custom docker image version :(")
        return image_cmd

    def run_impl(self, repo_path, build_path):
        self._install_clickhouse(build_path)
        logging.info(
            "Dump iptables before run %s",
            subprocess.check_output("sudo iptables -L", shell=True),
        )

        image_cmd = self._get_runner_image_cmd(repo_path)

        log_path = os.path.join(repo_path, "tests/testflows", "run.log")
        test_log_path = os.path.join(repo_path, "tests/testflows", "test.log")

        cmd = (
            f"(set -o pipefail && cd {repo_path}/tests/testflows && timeout -s 9 10h "
            f"./runner {self._get_runner_opts()} {image_cmd} | tee {log_path})"
        )

        logging.info("Executing cmd: %s", cmd)
        retcode = subprocess.Popen(
            cmd, shell=True, stderr=subprocess.STDOUT, stdout=subprocess.STDOUT
        ).wait()
        if retcode == 0:
            logging.info("Run successfully")
        else:
            logging.info("Some tests failed")

        shutil.copy(log_path, os.path.join(str(self.path()), "run.log"))
        shutil.copy(test_log_path, os.path.join(str(self.path()), "raw.log"))


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")

    repo_path = os.environ.get("CLICKHOUSE_TESTS_REPO_PATH")
    build_path = os.environ.get("CLICKHOUSE_TESTS_BUILD_PATH")
    result_path = os.environ.get("CLICKHOUSE_TESTS_RESULT_PATH")
    params_path = os.environ.get("CLICKHOUSE_TESTS_JSON_PARAMS_PATH")

    params = json.loads(open(params_path, "r").read())
    runner = ClickhouseTestFlowsTestsRunner(result_path, params)

    logging.info("Running tests")
    runner.run_impl(repo_path, build_path)
    logging.info("Tests finished")
