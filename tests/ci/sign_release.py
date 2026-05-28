#!/usr/bin/env python3
import hashlib
import logging
import os
import shutil
import subprocess
import sys
from pathlib import Path

from build_download_helper import download_builds_filter
from env_helper import TEMP_PATH, REPO_COPY, REPORT_PATH
from pr_info import PRInfo
from report import FAIL, OK, FAILURE, SUCCESS, JobReport, TestResult
from s3_helper import S3Helper
from stopwatch import Stopwatch

GPG_BINARY_SIGNING_KEY = os.getenv("GPG_BINARY_SIGNING_KEY")
GPG_BINARY_SIGNING_PASSPHRASE = os.getenv("GPG_BINARY_SIGNING_PASSPHRASE")

CHECK_NAME = os.getenv("CHECK_NAME", "Sign release")
GPG_HOME_PATH = Path(TEMP_PATH) / "gpg_home"
SIGNING_PUBKEY_PATH = Path(TEMP_PATH) / "signing_pubkey.asc"


def hash_file(file_path):
    BLOCK_SIZE = 65536 # The size of each read from the file

    file_hash = hashlib.sha256() # Create the hash object, can use something other than `.sha256()` if you wish
    with open(file_path, 'rb') as f: # Open the file to read it's bytes
        fb = f.read(BLOCK_SIZE) # Read from the file. Take in the amount declared above
        while len(fb) > 0: # While there is still data being read from the file
            file_hash.update(fb) # Update the hash
            fb = f.read(BLOCK_SIZE) # Read the next block from the file

    hash_file_path = file_path + '.sha256'
    with open(hash_file_path, 'x') as f:
        digest = file_hash.hexdigest()
        f.write(digest)
        print(f'Hashed {file_path}: {digest}')

    return hash_file_path


def import_signing_key(gpg_home_path):
    if not GPG_BINARY_SIGNING_KEY:
        raise RuntimeError("GPG_BINARY_SIGNING_KEY is not set")
    if not GPG_BINARY_SIGNING_PASSPHRASE:
        raise RuntimeError("GPG_BINARY_SIGNING_PASSPHRASE is not set")

    gpg_home_path.mkdir(mode=0o700, parents=True, exist_ok=True)
    gpg_home_path.chmod(0o700)
    subprocess.run(
        ["gpg", "--homedir", str(gpg_home_path), "--batch", "--import"],
        input=GPG_BINARY_SIGNING_KEY.encode(),
        check=True,
    )


def export_public_key(gpg_home_path, out_file_path):
    with open(out_file_path, "wb") as out_file:
        subprocess.run(
            ["gpg", "--homedir", str(gpg_home_path), "--armor", "--export"],
            stdout=out_file,
            check=True,
        )
    print(f"Exported signing public key to {out_file_path}")


def sign_file(file_path, gpg_home_path):

    out_file_path = f'{file_path}.gpg'

    subprocess.run(
        [
            "gpg",
            "--homedir",
            str(gpg_home_path),
            "-o",
            out_file_path,
            "--pinentry-mode=loopback",
            "--batch",
            "--yes",
            "--passphrase-fd",
            "0",
            "--sign",
            file_path,
        ],
        input=f"{GPG_BINARY_SIGNING_PASSPHRASE}\n".encode(),
        check=True,
    )
    print(f"Signed {file_path}")

    return out_file_path


def upload_file_to_s3(s3_helper, file_path, s3_path_prefix):
    s3_path = s3_path_prefix / os.path.basename(file_path)
    s3_helper.upload_build_file_to_s3(Path(file_path), str(s3_path))
    print(f'Uploaded file {file_path} to {s3_path}')


