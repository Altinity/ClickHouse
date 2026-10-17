from soak.rng import splitmix64, MASK64

MAX_BLOCK = 1_000_000       # an insert block is never larger; rid = op_id*MAX_BLOCK + j
NBUCKETS = 64               # partition/order spread
KSPACE = 1_000_000          # primary-key k space
SHARED_CONTENT = 128        # payload content slots per bucket (drives CA dedup)
PAYLOAD_LEN = 256           # bytes per payload (moderate; TTL bounds total)
BASE_TIME = 1_700_000_000   # fixed epoch base; ts = BASE_TIME + (op_id % TS_WINDOW)
TS_WINDOW = 7200            # seconds of ts spread

def det_blob(seed: int, bucket: int, slot: int) -> str:
    """Deterministic payload for (bucket, content slot). Identical (bucket,slot) -> identical bytes
    -> identical content blob in the CA pool (real cross-part/replica dedup)."""
    h = splitmix64(seed ^ (bucket * 1009) ^ (slot * 0x100000001B3))
    out = []
    while len(out) * 16 < PAYLOAD_LEN:
        h = splitmix64(h)
        out.append(f"{h:016x}")
    return "".join(out)[:PAYLOAD_LEN]

def insert_rids(op_id: int, n: int):
    assert n <= MAX_BLOCK
    return [op_id * MAX_BLOCK + j for j in range(n)]

def row_for_rid(seed: int, rid: int, base_time: int = BASE_TIME) -> dict:
    op_id = rid // MAX_BLOCK
    bucket = rid % NBUCKETS
    k = splitmix64(rid) % KSPACE
    slot = rid % SHARED_CONTENT
    v0 = (splitmix64(rid ^ 0x5a5a) % 2001) - 1000     # small signed init
    return {
        "op_id": op_id,
        "writer": op_id % 4,
        "bucket": bucket,
        "k": k,
        "ts": base_time + (op_id % TS_WINDOW),
        "version": 1,
        "v": v0,
        "payload": det_blob(seed, bucket, slot),
        "row_fp": splitmix64(rid),                     # IMMUTABLE identity
    }
