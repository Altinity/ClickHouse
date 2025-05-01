#!/usr/bin/env python3
import argparse
import os
from pathlib import Path
from itertools import combinations
import json
from datetime import datetime

import pandas as pd
from jinja2 import Environment, FileSystemLoader
import requests
from clickhouse_driver import Client
import boto3
from botocore.exceptions import NoCredentialsError

DATABASE_HOST_VAR = "CHECKS_DATABASE_HOST"
DATABASE_USER_VAR = "CHECKS_DATABASE_USER"
DATABASE_PASSWORD_VAR = "CHECKS_DATABASE_PASSWORD"
S3_BUCKET = "altinity-build-artifacts"
GITHUB_REPO = "Altinity/ClickHouse"

# Set up the Jinja2 environment
template_dir = os.path.dirname(__file__)

# Load the template
template = Environment(loader=FileSystemLoader(template_dir)).get_template(
    "ci_run_report.html.jinja"
)


def get_commit_statuses(sha: str) -> pd.DataFrame:
    """
    Fetch commit statuses for a given SHA and return as a pandas DataFrame.
    Handles pagination to get all statuses.

    Args:
        sha (str): Commit SHA to fetch statuses for.

    Returns:
        pd.DataFrame: DataFrame containing all statuses.
    """
    headers = {
        "Authorization": f"token {os.getenv('GITHUB_TOKEN')}",
        "Accept": "application/vnd.github.v3+json",
    }

    url = f"https://api.github.com/repos/{GITHUB_REPO}/commits/{sha}/statuses"

    all_data = []

    while url:
        response = requests.get(url, headers=headers)

        if response.status_code != 200:
            raise Exception(
                f"Failed to fetch statuses: {response.status_code} {response.text}"
            )

        data = response.json()
        all_data.extend(data)

        # Check for pagination links in the response headers
        if "Link" in response.headers:
            links = response.headers["Link"].split(",")
            next_url = None

            for link in links:
                parts = link.strip().split(";")
                if len(parts) == 2 and 'rel="next"' in parts[1]:
                    next_url = parts[0].strip("<>")
                    break

            url = next_url
        else:
            url = None

    # Parse relevant fields
    parsed = [
        {
            "job_name": item["context"],
            "job_status": item["state"],
            "message": item["description"],
            "results_link": item["target_url"],
        }
        for item in all_data
    ]

    # Create DataFrame
    df = pd.DataFrame(parsed)

    # Drop duplicates keeping the first occurrence (newest status for each context)
    # GitHub returns statuses in reverse chronological order
    df = df.drop_duplicates(subset=["job_name"], keep="first")

    # Sort by status and job name
    return df.sort_values(
        by=["job_status", "job_name"], ascending=[True, True]
    ).reset_index(drop=True)


def get_pr_info_from_number(pr_number: str) -> dict:
    """
    Fetch pull request information for a given PR number.

    Args:
        pr_number (str): Pull request number to fetch information for.

    Returns:
        dict: Dictionary containing PR information.
    """
    headers = {
        "Authorization": f"token {os.getenv('GITHUB_TOKEN')}",
        "Accept": "application/vnd.github.v3+json",
    }

    url = f"https://api.github.com/repos/{GITHUB_REPO}/pulls/{pr_number}"
    response = requests.get(url, headers=headers)

    if response.status_code != 200:
        raise Exception(
            f"Failed to fetch pull request info: {response.status_code} {response.text}"
        )

    return response.json()


def get_run_details(run_url: str) -> dict:
    """
    Fetch run details for a given run URL.
    """
    run_id = run_url.split("/")[-1]

    headers = {
        "Authorization": f"token {os.getenv('GITHUB_TOKEN')}",
        "Accept": "application/vnd.github.v3+json",
    }

    url = f"https://api.github.com/repos/{GITHUB_REPO}/actions/runs/{run_id}"
    response = requests.get(url, headers=headers)

    if response.status_code != 200:
        raise Exception(
            f"Failed to fetch run details: {response.status_code} {response.text}"
        )

    return response.json()


