"""CAS over GCS, end to end against a deterministic fake GCS XML service.

What this suite is for: the unit tests prove that a request marked `NativeConditional` gets
generation semantics and a `Default` one does not. They cannot prove that a real CAS mount, driving
the production capability battery over a real HTTP client, still works once generation semantics stop
being applied to every request on the client. That is what runs here.

The fake service (`gcs_mocks/server.py`) models generations and ETags as two disjoint domains, so no
assertion below can pass by accident on a value that would serve as either token. Its hostname
contains no `storage.googleapis.com`, so a mount that selects generation tokens proves the capability
came from the explicit `http_client` value.

Marking is observable on the wire on the OAuth path, by absence rather than presence: marking a
request deletes `x-amz-api-version`, so a `Default` request still carries it. That is what lets this
suite assert per-request marking on a single client rather than only per disk. It does not hold on
`gcs_hmac`, where the header is stripped from every request regardless of mode.

Not covered here, deliberately: CAS object attributes. The attribute parameter is plumbed through the
CAS write paths but no production caller fills it — every call site uses the `Backend` overloads that
forward an empty `ObjectMeta` — so an attribute round trip driven from SQL would be asserting the
fake's own behaviour. The prefix mapping is covered by the dialect and client unit tests.
"""

import json
import os
import re

import pytest

from helpers.cluster import ClickHouseCluster
from helpers.mock_servers import start_mock_servers

GCS_HOST = "fakegcs"
GCS_PORT = 8080
METADATA_HOST = "metadata.google.internal"
METADATA_PORT = 80

# One CAS disk per supported `http_client` value, plus an ordinary non-CAS disk on the same fake
# service. Each has its own bucket, so the capture log partitions by bucket with no ambiguity.
CAS_DISKS = {"cas_gcs_oauth": "oauthbucket", "cas_gcs_hmac": "hmacbucket"}
PLAIN_DISK = "plain_gcs_oauth"
PLAIN_BUCKET = "plainbucket"

NUM_ROWS = 200

cluster = ClickHouseCluster(__file__)


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    # The storage configuration is deliberately NOT passed as a main config: a CAS disk mounts at
    # server startup and runs its capability battery against the store, and there is no fallback when
    # the store is unreachable. The mock services can only be launched after their containers are up,
    # which is after this node's server has already started, so the config is installed and the server
    # restarted once the fake service answers.
    cluster.add_instance(
        "node",
        stay_alive=True,
    )
    cluster.add_instance(
        GCS_HOST,
        hostname=GCS_HOST,
        image="altinityinfra/python-bottle",
        tag="latest",
        stay_alive=True,
    )
    # The fake GCE metadata server takes the real default hostname rather than being pointed at by a
    # `metadata_service` override, because a CAS disk rejects `metadata_service` as an unknown setting
    # (`non_cas_keys` in ContentAddressedSettings.cpp lists `http_client` but not `metadata_service`,
    # `request_token_path` or `service_account`). Using the default keeps the disk configuration to
    # keys a CAS disk accepts, and exercises the production token-fetch path unchanged.
    cluster.add_instance(
        METADATA_HOST,
        hostname=METADATA_HOST,
        image="altinityinfra/python-bottle",
        tag="latest",
        stay_alive=True,
    )

    try:
        cluster.start()
        script_dir = os.path.join(os.path.dirname(__file__), "gcs_mocks")
        start_mock_servers(
            cluster,
            script_dir,
            [
                ("server.py", GCS_HOST, str(GCS_PORT)),
                ("auth.py", METADATA_HOST, str(METADATA_PORT)),
            ],
        )
        node = cluster.instances["node"]
        node.copy_file_to_container(
            os.path.join(os.path.dirname(__file__), "configs", "config.xml"),
            "/etc/clickhouse-server/config.d/cas_gcs.xml",
        )
        node.restart_clickhouse()

        for disk in CAS_DISKS:
            _create_and_fill(node, disk)
        _create_and_fill(node, PLAIN_DISK)
        yield cluster
    finally:
        cluster.shutdown()


