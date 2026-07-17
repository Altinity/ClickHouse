#!/usr/bin/env python3
"""Targeted acked-then-lost reproduction: hammer distinct SYNC dedup inserts while rustfs is
paused past the CAS budget + ch2 is restarted, tracking every insert the SERVER acked (HTTP 200).
Then diff acked-vs-present and (if any acked row is absent) that is the data-loss to trace.
Runs against the preserved repro cluster (ch1 http :8123, ch2 http :8124)."""
import concurrent.futures as cf, subprocess, sys, time, urllib.request, urllib.error, threading

CH1 = "http://localhost:8123/"
TABLE = "dl_probe"
PAYLOAD = "x" * 20000  # ~20KB/row so the manifest PUT is non-trivial and catches the pause
acked = set(); acked_lock = threading.Lock()
stop = threading.Event()

def q(url, sql, timeout=120):
    req = urllib.request.Request(url + "?" + urllib.parse.urlencode({"query": sql}), method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode()

import urllib.parse
def ddl():
    for port in (8123, 8124):
        u = f"http://localhost:{port}/"
        q(u, f"DROP TABLE IF EXISTS {TABLE} SYNC")
    for port, rep in ((8123, "r1"), (8124, "r2")):
        u = f"http://localhost:{port}/"
        q(u, f"CREATE TABLE {TABLE} (id UInt64, payload String) ENGINE=ReplicatedMergeTree('/clickhouse/tables/{TABLE}','{rep}') ORDER BY id SETTINGS storage_policy='ca'")
    print("ddl ok", flush=True)

def insert_one(i):
    # byte-identical retry until the SERVER acks (HTTP 200) or we give up; record acked ids.
    sql = f"INSERT INTO {TABLE} SETTINGS async_insert=0, insert_deduplicate=1 VALUES ({i}, '{PAYLOAD}')"
    deadline = time.time() + 240
    attempt = 0
    while time.time() < deadline and not stop.is_set():
        attempt += 1
        try:
            q(CH1, sql, timeout=100)
            with acked_lock: acked.add(i)      # server returned 200 => acked
            return
        except Exception as e:
            time.sleep(1.5)                    # retry the byte-identical insert (like the soak driver)
    print(f"insert {i} gave up after {attempt} attempts", flush=True)

def dock(*a): subprocess.run(["docker", *a], capture_output=True)

next_id = [0]; id_lock = threading.Lock(); submitted = [0]
def worker(stop_time):
    # keep inserting fresh ids until stop_time so the stream spans the ENTIRE fault window
    while time.time() < stop_time and not stop.is_set():
        with id_lock:
            next_id[0] += 1; i = next_id[0]; submitted[0] += 1
        insert_one(i)

def main():
    ddl()
    ex = cf.ThreadPoolExecutor(max_workers=8)
    # CONTINUOUS inserts for ~150s so the stream overlaps the whole pause; fault controller:
    #   t+8 PAUSE rustfs (105s > 90s CAS budget) ; t+16 RESTART ch2 ; t+113 UNPAUSE rustfs
    def faults():
        time.sleep(8); print("PAUSE rustfs", flush=True); dock("pause", "ca-soak-rustfs1-1")
        time.sleep(8); print("KILL ch2", flush=True); dock("kill", "ca-soak-ch2-1")
        time.sleep(50); print("START ch2", flush=True); dock("start", "ca-soak-ch2-1")
        time.sleep(47); print("UNPAUSE rustfs", flush=True); dock("unpause", "ca-soak-rustfs1-1")
    ft = threading.Thread(target=faults, daemon=True); ft.start()
    stop_time = time.time() + 150
    futs = [ex.submit(worker, stop_time) for _ in range(8)]
    for _ in cf.as_completed(futs): pass
    ft.join(timeout=5)
    print(f"submitted={submitted[0]} acked={len(acked)}", flush=True)
    # settle: let self-remount + replication converge
    for _ in range(24):
        try: q(CH1, "SELECT 1", timeout=10); break
        except Exception: time.sleep(5)
    time.sleep(30)
    q(CH1, f"SYSTEM SYNC REPLICA {TABLE}", timeout=180)
    present = set(int(x) for x in q(CH1, f"SELECT id FROM {TABLE} ORDER BY id FORMAT TabSeparated").split())
    lost = sorted(acked - present)
    print(f"PRESENT={len(present)} ACKED={len(acked)} LOST(acked-but-absent)={len(lost)}", flush=True)
    print("LOST_IDS=" + ",".join(map(str, lost[:50])), flush=True)
    for port in (8123, 8124): q(f"http://localhost:{port}/", "SYSTEM FLUSH LOGS", timeout=60)
    print("DL_PROBE_DONE", flush=True)

if __name__ == "__main__":
    main()
