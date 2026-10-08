#!/usr/bin/env python3
"""Pick the faster of the dataset mirror and public S3."""

from __future__ import annotations

import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Dict, Optional, Tuple

TIMEOUT = 2.0

PUBLIC_DATASETS_WEB = "https://clickhouse-datasets-web.s3.us-east-1.amazonaws.com"
PUBLIC_TPCDS = "https://tpc-ds-sf1.s3.amazonaws.com"
MIRROR_DATASETS_WEB = "http://dockerhub-proxy.dockerhub-proxy-zone:6000/datasets-web"
MIRROR_TPCDS = "http://dockerhub-proxy.dockerhub-proxy-zone:6000/tpc-ds-sf1"

ENDPOINTS: Dict[str, Tuple[str, str]] = {
    "datasets-web": (MIRROR_DATASETS_WEB, PUBLIC_DATASETS_WEB),
    "tpcds": (MIRROR_TPCDS, PUBLIC_TPCDS),
}


def _probe(url: str, timeout: float = TIMEOUT) -> Optional[float]:
    box: Dict[str, float] = {}

    def run():
        t0 = time.monotonic()
        try:
            req = urllib.request.Request(url, headers={"Range": "bytes=0-0"})
            urllib.request.urlopen(req, timeout=timeout).close()
            box["elapsed"] = time.monotonic() - t0
        except urllib.error.HTTPError:
            # Any HTTP response means the host answered.
            box["elapsed"] = time.monotonic() - t0
        except Exception:
            pass

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    thread.join(timeout + 0.5)
    return box.get("elapsed")


def pick(name: str, timeout: float = TIMEOUT) -> str:
    try:
        mirror, public = ENDPOINTS[name]
    except KeyError as e:
        raise ValueError(f"unknown endpoint pair [{name}]") from e

    with ThreadPoolExecutor(max_workers=2) as pool:
        future_mirror = pool.submit(_probe, mirror, timeout)
        future_public = pool.submit(_probe, public, timeout)
        t_mirror = future_mirror.result()
        t_public = future_public.result()

    candidates = []
    if t_mirror is not None:
        candidates.append((t_mirror, mirror))
    if t_public is not None:
        candidates.append((t_public, public))
    if not candidates:
        raise RuntimeError(
            f"pick_endpoint [{name}]: neither [{mirror}] nor [{public}] "
            f"responded within {timeout}s"
        )
    candidates.sort()
    winner = candidates[0][1]
    print(
        f"pick_endpoint [{name}]: {winner} ({candidates[0][0]:.3f}s)",
        file=sys.stderr,
    )
    return winner


def rewritten_create_sql(sql: str, web_base: str) -> str:
    return sql.replace(PUBLIC_DATASETS_WEB, web_base)


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "create-sql":
        if len(argv) != 2:
            print("usage: pick_endpoint.py create-sql <path>", file=sys.stderr)
            return 2
        try:
            sys.stdout.write(
                rewritten_create_sql(Path(argv[1]).read_text(), pick("datasets-web"))
            )
        except RuntimeError as e:
            print(e, file=sys.stderr)
            return 1
        return 0
    if len(argv) != 1 or argv[0] not in ENDPOINTS:
        names = "|".join(ENDPOINTS)
        print(f"usage: pick_endpoint.py {names}|create-sql <path>", file=sys.stderr)
        return 2
    try:
        print(pick(argv[0]))
    except RuntimeError as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