def _create_and_fill(node, disk):
    table = "t_" + disk
    node.query("DROP TABLE IF EXISTS {} SYNC".format(table))
    node.query(
        """
        CREATE TABLE {} (id Int64, data String)
        ENGINE = MergeTree() ORDER BY id
        SETTINGS storage_policy = '{}'
        """.format(
            table, disk
        )
    )
    node.query(
        "INSERT INTO {} SELECT number, toString(number) FROM numbers({})".format(
            table, NUM_ROWS
        )
    )


def _control(path):
    container = cluster.get_container_id(GCS_HOST)
    raw = cluster.exec_in_container(
        container, ["curl", "-sS", "http://localhost:{}{}".format(GCS_PORT, path)]
    )
    return json.loads(raw)


def _captured(bucket=None):
    records = _control("/_control/requests")
    if bucket is None:
        return records
    return [r for r in records if r["bucket"] == bucket]


def _minted():
    return _control("/_control/minted")


def _unquote(value):
    return value.strip().strip('"')


def _generation_preconditions(records):
    return [
        r["headers"]["x-goog-if-generation-match"]
        for r in records
        if "x-goog-if-generation-match" in r["headers"]
    ]


def _has_goog_metadata(record):
    return any(name.startswith("x-goog-meta-") for name in record["headers"])


def _is_translated(record):
    return "x-goog-if-generation-match" in record["headers"] or _has_goog_metadata(record)


# `AWS_HEADERS_CLEARED_BEFORE_GCS_AUTHENTICATION` in GCSConditionalDialect.cpp lists
# `x-amz-api-version`, and `prepareGcsRequestForOAuthAuthentication` — which runs ONLY for a marked
# request on the OAuth client — deletes every header in it. So on a `gcp_oauth` client the header's
# PRESENCE means the request was `Default` and its ABSENCE means the request was marked. That makes
# marking observable on the wire for the request kinds the SDK stamps with it, which measurement says
# are GET, HEAD and DELETE but not PUT.
#
# This does NOT hold on `gcs_hmac`: `prepareGcsRequestForGoog4Authentication` runs for every request
# that client sends, marked or not, so the header is always absent there and says nothing about mode.
#
# One other thing deletes the same header, and understanding why it does not fire here is what makes
# the discriminator trustworthy. `Client::BuildHttpRequest` also drops `x-amz-api-version` when
# `api_mode == ApiMode::GCS`, for every request and before the marking logic runs. `api_mode` becomes
# GCS only inside a block gated on `provider_type == ProviderType::GCS`, and `deduceProviderType` is
# pure endpoint-substring matching: GCS requires `storage.googleapis.com` in the URL. This fixture's
# endpoint deliberately contains no such substring, so `provider_type` is UNKNOWN, that block never
# runs, and the header survives to become a marking signal.
#
# Note what this does NOT mean. It is not that these disks have credentials: `gcp_oauth` deliberately
# builds an EMPTY credentials provider chain ("we don't provide any credentials to avoid signing" in
# Credentials.cpp), which is also why no SigV4 artifact such as `x-amz-content-sha256` ever appears.
# Against a real `storage.googleapis.com` endpoint `provider_type` WOULD be GCS, those empty
# credentials would select `ApiMode::GCS`, and the header would be stripped from every request. So
# this discriminator is an artifact of the fixture's non-GCS hostname and could not be reproduced
# against production GCS. Marking itself is unaffected — it depends on `http_client`, not the endpoint
# — so the test still fences the behaviour; only the ability to OBSERVE it is endpoint-dependent.
#
# Consequence for a future reader: if these assertions ever start failing uniformly rather than for
# one request, suspect that the endpoint or `provider_type` changed and the discriminator is gone,
# before suspecting that marking broke.
_MARKING_OBSERVABLE_METHODS = ("GET", "HEAD", "DELETE")


def _looks_default_on_oauth(record):
    return "x-amz-api-version" in record["headers"]


