---
description: 'Semantic ("sufficiently full") CAS persisted field names, shipped as a generation reset, with an asymmetric C++ member-naming rule'
sidebar_label: 'CAS semantic wire keys'
sidebar_position: 10
slug: /superpowers/specs/cas-semantic-wire-keys
title: 'CAS semantic wire keys design'
doc_type: 'guide'
---

# CAS semantic wire keys design {#cas-semantic-wire-keys-design}

Revision 3 (2026-08-29). Revision 2 encoded three adjudications: keys must be *sufficiently full
for understanding*, not exact C++ member names; there are no existing installations, so the change
ships as a **generation reset**, not a generation bump; and C++ members follow an **asymmetric
rule** — a member may be fuller than its wire key, never more cryptic. Revision 3 incorporates the
review of revision 2: the known-field sentinels stay, both pool-meta gates keep their own tests,
the member rule is enforced absolutely (at the cost of one rename), the closed value sets are
completed, and the `ProvenanceOp` wire words move into one constexpr table. The exact-full-name
alternative was independently implemented in Altinity PR #2288 and rejected for this design (see
rejected alternatives).

## Problem {#problem}

CAS persisted objects use JSON text, but most field names are two-to-five-character initialisms such
as `su`, `eat`, `fen`, `ome`, and `lfe`. An operator reading an object with `jq`, `less`, or an object
store console must keep the codec source open to understand it. This defeats an important reason for
using text formats.

Replacing every key with the corresponding full C++ member name is not an acceptable fix. Persisted
key bytes remain real work even when an object is stored as `.zst`: the writer produces and compresses
the full text, zstd emits the full decompressed text, and the JSON reader copies and compares every
key. Several limits are measured over canonical uncompressed text, so longer repeated keys also
reduce the number of records that fit under an existing limit.

The original size argument is additionally wrong for three can-grow-large formats:
`cas_run`, `cas_fold_seal`, and `cas_ref_catalog` are raw rather than compressed. The `cas_blob`
descriptor has a different hard constraint: it must fit before the pool-wide fixed payload offset,
which defaults to 256 bytes and may be as small as 240 bytes.

The format therefore needs a deliberate distinction between fields written once per object and
fields repeated once per record. It must improve raw readability without turning the data plane into
verbose JSON.

## Decision {#decision}

The semantic-keys generation replaces opaque initialisms with a semantic wire vocabulary:

- metadata written once per object uses descriptive names;
- repeated record fields use short, recognizable words whose meaning is clear in the record;
- the fixed `cas_blob` descriptor uses a separately budgeted compact vocabulary;
- common framing remains `type`, `v`, and `n`;
- `!` remains the must-understand prefix for critical fields;
- abbreviated record-tag values that would otherwise keep a row opaque are made readable in the
  same change;
- C++ member names obey the asymmetric rule: at least as understandable as the wire key, and
  fuller wherever the wire is budget-compressed.

The change is a pre-release hard cut shipped as a **generation reset**: `G_BUILD` returns to 1,
the accumulated change-point history is erased, pools are recreated, and readers gain no aliases,
dual-read branches, migration path, or write-down path for the old spellings. There are no
existing installations (adjudicated 2026-08-28), so nothing needs to survive the cut.

## Goals and non-goals {#goals-and-non-goals}

The goals are:

- make a raw object understandable without a source-level abbreviation table;
- keep high-cardinality records materially smaller than an exact-member-name encoding;
- preserve all existing object paths, value encodings, ordering rules, strictness, compression
  policies, and byte caps;
- preserve the 240-byte minimum and 256-byte default `blob_header_len`;
- make the complete post-reset vocabulary — keys, record tags, and closed value sets — explicit
  and testable;
- guarantee that no C++ member is more cryptic than its wire key.

The following are not goals:

- mirroring C++ nesting or member names mechanically;
- changing JSON into nested objects;
- changing numeric, string, hexadecimal, or enum representations except the record-tag words listed
  below;
- changing object-store keys or suffixes;
- increasing byte caps or the payload offset to offset longer names;
- adding a general schema-description framework or runtime compact/full naming profile;
- renaming existing full-name C++ members to shorter forms (the member rule is a floor, not a
  mirror).

## Naming rules {#naming-rules}

The format uses context to avoid redundant words. For example, a `ManifestRef` inside a manifest row
uses `epoch`, `build`, and `ord`, not `manifest_ref_writer_epoch`,
`manifest_ref_build_sequence`, and `manifest_ref_manifest_ordinal`.

The approved compact vocabulary is:

| Word | Meaning |
|---|---|
| `v` | format compatibility version in the common header |
| `n` | record count in the common trailer |
| `ns` | namespace inside a repeated catalog row |
| `src` | source identity |
| `seq` | sequence |
| `ord` | ordinal |
| `algo` | hash algorithm |
| `op` | operation |
| `ref` | reference or reference name, as determined by the containing record |
| `chver` | ClickHouse version in the fixed blob descriptor |

Established protocol and unit fragments such as `id`, `uuid`, `ms`, `gc`, `hb`, `snap`, `txn`, and
`pid` (the OS process id) retain their conventional meanings in compound names. No new
single-letter body key is allowed. `v` and `n` are framing exceptions, not examples for body
fields. A new compact word outside these two sets requires a format-design decision rather than
being introduced incidentally by a codec author.

Fields repeated per record need not reproduce a C++ member name, but must be understandable from the
object type and record tag. Fields written once per object should normally use the complete semantic
name unless the fixed blob budget applies.

## C++ member naming {#cpp-member-naming}

The wire compresses under byte budgets; C++ has no byte budget. The rule binding the two is
asymmetric:

> A C++ member may be fuller or longer than its wire key — and should be, wherever the wire key is
> budget-compressed — but a member must **never be more cryptic than its JSON key**.

Consequences:

- `ManifestRef::writer_epoch` ↔ wire `epoch`, `SourceEdgeRecord::source_id` ↔ wire `src`,
  `EnvelopeHeader::incarnation_tag` ↔ wire `tag` are all legal: the member out-explains the key.
- A member spelled `cr` against a wire key `condemn_round` would be illegal; so would introducing
  any new initialism member alongside a readable key. The rule is absolute, and this design itself
  introduces exactly one violation to fix: the wire key `key_generation` out-explains the member
  `RunRef::generation`, so that member is renamed `key_generation` rather than weakening the rule
  to a contextual one. No other member rename is required; beyond that the rule is a review gate
  for future codecs.
- Where a semantic wire word collides with a C++ keyword, the member uses the established
  abbreviation or a fuller form, both compliant: wire `namespace` ↔ member `ns` (an established
  fragment, not a cryptic invention), wire `class` ↔ member `classification`.
- Flattened wire keys correspond to member paths (`lease.seq` ↔ `lease_seq`,
  `hold->reason` ↔ `hold_reason`); the path segments obey the same rule.
- Where neither side is budget-constrained, prefer the same word on both sides.

## Common framing and critical fields {#common-framing-and-critical-fields}

`CasTextFormat` keeps the common shapes unchanged:

```json
{"type":"cas_ref_log","v":1}
{"n":42}
```

The keys `type`, `v`, and `n` therefore do not participate in the rename. The header `v` is the
single mechanism every reader uses to refuse what it cannot parse; it stays byte-identical across
this and any future cut.

A leading `!` continues to mean “must understand”. A tolerant reader skips an unknown ordinary key,
but an unknown `!` key raises `UNKNOWN_FORMAT_VERSION`. The prefix is orthogonal to the field name and
is retained before the semantic name. The ref-log seal link therefore becomes `!prev_epoch` and
`!prev_seq`.

The test-only unknown key `!x` is not a schema field and remains deliberately opaque. Tests may also
inject names such as `!future_critical_field` to exercise the generic fail-closed rule.

## Shared repeated-value vocabulary {#shared-repeated-value-vocabulary}

The flat shared value types use the same keys in every repeated record:

| Value type | Old keys | New keys |
|---|---|---|
| `BlobRef` | `ha`, `h` | `algo`, `digest` |
| `Token` | `tt`, `tv` | `token_type`, `token` |
| `ManifestRef` | `me`, `mb`, `mo` | `epoch`, `build`, `ord` |

`writeBlobRefFields`, `writeTokenFields`, and `writeManifestRefFields` remain the single writers for
these flat representations. `writeManifestRefFields` retains its prefix argument (the same prefix
also feeds `writeBindingFields`, which prefixes the binding kind and ref name); owner-transition
bindings pass `old_` or `new_`, producing `old_kind`, `old_ref`, `old_epoch`, `old_build`,
`old_ord`, and their `new_` counterparts. There is no runtime naming mode.

## Fixed blob descriptor {#fixed-blob-descriptor}

The new descriptor vocabulary is:

