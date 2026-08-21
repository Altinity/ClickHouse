"""The live-GCS characterization gate for the two native GCS HTTP clients.

## Why this suite exists, and what nothing else can replace

The unit tests prove which headers a request carries, and `test_cas_gcs` proves that a real CAS mount
still works once generation semantics stop being applied to every request on the client. Neither can
prove that *Google* accepts the resulting authenticated requests. A fake models whatever we assumed
when we wrote it, so a green `test_cas_gcs` is not evidence for anything below.

Two things in particular can only be settled here:

  - `deduceProviderType` is pure endpoint-substring matching, and the whole `ApiMode` block in
    `Client::BuildHttpRequest` is nested under `provider_type == ProviderType::GCS`. `test_cas_gcs`
    deliberately uses a hostname containing no `storage.googleapis.com`, so `provider_type` is UNKNOWN
    there and the api-mode transformations never run. Against a real endpoint they DO, and they run
    underneath the request-mode logic.
  - Whether the GOOG4 signed-header allowlist produces a signature Google actually accepts.

## Gating

Every group is opt-in through environment variables and skips cleanly when they are absent. This
suite touches a real bucket and issues real, billable requests, so it must never run by default.

  - `GCS_LIVE_BUCKET`             — required for any group. A bucket the caller is willing to have
                                    objects created and deleted in.
  - `GCS_LIVE_PREFIX`             — optional key prefix, default `clickhouse-gcs-live-gate`. A random
                                    per-run suffix is always appended, so two concurrent runs cannot
                                    share a prefix.
  - `GCS_LIVE_HMAC_ACCESS_KEY_ID` — a GOOG4 HMAC key pair. Enables groups 1 and 3.
  - `GCS_LIVE_HMAC_SECRET_ACCESS_KEY`
  - `GCS_LIVE_OAUTH_FROM_METADATA=1` — declares that the HOST running this suite can reach the GCE
                                    metadata server and that its service account may write to the
                                    bucket. Enables group 2. It is a declaration rather than a
                                    credential because a CAS disk rejects `metadata_service` as an
                                    unknown setting (`non_cas_keys` in `ContentAddressedSettings.cpp`
                                    lists `http_client` but not `metadata_service`), so the OAuth
                                    client must find the real default metadata host. There is nothing
                                    to point elsewhere.

Only disks whose gates are satisfied are written into the configuration. That is deliberate: a CAS
disk mounts and runs its capability battery at server startup with no fallback, so an unusable CAS
disk in the config would stop the server and take the other groups down with it.

## What this gate asserts, and what it deliberately does not

It asserts what a client can observe: that each operation SUCCEEDS against Google, that the operation
the test names was actually issued (read off `system.events`, so a statement that silently stopped
reaching S3 cannot leave an assertion vacuously true), and that the tokens CAS recorded are
generations rather than ETags.

It does NOT assert the outbound header set — that `x-goog-if-generation-match` appears on the wire,
that `x-amz-date` / `x-amz-content-sha256` / `x-amz-security-token` / `x-amz-api-version` are absent,
or which headers the GOOG4 signature covers. That is not an omission to be fixed later; it is not
observable from here, and the reason is worth writing down because the obvious fixes are all worse
than the gap:

  - `PocoHTTPClient` logs RESPONSE headers under `enable_s3_requests_logging` and never logs the
    request headers, so the server log cannot supply them.
  - Interposing a capturing proxy either requires spelling the endpoint as the proxy's own hostname,
    which makes `deduceProviderType` report UNKNOWN and switches off the very `ApiMode` behaviour this
    suite exists to exercise, or requires downgrading the connection to plain HTTP so the proxy can
    read the headers, which sends live credentials in clear text.

The outbound header set is covered by the unit tests, which inspect the request object directly and
need no network at all. What is left for this gate is acceptance — and acceptance is the part a unit
test structurally cannot reach.
"""

import os
import random
import string

import pytest

from helpers.cluster import ClickHouseCluster

