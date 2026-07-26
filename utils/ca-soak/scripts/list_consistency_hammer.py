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

    def list_all(self, prefix, page_size):
        """One complete paginated enumeration of the prefix. Returns keys in the order returned."""
        out, token = [], None
        pages = 0
        while True:
            kw = {"Bucket": self.bucket, "Prefix": prefix, "MaxKeys": page_size}
            if token:
                kw["ContinuationToken"] = token
            r = self.s3.list_objects_v2(**kw)
            out.extend(o["Key"] for o in r.get("Contents", []))
            pages += 1
            token = r.get("NextContinuationToken")
            if not r.get("IsTruncated"):
                break
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
    ap.add_argument("--page-size", type=int, default=1000, help="matches the CAS listing page size")
    ap.add_argument("--keep", action="store_true", help="do not delete the probe prefix afterwards")
    a = ap.parse_args()

    prefix = f"listprobe/{a.run}/"
    st = Store(a.endpoint, a.bucket, "clickhouse", "clickhouse")

    # `durable` holds every key whose PUT has RETURNED. A reader may only be blamed for missing one of
    # these. The lock makes the snapshot a listing compares against well-defined.
    durable = set()
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
            write_one()

    def deleter_loop():
        """Remove the OLDEST live keys — i.e. from behind a listing cursor that walks in key order.

        A key leaves `durable` BEFORE its DELETE is issued, never after. That ordering is what keeps the
        hole rule honest: a key still in the snapshot is one whose deletion had not even been requested
        when the listing began, so its absence cannot be excused as a race with this thread.
        """
        while not stop.is_set():
            with dlock:
                victims = sorted(durable)[:a.delete_batch]
                for v in victims:
                    durable.discard(v)
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
            keys, pages = st.list_all(prefix, a.page_size)
            got = set(keys)
            if not got:
                continue
            witness = max(got)                    # probe A's witness: the listing's own maximum
            holes = sorted(k for k in snapshot if k < witness and k not in got)
            dupes = len(keys) - len(got)
            with rlock:
                findings.append({"round": mine, "listed": len(got), "pages": pages,
                                 "snapshot": len(snapshot), "holes": holes, "dupes": dupes})
            print(f"  round {mine:>3}: listed={len(got):>6} pages={pages:>3} "
                  f"durable_before={len(snapshot):>6} HOLES={len(holes)} dupes={dupes}", flush=True)

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