def get_checks_fails(client: Client, job_url: str):
    """
    Get tests that did not succeed for the given job URL.
    Exclude checks that have status 'error' as they are counted in get_checks_errors.
    """
    columns = "check_status as job_status, check_name as job_name, test_status, test_name, report_url as results_link"
    query = f"""SELECT {columns} FROM `gh-data`.checks
                WHERE task_url LIKE '{job_url}%'
                AND test_status IN ('FAIL', 'ERROR')
                AND check_status!='error'
                ORDER BY check_name, test_name
                """
    return client.query_dataframe(query)


def get_checks_known_fails(client: Client, job_url: str, known_fails: dict):
    """
    Get tests that are known to fail for the given job URL.
    """
    assert len(known_fails) > 0, "cannot query the database with empty known fails"
    columns = "check_status as job_status, check_name as job_name, test_status, test_name, report_url as results_link"
    query = f"""SELECT {columns} FROM `gh-data`.checks
                WHERE task_url LIKE '{job_url}%'
                AND test_status='BROKEN'
                AND test_name IN ({','.join(f"'{test}'" for test in known_fails.keys())})
                ORDER BY test_name, check_name
                """

    df = client.query_dataframe(query)

    df.insert(
        len(df.columns) - 1,
        "reason",
        df["test_name"]
        .astype(str)
        .apply(
            lambda test_name: known_fails[test_name].get("reason", "No reason given")
        ),
    )

    return df


def get_checks_errors(client: Client, job_url: str):
    """
    Get checks that have status 'error' for the given job URL.
    """
    columns = "check_status as job_status, check_name as job_name, test_status, test_name, report_url as results_link"
    query = f"""SELECT {columns} FROM `gh-data`.checks
                WHERE task_url LIKE '{job_url}%'
                AND check_status=='error'
                ORDER BY check_name, test_name
                """
    return client.query_dataframe(query)


def drop_prefix_rows(df, column_to_clean):
    """
    Drop rows from the dataframe if:
    - the row matches another row completely except for the specified column
    - the specified column of that row is a prefix of the same column in another row
    """
    to_drop = set()
    reference_columns = [col for col in df.columns if col != column_to_clean]
    for (i, row_1), (j, row_2) in combinations(df.iterrows(), 2):
        if all(row_1[col] == row_2[col] for col in reference_columns):
            if row_2[column_to_clean].startswith(row_1[column_to_clean]):
                to_drop.add(i)
            elif row_1[column_to_clean].startswith(row_2[column_to_clean]):
                to_drop.add(j)
    return df.drop(to_drop)


def get_regression_fails(client: Client, job_url: str):
    """
    Get regression tests that did not succeed for the given job URL.
    """
    # If you rename the alias for report_url, also update the formatters in format_results_as_html_table
    # Nested SELECT handles test reruns
    query = f"""SELECT arch, job_name, status, test_name, results_link
            FROM (
               SELECT
                    architecture as arch,
                    test_name,
                    argMax(result, start_time) AS status,
                    job_url,
                    job_name,
                    report_url as results_link
               FROM `gh-data`.clickhouse_regression_results
               GROUP BY architecture, test_name, job_url, job_name, report_url
               ORDER BY length(test_name) DESC
            )
            WHERE job_url='{job_url}'
            AND status IN ('Fail', 'Error')
            """
    df = client.query_dataframe(query)
    df = drop_prefix_rows(df, "test_name")
    df["job_name"] = df["job_name"].str.title()
    return df