| Old | New | Meaning |
|---|---|---|
| `type` | `type` | object type |
| `v` | `v` | compatibility generation |
| `tag` | `tag` | incarnation tag |
| `bld` | `build` | build ID |
| `ts` | `time_ms` | creation time in milliseconds |
| `by` | `creator` | creator server ID |
| `op` | `op` | provenance operation |
| `ch` | `chver` | ClickHouse version integer |
| `ref` | `ref` | truncated diagnostic intended reference |

The canonical shape is:

```json
{"type":"cas_blob","v":1,"tag":"…","build":"…","time_ms":123,"creator":"…","op":"merge","chver":26008000,"ref":"…"}
```

At maximum legal value widths, the current non-`ref` JSON consumes 213 bytes. (The in-code
worst-case table at the top of `CasPoolMetaFormat.cpp` says 214 and 225; it charges the `,"by":`
prefix 7 bytes where a 2-character key costs 6 — the same off-by-one must be fixed in the same
change.) The four renames add exactly 15 bytes: `bld` to `build` adds 2, `ts` to `time_ms` adds 5,
`by` to `creator` adds 5, and `ch` to `chver` adds 3. The new non-`ref` JSON is therefore
228 bytes. The `ref` key framing, empty quotes, closing brace, and final newline require another
11 bytes, for an exact mandatory worst case of **239 bytes**.

Consequences:

- the existing minimum `blob_header_len = 240` remains valid, with a one-byte diagnostic `ref`
  budget at maximum field widths;
- the default `blob_header_len = 256` leaves 17 escaped bytes for the diagnostic `ref`;
- the encoded descriptor remains exactly `blob_header_len` bytes after space padding;
- the payload offset and every existing range-read calculation remain unchanged;
- `validatePoolBlobHeaderLen` keeps its numeric bounds, but its rationale and boundary tests change
  from a 224-byte to a 239-byte mandatory descriptor.

The production descriptor must fit 240 bytes without relying on omission of provenance fields.
Because the worst case now sits one byte under the floor, the `ProvenanceOp` wire words must live
in a single constexpr table shared by the encoder, the decoder, and the boundary test — today they
are two hand-maintained switches in `CasBlobEnvelopeFormat.cpp` — and the boundary test must derive
the longest word (today `mutation`) from that table rather than hard-coding it, so a future longer
provenance word trips a test instead of overflowing minimum-configured pools. The
test-only `!x` extension is exercised with a 256-byte header, where it also fits.

## Singleton control objects {#singleton-control-objects}

These fields occur once per small control object and use descriptive names:

| Format | Old | New |
|---|---|---|
| `cas_blob_meta` | `st`, `cr`, `sz` | `state`, `condemn_round`, `size` |
| `cas_pool_meta` | `pid`, `hln`, `gcs`, `mrg`, `alg` | `pool_id`, `blob_header_len`, `gc_shards`, `min_reader_generation`, `algos_used` |
| `cas_gc_state` | `rnd`, `gcs`, `sg`, `spt`, `sa`, `msc`, `lo`, `ls` | `round`, `gc_shards`, `snap_generation`, `snap_pruned_through`, `snap_attempt`, `manifest_sweep_cursor`, `lease_owner`, `lease_seq` |
| `cas_gc_hb` | `by`, `seq` | `owner`, `hb_seq` |
| `cas_gc_maintenance_state` | `cur` | `janitor_cursor` |
| `cas_owner` | `su`, `rt` | `server_uuid`, `retired_at_ms` |
| `cas_epoch` | `nwe` | `next_writer_epoch` |
| `cas_mount_lease` | `su`, `we`, `hn`, `pid`, `sat`, `seq`, `eat`, `ma`, `fen`, `write_attempt_id` | `server_uuid`, `writer_epoch`, `hostname`, `pid`, `started_at_ms`, `seq`, `expires_at_ms`, `min_active`, `gc_fenced`, `write_attempt_id` |

The mount lease keeps `pid` (an established fragment: the OS process id) and `seq` (approved
vocabulary). `min_active` mirrors the member `MountLease::min_active`; its `UINT64_MAX`
clean-farewell sentinel is part of the field contract and stays documented at the codec.

The value encodings do not change. In particular, full-range counters that are currently decimal JSON
strings remain strings, bounded values remain numbers, and IDs remain lowercase fixed-width hex.
`cas_owner`'s `retired_at_ms` remains conditionally emitted (a never-retired owner's body is the
one-key object).

## Ref checkpoint {#ref-checkpoint}

`cas_ref_ckpt` is a small strict singleton. Its optional fields become:

| Old | New |
|---|---|
| `le` | `life_epoch` |
| `cte`, `cts` | `committed_epoch`, `committed_seq` |
| `cse`, `css` | `snapshot_epoch`, `snapshot_seq` |
| `lse`, `lss` | `seal_epoch`, `seal_seq` |

The existing both-or-neither rules for each `RefTxnId` pair remain unchanged.

## Ref transaction log {#ref-transaction-log}

The once-per-object metadata line changes as follows:

| Old | New |
|---|---|
| `ns` | `namespace` |
| `we`, `rs` | `txn_epoch`, `txn_seq` |
| `!pse`, `!pss` | `!prev_epoch`, `!prev_seq` |

The `!prev_epoch` and `!prev_seq` keys retain the both-or-neither grammar and remain critical INV-2
chain evidence.

Operation rows are high-cardinality and use semantic compact names:

| Old | New |
|---|---|
| `op` | `op` |
| `obk`, `orn`, `ome`, `omb`, `omo` | `old_kind`, `old_ref`, `old_epoch`, `old_build`, `old_ord` |
| `nbk`, `nrn`, `nme`, `nmb`, `nmo` | `new_kind`, `new_ref`, `new_epoch`, `new_build`, `new_ord` |
| `rn`, `me`, `mb`, `mo`, `ts` in `set_published_at` | `ref`, `epoch`, `build`, `ord`, `published_ms` |

The complete closed set of `op` values — `namespace_birth`, `owner_transition`,
`set_published_at`, `remove_namespace`, `epoch_seal` — and the owner-kind values `committed` and
`precommit` are already descriptive and do not change. (`namespace_birth`, `remove_namespace`, and
`epoch_seal` rows are body-less: the row is the `op` key alone.)

## Ref snapshot {#ref-snapshot}

The once-per-object metadata line becomes:

| Old | New |
|---|---|
| `ns` | `namespace` |
| `we`, `rs` | `snapshot_epoch`, `snapshot_seq` |
| `lc` | `lifecycle` |

Repeated rows become:

| Old | New |
|---|---|
| `k` | `kind` |
| `rn` | `ref` |
| `me`, `mb`, `mo` | `epoch`, `build`, `ord` |
| `ts` | `published_ms` |

The abbreviated row-tag values also change: `c` becomes `committed`, and `p` becomes `precommit`.
`published_ms` remains committed-only, exactly as `ts` is today.

The reader-only sentinels are **retained together with their tests** and reclassified: they are
not compatibility scaffolding but **permanently forbidden known-field guards**. A tolerant reader
that merely skipped them as unknown keys would silently drop a persisted payload (`pl`, rejected on
ref-log and ref-snapshot rows) or silently accept removed terminal-lifecycle semantics (`rte`/`rts`,
rejected on the snapshot meta line) in a malformed, hand-edited, or erroneously produced
current-generation object. The absence of old installations removes the legitimate old writer; it
does not make such objects impossible. The sentinel spellings are not live vocabulary.

## Part manifest {#part-manifest}

The once-per-object descriptor line becomes:

| Old | New |
|---|---|
| `me`, `mb`, `mo` | `epoch`, `build`, `ord` |
| `ns` | `root_namespace` |
| `pd` | `payload_digest` |

Repeated entry rows become:

| Old | New |
|---|---|
| `p` | `path` |
| `pm` | `place` |
| `ha`, `h` | `algo`, `digest` |
| `sz` for blob entries | `size` |
| `il` for inline entries | `size` |

Both placements therefore use one `size` key. For a blob it remains the raw blob byte count; for an
inline entry it remains the following payload length. The placement word determines which case is
valid, exactly as it determines whether `sz` or `il` is valid today.

The payload-zone banner changes from `il=<n>` to `size=<n>`, so the abbreviation does not survive
in the payload zone alone. It is rebuilt from the decoded path and size and compared byte-for-byte
as before. Placement values `inline` and `blob`, raw payload framing, entry ordering, and the `n`
trailer do not change.

## GC outcome log {#gc-outcome-log}

Each repeated outcome row becomes:

| Old | New |
|---|---|
| `k` | `kind` |
| `ha`, `h` | `algo`, `digest` |
| `tt`, `tv` | `token_type`, `token` |
| `oc` | `outcome` |

The `kind` value set has a single word, `blob`; the complete `outcome` set is `deleted`, `absent`,
`replaced`, and `spared`. None of these change.

## Ref catalog {#ref-catalog}

`cas_ref_catalog` can grow to many raw rows, so each row uses compact semantic context:

