#!/usr/bin/env python3
"""Produce the non-self-extracting ClickHouse binaries for one architecture.

Downloads the self-extracting `clickhouse` and `clickhouse-stripped` binaries and runs them to unpack them.
Takes `usr/bin/clickhouse` from the clickhouse-common-static tgz and checks it still has a symbol table.
Uploads all three to <upload-url>/{clickhouse,clickhouse-stripped,clickhouse-with-symtable}.

Must run on a host of the same architecture, since the binaries are executed.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path


class ProcessError(Exception):
    pass


def run(cmd, **kwargs):
    print("+ " + " ".join(cmd), flush=True)
    try:
        subprocess.run(cmd, check=True, **kwargs)
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        raise ProcessError(f"{cmd[0]} {cmd[1] if len(cmd) > 1 else ''} failed: {e}") from None


def s3_cp(src, dst):
    run(["aws", "s3", "cp", "--no-progress", str(src), str(dst)])


def unpack_self_extracting(path):
    """Run the self-extracting binary once so it unpacks itself in place.
    """
    path.chmod(0o755)
    before = path.stat()
    run([str(path), "-q", "q"], cwd=path.parent)
    after = path.stat()
    print(f"{path.name}: {before.st_size} -> {after.st_size} bytes", flush=True)
    if (after.st_ino, after.st_size) == (before.st_ino, before.st_size):
        print(f"::warning::{path.name} did not change when run; it may not have been self-extracting", flush=True)


def extract_from_tgz(tgz, member_path, dest):
    with tarfile.open(tgz, "r:gz") as tar:
        for member in tar:
            if member.isfile() and os.path.normpath(member.name) == member_path:
                with tar.extractfile(member) as src, open(dest, "wb") as out:
                    shutil.copyfileobj(src, out)
                dest.chmod(0o755)
                return
    raise ProcessError(f"{member_path} not found in {tgz.name}")


def has_symtab(path):
    try:
        sections = subprocess.run(["readelf", "-S", "-W", str(path)], check=True,
                                  capture_output=True, text=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        raise ProcessError(f"readelf failed on {path.name}: {e}") from None
    return re.search(r"\]\s+\.symtab\s", sections) is not None


def process(arch, package_version, binaries_url, tgz_url, upload_url, workdir):
    binaries_url, tgz_url, upload_url = (u.rstrip("/") for u in (binaries_url, tgz_url, upload_url))

    for name in ("clickhouse", "clickhouse-stripped"):
        local = workdir / name
        s3_cp(f"{binaries_url}/{name}", local)
        unpack_self_extracting(local)

    tgz_name = f"clickhouse-common-static-{package_version}-{arch}.tgz"
    tgz = workdir / tgz_name
    s3_cp(f"{tgz_url}/{tgz_name}", tgz)
    with_symtab = workdir / "clickhouse-with-symtable"
    extract_from_tgz(tgz, f"clickhouse-common-static-{package_version}/usr/bin/clickhouse", with_symtab)
    if not has_symtab(with_symtab):
        raise ProcessError(f"clickhouse in {tgz_name} is missing .symtab")
    print("✓ Binary has symtable")

    for name in ("clickhouse", "clickhouse-stripped", "clickhouse-with-symtable"):
        s3_cp(workdir / name, f"{upload_url}/{name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--arch", required=True, choices=["amd64", "arm64"])
    parser.add_argument("--package-version", required=True)
    parser.add_argument("--binaries-url", required=True,
                        help="S3 prefix holding the self-extracting clickhouse and clickhouse-stripped")
    parser.add_argument("--tgz-url", required=True,
                        help="S3 prefix holding clickhouse-common-static-<version>-<arch>.tgz")
    parser.add_argument("--upload-url", required=True,
                        help="S3 prefix to upload the non-self-extracting binaries to")
    args = parser.parse_args()

    try:
        with tempfile.TemporaryDirectory(prefix="clickhouse-binaries-") as tmp:
            process(args.arch, args.package_version, args.binaries_url, args.tgz_url, args.upload_url, Path(tmp))
    except ProcessError as e:
        print(f"✗ {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