BUCKET = os.environ.get("GCS_LIVE_BUCKET", "")
BASE_PREFIX = os.environ.get("GCS_LIVE_PREFIX", "clickhouse-gcs-live-gate")
HMAC_KEY_ID = os.environ.get("GCS_LIVE_HMAC_ACCESS_KEY_ID", "")
HMAC_SECRET = os.environ.get("GCS_LIVE_HMAC_SECRET_ACCESS_KEY", "")
OAUTH_FROM_METADATA = os.environ.get("GCS_LIVE_OAUTH_FROM_METADATA", "") == "1"

HMAC_AVAILABLE = bool(BUCKET and HMAC_KEY_ID and HMAC_SECRET)
OAUTH_AVAILABLE = bool(BUCKET and OAUTH_FROM_METADATA)

# The endpoint must be spelled with `storage.googleapis.com`, not a regional or private alias: that
# substring is the whole of `deduceProviderType`, and the api-mode transformations this gate exists to
# exercise are nested under the provider it deduces.
GCS_ENDPOINT = "https://storage.googleapis.com"

RUN_ID = "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(12))
PREFIX = "{}/{}".format(BASE_PREFIX.strip("/"), RUN_ID)

HMAC_PLAIN_DISK = "live_hmac_plain"
# A second ordinary disk exists only so a partition can be MOVED between two volumes of one policy.
# That is the one SQL statement that reaches a server-side `CopyObject`: `FREEZE` and
# `REPLACE PARTITION` hardlink the LOCAL metadata files and issue no object-storage copy at all, so a
# test built on them would leave `S3CopyObject` at zero and prove nothing about GCS accepting a copy.
HMAC_PLAIN_DISK_2 = "live_hmac_plain_cold"
HMAC_TWO_VOLUME_POLICY = "live_hmac_two_volume"
# An ordinary `gcs_hmac` disk pointed at a bucket that does not exist, so a refused request can be
# observed ON THE GOOG4 PATH. The `s3` table function cannot stand in for this: `http_client` is a
# disk setting with no table-function spelling, so a bad-credential `s3(...)` read would exercise
# ordinary AWS SigV4 against GCS and pass while saying nothing about GOOG4.
HMAC_ABSENT_BUCKET_DISK = "live_hmac_absent_bucket"
CAS_OAUTH_DISK = "live_cas_oauth"
CAS_HMAC_DISK = "live_cas_hmac"

pytestmark = pytest.mark.skipif(
    not BUCKET,
    reason="live GCS gate: set GCS_LIVE_BUCKET (and the per-group credential variables) to run it",
)

cluster = ClickHouseCluster(__file__)


def _disk_xml(name, subprefix, cas, client, bucket=None):
    lines = [
        "            <{}>".format(name),
        "                <type>object_storage</type>",
        "                <object_storage_type>s3</object_storage_type>",
    ]
    if cas:
        lines += [
            "                <metadata_type>cas</metadata_type>",
            "                <server_root_id>{}</server_root_id>".format(name),
        ]
    lines += [
        "                <endpoint>{}/{}/{}/{}/</endpoint>".format(
            GCS_ENDPOINT, bucket or BUCKET, PREFIX, subprefix
        ),
        "                <http_client>{}</http_client>".format(client),
    ]
    if client == "gcs_hmac":
        lines += [
            "                <access_key_id>{}</access_key_id>".format(HMAC_KEY_ID),
            "                <secret_access_key>{}</secret_access_key>".format(HMAC_SECRET),
        ]
    lines.append("            </{}>".format(name))
    return "\n".join(lines)


def _policy_xml(name):
    return (
        "            <{name}>\n"
        "                <volumes><main><disk>{name}</disk></main></volumes>\n"
        "            </{name}>".format(name=name)
    )


