#!/usr/bin/env python3
import argparse
import os
from pathlib import Path

import requests
from clickhouse_driver import Client
import boto3
from botocore.exceptions import NoCredentialsError

DATABASE_HOST_VAR = "CHECKS_DATABASE_HOST"
DATABASE_USER_VAR = "CHECKS_DATABASE_USER"
DATABASE_PASSWORD_VAR = "CHECKS_DATABASE_PASSWORD"
S3_BUCKET = "altinity-build-artifacts"


def get_checks_fails(client: Client, job_url: str):
    columns = (
        "check_name, check_status, test_name, test_status, report_url as results_link"
    )
    query = f"""SELECT {columns} FROM `gh-data`.checks 
                WHERE task_url='{job_url}' 
                AND test_status in ('FAIL', 'ERROR') 
                """
    return client.query_dataframe(query)


def get_checks_errors(client: Client, job_url: str):
    columns = (
        "check_name, check_status, test_name, test_status, report_url as results_link"
    )
    query = f"""SELECT {columns} FROM `gh-data`.checks 
                WHERE task_url='{job_url}' 
                AND check_status=='error'
                """
    return client.query_dataframe(query)


def get_regression_fails(client: Client, job_url: str):
    # If you rename the alias for report_url, also update the formatters in format_results_as_html_table
    query = f"""SELECT arch, status, test_name, results_link
            FROM (
               SELECT
                    architecture as arch,
                    test_name,
                    argMax(result, start_time) AS status,
                    job_url,
                    report_url as results_link
               FROM `gh-data`.clickhouse_regression_results
               GROUP BY architecture, test_name, job_url, report_url) 
            WHERE job_url='{job_url}'
            AND result IN ('Fail', 'Error')
            """
    return client.query_dataframe(query)


def url_to_html_link(url: str) -> str:
    if not url:
        return ""
    text = url.split("/")[-1]
    if not text:
        text = "results"
    return f'<a href="{url}">{text}</a>'


def format_results_as_html_table(results) -> str:
    if results.empty:
        return ""
    results.columns = [col.replace("_", " ").title() for col in results.columns]
    html = results.to_html(
        index=False, formatters={"Results Link": url_to_html_link}, escape=False
    )
    return html


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Create a combined CI report.")
    parser.add_argument(
        "--actions-job-url", required=True, help="URL of the actions job"
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

    s3_path = (
        f"https://s3.amazonaws.com/{S3_BUCKET}/{args.pr_number}/{args.commit_sha}/"
    )
    report_destination_url = s3_path + "combined_report.html"
    ci_running_report_url = s3_path + "ci_running.html"

    response = requests.get(ci_running_report_url)
    if response.status_code == 200:
        ci_running_report = response.text
    else:
        print(
            f"Failed to download CI running report. Status code: {response.status_code}, Response: {response.text}"
        )
        exit(1)

    combined_report = f"""{ci_running_report.split("</body>")[0]}
<h2>Checks Fails</h2>
{format_results_as_html_table(get_checks_fails(db_client, args.actions_job_url))}

<h2>Checks Errors</h2>
{format_results_as_html_table(get_checks_errors(db_client, args.actions_job_url))}

<h2>Regression Fails</h2>
{format_results_as_html_table(get_regression_fails(db_client, args.actions_job_url))}

</body>
</html>
""".replace(
        "ClickHouse CI running for", "Combined CI Report for"
    )

    report_path = Path("combined_report.html")
    report_path.write_text(combined_report, encoding="utf-8")

    if args.no_upload:
        print(f"Report saved to {report_path}")
        exit(0)

    # Upload the report to S3
    s3_client = boto3.client("s3")

    try:
        s3_client.put_object(
            Bucket=S3_BUCKET,
            Key=f"{args.pr_number}/{args.commit_sha}/combined_report.html",
            Body=combined_report,
            ContentType="text/html; charset=utf-8",
        )
    except NoCredentialsError:
        print("Credentials not available for S3 upload.")

    print(report_destination_url)


if __name__ == "__main__":
    main()
