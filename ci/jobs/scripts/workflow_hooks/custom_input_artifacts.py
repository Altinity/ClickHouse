import os

import requests

from ci.praktika.runtime import RunConfig
from ci.praktika.mangle import _get_workflows
from ci.praktika._environment import _Environment
from ci.praktika.cache import Cache

S3_BASE_URL = "https://s3.amazonaws.com/altinity-build-artifacts"

RUN_URL = "https://github.com/Altinity/ClickHouse/actions/runs/18568621188"


def get_run_details(run_url: str) -> dict:
    """
    Fetch run details for a given run URL.
    """

    GITHUB_TOKEN = os.getenv("GITHUB_TOKEN") or os.getenv("GH_TOKEN")
    GITHUB_REPO = "Altinity/ClickHouse"

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


def get_full_artifact_url(s3_base_url, pr_number, branch, commit_hash, artifact_name):
    s3_base_url = s3_base_url.rstrip("/")
    if pr_number == 0 or pr_number is None:
        return f"{s3_base_url}/REFs/{branch}/{commit_hash}/{artifact_name}"
    else:
        return f"{s3_base_url}/PRs/{pr_number}/{commit_hash}/{artifact_name}"


def fetch_previous_workflow_config(run_url: str) -> dict:
    run_details = get_run_details(run_url)
    commit_sha = run_details["head_commit"]["id"]
    branch_name = run_details["head_branch"]
    workflow_name = run_details["name"]
    if len(run_details["pull_requests"]) > 0:
        pr_number = run_details["pull_requests"][0]["number"]
    else:
        pr_number = 0
    workflow_config_url = get_full_artifact_url(
        S3_BASE_URL,
        pr_number,
        branch_name,
        commit_sha,
        f"/config_workflow/workflow_config_{workflow_name.lower()}.json",
    )
    r = requests.get(workflow_config_url)
    r.raise_for_status()
    return r.json()


if __name__ == "__main__":
    cache = Cache()

    previous_workflow_config = fetch_previous_workflow_config(RUN_URL)

    current_workflow = _get_workflows(name=_Environment.get().WORKFLOW_NAME)[0]
    current_workflow_config = RunConfig.from_fs(current_workflow.name)

    current_workflow_config.custom_data["version"] = previous_workflow_config[
        "custom_data"
    ]["version"]

    for job in current_workflow.jobs:
        if not job.name.startswith(("Build", "Docker Server", "Docker Keeper")):
            continue

        if job.name in current_workflow_config.filtered_jobs:
            continue

        previous_job_digest = previous_workflow_config["digest_jobs"][job.name]
        cache_entry = cache.fetch_success(
            job_name=job.name, job_digest=previous_job_digest
        )

        if cache_entry is None:
            current_workflow_config.set_job_as_filtered(
                job.name,
                "Skipped, build result not provided by previous workflow, dependent jobs may fail",
            )
            continue

        current_workflow_config.set_job_as_filtered(
            job.name, "Skipped, build result supplied by user"
        )
        current_workflow_config.cache_jobs[job.name] = cache_entry
        current_workflow_config.cache_artifacts[job.name] = cache_entry
        for artifact in job.provides:
            current_workflow_config.cache_artifacts[artifact] = cache_entry

    current_workflow_config.dump()
