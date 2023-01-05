#!/usr/bin/env python3
import sys
import os
import logging
from env_helper import GPG_BINARY_SIGNING_KEY, GPG_BINARY_SIGNING_PASSPHRASE, TEMP_PATH, REPO_COPY, REPORTS_PATH
from github import Github
import subprocess
import hashlib
from s3_helper import S3Helper
from get_robot_token import get_best_robot_token
from pr_info import PRInfo
from build_download_helper import download_builds_filter
from rerun_helper import RerunHelper
from docker_pull_helper import get_images_with_versions
import hashlib


CHECK_NAME = "Sign release (actions)"

def hash_file(file_path):
    BLOCK_SIZE = 65536 # The size of each read from the file

    file_hash = hashlib.sha512() # Create the hash object, can use something other than `.sha256()` if you wish
    with open(file_path, 'rb') as f: # Open the file to read it's bytes
        fb = f.read(BLOCK_SIZE) # Read from the file. Take in the amount declared above
        while len(fb) > 0: # While there is still data being read from the file
            file_hash.update(fb) # Update the hash
            fb = f.read(BLOCK_SIZE) # Read the next block from the file

    hash_file_path = file_path + '.sha512'
    with open(hash_file_path, 'x') as f:
        digest = file_hash.hexdigest()
        f.write(digest)
        print(f'Hashed {file_path}: {digest}')

    return hash_file_path

def sign_file(file_path):
    priv_key_file_path = 'priv.key'
    with open(priv_key_file_path, 'x') as f:
        f.write(GPG_BINARY_SIGNING_KEY)

    os.system(f'echo {GPG_BINARY_SIGNING_PASSPHRASE} | gpg --bash --import {priv_key_file_path}')
    os.system(f'gpg --pinentry-mode=loopback --batch --yes --passphrase {GPG_BINARY_SIGNING_PASSPHRASE} --sign {file_path}')
    print(f"Signed {file_path}")
    os.remove(priv_key_file_path)

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
        hashed_file_path = hash_file(full_path)
        sign_file(hashed_file_path)

if __name__ == "__main__":
    main()
