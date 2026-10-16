# UBI 9 release images

Release builds produce Red Hat UBI 9 variants of the existing multi-architecture
ClickHouse Server and ClickHouse Keeper images. They use the same release
artifacts as the other image variants and are published with an `-ubi9` suffix:

- `altinity/clickhouse-server:<version>-ubi9`
- `altinity/clickhouse-keeper:<version>-ubi9`

The Dockerfiles download the release `.tgz` artifacts and their `.sha512`
files, verify every checksum, and install the files over a fresh UBI base. The
final images run as the existing ClickHouse UID and GID, `101:101`.

## Release and certification automation

The normal release build includes `ubi9` in the Server and Keeper OS matrix.
The normal publish workflow preserves the `-ubi9` suffix when it copies the
multi-architecture manifest from the staging registry to Docker Hub. Grype also
scans both UBI variants.

For production releases, the publish workflow can submit the immutable UBI
manifest digest to Red Hat Preflight. Configure these GitHub Actions settings:

- Repository variable `RED_HAT_CERTIFICATION_ENABLED` set to `true`.
- Repository secret `RED_HAT_PYXIS_API_TOKEN` containing the Red Hat API token.
- Repository secret `RED_HAT_CLICKHOUSE_SERVER_CERTIFICATION_COMPONENT_ID`
  containing the Server project component ID.
- Repository secret `RED_HAT_CLICKHOUSE_KEEPER_CERTIFICATION_COMPONENT_ID`
  containing the Keeper project component ID.

Certification is intentionally skipped for staging releases and non-UBI image
variants. Each submission also uploads the Preflight result as a workflow
artifact for release auditing.
