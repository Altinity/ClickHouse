#!/bin/bash
# RustFS container for the yardstick.
# usage: rustfs.sh start [--data DIR] | stop | status | rm-prefix <bucket/prefix>
set -euo pipefail

NAME=cas-yardstick-rustfs
IMAGE=rustfs/rustfs:1.0.0-rc.3
MC_IMAGE=minio/mc:RELEASE.2025-04-16T18-13-26Z
PORT=19121
DATA="$(realpath "$(dirname "$0")/../..")/tmp/cas-yardstick/rustfs-data"
MARKER=.cas-yardstick-rustfs
MC_TIMEOUT=60
# A full run leaves a prefix too large to remove within MC_TIMEOUT.
RM_PREFIX_TIMEOUT=1800

mc()
{
    # A fixed name lets a timeout remove the container, which outlives the killed client.
    timeout "$MC_TIMEOUT" docker run --rm --name cas-yardstick-mc --network host --entrypoint sh "$MC_IMAGE" -c \
        "mc alias set r http://127.0.0.1:$PORT clickhouse clickhouse >/dev/null && $1" \
        || { docker rm -f cas-yardstick-mc > /dev/null 2>&1; return 1; }
}

cmd=${1:-}
shift || true

case "$cmd" in
    start)
        while [ $# -gt 0 ]; do
            case "$1" in
                --data) DATA=$2; shift 2 ;;
                *) echo "unknown argument: $1" >&2; exit 2 ;;
            esac
        done
        docker rm -f "$NAME" > /dev/null 2>&1 || true
        # Refuse to wipe a directory this script did not create.
        if [ -e "$DATA" ] && [ -n "$(ls -A "$DATA")" ] && [ ! -f "$DATA/$MARKER" ]; then
            echo "refusing to wipe non-empty $DATA without $MARKER" >&2
            exit 1
        fi
        rm -rf "$DATA"
        mkdir -p "$DATA"
        touch "$DATA/$MARKER"
        docker run -d --name "$NAME" --network host --user "$(id -u):$(id -g)" \
            -e RUSTFS_SCANNER_ENABLED=false -e RUSTFS_HEAL_ENABLED=false \
            -e RUSTFS_RUNTIME_MAX_BLOCKING_THREADS=64 -e RUSTFS_RUNTIME_THREAD_KEEP_ALIVE=5 \
            -e RUSTFS_ALLOCATOR_RECLAIM_ENABLED=true -e RUSTFS_ALLOCATOR_RECLAIM_INTERVAL_SECS=30 \
            -e RUSTFS_DURABILITY_MODE=relaxed -e RUSTFS_ACCESS_KEY=clickhouse -e RUSTFS_SECRET_KEY=clickhouse \
            -e "RUSTFS_ADDRESS=0.0.0.0:$PORT" -e RUSTFS_OBS_USE_STDOUT=true \
            -v "$DATA:/data" --ulimit nofile=1048576:1048576 \
            --entrypoint /usr/bin/rustfs "$IMAGE" /data > /dev/null
        ready=0
        for _ in $(seq 1 30); do
            # An unauthenticated request is answered with 403 once the S3 endpoint is up.
            if [ "$(curl -s --max-time 5 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/" || true)" = 403 ]; then
                ready=1
                break
            fi
            sleep 1
        done
        if [ "$ready" != 1 ]; then
            echo "rustfs not ready after 30 s; container logs follow" >&2
            docker logs "$NAME" 2>&1 | tail -20 >&2
            exit 1
        fi
        mc "mc mb --ignore-existing r/test" > /dev/null
        echo "endpoint=http://127.0.0.1:$PORT image=$IMAGE"
        ;;
    stop)
        docker rm -f "$NAME" > /dev/null 2>&1 || true
        ;;
    status)
        if [ "$(docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null || true)" != true ]; then
            echo "rustfs container $NAME is not running" >&2
            exit 1
        fi
        echo "image=$(docker inspect -f '{{.Config.Image}}' "$NAME")"
        ;;
    rm-prefix)
        target=${1:?usage: rustfs.sh rm-prefix <bucket/prefix>}
        MC_TIMEOUT=$RM_PREFIX_TIMEOUT
        mc "mc rm --recursive --force r/$target"
        ;;
    *)
        echo "usage: $0 start [--data DIR] | stop | status | rm-prefix <bucket/prefix>" >&2
        exit 2
        ;;
esac