| Old | New |
|---|---|
| `k` | `kind` |
| `ent` tag value | `entry` |
| `ns` | `ns` |
| `st` | `state` |
| `inc` | `life` |
| `rsr` | `remove_round` |
| `csr` | `creator` |
| `cwe` | `creator_epoch` |
| `cfg` | `creator_fence` |

Within an `entry` row, `life` is the opaque namespace-life identity, while `creator` is the creator's
server-root ID. The existing creator/state and removal/state pairing rules remain unchanged, and the
`state` value set stays `creating`, `live`, `removing`.

## Source-edge run {#source-edge-run}

The `cas_run` header retains `type`, `v`, and `kind`; the existing `source_edge` kind value also
remains unchanged. Repeated rows become:

| Old | New |
|---|---|
| `b` | `ref` |
| `s` | `src` |
| `m` | `mark` |
| `pend` | `pending` |
| `tt`, `tv` | `token_type`, `token` |
| `sz` | `size` |
| `cr` | `condemn_round` |
| `mc` | `confirmed` |

`cr` maps to the same member concept as `cas_blob_meta`'s `condemn_round` — the GC round that
condemned the object — so both formats spell it `condemn_round`; giving the same concept two wire
spellings would defeat cross-format greppability. The field appears only on condemned rows, which
are a small fraction of a run, so the longer word does not touch the dominant active-row cost.

Marker values `edge`, `zero`, and `condemned` remain unchanged. The serialized `ref` value remains
the algorithm byte followed by the digest, so lexical `(ref, src)` ordering continues to equal the
binary `(algorithm, digest, source_id)` ordering required by the streaming merge.

## Fold seal {#fold-seal}

The once-per-object metadata line changes from `g` and `pg` to `generation` and
`parent_generation`.

Every repeated record uses `kind` instead of `k`. The record tags change as follows:

| Old | New |
|---|---|
| `rfl` | `ref_life` |
| `btr` | `blob_run` |
| `cnd` | `condemned` |

A `blob_run` row becomes:

| Old | New |
|---|---|
| `key` | `key` |
| `ck` | `checksum` |
| `shard` | `shard` |
| `gen` | `key_generation` |

`key_generation` is deliberately **not** `generation`: the metadata `generation` is the generation
this seal *is*, while a run row's value is the generation whose key namespace physically holds the
run object — and the two genuinely diverge when an idle shard carries its parent's run forward
verbatim. One word for both would read as corruption in exactly the situation the carry-forward is
designed for. The validator continues to cross-check the row value against the run `key`. The C++
member `RunRef::generation` is renamed `key_generation` in the same change — the one member rename
this design requires (see the member rule).

A `ref_life` row becomes:

| Old | New |
|---|---|
| `life` | `life` |
| `cls` | `class` |
| `lfe`, `lfs` | `fold_epoch`, `fold_seq` |
| `hr` | `hold_reason` |
| `hpe`, `hps` | `hold_epoch`, `hold_seq` |
| `hrc` | `retries` |
| `hnr` | `retry_round` |
| `rte`, `rts` | `remove_epoch`, `remove_seq` |

`hold_reason` (not a bare `hold`) because the value is a reason word such as
`witness_disappeared` — a key named `hold` reads as a boolean. It also matches the member path
`hold->reason`. The `class` values remain the closed numeric set `{0, 1, 2, 4}` (a machine
classification consumed by GC logic, documented at `CasFoldSealFormat.h`); the C++ member stays
`classification`, which the asymmetric member rule permits and the `class` keyword requires.

A `condemned` summary row becomes:

| Old | New |
|---|---|
| `shard` | `shard` |
| `ct` | `condemned` |
| `pt` | `pending` |
| `ocr` | `oldest_round` |

The context supplied by `kind` makes `class`, `hold_reason`, `retries`, `condemned`, `pending`, and
`oldest_round` unambiguous without repeating the full C++ nesting in every raw row. All ordering,
closed-set, both-or-neither, hold, cleanup-evidence, and shard-total validation remains unchanged.

## Closed value sets {#closed-value-sets}

Value representations do not change, but the complete vocabulary goal includes them, so the closed
sets are pinned here and in the goldens:

- `op` (ref log): `namespace_birth`, `owner_transition`, `set_published_at`, `remove_namespace`,
  `epoch_seal`;
