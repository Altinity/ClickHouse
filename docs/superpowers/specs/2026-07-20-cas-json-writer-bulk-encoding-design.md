# CAS text encoding — `CasJsonWriter` bulk-append writer (near-memcpy serialization)

- **Date:** 2026-07-20
- **Status:** design approved, ready for implementation plan
- **Area:** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/` (write-side
  JSON micro-vocabulary + all format codecs' encode paths)
- **Backlog item:** `utils/ca-soak/scenarios/BACKLOG.md` — *"OPTIMIZATION OPPORTUNITY (CPU,
  LOW-MEDIUM) — ref-ledger JSON encoding writes byte-by-byte instead of bulk-copying safe runs"*
  (logged 2026-07-19)

## Problem

`system.trace_log` CPU profiling of the 5h soak showed `CasRefLedger::flushRefBatch` →
`encodeRefLogTxn` / `encodeRefTableSnapshot` → `writeJSONString` / `writeBindingFields` /
`writeManifestRefFields` → `DB::WriteBuffer::write(char)` as a persistent constant-factor CPU
cost. Three independent inefficiencies stack up in the current encode path:

1. **Byte-by-byte escaping.** `writeStringValue` → `writeJSONString` (`src/IO/WriteHelpers.h:182`)
   escapes with a per-character `writeChar` loop; every byte pays the `finalized`/`canceled`
   check plus `nextIfAtEnd` in `WriteBuffer::write(char)`. Measured on a safe ~80-byte
   ref-ledger-key-shaped string: 177ns vs 23.5ns for a raw bulk write — **7.5×**.
2. **Call count.** One record (a `ManifestRef` + binding) is ~9-12 separate `WriteBuffer`
   calls — every brace, comma, colon, and quote is its own call, each paying the same
   per-call tax. `BM_EncodeRefLogTxn` (one promote-shaped transaction): **753ns**.
3. **Per-record heap allocations** (not in the backlog entry; found during design).
   `writeManifestRefFields` (`Formats/CasWireVocab.cpp:80-82`) and `writeBindingFields`
   (`Formats/CasRefLogFormat.cpp:77-79`) build prefixed key names via `String(prefix) + "me"`
   etc. — 2-3 `String` allocations per record. `writeHex128Value` allocates an intermediate
   `String` via `u128ToHex`.

The user-set goal: serialization in this hotpath should be **nearly as efficient as a memory
copy**, with a measurable acceptance gate (below).

## Constraints

- **Byte-identical output.** Canonical CAS text is compared byte-for-byte on retries and
  deterministic adoption, and the incremental budget counters
  (`RefTableState::snapshot_body_bytes` / `removal_body_bytes`, maintained by
  `applyOpInPlace` using these same encoders) assume the encoders' sizes. Every produced
  object must keep exactly the bytes the current implementation produces.
- **No trust-based shortcuts.** No "raw, pre-validated" string writer that skips escaping on
  the caller's promise: the single string writer escapes correctly for **all** inputs (the
  bulk-scan design below makes safe strings nearly free anyway, so there is nothing to buy
  by trusting callers).
- **Streaming formats must not accumulate.** The `RecordStream` family (`cas_run`,
  `object_cap = 0`) is never materialized whole; its writer must keep memory bounded by one
  line (`line_cap` = 4 KiB), not by record count.
- Read side (`JsonObjectReader`, all decode functions) is untouched.
- `writeJSONString` in `src/IO/WriteHelpers.h` is untouched (a global bulk-copy rewrite of it
  remains a possible separate upstream improvement, out of scope here).

## Design

### Component 1: `CasJsonWriter`

New class in `Formats/CasTextFormat.h`, replacing `WriteBuffer` in every CAS encode path. It
owns a `String buf` (constructor takes a reserve hint) and appends inline — a capacity check
plus a `memcpy`-class store, no virtual calls, no `finalized`/`canceled` lifecycle:

- `key(name, first)` — emits `{` or `,`, then `"name":`, as direct appends. A prefixed
  variant (for the `o`/`n`-prefixed `ManifestRef` and binding keys, e.g. `ome`, `nmb`)
  composes the key in a stack `char[8]` — zero heap allocations.
- `stringValue(s)` — the one string writer. Bulk-run escaping: scan for the next byte in
  the special set `{ < 0x20, '"', '\\', 0xE2 }` (vectorized where available, scalar
  fallback), bulk-append the safe run in one `memcpy`, emit the escape sequence, repeat.
  Escape emission is byte-identical to `writeJSONString` under the pinned CAS settings
  (`escape_forward_slashes = false`), including `\b \f \n \r \t`, `\u00XX` control escapes
  with the uppercase-hex nibble quirk, and the three-byte lookahead that rewrites
  `0xE2 0x80 0xA8` / `0xE2 0x80 0xA9` (U+2028/U+2029) as the six-character escapes
  `\u2028` / `\u2029` (a trailing or partial `0xE2` sequence is copied as-is, exactly as
  today). The semantics are **statically fixed** — `FormatSettings` is no longer consulted at all, so a
  process-wide settings change can no longer influence CAS bytes even in principle.
- `u64StringValue(v)` / `u64Number(v)` — jeaiii `itoa` (`base/itoa.h`) into a stack buffer,
  then one append. `hex128Value(v)` — `writeHexUIntLowercase` into a stack `char[32]`, then
  one quoted append (the intermediate `String` from `u128ToHex` dies).
- `boolValue(v)`, `closeObject(first)`, `newline()`, `size()`, `view()`, `clear()`,
  `take() &&` (moves the finished `String` out).

### Component 2: vocabulary migration

The free write-side vocabulary functions keep their names and call shapes but take
`CasJsonWriter &` instead of `WriteBuffer &`, becoming thin inline wrappers over the class:
`writeKey`, `writeStringValue`, `writeHex128Value`, `writeU64StringValue`, `writeBoolValue`,
`closeObject`, `writeHeaderLine`, `writeTrailerLine` (`CasTextFormat`); `writeTokenFields`,
`writeBlobRefFields`, `writeManifestRefFields` (`CasWireVocab`). Codec diffs stay almost
entirely "the type of `out` changed".

### Component 3: codec sweep

Every bounded format's encode function (~12 files: ref log, ref snapshot, part manifest, gc
state, gc outcomes, fold seal, pool meta, blob meta, blob envelope, server-root formats,
wire vocab helpers) replaces

```cpp
WriteBufferFromOwnString out;  ...  out.finalize();  return out.str();
```

with

```cpp
CasJsonWriter w(reserve_hint);  ...  return std::move(w).take();
```

Mechanical; validation calls and structure are unchanged. Size helpers
(`removalOpEncodedSize`, `removalFramingSize`, `snapshotFramingSize`,
`committedRowEncodedSize`, `precommitRowEncodedSize`) get faster for free — relevant because
`applyOpInPlace` calls them once per applied op.

### Component 4: streaming (line-scratch) mode

Two first-class usage modes of the same class:

- **Whole-object assembly** — bounded formats (all have an `object_cap`): one writer per
  object, `take()` at the end. This is components 2-3.
- **Line-scratch** — the `RecordStream` family. `SourceEdgeRunWriter` keeps its public
  `WriteBuffer &` contract, but internally holds ONE reused `CasJsonWriter`: per record it
  assembles the full NDJSON line in the scratch, issues ONE `out.write(line.data(),
  line.size())`, then `clear()` (which keeps capacity). Memory stays bounded by the largest
  line, never by record count; the surrounding `WriteBuffer` still streams to its
  destination. The GC fold's run writing gets the same bulk-write win as a side effect.

## Consistency consequences

Bytes do not change, therefore: retry/adoption byte-comparison, golden objects, the
incremental `snapshot_body_bytes` / `removal_body_bytes` counters, and every decode path
remain correct with no accompanying changes. This is enforced by the differential test below,
not assumed.

## Error handling

`CasJsonWriter` performs no I/O; its only failure is `std::bad_alloc` on growth, which
propagates. The `WriteBuffer` lifecycle tax (`finalize`/`canceled`) disappears from encode
paths — nothing to mis-finalize on exception unwind; the buffer is simply destroyed. All
validation stays where it is today: `checkCanonicalRefName` / `checkManifestRef` before
writing, `checkBudget` over the finished text after. No new fallback paths.

## Testing

1. **Differential reference test** (the byte-identity guarantee). The current
   `WriteBuffer`-based vocabulary implementation is preserved *inside a gtest only* as the
   reference. The test drives a corpus through both implementations and requires byte-equal
   output:
   - every `RefOpKind` with every combination of optional fields and `o`/`n` prefixes;
     snapshot meta/committed/precommit rows with and without optional ids;
   - adversarial strings through `stringValue` vs `writeJSONString` with the pinned
     settings: quotes, backslashes, all control bytes 0x00-0x1F, `\b \f \n \r \t`,
     `0xE2 0x80 0xA8` / `0xE2 0x80 0xA9`, a truncated `0xE2` / `0xE2 0x80` at end-of-string,
     `0xE2` followed by non-continuation bytes, invalid UTF-8, empty strings, and `/`
     (must NOT be escaped);
   - randomized fuzz strings over the full byte range.
2. **Existing gtest gate** stays green (`Cas*`/`CA*` plus the extra prefixes; re-verify the
   current gate filter during implementation — it has twice been narrower than the tests).
3. **Benchmark acceptance gate.** `benchmarks/benchmark_cas_ref_protocol.cpp` gains
   `BM_MemcpyTxnBytes`: the same promote-transaction bytes assembled from precomputed
   fragments by plain `memcpy` — the floor. Acceptance: the new `BM_EncodeRefLogTxn` lands
   **at most 3× the floor** (hard gate; 2× is the aspiration, and the contingency ladder
   below kicks in above 3×). History baseline: 753ns before this change. Before/after
   numbers go into the BACKLOG entry, which flips to RESOLVED.
4. **Integration gate:** the usual ca-soak ref-lane run, green.

## Contingency (only if the factor lands above 3×)

1. Templated compile-time key literals: merge `,"key":` into a single static literal — one
   append per key.
2. Merge adjacent literals only in the two hottest record writers (`writeOp`,
   `writeCommittedRow`).

Both are internal details of `CasJsonWriter` / the codecs and do not change this design.

## Out of scope

- `writeJSONString` (`src/IO/WriteHelpers.h`) and any generic ClickHouse IO primitive.
- The read side and any format/schema change (no new fields, no version bump — bytes are
  identical).
