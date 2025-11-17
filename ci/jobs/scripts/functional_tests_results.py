import dataclasses
import json
import os
import traceback
from typing import List
import re

import yaml

from praktika.result import Result

OK_SIGN = "[ OK "
FAIL_SIGN = "[ FAIL "
TIMEOUT_SIGN = "[ Timeout! "
UNKNOWN_SIGN = "[ UNKNOWN "
SKIPPED_SIGN = "[ SKIPPED "
HUNG_SIGN = "Found hung queries in processlist"
SERVER_DIED_SIGN = "Server died, terminating all processes"
SERVER_DIED_SIGN2 = "Server does not respond to health check"
DATABASE_SIGN = "Database: "

SUCCESS_FINISH_SIGNS = ["All tests have finished", "No tests were run"]

RETRIES_SIGN = "Some tests were restarted"


# def write_results(results_file, status_file, results, status):
#     with open(results_file, "w", encoding="utf-8") as f:
#         out = csv.writer(f, delimiter="\t")
#         out.writerows(results)
#     with open(status_file, "w", encoding="utf-8") as f:
#         out = csv.writer(f, delimiter="\t")
#         out.writerow(status)


def get_broken_tests_rules() -> dict:
    broken_tests_file_path = "tests/broken_tests.yaml"
    if (
        not os.path.isfile(broken_tests_file_path)
        or os.path.getsize(broken_tests_file_path) == 0
    ):
        raise ValueError(
            "There is something wrong with getting broken tests rules: "
            f"file '{broken_tests_file_path}' is empty or does not exist."
        )

    with open(broken_tests_file_path, "r", encoding="utf-8") as broken_tests_file:
        broken_tests = yaml.safe_load(broken_tests_file)

    compiled_rules = {"exact": {}, "pattern": {}}

    for test in broken_tests:
        regex = test.get("regex") is True
        rule = {
            "reason": test["reason"],
        }

        if test.get("message"):
            rule["message"] = re.compile(test["message"]) if regex else test["message"]

        if test.get("not_message"):
            rule["not_message"] = (
                re.compile(test["not_message"]) if regex else test["not_message"]
            )
        if test.get("check_types"):
            rule["check_types"] = test["check_types"]

        if regex:
            rule["regex"] = True
            compiled_rules["pattern"][re.compile(test["name"])] = rule
        else:
            compiled_rules["exact"][test["name"]] = rule

    print(
        f"INFO: Compiled {len(compiled_rules['exact'])} exact rules and {len(compiled_rules['pattern'])} pattern rules"
    )

    return compiled_rules


def test_is_known_fail(test_name, test_logs, known_broken_tests, test_options_string):
    matching_rules = []

    print(f"Checking known broken tests for failed test: {test_name}")
    print("Potential matching rules:")
    exact_rule = known_broken_tests["exact"].get(test_name)
    if exact_rule:
        print(f"{test_name} - {exact_rule}")
        matching_rules.append(exact_rule)

    for name_re, data in known_broken_tests["pattern"].items():
        if name_re.fullmatch(test_name):
            print(f"{name_re} - {data}")
            matching_rules.append(data)

    if not matching_rules:
        return False

    def matches_substring(substring, log, is_regex):
        if log is None:
            return False
        if is_regex:
            return bool(substring.search(log))
        return substring in log

    for rule_data in matching_rules:
        if rule_data.get("check_types") and not any(
            ct in test_options_string for ct in rule_data["check_types"]
        ):
            print(
                f"Check types didn't match: '{rule_data['check_types']}' not in '{test_options_string}'"
            )
            continue  # check_types didn't match → skip rule

        is_regex = rule_data.get("regex", False)
        not_message = rule_data.get("not_message")
        if not_message and matches_substring(not_message, test_logs, is_regex):
            print(f"Skip rule: Not message matched: '{rule_data['not_message']}'")
            continue  # not_message matched → skip rule
        message = rule_data.get("message")
        if message and not matches_substring(message, test_logs, is_regex):
            print(f"Skip rule: Message didn't match: '{rule_data['message']}'")
            continue

        print(f"Test {test_name} matched rule: {rule_data}")
        return rule_data["reason"]

    return False


