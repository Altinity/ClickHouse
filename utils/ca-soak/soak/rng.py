MASK64 = (1 << 64) - 1

def splitmix64(x: int) -> int:
    """Standard SplitMix64 finalizer. Deterministic 64-bit mix; used everywhere a value must be
    derived reproducibly from an integer. NEVER reimplemented in SQL — stored values are summed."""
    z = (x + 0x9E3779B97F4A7C15) & MASK64
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return (z ^ (z >> 31)) & MASK64

def seeded_stream(seed: int, n: int):
    """A reproducible stream of n 64-bit values from a seed (for ledger/chaos schedules)."""
    state = seed & MASK64
    for _ in range(n):
        state = splitmix64(state)
        yield state