def _write_config(path):
    """Build a storage configuration holding only the disks whose environment gates are satisfied."""
    disks = []
    policies = []
    if HMAC_AVAILABLE:
        disks += [
            _disk_xml(HMAC_PLAIN_DISK, "plain", cas=False, client="gcs_hmac"),
            _disk_xml(HMAC_PLAIN_DISK_2, "plain-cold", cas=False, client="gcs_hmac"),
            _disk_xml(
                HMAC_ABSENT_BUCKET_DISK,
                "absent",
                cas=False,
                client="gcs_hmac",
                bucket="clickhouse-gcs-live-gate-bucket-that-does-not-exist",
            ),
            _disk_xml(CAS_HMAC_DISK, "cas-hmac", cas=True, client="gcs_hmac"),
        ]
        policies += [
            _policy_xml(HMAC_ABSENT_BUCKET_DISK),
            _policy_xml(CAS_HMAC_DISK),
            "            <{policy}>\n"
            "                <volumes>\n"
            "                    <hot><disk>{hot}</disk></hot>\n"
            "                    <cold><disk>{cold}</disk></cold>\n"
            "                </volumes>\n"
            "            </{policy}>".format(
                policy=HMAC_TWO_VOLUME_POLICY, hot=HMAC_PLAIN_DISK, cold=HMAC_PLAIN_DISK_2
            ),
        ]
    if OAUTH_AVAILABLE:
        disks.append(_disk_xml(CAS_OAUTH_DISK, "cas-oauth", cas=True, client="gcp_oauth"))
        policies.append(_policy_xml(CAS_OAUTH_DISK))

    with open(path, "w", encoding="utf-8") as out:
        out.write("<clickhouse>\n    <storage_configuration>\n        <disks>\n")
        out.write("\n".join(disks))
        out.write("\n        </disks>\n        <policies>\n")
        out.write("\n".join(policies))
        out.write("\n        </policies>\n    </storage_configuration>\n</clickhouse>\n")


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    configs_dir = os.path.join(os.path.dirname(__file__), "configs")
    os.makedirs(configs_dir, exist_ok=True)
    config_path = os.path.join(configs_dir, "live_gcs_generated.xml")
    _write_config(config_path)

    cluster.add_instance("node", main_configs=[config_path], stay_alive=True)
    try:
        cluster.start()
        yield cluster
    finally:
        # Every disk this run created lives under `PREFIX`, which carries a per-run random suffix, so
        # dropping the tables is enough to leave the bucket as it was found. A CAS pool additionally
        # holds its own metadata under that prefix; the DROPs below let it retire that itself rather
        # than deleting a live pool's keys from underneath it.
        node = cluster.instances.get("node")
        if node is not None:
            for disk in (HMAC_PLAIN_DISK, CAS_HMAC_DISK, CAS_OAUTH_DISK):
                try:
                    node.query("DROP TABLE IF EXISTS t_{} SYNC".format(disk))
                    node.query("DROP TABLE IF EXISTS src_{} SYNC".format(disk))
                except Exception:  # noqa: BLE001 - teardown must not mask a test failure
                    pass
        cluster.shutdown()


def _events(node, names):
    """Current values of the named `system.events` counters, zero-filled for absent ones."""
    rows = node.query(
        "SELECT event, value FROM system.events WHERE event IN ({}) FORMAT TSV".format(
            ", ".join("'{}'".format(n) for n in names)
        )
    )
    seen = {}
    for line in rows.strip().splitlines():
        event, value = line.split("\t")
        seen[event] = int(value)
    return {name: seen.get(name, 0) for name in names}


def _create(node, disk, table=None):
    table = table or "t_" + disk
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
    return table


def _cas_tokens(node, disk):
    """Every non-empty incarnation token `system.cas_log` recorded for this disk."""
    node.query("SYSTEM FLUSH LOGS")
    raw = node.query(
        "SELECT DISTINCT token FROM system.cas_log WHERE disk_name = '{}' AND token != '' "
        "FORMAT TSV".format(disk)
    )
    # The transport quotes a generation, and `tokenForHead` is what strips that syntax; strip it here
    # too so the digit check below is about the token's DOMAIN and not about its quoting.
    return [line.strip().strip('"') for line in raw.strip().splitlines() if line.strip()]


# ---------------------------------------------------------------------------------------------------
# Group 1: Default requests on a `gcs_hmac` client, i.e. the ordinary object-storage path under GOOG4
# signing. Nothing here is content-addressed; the question is only whether GCS accepts every operation
# an ordinary ClickHouse disk issues once the signing path stopped being AWS SigV4.
# ---------------------------------------------------------------------------------------------------

