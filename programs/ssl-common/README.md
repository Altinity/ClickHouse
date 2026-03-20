# Running SSL and ACVP Tests Manually for ClickHouse FIPS Build

This guide explains how to run the AWS-LC SSL conformance tests and ACVP
cryptographic algorithm validation tests against the ClickHouse FIPS binary.

## Prerequisites

- ClickHouse built with `-DFIPS_CLICKHOUSE=1` and `-DAWSLC_SRC_DIR=<path>`
  (or the auto-populated source from the Docker build)
- The resulting binary must contain the `ssl-shim`, `ssl-handshaker`, and
  `acvp-server` modes (verify with `./clickhouse ssl-shim --help`)
- **Go >= 1.13** (the AWS-LC test harness is written in Go)
- A checkout of the AWS-LC source matching the FIPS build version
  (`AWS-LC-FIPS-2.0.0`), which is the same tree referenced by `AWSLC_SRC_DIR`

Throughout this document, the following paths are used as examples:

| Variable | Example Path |
|---|---|
| `CLICKHOUSE_BUILD` | `/path/to/ClickHouse/build` |
| `AWSLC_SRC` | `/path/to/aws-lc` (the AWS-LC source root) |

## SSL Tests (8,037 tests)

The SSL test suite exercises the full TLS stack: handshake flows, cipher
negotiation, session resumption, certificate handling, QUIC transport, and
more. It uses a Go test runner that drives the ClickHouse `ssl-shim` and
`ssl-handshaker` binaries.

### Create symlinks

The Go test runner invokes `bssl_shim` and `handshaker` by path. Create
symlinks from the ClickHouse binary:

```bash
ln -sf "$CLICKHOUSE_BUILD/programs/clickhouse" "$CLICKHOUSE_BUILD/programs/clickhouse-ssl-shim"
ln -sf "$CLICKHOUSE_BUILD/programs/clickhouse" "$CLICKHOUSE_BUILD/programs/clickhouse-ssl-handshaker"
```

### Run the tests

```bash
cd "$AWSLC_SRC/ssl/test/runner"

go test -v . \
  -shim-path    "$CLICKHOUSE_BUILD/programs/clickhouse-ssl-shim" \
  -handshaker-path "$CLICKHOUSE_BUILD/programs/clickhouse-ssl-handshaker" \
  -num-workers 16
```

`-num-workers` defaults to the number of CPU cores. Adjust as needed.

### Expected results

All 8,037 tests should pass. A passing run ends with output similar to:

```
PASS
ok  	boringssl.googlesource.com/boringssl/ssl/test/runner	142.538s
```

If any tests fail, the runner prints the failing test name and a diff of
the expected vs. actual TLS behavior.

## ACVP Tests (31 algorithm suites)

The ACVP (Automated Cryptographic Validation Protocol) tests validate that
the FIPS cryptographic module produces correct outputs for known-answer test
vectors. AWS-LC bundles 31 algorithm suites with pre-computed expected
results.

### Build the test tools

```bash
cd "$AWSLC_SRC/util/fipstools/acvp/acvptool"

# Build acvptool (the ACVP JSON ↔ modulewrapper translator)
go build -o /tmp/acvptool .

# Build testmodulewrapper (needed by 2 of the 31 test suites)
cd "$AWSLC_SRC/util/fipstools/acvp/acvptool/testmodulewrapper"
go build -o /tmp/testmodulewrapper .
```

### Create the acvp-server symlink

```bash
ln -sf "$CLICKHOUSE_BUILD/programs/clickhouse" "$CLICKHOUSE_BUILD/programs/clickhouse-acvp-server"
```

### Run the tests

```bash
cd "$AWSLC_SRC/util/fipstools/acvp/acvptool/test"

go run check_expected.go \
  -tool /tmp/acvptool \
  -module-wrappers "modulewrapper:$CLICKHOUSE_BUILD/programs/clickhouse-acvp-server,testmodulewrapper:/tmp/testmodulewrapper" \
  -tests tests.json
```

### Expected results

All 31 test suites should pass (some have no expected output and only verify
that the module does not crash):

```
All tests passed
```

The 31 validated algorithm suites are:

| Category | Algorithms |
|---|---|
| AES | CBC, CCM, CTR, ECB, GCM, GMAC, KW, KWP, XTS, CBC-CS3 (via testmodulewrapper) |
| DRBG | ctrDRBG, hmacDRBG (via testmodulewrapper) |
| ECDSA | KeyGen, SigGen, SigVer |
| HMAC | SHA-1, SHA2-224, SHA2-256, SHA2-384, SHA2-512, SHA2-512/256 |
| KAS | ECC-SSC, FFC-SSC |
| KDF | SP800-108, kdf-components (TLS, SSH), HKDF |
| RSA | KeyGen, SigGen, SigVer |
| TLS | TLS-1.2-KDF |
| Other | PBKDF, AES-GCM-internal-IV |

## Troubleshooting

### `clickhouse ssl-shim` says "unknown mode"

The binary was built without the FIPS test targets. Re-run CMake with:

```bash
cmake .. -DFIPS_CLICKHOUSE=1 -DAWSLC_SRC_DIR=/path/to/aws-lc
```

and rebuild. If `AWSLC_SRC_DIR` was auto-populated from the Docker build,
ensure the Docker build completed successfully (check that
`build/awslc-build/awslc-src/ssl/test/bssl_shim.cc` exists).

### Go test runner shows fewer than 8,037 tests

Make sure you are running from the `ssl/test/runner/` directory inside the
AWS-LC source tree, not from a different BoringSSL checkout.

### ACVP `check_expected.go` fails with "wrapper returned error"

Verify that `clickhouse-acvp-server` symlink exists and points to the
ClickHouse binary. Also verify it is executable:

```bash
"$CLICKHOUSE_BUILD/programs/clickhouse-acvp-server" --help
```

### Permission denied on Docker socket during build

The AWS-LC FIPS libraries are built inside a Docker container per the FIPS
140-3 security policy. Ensure the build user has access to the Docker
daemon (is in the `docker` group or has rootless Docker configured).
