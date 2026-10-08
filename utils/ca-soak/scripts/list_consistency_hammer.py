"""Does this object store ever return an incomplete LIST of a prefix it has already durably written?

Direct corroboration of BACKLOG {#probe-a-answered}: probe A's 58 "missing from the pre-fold scan" holes
survive every alternative explanation, leaving only "the store gave two different answers about the same
durable prefix". That conclusion was reached by ELIMINATION, and one elimination rests on reading the
append path rather than on an experiment. This tests the store directly and depends on neither.

Method mirrors probe A's rule exactly, so a hit here is the same shape of evidence:

  a key counts as a HOLE only if it is (a) known durable — its PUT returned success before this listing
  began — and (b) below the maximum key the SAME listing returned. (b) is the witness: without it, a key
  written concurrently and simply not yet visible would count, which proves nothing.

Writes go to `test/listprobe/<run>/`, a different top-level prefix from `soak_pool/`, so the CAS pool is
never touched.
"""
import argparse
import concurrent.futures as cf
import threading
import time

import boto3
from botocore.config import Config


class Store:
    def __init__(self, endpoint, bucket, key, secret):
        self.bucket = bucket
        self.s3 = boto3.client(
            "s3", endpoint_url=endpoint,
            aws_access_key_id=key, aws_secret_access_key=secret,
            config=Config(signature_version="s3v4", retries={"max_attempts": 3},
                          max_pool_connections=64),
        )

    def put(self, k, body=b"x"):
        self.s3.put_object(Bucket=self.bucket, Key=k, Body=body)

    def list_all(self, prefix, page_size, mode="continuation"):
        """One complete paginated enumeration of the prefix. Returns keys in the order returned.

        Two modes, and the difference is the whole point of this tool:

        ``continuation``
            One listing SESSION resumed by the server's opaque `ContinuationToken`. This is what boto3
            paginators do by default and what the first hammer runs used.

        ``start-after``
            A FRESH listing session per page, resumed by the LAST KEY RETURNED. **This is what CAS
            actually does** — `forEachListedKey` (CasBackend.h) documents its cursor as "last key
            returned", and `ObjectStorageBackend::list` builds a new `object_storage->iterate(...,
            start_after)` for every page, abandoning the previous iterator.

        The two exercise different store code paths. Under `start-after` each page boundary STITCHES TWO
        INDEPENDENT ENUMERATIONS together, so any disagreement between them lands exactly at a boundary —
        which matches the observed hole shape (short runs of adjacent keys, not whole pages). Under
        `continuation` the server keeps one cursor and no stitching happens. The first two runs found
        nothing while testing the mode CAS does NOT use.
        """
        out, token, after = [], None, None
        pages = 0
        while True:
            kw = {"Bucket": self.bucket, "Prefix": prefix, "MaxKeys": page_size}
            if mode == "continuation":
                if token:
                    kw["ContinuationToken"] = token
            elif after is not None:
                kw["StartAfter"] = after
            r = self.s3.list_objects_v2(**kw)
            got = [o["Key"] for o in r.get("Contents", [])]
            out.extend(got)
            pages += 1
            if mode == "continuation":
                token = r.get("NextContinuationToken")
                if not r.get("IsTruncated"):
                    break
            else:
                if not got or not r.get("IsTruncated"):
                    break
                after = got[-1]          # exactly CAS's `next_cursor = page.keys.back().key`
        return out, pages

    def delete_prefix(self, prefix):
        keys, _ = self.list_all(prefix, 1000)
        for i in range(0, len(keys), 1000):
            batch = [{"Key": k} for k in keys[i:i + 1000]]
            self.s3.delete_objects(Bucket=self.bucket, Delete={"Objects": batch})
        return len(keys)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--endpoint", default="http://172.19.0.2:11121")
    ap.add_argument("--bucket", default="test")
    ap.add_argument("--run", default="r1")
    ap.add_argument("--seed-keys", type=int, default=3000)
    ap.add_argument("--rounds", type=int, default=40, help="listing rounds under concurrent write")
    ap.add_argument("--writers", type=int, default=4)
    ap.add_argument("--listers", type=int, default=3)
    ap.add_argument("--deleters", type=int, default=0,
                    help="threads deleting the OLDEST live keys while listings walk. This is the "
                         "configuration that models the real ref prefix: GC removes folded logs from "
                         "BEHIND the listing cursor, and a paginated walk over a shrinking key space is "
                         "where store implementations differ. An add-only run exercises the wrong regime.")
    ap.add_argument("--delete-batch", type=int, default=50)
    ap.add_argument("--max-keys", type=int, default=0,
                    help="TARGET live key population, held from BOTH sides: writers idle at or above it, "
                         "deleters idle below its lower band. 0 = unbounded. Neither half is optional. "
                         "Unbounded writers made the first run grow to 888k keys / 888 pages before it had "
                         "to be killed; unthrottled deleters then emptied the SECOND run from 150k to one "
                         "key by round 31, because 3 deleters at 200 keys per batch outrun 4 writers doing "
                         "single PUTs by an order of magnitude. A run whose rounds list single digits "
                         "measures nothing.")
    ap.add_argument("--page-size", type=int, default=1000, help="matches the CAS listing page size")
    ap.add_argument("--paginate", choices=["continuation", "start-after"], default="start-after",
                    help="how to resume between pages. DEFAULT is start-after because that is what CAS "
                         "does; continuation is kept only to reproduce the first runs, which found "
                         "nothing precisely because they tested the wrong mode.")
    ap.add_argument("--keep", action="store_true", help="do not delete the probe prefix afterwards")
    a = ap.parse_args()

    prefix = f"listprobe/{a.run}/"
    st = Store(a.endpoint, a.bucket, "clickhouse", "clickhouse")

    # `durable` holds every key whose PUT has RETURNED. A reader may only be blamed for missing one of
    # these. The lock makes the snapshot a listing compares against well-defined.
    durable = set()
    delete_floor = [""]      # highest key ever HANDED to a deleter; see `deleter_loop` and the hole rule
    dlock = threading.Lock()
    stop = threading.Event()
    seq = [0]
    slock = threading.Lock()

    def next_key():
        with slock:
            seq[0] += 1
            n = seq[0]
        return f"{prefix}k-{n:08d}"

    def write_one():
        k = next_key()
        st.put(k)
        with dlock:
            durable.add(k)

    print(f"seeding {a.seed_keys} keys under {prefix} ...", flush=True)
    t0 = time.time()
    with cf.ThreadPoolExecutor(max_workers=16) as ex:
        list(ex.map(lambda _: write_one(), range(a.seed_keys)))
    print(f"  seeded {len(durable)} keys in {time.time()-t0:.1f}s", flush=True)

    findings = []
    rounds_done = [0]
    rlock = threading.Lock()

    def writer_loop():
        while not stop.is_set():
            if a.max_keys:
                with dlock:
                    at_cap = len(durable) >= a.max_keys
                if at_cap:
                    time.sleep(0.02)
                    continue
            write_one()

    def deleter_loop():
        """Remove the OLDEST live keys — i.e. from behind a listing cursor that walks in key order.

        A key leaves `durable` BEFORE its DELETE is issued, never after. That ordering is what keeps the
        hole rule honest: a key still in the snapshot is one whose deletion had not even been requested
        when the listing began, so its absence cannot be excused as a race with this thread.
        """
        # Lower band of the target population. Deleting below it starves the listings; see --max-keys.
        floor_pop = int(a.max_keys * 0.9) if a.max_keys else 0
        while not stop.is_set():
            with dlock:
                if floor_pop and len(durable) <= floor_pop:
                    victims = []
                else:
                    victims = sorted(durable)[:a.delete_batch]
                for v in victims:
                    durable.discard(v)
                # Raise the deletion floor BEFORE the DELETE is issued. Deletion is monotone from the
                # low end -- the deleter always takes the smallest live keys -- so "at or below the
                # floor" is exactly "may legitimately be gone", and the floor is the whole bookkeeping
                # the hole rule needs.
                if victims:
                    delete_floor[0] = max(delete_floor[0], victims[-1])
            if not victims:
                time.sleep(0.05)
                continue
            st.s3.delete_objects(Bucket=a.bucket,
                                 Delete={"Objects": [{"Key": v} for v in victims]})

    def lister_loop():
        while True:
            with rlock:
                if rounds_done[0] >= a.rounds:
                    return
                rounds_done[0] += 1
                mine = rounds_done[0]
            with dlock:
                snapshot = set(durable)           # known durable BEFORE this listing began
            keys, pages = st.list_all(prefix, a.page_size, a.paginate)
            got = set(keys)
            if not got:
                continue
            with dlock:
                floor_after = delete_floor[0]     # anything at or below this may legitimately be gone
            witness = max(got)                    # probe A's witness: the listing's own maximum
            # Three conditions, and the floor one is NOT optional once deleters run. Without it every key
            # legitimately deleted DURING the walk counts as a hole, and the run manufactures exactly the
            # finding it is supposed to test for. `> floor_after` uses the floor as of the walk's END, so
            # a key deleted at any point during the walk is excluded.
            holes = sorted(k for k in snapshot
                           if k < witness and k > floor_after and k not in got)
            dupes = len(keys) - len(got)
            with rlock:
                findings.append({"round": mine, "listed": len(got), "pages": pages,
                                 "snapshot": len(snapshot), "holes": holes, "dupes": dupes})
            excluded = sum(1 for k in snapshot if k <= floor_after)
            print(f"  round {mine:>3}: listed={len(got):>6} pages={pages:>3} "
                  f"durable_before={len(snapshot):>6} deleted_under_walk={excluded:>6} "
                  f"HOLES={len(holes)} dupes={dupes}", flush=True)

    print(f"{a.rounds} listing rounds, {a.writers} writers + {a.listers} listers + "
          f"{a.deleters} deleters, page_size={a.page_size} ...", flush=True)
    with cf.ThreadPoolExecutor(max_workers=a.writers + a.listers + a.deleters) as ex:
        futs = [ex.submit(lister_loop) for _ in range(a.listers)]
        bg = [ex.submit(writer_loop) for _ in range(a.writers)]
        bg += [ex.submit(deleter_loop) for _ in range(a.deleters)]
        for f in futs:
            f.result()
        stop.set()
        for f in bg:
            f.result()

    total_holes = sum(len(f["holes"]) for f in findings)
    total_dupes = sum(f["dupes"] for f in findings)
    print("\n================ VERDICT ================")
    print(f"listing rounds        : {len(findings)}")
    print(f"keys durable at end   : {len(durable)}")
    print(f"rounds WITH holes     : {sum(1 for f in findings if f['holes'])}")
    print(f"total holes           : {total_holes}")
    print(f"total duplicate keys  : {total_dupes}")
    if total_holes:
        worst = max(findings, key=lambda f: len(f["holes"]))
        print(f"worst round           : {worst['round']} with {len(worst['holes'])} holes")
        print(f"  sample              : {worst['holes'][:5]}")
        print("\nA key here was durable BEFORE the listing started and sits BELOW a key the SAME listing"
              "\nreturned. The store returned an incomplete answer about a prefix it had already written.")
    else:
        print("\nNo hole observed. This does NOT clear the store — probe A's holes were 58 in ~1.4M"
              "\nkeys listed across a 4-hour run, so absence over a short hammer is weak evidence."
              "\nScale the run or add write pressure before drawing any conclusion.")

    if not a.keep:
        n = st.delete_prefix(prefix)
        print(f"\ncleaned up {n} probe keys")


if __name__ == "__main__":
    main()
