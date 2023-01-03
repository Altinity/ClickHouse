#!/usr/bin/env python3
import sys
import os
import logging
from env_helper import GPG_BINARY_SIGNING_KEY, TEMP_PATH, REPO_COPY, REPORTS_PATH
from github import Github
import subprocess
import hashlib
from s3_helper import S3Helper
from get_robot_token import get_best_robot_token
from pr_info import PRInfo
from build_download_helper import download_builds_filter
from rerun_helper import RerunHelper
from docker_pull_helper import get_images_with_versions

CHECK_NAME = "Sign release (actions)"

def main():
    temp_path = TEMP_PATH
    repo_path = REPO_COPY
    reports_path = REPORTS_PATH

    pr_info = PRInfo()

    gh = Github(get_best_robot_token())

    rerun_helper = RerunHelper(gh, pr_info, CHECK_NAME)
    if rerun_helper.is_already_finished_by_status():
        logging.info("Check is already finished according to github status, exiting")
        sys.exit(0)

    packages_path = os.path.join(temp_path, "packages")
    if not os.path.exists(packages_path):
        os.makedirs(packages_path)

    # def url_filter(url):
    #     return url.endswith(".deb") and (
    #         "clickhouse-common-static_" in url or "clickhouse-server_" in url
    #     )

    download_builds_filter(CHECK_NAME, reports_path, packages_path)

    for f in os.listdir(packages_path):
        full_path = os.path.join(packages_path, f)
        print(f"aaa: {full_path}")

if __name__ == "__main__":
    main()
