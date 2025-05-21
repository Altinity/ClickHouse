#!/usr/bin/env python3
import argparse
import os
from pathlib import Path
from itertools import combinations
import json
from datetime import datetime

import requests
import pandas as pd
from clickhouse_driver import Client
import boto3
from botocore.exceptions import NoCredentialsError
import pandas as pd

DATABASE_HOST_VAR = "CHECKS_DATABASE_HOST"
DATABASE_USER_VAR = "CHECKS_DATABASE_USER"
DATABASE_PASSWORD_VAR = "CHECKS_DATABASE_PASSWORD"
S3_BUCKET = "altinity-build-artifacts"
GITHUB_REPO = "Altinity/ClickHouse"


css = """
    /* Base colors for Altinity */
    :root {
        --altinity-background: #000D45;
        --altinity-accent: #189DCF;
        --altinity-highlight: #FFC600;
        --altinity-gray: #6c757d;
        --altinity-light-gray: #f8f9fa;
        --altinity-white: #ffffff;
    }

    /* Body and heading fonts */
    body {
        font-family: Arimo, "Proxima Nova", "Helvetica Neue", Helvetica, Arial, sans-serif;
        font-size: 1rem;
        background-color: var(--altinity-background);
        color: var(--altinity-light-gray);
        padding: 2rem;
    }

    h1, h2, h3, h4, h5, h6 {
        font-family: Figtree, "Proxima Nova", "Helvetica Neue", Helvetica, Arial, sans-serif;
        color: var(--altinity-white);
    }

    .logo {
        width: auto;
        height: 5em;
    }

    /* General table styling */
    table {
        min-width: min(900px, 98vw);
        margin: 1rem 0;
        border-collapse: collapse;
        background-color: var(--altinity-white);
        border: 1px solid var(--altinity-accent);
        box-shadow: 0 0 8px rgba(0, 0, 0, 0.05);
        color: var(--altinity-background);
    }

    /* Table header styling */
    th {
        background-color: var(--altinity-accent);
        color: var(--altinity-white);
        padding: 10px 16px;
        text-align: left;
        border: none;
        border-bottom: 2px solid var(--altinity-background);
        white-space: nowrap;
    }
    th.hth {
        border-bottom: 1px solid var(--altinity-accent);
        border-right: 2px solid var(--altinity-background);
    }

    /* Table header sorting styling */
    th {
        cursor: pointer;
    }
    th.no-sort {
        pointer-events: none;
    }
    th::after, 
    th::before {
        transition: color 0.2s ease-in-out;
        font-size: 1.2em;
        color: transparent;
    }
    th::after {
        margin-left: 3px;
        content: '\\025B8';
    }
    th:hover::after {
        color: inherit;
    }
    th.dir-d::after {
        color: inherit;
        content: '\\025BE';
    }
    th.dir-u::after {
        color: inherit;
        content: '\\025B4';
    }

    /* Table body row styling */
    tr:hover {
        background-color: var(--altinity-light-gray);
    }

    /* Table cell styling */
    td {
        padding: 8px 8px;
        border: 1px solid var(--altinity-accent);
    }

    /* Link styling */
    a {
        color: var(--altinity-accent);
        text-decoration: none;
    }
    a:hover {
        color: var(--altinity-highlight);
        text-decoration: underline;
    }
"""

script = """
<script>
    document.addEventListener('click', function (e) {
    try {
        function findElementRecursive(element, tag) {
        return element.nodeName === tag ? element : 
        findElementRecursive(element.parentNode, tag)
        }
        var descending_th_class = ' dir-d '
        var ascending_th_class = ' dir-u '
        var ascending_table_sort_class = 'asc'
        var regex_dir = / dir-(u|d) /
        var alt_sort = e.shiftKey || e.altKey
        var element = findElementRecursive(e.target, 'TH')
        var tr = findElementRecursive(element, 'TR')
        var table = findElementRecursive(tr, 'TABLE')
        function reClassify(element, dir) {
        element.className = element.className.replace(regex_dir, '') + dir
        }
        function getValue(element) {
        return (
            (alt_sort && element.getAttribute('data-sort-alt')) || 
        element.getAttribute('data-sort') || element.innerText
        )
        }
        if (true) {
        var column_index
        var nodes = tr.cells
        for (var i = 0; i < nodes.length; i++) {
            if (nodes[i] === element) {
            column_index = element.getAttribute('data-sort-col') || i
            } else {
            reClassify(nodes[i], '')
            }
        }
        var dir = descending_th_class
        if (
            element.className.indexOf(descending_th_class) !== -1 ||
            (table.className.indexOf(ascending_table_sort_class) !== -1 &&
            element.className.indexOf(ascending_th_class) == -1)
        ) {
            dir = ascending_th_class
        }
        reClassify(element, dir)
        var org_tbody = table.tBodies[0]
        var rows = [].slice.call(org_tbody.rows, 0)
        var reverse = dir === ascending_th_class
        rows.sort(function (a, b) {
            var x = getValue((reverse ? a : b).cells[column_index])
            var y = getValue((reverse ? b : a).cells[column_index])
            return isNaN(x - y) ? x.localeCompare(y) : x - y
        })
        var clone_tbody = org_tbody.cloneNode()
        while (rows.length) {
            clone_tbody.appendChild(rows.splice(0, 1)[0])
        }
        table.replaceChild(clone_tbody, org_tbody)
        }
    } catch (error) {
    }
    });
</script>
"""

