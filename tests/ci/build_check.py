#!/usr/bin/env python3

import subprocess
import logging
import json
import os
import sys
import time
from typing import List, Tuple

from ci_config import CI_CONFIG, BuildConfig
from commit_status_helper import get_commit_filtered_statuses, get_commit
from docker_pull_helper import get_image_with_version
from env_helper import (
    GITHUB_JOB,
    IMAGES_PATH,
    REPO_COPY,
    S3_BUILDS_BUCKET,
    S3_DOWNLOAD,
    TEMP_PATH,
    CLICKHOUSE_STABLE_VERSION_SUFFIX,
)
from get_robot_token import get_best_robot_token
from github_helper import GitHub
from pr_info import PRInfo
from s3_helper import S3Helper
from tee_popen import TeePopen
from version_helper import (
    ClickHouseVersion,
    Git,
    get_version_from_repo,
    update_version_local,
)

IMAGE_NAME = "altinityinfra/binary-builder"
BUILD_LOG_NAME = "build_log.log"


def _can_export_binaries(build_config: BuildConfig) -> bool:
    if build_config["package_type"] != "deb":
        return False
    if build_config["sanitizer"] != "":
        return True
    if build_config["build_type"] != "":
        return True
    return False


def get_packager_cmd(
    build_config: BuildConfig,
    packager_path: str,
    output_path: str,
    build_version: str,
    image_version: str,
    official: bool,
) -> str:
    package_type = build_config["package_type"]
    comp = build_config["compiler"]
    cmake_flags = "-DENABLE_CLICKHOUSE_SELF_EXTRACTING=1"
    cmd = (
        f"cd {packager_path} && CMAKE_FLAGS='{cmake_flags}' ./packager --output-dir={output_path} "
        f"--package-type={package_type} --compiler={comp}"
    )

    if build_config["build_type"]:
        cmd += f" --build-type={build_config['build_type']}"
    if build_config["sanitizer"]:
        cmd += f" --sanitizer={build_config['sanitizer']}"
    if build_config["tidy"] == "enable":
        cmd += " --clang-tidy"

    cmd += " --cache=ccache"
    cmd += " --s3-rw-access"
    cmd += f" --s3-bucket={S3_BUILDS_BUCKET}"

    if "additional_pkgs" in build_config and build_config["additional_pkgs"]:
        cmd += " --additional-pkgs"

    cmd += f" --docker-image-version={image_version}"
    cmd += f" --version={build_version}"

    if _can_export_binaries(build_config):
        cmd += " --with-binaries=tests"

    if official:
        cmd += " --official"

    return cmd


def build_clickhouse(
    packager_cmd: str, logs_path: str, build_output_path: str
) -> Tuple[str, bool]:
    build_log_path = os.path.join(logs_path, BUILD_LOG_NAME)
    success = False
    with TeePopen(packager_cmd, build_log_path) as process:
        retcode = process.wait()
        if os.path.exists(build_output_path):
            build_results = os.listdir(build_output_path)
        else:
            build_results = []

        if retcode == 0:
            if len(build_results) > 0:
                success = True
                logging.info("Built successfully")
            else:
                logging.info(
                    "Success exit code, but no build artifacts => build failed"
                )
        else:
            logging.info("Build failed")
    return build_log_path, success


def check_for_success_run(
    s3_helper: S3Helper,
    s3_prefix: str,
    build_name: str,
    build_config: BuildConfig,
) -> None:
    # the final empty argument is necessary for distinguish build and build_suffix
    logged_prefix = os.path.join(S3_BUILDS_BUCKET, s3_prefix, "")
    logging.info("Checking for artifacts in %s", logged_prefix)
    try:
        # Performance artifacts are now part of regular build, so we're safe
        build_results = s3_helper.list_prefix(s3_prefix)
    except Exception as ex:
        logging.info("Got exception while listing %s: %s\nRerun", logged_prefix, ex)
        return

    if build_results is None or len(build_results) == 0:
        logging.info("Nothing found in %s, rerun", logged_prefix)
        return

    logging.info("Some build results found:\n%s", build_results)
    build_urls = []
    log_url = ""
    for url in build_results:
        url_escaped = url.replace("+", "%2B").replace(" ", "%20")
        if BUILD_LOG_NAME in url:
            log_url = f"{S3_DOWNLOAD}/{S3_BUILDS_BUCKET}/{url_escaped}"
        else:
            build_urls.append(f"{S3_DOWNLOAD}/{S3_BUILDS_BUCKET}/{url_escaped}")
    if not log_url:
        # log is uploaded the last, so if there's no log we need to rerun the build
        return

    success = len(build_urls) > 0
    create_json_artifact(
        TEMP_PATH,
        build_name,
        log_url,
        build_urls,
        build_config,
        0,
        success,
    )
    # Fail build job if not successeded
    if not success:
        sys.exit(1)
    else:
        sys.exit(0)


