#!/usr/bin/env python3
import json

from testflows.core import *

xfails = {
    "/docker vulnerabilities/CVE-2026-2673@nvd﹕cpe,High": [
        (Fail, "Marked as Low severity by OpenSSL: https://openssl-library.org/news/secadv/20260313.txt")
    ],
}

@TestModule
@XFails(xfails)
@Name("docker vulnerabilities")
def docker_vulnerabilities(self):
    with Given("I gather grype scan results"):
        with open("./result.json", "r") as f:
            results = json.load(f)

    for vulnerability in results["matches"]:
        with Test(
            f"{vulnerability['vulnerability']['id']}@{vulnerability['vulnerability']['namespace']},{vulnerability['vulnerability']['severity']}",
            flags=TE,
        ):
            note(vulnerability)
            critical_levels = set(["HIGH", "CRITICAL"])
            if vulnerability['vulnerability']["severity"].upper() in critical_levels:
                with Then(
                    f"Found vulnerability of {vulnerability['vulnerability']['severity']} severity"
                ):
                    result(Fail)


if main():
    docker_vulnerabilities()