def get_cves(pr_number, commit_sha):
    """
    Fetch Grype results from S3.

    If no results are available for download, returns ... (Ellipsis).
    """
    s3_client = boto3.client("s3", endpoint_url=os.getenv("S3_URL"))
    s3_prefix = f"{pr_number}/{commit_sha}/grype/"

    results = []

    response = s3_client.list_objects_v2(
        Bucket=S3_BUCKET, Prefix=s3_prefix, Delimiter="/"
    )
    grype_result_dirs = [
        content["Prefix"] for content in response.get("CommonPrefixes", [])
    ]

    if len(grype_result_dirs) == 0:
        # We were asked to check the CVE data, but none was found,
        # maybe this is a preview report and grype results are not available yet
        return ...

    for path in grype_result_dirs:
        file_key = f"{path}result.json"
        file_response = s3_client.get_object(Bucket=S3_BUCKET, Key=file_key)
        content = file_response["Body"].read().decode("utf-8")
        results.append(json.loads(content))

    rows = []
    for scan_result in results:
        for match in scan_result["matches"]:
            rows.append(
                {
                    "docker_image": scan_result["source"]["target"]["userInput"],
                    "severity": match["vulnerability"]["severity"],
                    "identifier": match["vulnerability"]["id"],
                    "namespace": match["vulnerability"]["namespace"],
                }
            )

    if len(rows) == 0:
        return pd.DataFrame()

    df = pd.DataFrame(rows).drop_duplicates()
    df = df.sort_values(
        by="severity",
        key=lambda col: col.str.lower().map(
            {"critical": 1, "high": 2, "medium": 3, "low": 4, "negligible": 5}
        ),
    )
    return df


def url_to_html_link(url: str) -> str:
    if not url:
        return ""
    text = url.split("/")[-1]
    if not text:
        text = "results"
    return f'<a href="{url}">{text}</a>'


def format_test_name_for_linewrap(text: str) -> str:
    """Tweak the test name to improve line wrapping."""
    return text.replace(".py::", "/")


def format_test_status(text: str) -> str:
    """Format the test status for better readability."""
    color = (
        "red"
        if text.lower().startswith("fail")
        else "orange" if text.lower() in ("error", "broken") else "green"
    )
    return f'<span style="font-weight: bold; color: {color}">{text}</span>'


def format_results_as_html_table(results) -> str:
    if len(results) == 0:
        return "<p>Nothing to report</p>"
    results.columns = [col.replace("_", " ").title() for col in results.columns]
    html = results.to_html(
        index=False,
        formatters={
            "Results Link": url_to_html_link,
            "Test Name": format_test_name_for_linewrap,
            "Test Status": format_test_status,
            "Job Status": format_test_status,
            "Status": format_test_status,
            "Message": lambda m: m.replace("\n", " "),
            "Identifier": lambda i: url_to_html_link(
                "https://nvd.nist.gov/vuln/detail/" + i
            ),
        },
        escape=False,
        border=0,
        classes=["test-results-table"],
    )
    return html


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Create a combined CI report.")
    parser.add_argument(  # Need the full URL rather than just the ID to query the databases
        "--actions-run-url", required=True, help="URL of the actions run"
    )
    parser.add_argument(
        "--pr-number", help="Pull request number for the S3 path", type=int
    )
    parser.add_argument("--commit-sha", help="Commit SHA for the S3 path")
    parser.add_argument(
        "--no-upload", action="store_true", help="Do not upload the report"
    )
    parser.add_argument(
        "--known-fails", type=str, help="Path to the file with known fails"
    )
    parser.add_argument(
        "--cves", action="store_true", help="Get CVEs from Grype results"
    )
    parser.add_argument(
        "--mark-preview", action="store_true", help="Mark the report as a preview"
    )
    return parser.parse_args()


