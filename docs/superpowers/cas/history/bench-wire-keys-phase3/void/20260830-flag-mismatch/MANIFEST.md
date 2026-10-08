# VOID — this run's two sides were not comparable

These are the throughput, byte and assembly artifacts of the first phase-3 measurement. They must
not be quoted. The two benchmark binaries were compiled with different flags, which was found by an
external campaign review after the numbers had already been published:

| | after side (`build/`) | before side (`cas-p2-before/build/`) |
|---|---|---|
| ISA baseline | `-march=x86-64-v2` (`X86_ARCH_LEVEL=2`) | `-march=x86-64-v3` (cache default 3) |
| Frame pointers | `-fno-omit-frame-pointer` | `-fomit-frame-pointer` |

Two independent contaminations. The before side targeted a newer ISA baseline — v3 carries AVX2,
BMI2 and FMA that v2 does not — so it could be faster for reasons unrelated to the wire keys, which
inflates the measured regression. And the after side reserved `rbp` as a frame pointer while the
before side did not, which raises register pressure on the after side by itself — and register
pressure was the entire basis of the assembly review's "spills increased" finding.

What went wrong in the method: the two SOURCES were verified identical (the recorded patch
round-trips to the before-side file byte for byte) and both binaries were rebuilt in the same
session, but the two BUILD TREES were never diffed. Identical sources fed to differently configured
compilers are not a comparison. One `diff` of the two `CMakeCache.txt` files would have caught it.

The replacement run is in the parent directory, taken after rebuilding the before side with
`-DX86_ARCH_LEVEL=2 -DDISABLE_OMIT_FRAME_POINTER=ON`.
