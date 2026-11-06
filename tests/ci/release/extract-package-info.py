#! /usr/bin/env python3

import os
import re
import sys
import argparse
from datetime import datetime, timezone

import requests
from packaging import version

GITHUB_TOKEN = os.getenv("GITHUB_TOKEN") or os.getenv("GH_TOKEN")
GITHUB_REPO = os.getenv("GITHUB_REPOSITORY", "Altinity/ClickHouse")


def generate_time(folder_time: str = None) -> str:
    """Generate time string.
    """
    if folder_time:
        # Ends up in $GITHUB_ENV, S3 paths and ansible extra vars (which Ansible templates),
        # so only allow plain path characters, e.g. 2026-10-30T15-35-51.747.
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", folder_time):
            print(f"Error: invalid folder_time {folder_time!r}", file=sys.stderr)
            sys.exit(1)
        return folder_time

    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H-%M-%S.%f")[:-3]


def s3_object_exists(bucket: str, key: str) -> bool:
    """Check if an S3 object exists using AWS credentials from the environment."""
    import boto3
    from botocore.exceptions import ClientError

    try:
        boto3.client("s3").head_object(Bucket=bucket, Key=key)
        return True
    except ClientError as e:
        code = str(e.response.get("Error", {}).get("Code", ""))
        if code in ("404", "NoSuchKey", "NotFound"):
            print(f"S3 object not found: s3://{bucket}/{key}", file=sys.stderr)
            return False
        print(f"S3 head failed for s3://{bucket}/{key}: {e}", file=sys.stderr)
        return False


def set_env_variable(name: str, value: str) -> None:
    """Set an environment variable. Works for both GitHub Actions and standalone scripts.
    """
    os.environ[name] = str(value)

    github_env = os.getenv("GITHUB_ENV")
    if github_env:
        with open(github_env, "a") as f:
            f.write(f"{name}={value}\n")
    print(f"✓ Set {name}={value}")


def get_repo_prefix(altinity_build_feature: str) -> str:
    """Get the repository prefix based on the Altinity build feature.
    """
    repo_prefix_map = {
        "altinityhotfix": "hotfix-",
        "altinityfips": "fips-",
        "altinityantalya": "antalya-",
        "altinitystable": "",
        "altinitytest": "",
    }

    if altinity_build_feature not in repo_prefix_map:
        print(f"Error: Build feature not supported: {altinity_build_feature}", file=sys.stderr)
        sys.exit(1)

    return repo_prefix_map[altinity_build_feature]


def repo_from_workflow_url(run_url: str) -> str:
    """owner/repo from a github.com/.../actions/runs/<id> URL, else GITHUB_REPO."""
    match = re.search(r"github\.com/([^/]+/[^/]+)/actions/runs/", run_url)
    return match.group(1) if match else GITHUB_REPO


def get_run_details(run_url: str) -> dict:
    """Fetch run details for a given run .
    """
    run_id = run_url.split("/")[-1]
    repo = repo_from_workflow_url(run_url)

    headers = {
        "Authorization": f"token {GITHUB_TOKEN}",
        "Accept": "application/vnd.github.v3+json",
    }

    url = f"https://api.github.com/repos/{repo}/actions/runs/{run_id}"
    response = requests.get(url, headers=headers)

    if response.status_code != 200:
        raise Exception(
            f"Failed to fetch run details: {response.status_code} {response.text}"
        )

    return response.json()


def main():
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "--artifact-dest-bucket",
        type=str,
        default=os.getenv("S3_STORAGE_BUCKET", "altinity-test-reports"),
        help="S3 bucket where artifacts are moved to (default: S3_STORAGE_BUCKET env var or 'altinity-test-reports')"
    )

    parser.add_argument(
        "--artifact-src-bucket",
        type=str,
        default=os.getenv("SRC_BUCKET", "altinity-build-artifacts"),
        help="S3 bucket where artifacts are stored (default: SRC_BUCKET env var or 'altinity-build-artifacts')"
    )

    parser.add_argument(
        "--folder-time",
        type=str,
        default=os.getenv("FOLDER_TIME"),
        help="Folder time string (optional, will be auto-generated if not provided)"
    )

    parser.add_argument(
        "--package-version",
        type=str,
        required=True,
        help="Package version"
        )

    parser.add_argument(
        "--workflow-url",
        type=str,
        required=True
    )

    args = parser.parse_args()

    # Accept "v25.8.1.2.altinitystable" as well as "25.8.1.2.altinitystable".
    args.package_version = args.package_version.strip()
    if args.package_version[:1] in ("v", "V"):
        args.package_version = args.package_version[1:]
    set_env_variable("PACKAGE_VERSION", args.package_version)

    folder_time = generate_time(args.folder_time)

    run_details = get_run_details(args.workflow_url)
    commit_sha = run_details["head_commit"]["id"]
    branch_name = run_details["head_branch"]
    if len(run_details["pull_requests"]) > 0:
        pr_number = run_details["pull_requests"][0]["number"]
    else:
        pr_number = 0

    package_version = version.parse(".".join(args.package_version.split(".")[:-1]))
    print(f"Parsed version: {package_version} (from {args.package_version})")

    altinity_build_feature = args.package_version.split(".")[-1]
    repo_prefix = get_repo_prefix(altinity_build_feature)

    if pr_number != 0:
        artifact_url = f"PRs/{pr_number}/{commit_sha}"
        test_results_url = f"PRs/{pr_number}" if package_version >= version.parse("25.6") else pr_number
    elif package_version >= version.parse("25.6"):
        artifact_url = f"REFs/{branch_name}/{commit_sha}"
        test_results_url = f"REFs/{branch_name}/{commit_sha}"
    else:
        artifact_url = f"{package_version.major}.{package_version.minor}/{commit_sha}"
        test_results_url = 0
    print(f"Artifact URL: {artifact_url}")

    set_env_variable("COMMIT_HASH", commit_sha)
    set_env_variable("DEST_URL", f"s3://{args.artifact_dest_bucket}/builds/pre-released/v{args.package_version}/{folder_time}")
    set_env_variable("DOCKER_VERSION", f"{pr_number}-{args.package_version}")
    set_env_variable("FOLDER_TIME", folder_time)
    set_env_variable("MAJOR_VERSION", str(package_version.major))
    set_env_variable("NEEDS_BINARY_PROCESSING", package_version >= version.parse("24"))
    set_env_variable("PR_NUMBER", str(pr_number))
    set_env_variable("REPO_PREFIX", repo_prefix)
    set_env_variable("SRC_URL", f"s3://{args.artifact_src_bucket}/{artifact_url}")
    set_env_variable("TEST_RESULTS_SRC", str(test_results_url))

    # Validate clickhouse-server amd64 deb file with correct version exists
    if package_version >= version.parse("25.6"):
        build_dir = "build_amd_release"
        set_env_variable("BUILD_DIR", build_dir)
        set_env_variable("ARM_BUILD_DIR", "build_arm_release")
    else:
        build_dir = "package_release"
        set_env_variable("BUILD_DIR", build_dir)
        set_env_variable("ARM_BUILD_DIR", "package_aarch64")

    deb_key = f"{artifact_url}/{build_dir}/clickhouse-server_{args.package_version}_amd64.deb"
    deb_exists = s3_object_exists(args.artifact_src_bucket, deb_key)

    if not deb_exists:
        print(f"\n✗ ERROR: Required DEB file not found", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