def test_data_is_readable_on_every_disk():
    """Mount, write and read back on both `http_client` values and on the ordinary disk.

    A CAS mount runs `runCapabilityProbe` against the store and refuses the mount unless conditional
    create, conditional overwrite, wrong-token delete rejection and correct-token delete all behave.
    Reaching a correct SELECT therefore proves the whole battery passed over GCS generation
    semantics. Would fail if: the request-mode plumbing stopped marking any CAS operation, since the
    fake rejects a generation sent as an ETag and an ETag sent as a generation.
    """
    node = cluster.instances["node"]
    expected_sum = (NUM_ROWS - 1) * NUM_ROWS // 2
    for disk in list(CAS_DISKS) + [PLAIN_DISK]:
        table = "t_" + disk
        assert int(node.query("SELECT count() FROM {}".format(table))) == NUM_ROWS
        assert int(node.query("SELECT sum(id) FROM {}".format(table))) == expected_sum


def test_fake_service_keeps_the_two_token_domains_disjoint():
    """The fixture's own invariant, asserted rather than assumed.

    Negative control: nothing in ClickHouse can flip this — it is a property of the fake. It is
    asserted anyway because every assertion below is only meaningful while it holds.
    """
    minted = _minted()
    assert minted["generations"], "the fake minted no generation, so nothing below is meaningful"
    assert minted["etags"], "the fake minted no ETag, so nothing below is meaningful"
    for generation in minted["generations"]:
        assert re.fullmatch(r"[0-9]{16}", generation), generation
    for etag in minted["etags"]:
        assert not _unquote(etag).isdigit(), etag
    assert not (set(minted["generations"]) & {_unquote(e) for e in minted["etags"]})


@pytest.mark.parametrize("disk", sorted(CAS_DISKS))
def test_cas_conditional_ops_use_generation_preconditions(disk):
    """Create-if-absent and compare-and-set overwrite both travel as generation preconditions.

    Would fail if: `Client::BuildHttpRequest` stopped copying the mode, or CAS stopped marking its
    conditional writes — the preconditions would then arrive as ETag-valued `If-Match` /
    `If-None-Match` instead, and both value-domain assertions would break.

    The accepted-versus-rejected split matters. An earlier version of this test required EVERY
    precondition to name a minted generation, and it failed against correct behaviour: the capability
    battery fabricates known-wrong tokens on purpose, so a precondition the service never minted is
    expected as long as the service refused it.
    """
    records = _captured(CAS_DISKS[disk])
    assert records, "no request reached the fake for disk {}".format(disk)

    conditional = [r for r in records if "x-goog-if-generation-match" in r["headers"]]
    preconditions = [r["headers"]["x-goog-if-generation-match"] for r in conditional]
    assert "0" in preconditions, "no create-if-absent precondition was sent"
    assert [p for p in preconditions if p != "0"], "no compare-and-set precondition was sent"

    minted = _minted()
    known = set(minted["generations"]) | {"0"}
    etag_values = {_unquote(e) for e in minted["etags"]}

    for record in conditional:
        value = record["headers"]["x-goog-if-generation-match"]
        # Always: the value lives in the generation domain and never in the ETag domain. This is the
        # cross-domain check the whole fixture exists for.
        assert value.isdigit(), "a non-numeric value reached the generation domain: {!r}".format(value)
        assert value not in etag_values, "an ETag was sent as a generation: {}".format(value)

        # The capability battery deliberately fabricates wrong tokens (`900000000000000001` and
        # friends in `CasProbe`) to prove the store enforces preconditions, so a precondition the
        # service never minted is expected — but ONLY if the service rejected it. An ACCEPTED
        # precondition must name a real generation, which is the half that would break if the token
        # plumbing regressed.
        if record["status"] < 300:
            assert value in known, "the service accepted a precondition it never minted: {}".format(value)
        else:
            assert record["status"] == 412, (
                "a conditional request failed with {} rather than a precondition failure".format(
                    record["status"]
                )
            )


@pytest.mark.parametrize("disk", sorted(CAS_DISKS))
def test_no_cas_request_sends_a_generation_as_an_etag(disk):
    """The exact-delete safety invariant, stated over the whole captured run.

    A numeric generation placed in an ETag-valued `If-Match` is the failure mode the design calls
    safety-critical: the design does not assume whether GCS would reject, compare or ignore it.
    Would fail if: any CAS conditional operation lost its mode, since CAS token values are
    generations here.
    """
    generations = set(_minted()["generations"])
    for record in _captured(CAS_DISKS[disk]):
        for header in ("if-match", "if-none-match"):
            value = record["headers"].get(header)
            if value is None:
                continue
            assert _unquote(value) not in generations, (
                "{} {} sent a generation in {}: {}".format(
                    record["method"], record["key"], header, value
                )
            )


