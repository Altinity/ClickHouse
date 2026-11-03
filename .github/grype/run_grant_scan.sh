set -x
set -e

IMAGE=$1

GRANT_VERSION=${GRANT_VERSION:-"v0.6.8"}
SAFE_IMAGE=$(echo "$IMAGE" | sed 's/[\/:]/_/g')

docker pull anchore/grant:${GRANT_VERSION}

docker run --rm \
 --volume /var/run/docker.sock:/var/run/docker.sock \
 --name Grant anchore/grant:${GRANT_VERSION} \
 list "$IMAGE" --output json > "${SAFE_IMAGE}-licenses.json"

ls -sh