logo = """
<p><img class="logo" src="data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0iVVRGLTgiPz48c3ZnIGlkPSJhIiB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCA0NjEuNTUgMTA1Ljk5Ij48ZGVmcz48c3R5bGU+LmJ7ZmlsbDojZmZmO30uY3tmaWxsOiMxOTlkY2Y7fTwvc3R5bGU+PC9kZWZzPjxnPjxwb2x5Z29uIGNsYXNzPSJjIiBwb2ludHM9Ii4wOSA1MC45NiA2Ni44NiAxMi4xMiA0NS44OCAwIDQ1Ljg4IC4wNCAyMi45NCAxMy4zIDIyLjkzIDEzLjMgMjIuOTMgMTMuMyAuMDkgMjYuNDkgLjA5IDI2LjQ5IC4wOSAyNi40OSAwIDI2LjU0IC4wOSAyNi41OSAuMDkgNTAuOTYiLz48cG9seWdvbiBjbGFzcz0iYyIgcG9pbnRzPSI0LjIxIDUzLjE5IDIyLjk0IDY0LjA4IDIyLjk0IDQyLjI5IDQuMjEgNTMuMTkiLz48cG9seWdvbiBjbGFzcz0iYyIgcG9pbnRzPSI0My43NSA4MC43OSAuMjMgNTUuNTEgLjA5IDU1LjU5IC4wOSA3OS40MyAwIDc5LjQ4IC4wOSA3OS41NCAuMDkgMTA1Ljk5IDIyLjggOTIuODggMjIuOCA5Mi44OCA0My43NSA4MC43OSIvPjxwb2x5Z29uIGNsYXNzPSJjIiBwb2ludHM9IjY0LjIyIDM2Ljk2IDY2Ljc5IDM4LjQ1IDg5LjYxIDI1LjE3IDcwLjkyIDE0LjM4IDQ4LjAzIDI3LjcgNjQuMTggMzcuMDIgNjQuMjIgMzYuOTYiLz48Zz48cG9seWdvbiBjbGFzcz0iYyIgcG9pbnRzPSI3Ni4zMyA0NCA5MS42NiA1Mi45MiA5MS42NiA1Mi44MiA5MS42MyA1Mi44MyA3Ni4zMyA0NCIvPjxwb2x5Z29uIGNsYXNzPSJjIiBwb2ludHM9IjY4LjcxIDQ0LjIgNjguNzEgOTIuNTEgOTEuNjYgMTA1Ljc2IDkxLjY2IDU3LjU1IDY4LjcxIDQ0LjIiLz48L2c+PHBvbHlnb24gY2xhc3M9ImMiIHBvaW50cz0iNzAuNzcgNDAuNzYgNzYuMjggNDMuOTcgOTEuNjYgNTIuODUgOTEuNjYgMjguNjEgNzAuNzcgNDAuNzYiLz48L2c+PHBhdGggY2xhc3M9ImIiIGQ9Ik0xNDkuOTIsMjkuNjZoMTIuMzhsMTkuNzIsNDYuNjdoLTEzLjc3bC0zLjM4LTguMjdoLTE3Ljg3bC0zLjMxLDguMjdoLTEzLjVsMTkuNzItNDYuNjdabTExLjI1LDI4LjRsLTUuMTYtMTMuMTctNS4yMywxMy4xN2gxMC4zOVoiLz48cGF0aCBjbGFzcz0iYiIgZD0iTTE4Ni41MywyOS45OWgxMi44NHYzNS4wOGgyMi40NHYxMS4yNWgtMzUuMjhWMjkuOTlaIi8+PHBhdGggY2xhc3M9ImIiIGQ9Ik0yMzAsNDEuMjVoLTEzLjl2LTExLjI1aDQwLjY0djExLjI1aC0xMy45djM1LjA4aC0xMi44NFY0MS4yNVoiLz48cGF0aCBjbGFzcz0iYiIgZD0iTTI2Mi42MywyOS45OWgxMi45MXY0Ni4zM2gtMTIuOTFWMjkuOTlaIi8+PHBhdGggY2xhc3M9ImIiIGQ9Ik0yODQuMDEsMjkuOTloMTEuOThsMTkuMDYsMjQuNDlWMjkuOTloMTIuNzF2NDYuMzNoLTExLjI1bC0xOS43OS0yNS40MnYyNS40MmgtMTIuNzFWMjkuOTlaIi8+PHBhdGggY2xhc3M9ImIiIGQ9Ik0zMzYuMjQsMjkuOTloMTIuOTF2NDYuMzNoLTEyLjkxVjI5Ljk5WiIvPjxwYXRoIGNsYXNzPSJiIiBkPSJNMzY4Ljk0LDQxLjI1aC0xMy45di0xMS4yNWg0MC42NHYxMS4yNWgtMTMuOXYzNS4wOGgtMTIuODRWNDEuMjVaIi8+PHBhdGggY2xhc3M9ImIiIGQ9Ik00MTYuNjgsNTguOThsLTE3LjYxLTI4Ljk5aDE0LjYzbDkuNTMsMTYuODgsOS42LTE2Ljg4aDE0LjM2bC0xNy42MSwyOC43OXYxNy41NGgtMTIuOTF2LTE3LjM0WiIvPjxnPjxwYXRoIGNsYXNzPSJiIiBkPSJNNDU3Ljk5LDM0Ljg5Yy4yOS0uMDksLjU0LS4yNCwuNzMtLjQ0LC4yNS0uMjUsLjM3LS41OCwuMzctMSwwLS40Ny0uMTgtLjg1LS41My0xLjEyLS4zNC0uMjYtLjc5LS40LTEuMzMtLjRoLTIuMDZjLS4wNywwLS4xMiwuMDYtLjEyLC4xMnY0LjYxYzAsLjA3LC4wNiwuMTIsLjEyLC4xMmguNjhjLjA3LDAsLjEyLS4wNiwuMTItLjEydi0xLjYyaC45OWwxLjI5LDEuNjlzLjA2LC4wNSwuMSwuMDVoLjg0cy4wOS0uMDMsLjExLS4wN2MuMDItLjA0LC4wMi0uMDktLjAxLS4xM2wtMS4zMi0xLjcxWm0uMTUtMS40YzAsLjIzLS4wOCwuMzktLjI1LC41MS0uMTgsLjEzLS40MiwuMTktLjcyLC4xOWgtMS4xOXYtMS4zOWgxLjIzYy4zLDAsLjU0LC4wNiwuNywuMTksLjE1LC4xMiwuMjMsLjI4LC4yMywuNVoiLz48cGF0aCBjbGFzcz0iYiIgZD0iTTQ2MS4yLDMyLjY5Yy0uMjQtLjU2LS41Ny0xLjA1LS45OC0xLjQ3LS40MS0uNDItLjktLjc1LTEuNDYtLjk5LS41Ni0uMjQtMS4xNy0uMzYtMS44Mi0uMzZzLTEuMjYsLjEyLTEuODIsLjM3Yy0uNTYsLjI1LTEuMDYsLjU4LTEuNDgsMS0uNDIsLjQyLS43NSwuOTItLjk4LDEuNDctLjI0LC41Ni0uMzYsMS4xNi0uMzYsMS43OXMuMTIsMS4yMywuMzYsMS43OWMuMjQsLjU2LC41NiwxLjA1LC45OCwxLjQ3LC40MSwuNDIsLjksLjc1LDEuNDYsLjk5LC41NiwuMjQsMS4xNywuMzYsMS44MSwuMzZzMS4yNi0uMTIsMS44Mi0uMzdjLjU2LS4yNSwxLjA2LS41OCwxLjQ3LTEsLjQyLS40MiwuNzUtLjkyLC45OC0xLjQ3LC4yNC0uNTYsLjM2LTEuMTYsLjM2LTEuNzlzLS4xMi0xLjIzLS4zNi0xLjc5Wm0tLjMsMS43OWMwLC41NC0uMSwxLjA2LS4zLDEuNTUtLjIsLjQ5LS40OCwuOTEtLjg0LDEuMjctLjM1LC4zNi0uNzgsLjY1LTEuMjcsLjg2LS40OSwuMjEtMS4wMiwuMzItMS41NywuMzJzLTEuMDktLjExLTEuNTYtLjMxYy0uNDgtLjIxLS45LS41LTEuMjUtLjg2LS4zNS0uMzYtLjYzLS43OC0uODMtMS4yNy0uMi0uNDgtLjMtMS0uMy0xLjU0cy4xLTEuMDYsLjMtMS41NWMuMi0uNDgsLjQ4LS45MSwuODQtMS4yNywuMzYtLjM2LC43OC0uNjUsMS4yNi0uODYsLjQ4LS4yMSwxLjAxLS4zMiwxLjU4LS4zMnMxLjA5LC4xMSwxLjU3LC4zMWMuNDgsLjIxLC45LC41LDEuMjUsLjg2LC4zNSwuMzYsLjYzLC43OCwuODMsMS4yNywuMiwuNDgsLjMsMSwuMywxLjU0WiIvPjwvZz48L3N2Zz4=" alt="logo"/></p>
"""


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

    return (
        pd.DataFrame(parsed)
        .sort_values(by=["job_status", "job_name"], ascending=[True, True])
        .reset_index(drop=True)
    )


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
    s3_client = boto3.client("s3", endpoint_url=os.getenv("S3_URL"))
    s3_prefix = f"{pr_number}/{commit_sha}/grype/"

    results = []

    response = s3_client.list_objects_v2(
        Bucket=S3_BUCKET, Prefix=s3_prefix, Delimiter="/"
    )
    grype_result_dirs = [
        content["Prefix"] for content in response.get("CommonPrefixes", [])
    ]

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
    ).replace(' border="1"', "")
    return html


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Create a combined CI report.")
    parser.add_argument(
        "--actions-run-url", required=True, help="URL of the actions run"
    )
    parser.add_argument(
        "--pr-number", required=True, help="Pull request number for the S3 path"
    )
    parser.add_argument(
        "--commit-sha", required=True, help="Commit SHA for the S3 path"
    )
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

    if args.pr_number == "0":
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
    if len(fail_results["docker_images_cves"]) > 0:
        high_cve_count = (
            fail_results["docker_images_cves"]["severity"]
            .str.lower()
            .isin(("high", "critical"))
            .sum()
        )

    title = "ClickHouse® CI Workflow Run Report"

    html_report = f"""
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>{css}
    </style>
    <title>{title}</title>
</head>
<body>
    {logo}
    <h1>{title}</h1>
    <table>
        <tr>
            <th class='hth no-sort'>Pull Request</th><td>{pr_info_html}</td>
        </tr>
        <tr>
            <th class='hth no-sort'>Workflow Run</th><td><a href="{args.actions_run_url}">{args.actions_run_url.split('/')[-1]}</a></td>
        </tr>
        <tr>
            <th class='hth no-sort'>Commit</th><td><a href="https://github.com/{GITHUB_REPO}/commit/{args.commit_sha}">{args.commit_sha}</a></td>
        </tr>
        <tr>
            <th class='hth no-sort'>Date</th><td>{datetime.utcnow().strftime('%Y-%m-%d %H:%M:%S')} UTC</td>
        </tr>
    </table>

    <h2>Table of Contents</h2>
{'<p style="font-weight: bold;color: #F00;">This is a preview. FinishCheck has not completed.</p>' if args.mark_preview else ""}
<ul>
    <li><a href="#ci-jobs-status">CI Jobs Status</a> ({sum(fail_results['job_statuses']['job_status'] != 'success')} fail/error)</li>
    <li><a href="#checks-errors">Checks Errors</a> ({len(fail_results['checks_errors'])})</li>
    <li><a href="#checks-fails">Checks New Fails</a> ({len(fail_results['checks_fails'])})</li>
    <li><a href="#regression-fails">Regression New Fails</a> ({len(fail_results['regression_fails'])})</li>
    <li><a href="#docker-images-cves">Docker Images CVEs</a> ({'N/A' if not args.cves else f'{high_cve_count} high/critical'})</li>
    <li><a href="#checks-known-fails">Checks Known Fails</a> ({'N/A' if not args.known_fails else len(fail_results['checks_known_fails'])})</li>
</ul>

<h2 id="ci-jobs-status">CI Jobs Status</h2> 
{format_results_as_html_table(fail_results['job_statuses'])}

<h2 id="checks-errors">Checks Errors</h2>
{format_results_as_html_table(fail_results['checks_errors'])}

<h2 id="checks-fails">Checks New Fails</h2>
{format_results_as_html_table(fail_results['checks_fails'])}

<h2 id="regression-fails">Regression New Fails</h2>
{format_results_as_html_table(fail_results['regression_fails'])}

<h2 id="docker-images-cves">Docker Images CVEs</h2>
{"<p>Not Checked</p>" if not args.cves else format_results_as_html_table(fail_results['docker_images_cves'])}

<h2 id="checks-known-fails">Checks Known Fails</h2>
{"<p>Not Checked</p>" if not args.known_fails else format_results_as_html_table(fail_results['checks_known_fails'])}

{script}
</body>
</html>
"""
    report_name = "ci_run_report.html"
    report_path = Path(report_name)
    report_path.write_text(html_report, encoding="utf-8")

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
            Body=html_report,
            ContentType="text/html; charset=utf-8",
        )
    except NoCredentialsError:
        print("Credentials not available for S3 upload.")

    print(f"https://s3.amazonaws.com/{S3_BUCKET}/" + report_destination_key)


if __name__ == "__main__":
    main()