- binding kinds (`old_kind`/`new_kind`): `committed`, `precommit`;
- snapshot row `kind`: `committed`, `precommit`; catalog row `kind`: `entry` (sole value);
- fold-seal record `kind`: `ref_life`, `blob_run`, `condemned`;
- `token_type`: `etag`, `generation`, `emulated`;
- `algo` (and the leading algorithm byte in a run `ref`): `ch128`, `xxh3`, `sha256`;
- `mark`: `edge`, `zero`, `condemned`;
- `outcome`: `deleted`, `absent`, `replaced`, `spared`; outcome `kind`: `blob` (sole value);
- catalog `state`: `creating`, `live`, `removing`;
- `cas_blob_meta` `state`: `clean`, `condemned`;
- `lifecycle`: `live` (sole value, hard-required);
- `hold_reason` (append-only): `gap_below_witness`, `unconsumed_seal_crossing`,
  `witness_disappeared`, `body_undecodable`, `manifest_body_missing`, `checkpoint_undecodable`;
- `class`: numeric `{0, 1, 2, 4}` — deliberately not words (see fold seal);
- `place`: `inline`, `blob`; run header `kind`: `source_edge`; blob descriptor `op`: `other`,
  `insert`, `merge`, `mutation`, `attach`, `repack`;
- object `type` values: exactly the seventeen registry type strings in `CasFormat.cpp`.

## Decompressed-byte accounting {#decompressed-byte-accounting}

Compression policy is not used to decide whether a repeated key may be long. The relevant quantity is
the number of key and tag bytes emitted after decompression. The mappings deliberately remain
substantially smaller than exact member names.

The following deltas count only key and changed record-tag bytes; JSON punctuation and values are
unchanged:

| Repeated record | Increase |
|---|---:|
| active `cas_run` row | 7 bytes |
| condemned `cas_run` row | 41 bytes |
| blob `PartManifest` entry | 15 bytes |
| inline `PartManifest` entry | 8 bytes, plus 2 bytes in its payload banner |
| `GcOutcomes` row | 26 bytes |
| committed ref-snapshot row | 29 bytes including `c` to `committed` |
| precommit ref-snapshot row | 19 bytes including `p` to `precommit` |
| base ref-catalog row | 9 bytes including `ent` to `entry` |

These are accepted readability costs. Exact-member-name alternatives are rejected because they add
another tens of bytes per row, particularly for `source_id`, `delete_pending`,
`marker_confirmed`, and nested manifest-reference fields.

Numeric object and line caps do not increase. Consequently, a canonical object near a current
uncompressed byte limit may admit fewer records after the rename. This is intentional: increasing a
cap would increase whole-read memory and line-allocation exposure merely to compensate for spelling.
Admission estimators and fold-seal reservation helpers must continue measuring through the real
encoders rather than using hand-maintained byte constants.

Implementation verification records before/after encode and decode throughput for representative
large `cas_run`, `RefSnapshot`, and `PartManifest` inputs. These measurements are review evidence,
not a timing assertion in CI. Exact encoded-byte deltas and boundary sizes are pinned in deterministic
unit tests.

## Generation reset {#compatibility-and-generation-floor}

There are no existing installations and no persisted data that must survive, so the breaking change
ships as a reset, not a bump. Implementation must:

- set `G_BUILD` to 1; `currentCompatibilityVersion` and `currentWriterVersion` stamp every newly
  encoded object with `v:1`;
- collapse the change-point history of all 17 registered formats (and the reserved `Roster` row) to
  the `{1, 1}` baseline, and delete the named legacy generation constants
  (`kContiguousRefStreamsGeneration` through `kMountWriteAttemptIdGeneration`) together with every
  reference to them;
- keep the floor machinery itself — `decodePoolMeta`'s backward gate and the
  `min_reader_generation` forward gate — dormant at 1 for the next real breaking change; new pools
  mint `min_reader_generation = 1`;
- keep the normal forward gate, so a reader rejects `v` above `G_BUILD` with
  `UNKNOWN_FORMAT_VERSION` before reading a body;
- keep the known-field rejection sentinels (`pl` on ref-log and ref-snapshot rows, `rte`/`rts` on
  the ref-snapshot meta line) and their tests: they are forbidden-field guards against malformed or
  hand-edited current-generation objects, not compatibility scaffolding (see the ref snapshot
  section);
- delete only the refusal tests specific to historical generations 3–10 (the pre-contiguous,
  generation-five, and generation-six pool fixtures): their subject matter no longer exists, and
  their fixtures could only be kept by encoding objects no generation ever wrote. The two pool
  gates themselves keep their own post-reset tests (see the test strategy) — deleting the
  historical fixtures must not delete the last test of either gate;
- re-stamp every test fixture that uses a historical header version (the `"v":3`-style literals
  whose comments say "any version <= G_BUILD passes the gate") to 1, since the forward gate at
  `G_BUILD = 1` would otherwise reject them before the body under test;
