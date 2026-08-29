# CAS wire keys phase 2 — tasks 10–12 report

## Task 10 — `cas_part_manifest`

Status: complete.

Inventory before the flip used both `"key":` and escaped `\"key\":` forms over the required
source, test, integration, soak, and `*cas*` stateless scopes. `ns` had 8 hits in four non-manifest
owners: `cas_ref_catalog` (three test/soak uses), `cas_fold_seal` (one fixture), and a namespace
shell assertion (one); none belongs to this format. `p` had one `jwt_jwk` integration hit; it is not
CAS. `sz` had two `cas_run` hits (its encoding pin and header comment), reserved for Task 11.
`pd`, `pm`, and `il` had no remaining hits in the scoped inventory. The bare-token sweep found two
`ns` hits in the ref-catalog soak reader and three unrelated integration `p` hits; it found no
part-manifest parser or assertion.

Files: `CasPartManifestFormat.{h,cpp}`, its format test, and `Formats/README.md`.

The descriptor now writes `root_namespace` and `payload_digest`; entries write `path`, `place`, and
one `size`. The decoder collects `std::optional<uint64_t> size` while reading the entire row, then
uses it only after `place` selects Blob versus Inline. Missing Blob and Inline `size` both retain
their `CORRUPTED_DATA` rejection. Duplicate keys are rejected by `JsonObjectReader`; the dedicated
pin verifies that unchanged policy. Both placements have an order-independence pin with `size`
before `place`. The raw-payload banner is now `==> "…" size=<n> <==`, and its mismatch negative
uses `size` too.

Expectations: the battery golden changed the descriptor and entry key spellings and `il=12` to
`size=12`. Its digest remains dynamically computed by `computePayloadDigest` from the semantic
sample, so it independently proves the decoded path, placement, Blob reference/byte count, and
Inline payload. No literal body-size, SipHash, reservation, or budget pin changed: those helpers
continue to measure the real encoder.

Gate:

```text
[==========] 2212 tests from 284 test suites ran. (162986 ms total)
[  PASSED  ] 2212 tests.
```

There was no `[  FAILED  ]` line. Deviation: `grep -aE` was needed for the requested gate extraction
because the test log contains raw NUL output from a binary-payload test.
