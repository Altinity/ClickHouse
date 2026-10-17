"""The card interface: one fixed workload measured on one disk."""
import threading
from concurrent.futures import ThreadPoolExecutor

DB = 'yardstick'
TABLE_SETTINGS = 'primary_key_lazy_load = 0, prewarm_mark_cache = 0, prewarm_primary_key_cache = 0'


# Merging needs two or more parts, so this keeps the part count fixed (SYSTEM STOP MERGES would also block
# mutations and is lost on restart and DETACH).
NO_MERGES = 'max_parts_to_merge_at_once = 1'

# One stream per INSERT ... SELECT, so each statement writes exactly one part.
SINGLE_PART = {'max_insert_threads': 1}


def table_engine(policy: str, extra_settings: str = '') -> str:
    """Engine clause every card table uses, so "cold" means the same on every disk."""
    extra = f', {extra_settings}' if extra_settings else ''
    return f"ENGINE = MergeTree ORDER BY k SETTINGS storage_policy = '{policy}', {TABLE_SETTINGS}{extra}"


def supports_mutations(policy: str) -> tuple[bool, str]:
    """The plain-rewritable disk is immutable: the server rejects every ALTER ... UPDATE and DELETE on it."""
    if policy.startswith('s3plain'):
        return False, 'mutations are not supported on the immutable s3plain disk'
    return True, ''


def parts_and_rows(c, table: str) -> tuple[int, int]:
    """Active part count and row count of `db.name` from part metadata, without reading table data."""
    db, name = table.split('.', 1)
    text = c.query(f"SELECT count(), sum(rows) FROM system.parts WHERE database = '{db}' AND table = '{name}' "
                   'AND active')
    parts, rows = (int(x) for x in text.split())
    return parts, rows


def run_workers(count: int, work) -> list:
    """Results of `work(w, stop)` for w in range(count), each on its own thread, in worker order.
    A failing worker sets `stop`, which `work` checks between its operations. On Ctrl-C the pool is not joined: the
    runner's teardown and server stop end the queries still in flight."""
    stop = threading.Event()

    def guarded(w):
        try:
            return work(w, stop)
        except BaseException:
            stop.set()
            raise

    pool = ThreadPoolExecutor(count)
    interrupted = False
    try:
        return [f.result() for f in [pool.submit(guarded, w) for w in range(count)]]
    except KeyboardInterrupt:
        interrupted = True
        stop.set()
        raise
    finally:
        pool.shutdown(wait=not interrupted, cancel_futures=interrupted)


class Card:
    """Per pass: setup, optional cold reset, measured cold, measured hot if `has_hot`, teardown.
    Instances are reused across passes: `measured` must reset any state kept on the card."""
    name: str
    unit: str
    has_hot: bool = True
    stops_merges: bool  # merges cannot change the part count during the card
    COUNTS: dict[str, int] = {}  # every count and row parameter; `counts(scale)` scales them all

    @property
    def table(self) -> str:
        """What the runner settles before each window: `db.name`, or `db.*`."""
        return f'{DB}.{self.name}'

    def supports(self, policy: str) -> tuple[bool, str]:
        return True, ''

    def counts(self, scale: float) -> dict[str, int]:
        return {k: max(1, round(scale * n)) for k, n in self.COUNTS.items()}

    def setup(self, c, policy: str, counts: dict) -> None:
        raise NotImplementedError

    def before_window(self, c, phase: str) -> None:
        """Unmeasured preparation the runner calls right before settling a window."""

    def cold_reset_needed(self) -> bool:
        """True when `setup` builds a fixture that the cold pass must read from a restarted process."""
        raise NotImplementedError

    def measured(self, c, policy: str, counts: dict, phase: str) -> dict:
        """Return {'unit_count': int, 'ops': list[dict]}; each op row carries `ms`, the whole unit's time."""
        raise NotImplementedError

    def invariants(self, c, counts: dict, phase: str, when: str) -> dict:
        """Return name -> (expected, actual) for `when` in ('before', 'after')."""
        raise NotImplementedError

    def teardown(self, c) -> None:
        c.query(f'DROP DATABASE IF EXISTS {DB} SYNC')