def main():
    stopwatch = Stopwatch()
    reports_path = Path(REPORT_PATH)
    test_results = []
    state = SUCCESS
    description = "Signed artifact hashes successfully"

    if not os.path.exists(TEMP_PATH):
        os.makedirs(TEMP_PATH)

    pr_info = PRInfo()

    logging.info("Repo copy path %s", REPO_COPY)

    s3_helper = S3Helper()

    s3_path_prefix = Path(f"{pr_info.number}/{pr_info.sha}/" + CHECK_NAME.lower().replace(
        " ", "_"
    ).replace("(", "_").replace(")", "_").replace(",", "_"))

    # downloads `package_release` artifacts generated
    download_builds_filter(CHECK_NAME, reports_path, Path(TEMP_PATH))

    try:
        import_signing_key(GPG_HOME_PATH)

        for f in os.listdir(TEMP_PATH):
            full_path = os.path.join(TEMP_PATH, f)
            if os.path.isdir(full_path):
                continue
            hashed_file_path = hash_file(full_path)
            signed_file_path = sign_file(hashed_file_path, GPG_HOME_PATH)
            upload_file_to_s3(s3_helper, signed_file_path, s3_path_prefix)
            test_results.append(TestResult(name=os.path.basename(full_path), status=OK))

        export_public_key(GPG_HOME_PATH, SIGNING_PUBKEY_PATH)
        upload_file_to_s3(s3_helper, SIGNING_PUBKEY_PATH, s3_path_prefix)
        test_results.append(TestResult(name=SIGNING_PUBKEY_PATH.name, status=OK))
    except Exception as ex:
        state = FAILURE
        description = f"Failed to sign release artifacts: {ex}"
        test_results.append(TestResult(name=CHECK_NAME, status=FAIL, raw_logs=str(ex)))
    finally:
        shutil.rmtree(GPG_HOME_PATH, ignore_errors=True)

    # Signed hashes are:
    # clickhouse-client_22.3.15.2.altinitystable_amd64.deb.sha512.gpg              clickhouse-keeper_22.3.15.2.altinitystable_x86_64.apk.sha512.gpg
    # clickhouse-client-22.3.15.2.altinitystable-amd64.tgz.sha512.gpg              clickhouse-keeper-22.3.15.2.altinitystable.x86_64.rpm.sha512.gpg
    # clickhouse-client_22.3.15.2.altinitystable_x86_64.apk.sha512.gpg             clickhouse-keeper-dbg_22.3.15.2.altinitystable_amd64.deb.sha512.gpg
    # clickhouse-client-22.3.15.2.altinitystable.x86_64.rpm.sha512.gpg             clickhouse-keeper-dbg-22.3.15.2.altinitystable-amd64.tgz.sha512.gpg
    # clickhouse-common-static_22.3.15.2.altinitystable_amd64.deb.sha512.gpg       clickhouse-keeper-dbg_22.3.15.2.altinitystable_x86_64.apk.sha512.gpg
    # clickhouse-common-static-22.3.15.2.altinitystable-amd64.tgz.sha512.gpg       clickhouse-keeper-dbg-22.3.15.2.altinitystable.x86_64.rpm.sha512.gpg
    # clickhouse-common-static_22.3.15.2.altinitystable_x86_64.apk.sha512.gpg      clickhouse-keeper.sha512.gpg
    # clickhouse-common-static-22.3.15.2.altinitystable.x86_64.rpm.sha512.gpg      clickhouse-library-bridge.sha512.gpg
    # clickhouse-common-static-dbg_22.3.15.2.altinitystable_amd64.deb.sha512.gpg   clickhouse-odbc-bridge.sha512.gpg
    # clickhouse-common-static-dbg-22.3.15.2.altinitystable-amd64.tgz.sha512.gpg   clickhouse-server_22.3.15.2.altinitystable_amd64.deb.sha512.gpg
    # clickhouse-common-static-dbg_22.3.15.2.altinitystable_x86_64.apk.sha512.gpg  clickhouse-server-22.3.15.2.altinitystable-amd64.tgz.sha512.gpg
    # clickhouse-common-static-dbg-22.3.15.2.altinitystable.x86_64.rpm.sha512.gpg  clickhouse-server_22.3.15.2.altinitystable_x86_64.apk.sha512.gpg
    # clickhouse-keeper_22.3.15.2.altinitystable_amd64.deb.sha512.gpg              clickhouse-server-22.3.15.2.altinitystable.x86_64.rpm.sha512.gpg
    # clickhouse-keeper-22.3.15.2.altinitystable-amd64.tgz.sha512.gpg              clickhouse.sha512.gpg

    JobReport(
        description=description,
        test_results=test_results,
        status=state,
        start_time=stopwatch.start_time_str,
        duration=stopwatch.duration_seconds,
        additional_files=[],
    ).dump()

    if state == FAILURE:
        sys.exit(1)

    sys.exit(0)

if __name__ == "__main__":
    main()