def create_json_artifact(
    temp_path: str,
    build_name: str,
    log_url: str,
    build_urls: List[str],
    build_config: BuildConfig,
    elapsed: int,
    success: bool,
) -> None:
    subprocess.check_call(
        f"echo 'BUILD_URLS=build_urls_{build_name}' >> $GITHUB_ENV", shell=True
    )

    result = {
        "log_url": log_url,
        "build_urls": build_urls,
        "build_config": build_config,
        "elapsed_seconds": elapsed,
        "status": success,
        "job_name": GITHUB_JOB,
    }

    json_name = "build_urls_" + build_name + ".json"

    print(f"Dump json report {result} to {json_name} with env build_urls_{build_name}")

    with open(os.path.join(temp_path, json_name), "w", encoding="utf-8") as build_links:
        json.dump(result, build_links)


def get_release_or_pr(pr_info: PRInfo, version: ClickHouseVersion) -> Tuple[str, str]:
    "Return prefixes for S3 artifacts paths"
    # FIXME performance
    # performance builds are havily relies on a fixed path for artifacts, that's why
    # we need to preserve 0 for anything but PR number
    # It should be fixed in performance-comparison image eventually
    # For performance tests we always set PRs prefix
    performance_pr = "PRs/0"
    if "release" in pr_info.labels or "release-lts" in pr_info.labels:
        # for release pull requests we use branch names prefixes, not pr numbers
        return pr_info.head_ref, performance_pr
    if pr_info.number == 0:
        # for pushes to master - major version
        return f"{version.major}.{version.minor}", performance_pr
    # PR number for anything else
    pr_number = f"PRs/{pr_info.number}"
    return pr_number, pr_number


def upload_master_static_binaries(
    pr_info: PRInfo,
    build_config: BuildConfig,
    s3_helper: S3Helper,
    build_output_path: str,
) -> None:
    """Upload binary artifacts to a static S3 links"""
    static_binary_name = build_config.get("static_binary_name", False)
    if pr_info.number != 0:
        return
    elif not static_binary_name:
        return
    elif pr_info.base_ref != "master":
        return

    s3_path = "/".join((pr_info.base_ref, static_binary_name, "clickhouse"))
    binary = os.path.join(build_output_path, "clickhouse")
    url = s3_helper.upload_build_file_to_s3(binary, s3_path)
    print(f"::notice ::Binary static URL: {url}")


def mark_failed_reports_pending(build_name: str, sha: str) -> None:
    try:
        gh = GitHub(get_best_robot_token())
        commit = get_commit(gh, sha)
        statuses = get_commit_filtered_statuses(commit)
        report_status = [
            name
            for name, builds in CI_CONFIG["builds_report_config"].items()
            if build_name in builds
        ][0]
        for status in statuses:
            if status.context == report_status and status.state in ["failure", "error"]:
                logging.info(
                    "Commit already have failed status for '%s', setting it to 'pending'",
                    report_status,
                )
                commit.create_status(
                    "pending", status.url, "Set to pending on rerun", report_status
                )
    except:  # we do not care about any exception here
        logging.info("Failed to get or mark the reports status as pending, continue")


