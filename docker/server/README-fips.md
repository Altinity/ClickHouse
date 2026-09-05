# ClickHouse server on Ubuntu 22.04 FIPS

`Dockerfile.ubuntu-fips` builds the ClickHouse server image on an Ubuntu 22.04 FIPS base instead
of stock `ubuntu:22.04`.

There is no public `ubuntu-2204-fips` image to pull. Canonical publishes FIPS images only as
cloud VM images, not as OCI base images, so the FIPS base is produced as part of this build from
`ubuntu:22.04` plus an Ubuntu Pro subscription.

## Prerequisites

1. **An Ubuntu Pro subscription with FIPS entitlement**, to build. The token is supplied as a
   BuildKit secret and never lands in the resulting image.
2. **Ubuntu Pro coverage on every host that runs the image**, including Kubernetes worker nodes.
3. **A FIPS-enabled host kernel.** Containers share the host kernel, so
   `/proc/sys/crypto/fips_enabled` inside the container reflects the host. The image is not a
   FIPS-compliant deployment on a non-FIPS host.

## Build

Create `pro-attach-config.yaml`:

```yaml
token: <your-ubuntu-pro-token>
enable_services:
  - esm-infra
  - esm-apps
  - fips-updates
```

Then build:

```bash
docker buildx build docker/server -f docker/server/Dockerfile.ubuntu-fips --secret id=pro-attach-config,src=pro-attach-config.yaml --build-arg DIRECT_DOWNLOAD_URLS="<clickhouse deb urls>" --load -t altinity/clickhouse-server:<version>-fips
```

## Updating a running container

The image ships **detached**: FIPS packages installed, but no subscription token
or ESM apt sources. To take updates, attach your own subscription inside the
container:

```bash
pro attach <your-token>
```

That restores everything `pro detach` removed at build time: the ESM apt
sources, the archive keyrings, the apt credentials, and the FIPS apt pin. From
there `apt update && apt upgrade` behaves like any Ubuntu Pro FIPS machine.

## Verify

```bash
./docker/server/fips-verify.sh altinity/clickhouse-server:<version>-fips
```

The script mirrors the compliance checklist. It needs `docker` and `trivy` on PATH. Exit status:

| Code | Meaning |
|------|---------|
| 0 | every hard check passed |
| 1 | at least one check failed |
| 2 | the image could not be evaluated (not Ubuntu, no dpkg, no trivy, not a ClickHouse image) |

## Safety of the Ubuntu Pro token

All Pro work happens in a single `RUN`, so the token never lands in a layer.

1. `pro attach` enables `esm-infra`, `esm-apps` and `fips-updates`.
2. `apt-get upgrade` pulls the ESM/FIPS-patched packages, then the build installs the FIPS
   userspace packages by name.
3. `pro detach` removes the machine token, the ESM apt sources, the archive keyrings and the FIPS
   apt pin. The FIPS packages stay installed.
4. `ubuntu-pro-client` stays installed so you can attach at runtime. `ca-certificates` stays too;
   `wget` needs it during the build, the server needs it afterwards.

## ClickHouse's own FIPS status

ClickHouse uses AWS-LC-FIPS-2.0.0 (CMVP cert #4816, FIPS 140-3 Level 1),
statically linked; `ldd` on the binary shows no `libssl`/`libcrypto`. The
Ubuntu FIPS base addresses the surrounding userspace and the compliance
checklist, not ClickHouse's cryptography. The server logs a FIPS banner at
startup reporting its KAT and integrity self-test results.