def main():
    args = parse_args()

    if args.pr_number is None or args.commit_sha is None:
        run_details = get_run_details(args.actions_run_url)
        if args.pr_number is None:
            if len(run_details["pull_requests"]) > 0:
                args.pr_number = run_details["pull_requests"][0]["number"]
            else:
                args.pr_number = 0
        if args.commit_sha is None:
            args.commit_sha = run_details["head_commit"]["id"]

    db_client = Client(
        host=os.getenv(DATABASE_HOST_VAR),
        user=os.getenv(DATABASE_USER_VAR),
        password=os.getenv(DATABASE_PASSWORD_VAR),
        port=9440,
        secure="y",
        verify=False,
        settings={"use_numpy": True},
    )

    fail_results = {
        "job_statuses": get_commit_statuses(args.commit_sha),
        "checks_fails": get_checks_fails(db_client, args.actions_run_url),
        "checks_known_fails": [],
        "checks_errors": get_checks_errors(db_client, args.actions_run_url),
        "regression_fails": get_regression_fails(db_client, args.actions_run_url),
        "docker_images_cves": (
            [] if not args.cves else get_cves(args.pr_number, args.commit_sha)
        ),
    }

    # get_cves returns ... in the case where no Grype result files were found.
    # This might occur when run in preview mode.
    cves_not_checked = not args.cves or (
        args.mark_preview and fail_results["docker_images_cves"] is ...
    )

    if args.known_fails:
        if not os.path.exists(args.known_fails):
            print(f"Known fails file {args.known_fails} not found.")
            exit(1)

        with open(args.known_fails) as f:
            known_fails = json.load(f)

        if known_fails:
            fail_results["checks_known_fails"] = get_checks_known_fails(
                db_client, args.actions_run_url, known_fails
            )

    if args.pr_number == 0:
        pr_info_html = "Release"
    else:
        try:
            pr_info = get_pr_info_from_number(args.pr_number)
            pr_info_html = f"""<a href="https://github.com/{GITHUB_REPO}/pull/{pr_info["number"]}">
                    #{pr_info.get("number")} ({pr_info.get("base", {}).get('ref')} <- {pr_info.get("head", {}).get('ref')})  {pr_info.get("title")}
                    </a>"""
        except Exception as e:
            pr_info_html = e

    high_cve_count = 0
    if not cves_not_checked and len(fail_results["docker_images_cves"]) > 0:
        high_cve_count = (
            fail_results["docker_images_cves"]["severity"]
            .str.lower()
            .isin(("high", "critical"))
            .sum()
        )

    # Define the context for rendering
    context = {
        "title": "ClickHouse® CI Workflow Run Report",
        "github_repo": GITHUB_REPO,
        "pr_info_html": pr_info_html,
        "workflow_id": args.actions_run_url.split("/")[-1],
        "commit_sha": args.commit_sha,
        "date": f"{datetime.utcnow().strftime('%Y-%m-%d %H:%M:%S')} UTC",
        "is_preview": args.mark_preview,
        "counts": {
            "jobs_status": f"{sum(fail_results['job_statuses']['job_status'] != 'success')} fail/error",
            "checks_errors": len(fail_results["checks_errors"]),
            "checks_new_fails": len(fail_results["checks_fails"]),
            "regression_new_fails": len(fail_results["regression_fails"]),
            "cves": "N/A" if cves_not_checked else f"{high_cve_count} high/critical",
            "checks_known_fails": (
                "N/A"
                if not args.known_fails
                else len(fail_results["checks_known_fails"])
            ),
        },
        "ci_jobs_status_html": format_results_as_html_table(
            fail_results["job_statuses"]
        ),
        "checks_errors_html": format_results_as_html_table(
            fail_results["checks_errors"]
        ),
        "checks_fails_html": format_results_as_html_table(fail_results["checks_fails"]),
        "regression_fails_html": format_results_as_html_table(
            fail_results["regression_fails"]
        ),
        "docker_images_cves_html": (
            "<p>Not Checked</p>"
            if cves_not_checked
            else format_results_as_html_table(fail_results["docker_images_cves"])
        ),
        "checks_known_fails_html": (
            "<p>Not Checked</p>"
            if not args.known_fails
            else format_results_as_html_table(fail_results["checks_known_fails"])
        ),
    }

    # Render the template with the context
    rendered_html = template.render(context)

    report_name = "ci_run_report.html"
    report_path = Path(report_name)
    report_path.write_text(rendered_html, encoding="utf-8")

    if args.no_upload:
        print(f"Report saved to {report_path}")
        exit(0)

    report_destination_key = f"{args.pr_number}/{args.commit_sha}/{report_name}"

    # Upload the report to S3
    s3_client = boto3.client("s3", endpoint_url=os.getenv("S3_URL"))

    try:
        s3_client.put_object(
            Bucket=S3_BUCKET,
            Key=report_destination_key,
            Body=rendered_html,
            ContentType="text/html; charset=utf-8",
        )
    except NoCredentialsError:
        print("Credentials not available for S3 upload.")

    print(f"https://s3.amazonaws.com/{S3_BUCKET}/" + report_destination_key)


if __name__ == "__main__":
    main()
