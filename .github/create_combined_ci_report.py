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

def get_checks_fails(client: Client, job_url: str):
    columns = "check_name, check_status, test_name, test_status, report_url as results_link"
    query = f"""SELECT {columns} FROM `gh-data`.checks 
                WHERE task_url='{job_url}' 
                AND test_status in ('FAIL', 'ERROR') 
                """
    return client.query_dataframe(query)


def get_checks_errors(client: Client, job_url: str):
    columns = "check_name, check_status, test_name, test_status, report_url as results_link"
    query = f"""SELECT {columns} FROM `gh-data`.checks 
                WHERE task_url='{job_url}' 
                AND check_status=='error'
                """
    return client.query_dataframe(query)


def get_regression_fails(client: Client, job_url: str):
    columns = "architecture, result, test_name, report_url as results_link"
    query = f"""SELECT {columns} FROM `gh-data`.clickhouse_regression_results 
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
    html = results.to_html(index=False, formatters={"Results Link": url_to_html_link}, escape=False)
    return html
    

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Create a combined CI report.")
    parser.add_argument(
        "--actions-job-url", required=True, help="URL of the actions job"
    )
    parser.add_argument(
        "--ci-running-report-url", required=True, help="URL of the CI running report"
    )
    return parser.parse_args()


def main():
    args = parse_args()

    client = Client(
        host=os.getenv(DATABASE_HOST_VAR),
        user=os.getenv(DATABASE_USER_VAR),
        password=os.getenv(DATABASE_PASSWORD_VAR),
        port=9440,
        secure="y",
        verify=False,
        settings={"use_numpy": True},
    )

    ci_running_report_url = args.ci_running_report_url
    response = requests.get(ci_running_report_url)

    report_destination_url = ci_running_report_url.replace("ci_running.html", "combined_report.html")
    _, pr_number, commit_sha, _ = ci_running_report_url.rsplit("/", 3)

    if response.status_code == 200:
        ci_running_report = response.text
    else:
        print(
            f"Failed to download CI running report. Status code: {response.status_code}"
        )

    combined_report = f"""{ci_running_report.split("</body>")[0]}
<h2>Checks Fails</h2>
{format_results_as_html_table(get_checks_fails(client, args.actions_job_url))}

<h2>Checks Errors</h2>
{format_results_as_html_table(get_checks_errors(client, args.actions_job_url))}

<h2>Regression Fails</h2>
{format_results_as_html_table(get_regression_fails(client, args.actions_job_url))}

</body>
</html>
"""
    
    report_path = Path("combined_report.html")
    report_path.write_text(combined_report, encoding="utf-8")

    # Upload the report to S3
    s3_client = boto3.client('s3')

    try:
        s3_client.put_object(
            Bucket="altinity-build-artifacts",
            Key=f"{pr_number}/{commit_sha}/combined_report.html",
            Body=combined_report,
            ContentType='text/html'
        )
    except NoCredentialsError:
        print("Credentials not available for S3 upload.")

    print(report_destination_url)


if __name__ == "__main__":
    main()