@pytest.mark.parametrize("disk", sorted(CAS_DISKS))
def test_stale_exact_delete_preserves_the_object_and_a_matching_one_removes_it(disk):
    """The capability battery's delete pair, read off the wire.

    `runCapabilityProbe` deletes with a known-wrong generation, requires the object to survive, then
    deletes with the correct one. Both halves must be visible, on one key, in that order.
    Would fail if: the wrong-token DELETE were honoured (no 412 would appear), or the correct-token
    DELETE were refused (mount would fail before this test ran).
    """
    records = _captured(CAS_DISKS[disk])
    deletes = [
        r
        for r in records
        if r["method"] == "DELETE" and "x-goog-if-generation-match" in r["headers"]
    ]
    assert deletes, "no generation-conditioned DELETE was sent"

    rejected = [r for r in deletes if r["status"] == 412]
    accepted = [r for r in deletes if r["status"] == 204]
    assert rejected, "no DELETE was rejected on a stale generation"
    assert accepted, "no DELETE was accepted on a matching generation"

    keys_with_both = set(r["key"] for r in rejected) & set(r["key"] for r in accepted)
    assert keys_with_both, "no single key saw both a rejected and an accepted exact DELETE"

    key = sorted(keys_with_both)[0]
    first_rejection = min(r["seq"] for r in rejected if r["key"] == key)
    later_success = min(r["seq"] for r in accepted if r["key"] == key)
    assert first_rejection < later_success

    # Between the rejection and the successful delete the object must still be readable: that is the
    # half of the battery that proves the store did not honour the stale token.
    survived = [
        r
        for r in records
        if r["key"] == key
        and r["method"] in ("GET", "HEAD")
        and r["status"] == 200
        and first_rejection < r["seq"] < later_success
    ]
    assert survived, "the object was not observed alive between the stale and the matching DELETE"


@pytest.mark.parametrize("disk", sorted(CAS_DISKS))
def test_list_stays_unmarked_and_its_etag_never_becomes_a_cas_token(disk):
    """LIST keeps upstream ETag semantics on a CAS disk.

    Would fail if: LIST acquired the request mode — it would carry a translated header — or if a
    LIST-derived ETag were ever accepted as a generation, which the value-domain check catches.
    """
    records = _captured(CAS_DISKS[disk])
    lists = [r for r in records if r["method"] == "GET" and not r["key"]]
    assert lists, "no LIST reached the fake"
    for record in lists:
        assert not _is_translated(record), "a LIST carried a translated header: {}".format(
            record["query"]
        )

    etag_values = {_unquote(e) for e in _minted()["etags"]}
    for value in _generation_preconditions(records):
        assert value not in etag_values


def test_ordinary_gcp_oauth_traffic_keeps_upstream_semantics():
    """The upgrade regression this change exists to remove, checked on a non-CAS disk.

    The two absence assertions alone would be vacuous, and that is worth spelling out: a generation
    precondition is only ever emitted for a request that already carried `If-Match`/`If-None-Match`,
    and `x-goog-meta-*` only for one that carried `x-amz-meta-*`. An ordinary disk sends none of
    those, so marking every request on the client — the exact regression this plan removes — would
    leave those two assertions green. They are kept because they are cheap and true, not because they
    fence anything.

    The assertion that DOES fence it is the last one. Marking a request runs
    `prepareGcsRequestForOAuthAuthentication`, which deletes `x-amz-api-version`, so an ordinary
    request must still carry it. Would fail if: `Client::BuildHttpRequest` marked requests it should
    not — the header would vanish from this bucket.
    """
    records = _captured(PLAIN_BUCKET)
    assert records, "the ordinary disk sent no request, so this test would be vacuous"
    for record in records:
        assert "x-goog-if-generation-match" not in record["headers"], record["query"]
        assert not _has_goog_metadata(record), record["query"]

    observable = [r for r in records if r["method"] in _MARKING_OBSERVABLE_METHODS]
    assert observable, "no GET/HEAD/DELETE on the ordinary disk, so the check below would be vacuous"
    for record in observable:
        assert _looks_default_on_oauth(record), (
            "an ordinary {} on {} lost x-amz-api-version, so it was marked".format(
                record["method"], record["key"] or "(list)"
            )
        )