class FTResultsProcessor:
    @dataclasses.dataclass
    class Summary:
        total: int
        skipped: int
        unknown: int
        failed: int
        success: int
        broken: int
        test_results: List[Result]
        hung: bool = False
        server_died: bool = False
        retries: bool = False
        success_finish: bool = False
        test_end: bool = True

    def __init__(self, wd, test_options):
        self.tests_output_file = f"{wd}/test_result.txt"
        self.debug_files = []
        self.test_options = test_options

    def _process_test_output(self):
        total = 0
        skipped = 0
        unknown = 0
        failed = 0
        success = 0
        broken = 0
        hung = False
        server_died = False
        retries = False
        success_finish = False
        test_results = []
        test_end = True

        known_broken_tests = get_broken_tests_rules()

        with open(self.tests_output_file, "r", encoding="utf-8") as test_file:
            for line in test_file:
                original_line = line
                line = line.strip()

                if any(s in line for s in SUCCESS_FINISH_SIGNS):
                    success_finish = True
                # Ignore hung check report, since it may be quite large.
                # (and may break python parser which has limit of 128KiB for each row).
                if HUNG_SIGN in line:
                    hung = True
                    break
                if SERVER_DIED_SIGN in line or SERVER_DIED_SIGN2 in line:
                    server_died = True
                if RETRIES_SIGN in line:
                    retries = True
                if any(
                    sign in line
                    for sign in (OK_SIGN, FAIL_SIGN, UNKNOWN_SIGN, SKIPPED_SIGN)
                ):
                    test_name = line.split(" ")[2].split(":")[0]

                    if test_name == "+":
                        # TODO: investigate and remove
                        # https://github.com/ClickHouse/ClickHouse/issues/81888
                        print(
                            f"ERROR: incorrect test name: {test_name} in line:\n{line}"
                        )
                        continue

                    test_time = None
                    try:
                        time_token = line.split("]")[1].strip().split()[0]
                        float(time_token)
                        test_time = time_token
                    except:
                        print(f"ERROR: Failed to parse time str from line [{line}]")

                    total += 1
                    if TIMEOUT_SIGN in line:
                        failed += 1
                        test_results.append((test_name, "Timeout", test_time, []))
                    elif FAIL_SIGN in line:
                        failed += 1
                        test_results.append((test_name, "FAIL", test_time, []))
                    elif UNKNOWN_SIGN in line:
                        unknown += 1
                        test_results.append((test_name, "FAIL", test_time, []))
                    elif SKIPPED_SIGN in line:
                        skipped += 1
                        test_results.append((test_name, "SKIPPED", test_time, []))
                    else:
                        success += int(OK_SIGN in line)
                        test_results.append((test_name, "OK", test_time, []))
                    test_end = False
                elif (
                    len(test_results) > 0
                    and test_results[-1][1] in ("FAIL", "SKIPPED")
                    and not test_end
                ):
                    test_results[-1][3].append(original_line)
                # Database printed after everything else in case of failures,
                # so this is a stop marker for capturing test output.
                #
                # And it is handled after everything else to include line with database into the report.
                if DATABASE_SIGN in line:
                    test_end = True

        test_options_string = ", ".join(self.test_options)

        test_results_ = []
        for test in test_results:
            try:
                test_results_.append(
                    Result(
                        name=test[0],
                        status=test[1],
                        start_time=None,
                        duration=float(test[2]),
                        info="".join(test[3])[:16384],
                    )
                )

                if test[1] == "FAIL":
                    broken_message = test_is_known_fail(
                        test[0],
                        test_results_[-1].info,
                        known_broken_tests,
                        test_options_string,
                    )

                    if broken_message:
                        broken += 1
                        failed -= 1
                        test_results_[-1].set_status(Result.StatusExtended.BROKEN)
                        test_results_[-1].set_label(Result.Label.BROKEN)
                        test_results_[-1].info += "\nMarked as broken: " + broken_message

            except Exception as e:
                print(f"ERROR: Failed to parse test results: {test}")
                traceback.print_exc()
                self.debug_files.append(self.tests_output_file)
                if test[0] == "+":
                    # TODO: investigate and remove
                    # https://github.com/ClickHouse/ClickHouse/issues/81888
                    continue
                test_results_.append(
                    Result(
                        name=test[0],
                        status=Result.Status.ERROR,
                        start_time=None,
                        duration=None,
                        info=f"test results parse failure:\n{traceback.print_exc()}",
                    )
                )
        test_results = test_results_

        s = self.Summary(
            total=total,
            skipped=skipped,
            unknown=unknown,
            failed=failed,
            success=success,
            broken=broken,
            test_results=test_results,
            hung=hung,
            server_died=server_died,
            success_finish=success_finish,
            retries=retries,
        )

        return s

    def run(self):
        state = Result.Status.SUCCESS
        s = self._process_test_output()
        test_results = s.test_results

        # # Check test_results.tsv for sanitizer asserts, crashes and other critical errors.
        # # If the file is present, it's expected to be generated by stress_test.lib check for critical errors
        # # In the end this file will be fully regenerated, including both results from critical errors check and
        # # functional test results.
        # if test_results_path and os.path.exists(test_results_path):
        #     with open(test_results_path, "r", encoding="utf-8") as test_results_file:
        #         existing_test_results = list(
        #             csv.reader(test_results_file, delimiter="\t")
        #         )
        #         for test in existing_test_results:
        #             if len(test) < 2:
        #                 unknown += 1
        #             else:
        #                 test_results.append(test)
        #
        #                 if test[1] != "OK":
        #                     failed += 1
        #                 else:
        #                     success += 1

        # is_flaky_check = 1 < int(os.environ.get("NUM_TRIES", 1))
        # logging.info("Is flaky check: %s", is_flaky_check)
        # # If no tests were run (success == 0) it indicates an error (e.g. server did not start or crashed immediately)
        # # But it's Ok for "flaky checks" - they can contain just one test for check which is marked as skipped.
        # if failed != 0 or unknown != 0 or (success == 0 and (not is_flaky_check)):
        if s.failed != 0 or s.unknown != 0:
            state = Result.Status.FAILED

        info = ""
        if s.hung:
            state = Result.Status.FAILED
            test_results.append(
                Result("Some queries hung", "FAIL", info="Some queries hung")
            )
        elif s.server_died:
            state = Result.Status.FAILED
            # When ClickHouse server crashes, some tests are still running
            # and fail because they cannot connect to server
            for result in test_results:
                if result.status == "FAIL":
                    result.status = "SERVER_DIED"
            test_results.append(Result("Server died", "FAIL", info="Server died"))
        elif not s.success_finish:
            state = Result.Status.ERROR
            info = "The test runner was terminated unexpectedly"
        elif s.retries:
            test_results.append(
                Result("Some tests restarted", "SKIPPED", info="Some tests restarted")
            )
        else:
            pass

        if not info:
            info = f"Failed: {s.failed}, Passed: {s.success}, Skipped: {s.skipped}, Broken: {s.broken}"

        result = Result.create_from(
            name="Tests",
            results=test_results,
            status=state,
            files=[],
            info=info,
            with_info_from_results=False,
        )

        if not result.is_ok():
            order = {
                "FAIL": 0,
                "SERVER_DIED": 1,
                "Timeout": 2,
                "NOT_FAILED": 3,
                "BROKEN": 4,
                "OK": 5,
                "SKIPPED": 6,
            }
            result.results.sort(key=lambda x: order.get(x.status, -1))

        return result
