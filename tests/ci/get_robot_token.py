#!/usr/bin/env python3
import logging

import boto3  # type: ignore
from github import Github  # type: ignore


def get_parameter_from_ssm(name, decrypt=True, client=None):
    if not client:
        client = boto3.client("ssm", region_name="us-east-1")
    return client.get_parameter(Name=name, WithDecryption=decrypt)["Parameter"]["Value"]


# Original CI code uses the "_original" version of this method. Each robot token is rate limited
# and the original implementation selects the "best one". To make it simpler and iterate faster,
# we are using only one robot and keeping the method signature. In the future we might reconsider
# having multiple robot tokens
def get_best_robot_token(token_prefix_env_name="github_robot_token", total_tokens=4):
    return get_parameter_from_ssm(token_prefix_env_name)

def get_best_robot_token_original(token_prefix_env_name="github_robot_token_", total_tokens=4):
    client = boto3.client("ssm", region_name="us-east-1")
    parameters = client.describe_parameters(
        ParameterFilters=[
            {"Key": "Name", "Option": "BeginsWith", "Values": [token_prefix_env_name]}
        ]
    )["Parameters"]
    assert parameters
    token = {"login": "", "value": "", "rest": 0}

    for token_name in [p["Name"] for p in parameters]:
        value = get_parameter_from_ssm(token_name, True, client)
        gh = Github(value, per_page=100)
        # Do not spend additional request to API by accessin user.login unless
        # the token is chosen by the remaining requests number
        user = gh.get_user()
        rest, _ = gh.rate_limiting
        logging.info("Get token with %s remaining requests", rest)
        if token["rest"] < rest:
            token = {"user": user, "value": value, "rest": rest}

    assert token["value"]
    logging.info(
        "User %s with %s remaining requests is used", token["user"].login, token["rest"]
    )

    return token["value"]