- replace the per-generation history table in `Formats/README.md` with a single note recording the
  reset.

Readers recognize only the new live spellings. They do not accept old keys as aliases. In a
tolerant format, an old optional spelling has the same behavior as any unknown ordinary key: it may
be skipped, but it never populates the renamed field. Required-field validation and strict formats
continue to reject incomplete or unknown shapes according to their existing policies. A stray
pre-reset object or pool — which should not exist — fails closed as `CORRUPTED_DATA` or
`UNKNOWN_FORMAT_VERSION`; no friendlier taxonomy is owed to data that was never released.

## Codec structure {#codec-structure}

The implementation remains a mechanical codec change rather than a new schema subsystem:

- `CasTextFormat` retains `type`, `v`, `n`, and the existing `!` handling;
- `CasWireVocab` owns the shared `BlobRef`, `Token`, and `ManifestRef` spellings;
- `CasRefWireVocab` continues to own value representation and validation for `RefTxnId`, while each
  containing format supplies contextual keys such as `txn_epoch`, `snapshot_epoch`, or
  `committed_epoch`;
- each codec keeps its format-local record names and tag values next to its writer and reader;
- the `ProvenanceOp` wire words move from two switches into one constexpr table read by the
  encoder, the decoder, and the boundary test;
- no map lookup, schema object, heap allocation, or runtime branch is added to select key names.

Writer order remains canonical. Reader strictness remains exactly as registered in `CasFormat`.
Unknown critical fields continue to fail before strict/tolerant handling, and exception taxonomy does
not change.

## Test strategy {#test-strategy}

The implementation updates every exact-byte fixture and adds coverage in five layers.

First, each of the 17 registered formats receives or retains a canonical encode/decode golden that
pins the post-reset header, field order, key spelling, tag spelling, and value representation.
Goldens remain inline with the codec unit tests; no generated golden-update command is introduced.

Second, `cas_format_test_battery` must cover the same set of `FormatId` values as the live traits
registry. The current omissions for `RefCkpt`, `GcMaintenanceState`, and `RunFile` are closed, and a
set-equality assertion prevents another registered codec from silently missing the common battery.
(The registry's `TRAITS` table currently lives in an anonymous namespace with no enumeration
accessor; the assertion needs one small export.)

Third, compatibility tests pin:

- the forward gate: `v:2` is rejected with `UNKNOWN_FORMAT_VERSION` before the body;
- the pool forward gate separately: a pool meta with `v:1` and `min_reader_generation: 2` is
  rejected with `UNKNOWN_FORMAT_VERSION` — this is `decodePoolMeta`'s own check, distinct from the
  header gate, and the `v:2` test cannot catch its removal;
- the dormant backward gate: a header stamped `v:0` is rejected with `UNKNOWN_FORMAT_VERSION`;
- a freshly created pool mints `min_reader_generation: 1`;
- objects use only new live spellings, stamped `v:1`;
- representative old spellings are not aliases — a tolerant reader skips them without populating
  the renamed field, a strict reader rejects them;
- unknown ordinary fields retain strict/tolerant behavior;
- unknown `!` fields still raise `UNKNOWN_FORMAT_VERSION`;
- `!prev_epoch` and `!prev_seq` retain both-or-neither validation;
- the known-field sentinels still reject `pl` on rows and `rte`/`rts` on the snapshot meta line.

Fourth, blob-envelope boundary tests construct maximum-width mandatory values — deriving the
longest `op` word from the shared constexpr `ProvenanceOp` wire-word table that replaces the two
switches in `CasBlobEnvelopeFormat.cpp` — and assert:

- `blob_header_len = 240` succeeds with a one-byte truncated `ref` budget;
- `blob_header_len = 256` succeeds and permits exactly 17 escaped `ref` bytes at the mandatory
  maximum;
- the returned header is exactly the configured length and the payload offset is unchanged;
- the 256-byte test-only unknown-critical descriptor still fits and fails decode as
  `UNKNOWN_FORMAT_VERSION`.

Fifth, byte-budget tests pin the repeated-row deltas listed above, all existing line and object cap
boundaries, `RefLog` and `RefSnapshot` encoded-size helpers, `RefCatalog` admission reservations, and
fold-seal worst-case reservation helpers. The tests validate the real encoder output rather than
duplicating a second byte formula.

