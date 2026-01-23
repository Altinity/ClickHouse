#! /usr/bin/env python3

import os
import sys
import argparse
from datetime import datetime, timezone

import requests
from packaging import version

GITHUB_TOKEN = os.getenv("GITHUB_TOKEN") or os.getenv("GH_TOKEN")
GITHUB_REPO = "Altinity/ClickHouse"


def generate_time(folder_time: str = None) -> str:
    """Generate time string.
    """
    if folder_time:
        return folder_time

    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H-%M-%S.%f")[:-3]


def file_exists(url: str) -> bool:
    """Check if a file exists at the given URL.
    """
    try:
        response = requests.head(url, allow_redirects=True)
        return response.status_code == 200
    except requests.RequestException:
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


def get_run_details(run_url: str) -> dict:
    """Fetch run details for a given run .
    """
    run_id = run_url.split("/")[-1]

    headers = {
        "Authorization": f"token {GITHUB_TOKEN}",
        "Accept": "application/vnd.github.v3+json",
    }

    url = f"https://api.github.com/repos/{GITHUB_REPO}/actions/runs/{run_id}"
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
    set_env_variable("DEST_URL", f"s3://{args.artifact_dest_bucket}/builds/stable/v{package_version}/{folder_time}")
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
        deb_exists = file_exists(f"https://s3.amazonaws.com/{args.artifact_src_bucket}/{artifact_url}/build_amd_release/clickhouse-server_{args.package_version}_amd64.deb")
        set_env_variable("BUILD_DIR", "build_amd_release")
    else:
        deb_exists = file_exists(f"https://s3.amazonaws.com/{args.artifact_src_bucket}/{artifact_url}/package_release/clickhouse-server_{args.package_version}_amd64.deb")
        set_env_variable("BUILD_DIR", "package_release")

    if not deb_exists:
        print(f"\n✗ ERROR: Required DEB file not found", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
