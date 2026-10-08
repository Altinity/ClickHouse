from soak.rng import splitmix64, seeded_stream

def test_splitmix64_is_deterministic_and_64bit():
    a = splitmix64(0)
    b = splitmix64(0)
    assert a == b
    assert 0 <= a < 2**64
    assert splitmix64(1) != splitmix64(2)

def test_splitmix64_known_vector():
    # splitmix64 with state increment 0x9E3779B97F4A7C15; first output for seed 0.
    assert splitmix64(0) == 16294208416658607535

def test_seeded_stream_reproducible():
    assert list(seeded_stream(42, 5)) == list(seeded_stream(42, 5))
    assert list(seeded_stream(42, 5)) != list(seeded_stream(43, 5))