Raw JSON assertions outside the codec tests must be updated, notably encoding pins, ref-seal splice
tests, catalog raw-row helpers, the orphan-manifest sweep splice, and the GCS mock that rewrites
`BlobMeta`. `utils/ca-soak` scenarios are searched for direct JSON inspection and updated when they
assert persisted spellings.

No repository-wide source linter for forbidden key literals is added. The complete golden coverage,
registry/battery equality, and the documented vocabulary are the enforcement mechanism.

## Documentation changes {#documentation-changes}

`Formats/README.md` replaces “keys 2–5 chars” with the naming rule:

- descriptive names for once-per-object metadata;
- semantic compact words for repeated records;
- the explicitly budgeted blob descriptor vocabulary;
- fixed framing `type`, `v`, and `n`;
- `!` as the must-understand prefix;
- the asymmetric C++ member rule.

The README object examples, per-format comments, and the generation-history table are updated in the
same commit as the codecs, and the descriptor worst-case comment in `CasPoolMetaFormat.cpp` is
corrected (213/224, becoming 228/239) in the same change. The backlog item at
`wire-keys-full-words` is resolved by pointing to this design and recording that exact full member
names were deliberately rejected for repeated records and the fixed blob descriptor.

## Rejected alternatives {#rejected-alternatives}

### Exact C++ member names everywhere {#exact-cpp-member-names-everywhere}

This maximizes local readability but makes the fixed blob descriptor exceed 256 bytes, inflates raw
record streams, increases decompression and parser traffic, and changes effective record capacity
under uncompressed byte caps. It also leaks incidental C++ nesting into a wire contract.

This alternative was independently implemented in Altinity PR #2288 (an open draft at the time of
writing) and rejected for this design by the 2026-08-28 decision ("достаточно полные" keys, not
full names); the fate of the PR itself is adjudicated separately. The PR's own consequences illustrate the
costs: the descriptor no longer fit, so the default payload offset grew from 256 to 384 and the
floor from 240 to 320; active run rows grew ~20%; and the framing rename `v`→`version` made the
version gate itself unreadable across the cut. Its useful parts are adopted here instead: the
thoroughness of its golden/splice/integration sweep sets the bar for the test strategy, and its
key-collision resolutions informed `condemn_round`, `hold_reason`, and `key_generation`.

### Keep all current keys and add a mapping table {#keep-all-current-keys-and-add-a-mapping-table}

This preserves bytes but does not solve the reviewer problem: an operator still needs a second
document to interpret an object. Documentation is useful for exact semantics, not for decoding every
field name.

### Choose a universal maximum key length {#choose-a-universal-maximum-key-length}

A rule such as “all keys are at most eight characters” is easy to state but produces subjective new
initialisms and ignores record multiplicity. The design instead controls a small semantic vocabulary
and gives fixed headers and repeated rows explicit treatment.

### Compact and full runtime profiles {#compact-and-full-runtime-profiles}

Allowing the same value type to serialize under selectable key profiles adds schema state and reader
branches without a protocol need. There is one canonical spelling per containing format.

### Mirroring the wire vocabulary back into C++ members {#mirroring-wire-vocabulary-into-members}

Renaming `ManifestRef::writer_epoch` to `epoch` and the like would make the two sides byte-identical
but shortens code that has no byte budget. The asymmetric member rule keeps the C++ side at least as
readable as the wire without a churn-only rename pass.

## Acceptance criteria {#acceptance-criteria}

The change is complete when:

- every production writer and corresponding reader uses the key tables in this document;
- `G_BUILD == 1`, every change-point history is the `{1, 1}` baseline, the legacy generation
  constants and the generation-3–10 refusal tests are deleted, the known-field sentinels and both
  pool gates are retained with post-reset tests, and all fixtures stamp `v:1`;
- a maximum-width production blob descriptor fits 240 bytes (one spare byte) and a default
  descriptor fits 256 bytes with a 17-byte `ref` budget, with the worst case derived from the
  single constexpr `ProvenanceOp` wire-word table shared with the codec;
- no C++ member is more cryptic than its wire key, including the one rename this requires
  (`RunRef::generation` to `key_generation`);
- common, codec, corruption, byte-budget, and exact-encoding unit tests pass for all 17 formats,
  and the battery covers exactly the registry;
- raw assertions in integration tests and `utils/ca-soak` use the new spellings;
- `Formats/README.md`, codec comments, the `CasPoolMetaFormat.cpp` worst-case table, and the
  backlog no longer claim a universal 2–5-character or exact-full-member-name convention;
- local before/after measurements report the decompressed bytes and encode/decode throughput of the
  representative high-cardinality formats for review.
