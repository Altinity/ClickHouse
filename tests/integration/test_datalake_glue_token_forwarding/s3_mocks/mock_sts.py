import json
import sys
from datetime import datetime, timedelta, timezone
from urllib.parse import parse_qs

from bottle import request, response, route, run

recorded = []


@route("/")
def ping():
    response.content_type = "text/plain"
    response.set_header("Content-Length", 2)
    return "OK"


@route("/_requests")
def list_requests():
    response.content_type = "application/json"
    return json.dumps(recorded)


@route("/_reset")
def reset():
    recorded.clear()
    response.content_type = "text/plain"
    return "OK"


@route("/", method="POST")
def sts():
    body = request.body.read().decode()
    params = {key: values[0] for key, values in parse_qs(body).items()}

    recorded.append(
        {
            "action": params.get("Action", ""),
            "version": params.get("Version", ""),
            "role_arn": params.get("RoleArn", ""),
            "role_session_name": params.get("RoleSessionName", ""),
            "web_identity_token": params.get("WebIdentityToken", ""),
            "query_string": request.query_string,
        }
    )

    if params.get("RoleSessionName") == "rejected":
        response.status = 403
        response.content_type = "text/xml"
        return """
            <ErrorResponse xmlns="https://sts.amazonaws.com/doc/2011-06-15/">
                <Error>
                    <Type>Sender</Type>
                    <Code>InvalidIdentityToken</Code>
                    <Message>Incorrect token audience</Message>
                </Error>
            </ErrorResponse>
        """

    expiration = datetime.now(timezone.utc) + timedelta(hours=1)

    return f"""
        <AssumeRoleWithWebIdentityResponse xmlns="https://sts.amazonaws.com/doc/2011-06-15/">
            <AssumeRoleWithWebIdentityResult>
                <Credentials>
                    <AccessKeyId>testing</AccessKeyId>
                    <SecretAccessKey>testing</SecretAccessKey>
                    <SessionToken>session-for-{params.get("RoleSessionName", "")}</SessionToken>
                    <Expiration>{expiration.strftime("%Y-%m-%dT%H:%M:%SZ")}</Expiration>
                </Credentials>
            </AssumeRoleWithWebIdentityResult>
        </AssumeRoleWithWebIdentityResponse>
    """


run(host="0.0.0.0", port=int(sys.argv[1]))