requires_hmac = pytest.mark.skipif(
    not HMAC_AVAILABLE,
    reason="set GCS_LIVE_HMAC_ACCESS_KEY_ID and GCS_LIVE_HMAC_SECRET_ACCESS_KEY",
)
requires_oauth = pytest.mark.skipif(
    not OAUTH_AVAILABLE,
    reason="set GCS_LIVE_OAUTH_FROM_METADATA=1 on a host with GCE metadata credentials",
)


@requires_hmac
def test_default_gcs_hmac_accepts_the_ordinary_object_storage_operation_set():
    """Every S3 operation an ordinary disk issues, accepted by GCS under GOOG4 signing.

    The `system.events` deltas are what make this non-vacuous: each named operation must have been
    issued at least once, so a statement that quietly stopped reaching object storage — because a
    default changed, or because a part stayed in memory — cannot leave the assertion true.

    The statement-to-operation mapping is deliberately NOT pinned. Which statement produces a listing
    or a batch delete is a ClickHouse implementation detail that moves between versions; whether GCS
    accepts a listing or a batch delete is what this gate is asking. Pinning the mapping would make
    this test fail on refactors that say nothing about GCS.

    Would fail if: GOOG4 signing produced a signature Google rejects for some operation, or the
    existing `access_key_id`/`secret_access_key` spelling stopped being accepted by the `gcs_hmac`
    selector (the disk would not resolve and no statement below would run).
    """
    node = cluster.instances["node"]
    counters = [
        "S3PutObject",
        "S3GetObject",
        "S3HeadObject",
        "S3ListObjects",
        "S3CopyObject",
        "S3DeleteObjects",
        "S3CreateMultipartUpload",
        "S3UploadPart",
        "S3CompleteMultipartUpload",
    ]
    before = _events(node, counters)

    table = _create(node, HMAC_TWO_VOLUME_POLICY, "t_" + HMAC_PLAIN_DISK)

    # A single-part PUT with custom metadata, then the HEAD that `s3_check_objects_after_upload`
    # issues to verify it.
    node.query(
        "INSERT INTO {} SELECT number, toString(number) FROM numbers(500)".format(table),
        settings={"s3_check_objects_after_upload": 1},
    )
    # A multipart upload: a tiny single-part ceiling rather than a large body, so the run does not
    # depend on how large a default part happens to be.
    node.query(
        "INSERT INTO {} SELECT number, repeat('x', 4096) FROM numbers(500, 4000)".format(table),
        settings={
            "s3_min_upload_part_size": 5 * 1024 * 1024,
            "s3_max_single_part_upload_size": 1024,
        },
    )
    assert int(node.query("SELECT count() FROM {}".format(table))) == 4500
    assert int(node.query("SELECT sum(id) FROM {}".format(table))) > 0

    # A server-side copy: moving a partition between the two volumes of one policy copies each object
    # and then deletes the source. This is the only statement here that reaches `CopyObject`.
    node.query("ALTER TABLE {} MOVE PARTITION tuple() TO VOLUME 'cold'".format(table))
    assert int(node.query("SELECT count() FROM {}".format(table))) == 4500

    # A merge (listing plus more writes), then the deletes.
    node.query("OPTIMIZE TABLE {} FINAL".format(table))
    node.query("ALTER TABLE {} DROP PARTITION tuple()".format(table))
    assert int(node.query("SELECT count() FROM {}".format(table))) == 0

    after = _events(node, counters)
    for name in counters:
        assert after[name] > before[name], (
            "{} was never issued ({} -> {}), so GCS acceptance of it is unproven".format(
                name, before[name], after[name]
            )
        )

    # `S3DeleteObjects` counts the singular `DeleteObject` and the batch `DeleteObjects` together --
    # ProfileEvents.cpp describes it as "DeleteObject(s) calls" -- so the two cannot be separated from
    # here. Both are exercised by the statements above (a merge removes single objects, a dropped
    # partition removes them in batches), but only their sum is observable.


