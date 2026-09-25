set -x
set -e

IMAGE=$1
VERSION=$2

SYFT_VERSION=${SYFT_VERSION:-"v1.51.1"}

docker pull anchore/syft:${SYFT_VERSION}

docker run --rm \
 --volume /var/run/docker.sock:/var/run/docker.sock \
 --volume "$PWD:/out" --workdir /out \
 --name Syft anchore/syft:${SYFT_VERSION} \
 --scope all-layers \
 -o table \
 -o "spdx-json=${VERSION}.spdx.json" \
 -o "cyclonedx-json=${VERSION}.cdx.json" \
 "$IMAGE"

ls -sh
