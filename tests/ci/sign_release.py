#!/usr/bin/env python3
import sys
import os
import logging
import subprocess
import base64
import binascii
from env_helper import TEMP_PATH, REPO_COPY, REPORT_PATH
from s3_helper import S3Helper
from pr_info import PRInfo
from build_download_helper import download_builds_filter
from report import FAIL, OK, FAILURE, SUCCESS, JobReport, TestResult
from stopwatch import Stopwatch
import hashlib
from pathlib import Path
from tempfile import NamedTemporaryFile
from typing import Tuple

GPG_BINARY_SIGNING_KEY = os.getenv("GPG_BINARY_SIGNING_KEY")
GPG_BINARY_SIGNING_PASSPHRASE = os.getenv("GPG_BINARY_SIGNING_PASSPHRASE")

CHECK_NAME = os.getenv("CHECK_NAME", "Sign release")
SIGNING_PUBLIC_KEY_FILE = "signing_pubkey.asc"
KEY_FORMAT_GUIDANCE = (
    "GPG_BINARY_SIGNING_KEY must be either an ASCII-armored OpenPGP private key "
    "(-----BEGIN PGP ...) or base64-encoded key payload (armored text or binary key bytes)."
)

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

def _normalize_private_key_material(raw_key: str) -> Tuple[bytes, str]:
    if raw_key is None or not raw_key.strip():
        raise ValueError(
            "GPG_BINARY_SIGNING_KEY is missing or empty. "
            + KEY_FORMAT_GUIDANCE
        )

    stripped_key = raw_key.strip()
    if "-----BEGIN PGP " in stripped_key:
        return stripped_key.encode("utf-8"), "armored"

    key_no_whitespace = "".join(stripped_key.split())
    try:
        decoded_key = base64.b64decode(key_no_whitespace, validate=True)
    except (binascii.Error, ValueError) as ex:
        raise ValueError(
            "Failed to parse GPG_BINARY_SIGNING_KEY as armored or base64 data. "
            + KEY_FORMAT_GUIDANCE
        ) from ex

    if not decoded_key:
        raise ValueError("Decoded GPG_BINARY_SIGNING_KEY is empty. " + KEY_FORMAT_GUIDANCE)

    try:
        decoded_text = decoded_key.decode("utf-8")
    except UnicodeDecodeError:
        return decoded_key, "base64->binary"

    if "-----BEGIN PGP " in decoded_text:
        return decoded_text.strip().encode("utf-8"), "base64->armored"

    return decoded_key, "base64->binary"


def import_private_signing_key():
    if GPG_BINARY_SIGNING_PASSPHRASE is None or GPG_BINARY_SIGNING_PASSPHRASE == "":
        raise ValueError("GPG_BINARY_SIGNING_PASSPHRASE is missing or empty")

    private_key_bytes, key_mode = _normalize_private_key_material(GPG_BINARY_SIGNING_KEY)
    logging.info("Detected signing key format: %s", key_mode)

    with NamedTemporaryFile("wb", delete=False, suffix=".key") as key_file:
        key_file.write(private_key_bytes)
        priv_key_file_path = key_file.name

    try:
        subprocess.run(
            [
                "gpg",
                "--batch",
                "--yes",
                "--pinentry-mode=loopback",
                "--passphrase",
                GPG_BINARY_SIGNING_PASSPHRASE,
                "--import",
                priv_key_file_path,
            ],
            check=True,
        )
    finally:
        os.remove(priv_key_file_path)


def sign_file(file_path):
    out_file_path = f'{file_path}.gpg'
    subprocess.run(
        [
            "gpg",
            "-o",
            out_file_path,
            "--pinentry-mode=loopback",
            "--batch",
            "--yes",
            "--passphrase",
            GPG_BINARY_SIGNING_PASSPHRASE,
            "--sign",
            file_path,
        ],
        check=True,
    )
    print(f"Signed {file_path}")

    return out_file_path


def export_public_signing_key(out_file_path: Path):
    subprocess.run(
        ["gpg", "--armor", "--output", str(out_file_path), "--export"],
        check=True,
    )
    print(f"Exported signing public key to {out_file_path}")

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
        import_private_signing_key()
        for f in os.listdir(TEMP_PATH):
            full_path = os.path.join(TEMP_PATH, f)
            if os.path.isdir(full_path):
                continue
            hashed_file_path = hash_file(full_path)
            signed_file_path = sign_file(hashed_file_path)
            s3_path = s3_path_prefix / os.path.basename(signed_file_path)
            s3_helper.upload_build_file_to_s3(Path(signed_file_path), str(s3_path))
            print(f'Uploaded file {signed_file_path} to {s3_path}')
            test_results.append(TestResult(name=os.path.basename(full_path), status=OK))

        public_key_path = Path(TEMP_PATH) / SIGNING_PUBLIC_KEY_FILE
        export_public_signing_key(public_key_path)
        s3_helper.upload_build_file_to_s3(
            public_key_path, str(s3_path_prefix / SIGNING_PUBLIC_KEY_FILE)
        )
        test_results.append(TestResult(name=SIGNING_PUBLIC_KEY_FILE, status=OK))
    except Exception as ex:
        state = FAILURE
        description = f"Failed to sign release artifacts: {ex}"
        test_results.append(TestResult(name=CHECK_NAME, status=FAIL, raw_logs=str(ex)))

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