def test_marked_and_default_heads_coexist_on_one_oauth_client():
    """Per-request marking, observed on ONE client, for ONE method, in ONE bucket.

    The CAS `gcp_oauth` disk owns a single S3 client and issues HEADs of both kinds: CAS metadata
    reads are marked, while `probeSentinelRaw` deliberately goes through the ordinary throwing
    `getObjectMetadata` because it must tell no-such-key from no-such-bucket from a transient failure,
    and it discards the metadata anyway. Marking deletes `x-amz-api-version`, so the two kinds are
    distinguishable on the wire even though they are the same verb on the same key space.

    This is the assertion I earlier reported the fixture could not make. I was wrong for a specific
    reason worth keeping: marking adds no header, which is true, but it REMOVES one, and an absence is
    just as observable as a presence.

    Would fail if: every request were marked (the `Default` HEAD would lose the header) or none were
    (all the marked HEADs would keep it). Both directions fire, which is what makes it a partition
    rather than a one-sided check.

    Only the OAuth disk can support this. On `gcs_hmac`,
    `prepareGcsRequestForGoog4Authentication` runs for every request the client sends, so the header
    is absent regardless of mode and carries no information.
    """
    heads = [r for r in _captured(CAS_DISKS["cas_gcs_oauth"]) if r["method"] == "HEAD"]
    assert heads, "no HEAD reached the fake, so this test would be vacuous"

    default_heads = [r for r in heads if _looks_default_on_oauth(r)]
    marked_heads = [r for r in heads if not _looks_default_on_oauth(r)]

    assert marked_heads, "no HEAD was marked — CAS metadata reads lost their request mode"
    assert default_heads, (
        "every HEAD was marked — the sentinel probe's ordinary metadata read was marked too, "
        "which is the whole-client marking regression this plan removes"
    )


@pytest.mark.parametrize("disk", sorted(CAS_DISKS))
def test_translated_requests_are_confined_to_conditional_operations(disk):
    """Translated headers appear only where a precondition or custom metadata was actually sent.

    Weaker than the test above and deliberately kept for both disks, since it is the only isolation
    statement available on `gcs_hmac`: a request carrying no CAS precondition must carry no
    `x-goog-if-generation-match`, so a blanket translation would show up here as a generation
    precondition on a plain read.

    Would fail if: the dialect began emitting generation preconditions for requests that carried no
    ETag precondition — for instance by defaulting a missing precondition to `0`.
    """
    records = _captured(CAS_DISKS[disk])
    translated = [r for r in records if _is_translated(r)]
    assert translated, "nothing was translated at all, so this test would be vacuous"

    for record in records:
        if record["method"] == "GET" and not record["key"]:
            assert not _is_translated(record), "a LIST carried a translated header"

    # Every translated request is a mutation: a conditional PUT, a CAS-owned copy, or an exact DELETE.
    # A plain read must never acquire one.
    for record in translated:
        assert record["method"] in ("PUT", "DELETE", "POST"), (
            "a {} on {} carried a translated header but is not a mutation".format(
                record["method"], record["key"] or "(list)"
            )
        )


@pytest.mark.parametrize("disk", sorted(CAS_DISKS))
def test_the_fake_refused_nothing_it_had_to_serve(disk):
    """A `501 NotImplemented` from the fake means the mount needed an operation the fake refuses.

    That is a fixture gap, not a product bug, and it must not hide behind a passing suite. Would fail
    if: a CAS path started using multipart upload, versioning or another refused operation.
    """
    refused = [r for r in _captured(CAS_DISKS[disk]) if r["status"] == 501]
    assert not refused, "the fake refused operations it was asked for: {}".format(
        [(r["method"], r["key"], r["query"]) for r in refused]
    )