def main():
    logging.basicConfig(level=logging.INFO)

    build_name = sys.argv[1]

    build_config = CI_CONFIG["build_config"][build_name]

    if not os.path.exists(TEMP_PATH):
        os.makedirs(TEMP_PATH)

    pr_info = PRInfo()

    logging.info("Repo copy path %s", REPO_COPY)

    s3_helper = S3Helper()

    version = get_version_from_repo(git=Git(True))
    release_or_pr, performance_pr = get_release_or_pr(pr_info, version)

    s3_path_prefix = "/".join((release_or_pr, pr_info.sha, build_name))
    # FIXME performance
    s3_performance_path = "/".join(
        (performance_pr, pr_info.sha, build_name, "performance.tar.zst")
    )

    # If this is rerun, then we try to find already created artifacts and just
    # put them as github actions artifact (result)
    check_for_success_run(s3_helper, s3_path_prefix, build_name, build_config)

    # If it's a latter running, we need to mark possible failed status
    mark_failed_reports_pending(build_name, pr_info.sha)

    docker_image = get_image_with_version(IMAGES_PATH, IMAGE_NAME)
    image_version = docker_image.version

    logging.info("Got version from repo %s", version.string)

    if "official" in build_config:
        official_flag = build_config["official"]

    official_flag = True
    version._flavour = version_type = CLICKHOUSE_STABLE_VERSION_SUFFIX
    # TODO (vnemkov): right now we'll use simplified version management:
    # only update git hash and explicitly set stable version suffix.
    # official_flag = pr_info.number == 0
    # version_type = "testing"
    # if "release" in pr_info.labels or "release-lts" in pr_info.labels:
    #     version_type = CLICKHOUSE_STABLE_VERSION_SUFFIX
    #     official_flag = True

    update_version_local(version, version_type)

    logging.info(f"Updated local files with version : {version.string} / {version.describe}")

    logging.info("Build short name %s", build_name)

    build_output_path = os.path.join(TEMP_PATH, build_name)
    if not os.path.exists(build_output_path):
        os.makedirs(build_output_path)

    packager_cmd = get_packager_cmd(
        build_config,
        os.path.join(REPO_COPY, "docker/packager"),
        build_output_path,
        version.string,
        image_version,
        official_flag,
    )

    logging.info("Going to run packager with %s", packager_cmd)

    logs_path = os.path.join(TEMP_PATH, "build_log")
    if not os.path.exists(logs_path):
        os.makedirs(logs_path)

    start = time.time()
    log_path, success = build_clickhouse(packager_cmd, logs_path, build_output_path)
    elapsed = int(time.time() - start)
    subprocess.check_call(
        f"sudo chown -R ubuntu:ubuntu {build_output_path}", shell=True
    )
    logging.info("Build finished with %s, log path %s", success, log_path)

    # FIXME performance
    performance_urls = []
    performance_path = os.path.join(build_output_path, "performance.tar.zst")
    if os.path.exists(performance_path):
        performance_urls.append(
            s3_helper.upload_build_file_to_s3(performance_path, s3_performance_path)
        )
        logging.info(
            "Uploaded performance.tar.zst to %s, now delete to avoid duplication",
            performance_urls[0],
        )
        os.remove(performance_path)

    build_urls = (
        s3_helper.upload_build_folder_to_s3(
            build_output_path,
            s3_path_prefix,
            keep_dirs_in_s3_path=False,
            upload_symlinks=False,
        )
        + performance_urls
    )
    logging.info("Got build URLs %s", build_urls)

    print("::notice ::Build URLs: {}".format("\n".join(build_urls)))

    if os.path.exists(log_path):
        log_url = s3_helper.upload_build_file_to_s3(
            log_path, s3_path_prefix + "/" + os.path.basename(log_path)
        )
        logging.info("Log url %s", log_url)
    else:
        logging.info("Build log doesn't exist")

    print(f"::notice ::Log URL: {log_url}")

    create_json_artifact(
        TEMP_PATH, build_name, log_url, build_urls, build_config, elapsed, success
    )

    upload_master_static_binaries(pr_info, build_config, s3_helper, build_output_path)
    # Fail build job if not successeded
    if not success:
        sys.exit(1)


if __name__ == "__main__":
    main()