@requires_hmac
def test_default_gcs_hmac_reports_a_typed_error_for_a_refused_request():
    """A refused request must arrive as a typed S3 error, not an unparsed body.

    GCS answers the XML API with an `<Error><Code>` document, and the whole point of keeping the
    request on the S3 XML path is that the SDK parses it. Would fail if: the GOOG4 path returned a
    response the error parser cannot read, which would surface as a generic transport failure with the
    real cause only in the body.

    The refusal has to come from a disk rather than from `s3(...)`, because the table function has no
    spelling for `http_client` and would therefore sign with ordinary AWS SigV4 — passing, or failing,
    for a reason that has nothing to do with GOOG4.
    """
    node = cluster.instances["node"]
    table = _create(node, HMAC_ABSENT_BUCKET_DISK)
    error = node.query_and_get_error(
        "INSERT INTO {} SELECT number, toString(number) FROM numbers(10)".format(table)
    )
    # A parsed S3 error names the bucket problem. An unparsed one surfaces as a bare transport or
    # timeout failure, which is what must not appear.
    assert ("NoSuchBucket" in error) or ("S3_ERROR" in error) or ("ACCESS_DENIED" in error), error


# ---------------------------------------------------------------------------------------------------
# Groups 2 and 3: NativeConditional requests, on `gcp_oauth` and on `gcs_hmac`. Reaching a readable
# table is the strongest single assertion available: a CAS mount runs `runCapabilityProbe`, which
# requires conditional create, conditional overwrite, a REFUSED delete on a wrong token and an
# accepted delete on the right one — all against live GCS, all before the mount is allowed to
# complete. A mounted disk means Google accepted every one of them.
# ---------------------------------------------------------------------------------------------------


def _run_cas_group(disk):
    node = cluster.instances["node"]
    table = _create(node, disk)

    node.query("INSERT INTO {} SELECT number, toString(number) FROM numbers(300)".format(table))
    node.query("INSERT INTO {} SELECT number, toString(number) FROM numbers(300, 300)".format(table))
    assert int(node.query("SELECT count() FROM {}".format(table))) == 600
    assert int(node.query("SELECT uniqExact(data) FROM {}".format(table))) == 600

    # A merge rewrites part metadata through the same conditional-write path, and dropping a partition
    # drives the exact, token-carrying DELETE.
    node.query("OPTIMIZE TABLE {} FINAL".format(table))
    node.query("ALTER TABLE {} DROP PARTITION tuple()".format(table))
    assert int(node.query("SELECT count() FROM {}".format(table))) == 0

    tokens = _cas_tokens(node, disk)
    assert tokens, "no incarnation token was recorded, so the token assertion below is vacuous"
    for token in tokens:
        assert token.isdigit(), (
            "CAS recorded a non-numeric incarnation token {!r} on {}: the response carried an ETag "
            "where the generation adapter should have supplied a generation".format(token, disk)
        )


@requires_oauth
def test_native_conditional_gcp_oauth_mounts_and_keeps_generation_tokens():
    """Group 2. Conditional PUT, native-token HEAD and exact DELETE under bearer-token auth.

    Would fail if: GCS refused a conditional create carrying `x-goog-if-generation-match`, refused an
    exact DELETE, or answered a token-producing write without a generation — the token recorded would
    then be an ETag and the digit assertion would break. It would also fail if the OAuth cleanup left
    a stale AWS signing artifact on the request that Google rejects, which is one of the two things
    only a live endpoint can settle: against `storage.googleapis.com` the `ApiMode::GCS` block in
    `Client::BuildHttpRequest` becomes active, and `test_cas_gcs` cannot reach it.
    """
    _run_cas_group(CAS_OAUTH_DISK)


@requires_hmac
def test_native_conditional_gcs_hmac_mounts_and_keeps_generation_tokens():
    """Group 3. The same three operations under GOOG4 signing.

    Would fail if: the GOOG4 signed-header allowlist produced a signature Google rejects for a
    conditional request — the conditional headers are exactly the ones an allowlist bug would drop or
    fail to cover, and no unit test can tell a signature Google accepts from one it does not.
    """
    _run_cas_group(CAS_HMAC_DISK)
