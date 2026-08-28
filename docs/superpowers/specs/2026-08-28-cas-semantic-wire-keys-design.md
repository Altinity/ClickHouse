---
description: 'Generation-11 design for readable but byte-conscious CAS persisted field names'
sidebar_label: 'CAS semantic wire keys'
sidebar_position: 10
slug: /superpowers/specs/cas-semantic-wire-keys
title: 'CAS semantic wire keys design'
doc_type: 'guide'
---

# CAS semantic wire keys design {#cas-semantic-wire-keys-design}

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

Generation 11 replaces opaque initialisms with a semantic wire vocabulary:

- metadata written once per object uses descriptive names;
- repeated record fields use short, recognizable words whose meaning is clear in the record;
- the fixed `cas_blob` descriptor uses a separately budgeted compact vocabulary;
- common framing remains `type`, `v`, and `n`;
- `!` remains the must-understand prefix for critical fields;
- abbreviated record-tag values that would otherwise keep a row opaque are made readable in the
  same breaking generation.

The change is a pre-release hard cut. Every live persisted format is generation 11, old pools are
recreated, and readers gain no aliases, dual-read branches, migration path, or write-down path for
the old spellings.

## Goals and non-goals {#goals-and-non-goals}

The goals are:

- make a raw object understandable without a source-level abbreviation table;
- keep high-cardinality records materially smaller than an exact-member-name encoding;
- preserve all existing object paths, value encodings, ordering rules, strictness, compression
  policies, and byte caps;
- preserve the 240-byte minimum and 256-byte default `blob_header_len`;
- make the complete generation-11 vocabulary explicit and testable.

The following are not goals:

- mirroring C++ nesting or member names mechanically;
- changing JSON into nested objects;
- changing numeric, string, hexadecimal, or enum representations except the record-tag words listed
  below;
- changing object-store keys or suffixes;
- increasing byte caps to offset longer names;
- adding a general schema-description framework or runtime compact/full naming profile.

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

Established protocol and unit fragments such as `id`, `uuid`, `ms`, `gc`, `hb`, `snap`, and `txn`
retain their conventional meanings in compound names. No new single-letter body key is allowed. `v`
and `n` are framing exceptions, not examples for body fields. A new compact word outside these two
sets requires a format-design decision rather than being introduced incidentally by a codec author.

Fields repeated per record need not reproduce a C++ member name, but must be understandable from the
object type and record tag. Fields written once per object should normally use the complete semantic
name unless the fixed blob budget applies.

## Common framing and critical fields {#common-framing-and-critical-fields}

`CasTextFormat` keeps the common shapes unchanged:

```json
{"type":"cas_ref_log","v":11}
{"n":42}
```

The keys `type`, `v`, and `n` therefore do not participate in the rename.

A leading `!` continues to mean “must understand”. A tolerant reader skips an unknown ordinary key,
but an unknown `!` key raises `UNKNOWN_FORMAT_VERSION`. The prefix is orthogonal to the field name and
is retained before the semantic name. The ref-log seal link therefore becomes `!prev_epoch` and
`!prev_seq`.

The test-only unknown key `!x` is not a schema field and remains deliberately opaque. Tests may also
inject names such as `!future_critical_field` to exercise the generic fail-closed rule.

## Shared repeated-value vocabulary {#shared-repeated-value-vocabulary}

The flat shared value types use the same keys in every repeated record:

| Value type | Current keys | Generation-11 keys |
|---|---|---|
| `BlobRef` | `ha`, `h` | `algo`, `digest` |
| `Token` | `tt`, `tv` | `token_type`, `token` |
| `ManifestRef` | `me`, `mb`, `mo` | `epoch`, `build`, `ord` |

`writeBlobRefFields`, `writeTokenFields`, and `writeManifestRefFields` remain the single writers for
these flat representations. `writeManifestRefFields` retains its prefix argument; owner-transition
bindings pass `old_` or `new_`, producing `old_epoch`, `old_build`, `old_ord`, and their `new_`
counterparts. There is no runtime naming mode.

## Fixed blob descriptor {#fixed-blob-descriptor}

The generation-11 descriptor vocabulary is:

| Current | Generation 11 | Meaning |
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
{"type":"cas_blob","v":11,"tag":"…","build":"…","time_ms":123,"creator":"…","op":"merge","chver":26008000,"ref":"…"}
```

At maximum legal value widths, the current non-`ref` JSON consumes 214 bytes. The four renames add
exactly 15 bytes: `bld` to `build` adds 2, `ts` to `time_ms` adds 5, `by` to `creator` adds 5, and
`ch` to `chver` adds 3. The generation-11 non-`ref` JSON is therefore 229 bytes. The `ref` key,
empty quotes, closing brace, and final newline require another 11 bytes, for an exact mandatory
worst case of 240 bytes.

Consequences:

- the existing minimum `blob_header_len = 240` remains valid, with a zero-byte diagnostic `ref`
  budget at maximum field widths;
- the default `blob_header_len = 256` leaves 16 escaped bytes for the diagnostic `ref`;
- the encoded descriptor remains exactly `blob_header_len` bytes after space padding;
- the payload offset and every existing range-read calculation remain unchanged;
- `validatePoolBlobHeaderLen` keeps its numeric bounds, but its rationale and boundary tests change
  from a 225-byte to a 240-byte mandatory descriptor.

The production descriptor must fit 240 bytes without relying on omission of provenance fields. The
test-only `!x` extension is exercised with a 256-byte header, where it also fits.

## Singleton control objects {#singleton-control-objects}

These fields occur once per small control object and use descriptive names:

| Format | Current | Generation 11 |
|---|---|---|
| `cas_blob_meta` | `st`, `cr`, `sz` | `state`, `condemn_round`, `size` |
| `cas_pool_meta` | `pid`, `hln`, `gcs`, `mrg`, `alg` | `pool_id`, `blob_header_len`, `gc_shards`, `min_reader_generation`, `algos_used` |
| `cas_gc_state` | `rnd`, `gcs`, `sg`, `spt`, `sa`, `msc`, `lo`, `ls` | `round`, `gc_shards`, `snap_generation`, `snap_pruned_through`, `snap_attempt`, `manifest_sweep_cursor`, `lease_owner`, `lease_seq` |
| `cas_gc_hb` | `by`, `seq` | `owner`, `hb_seq` |
| `cas_gc_maintenance_state` | `cur` | `janitor_cursor` |
| `cas_owner` | `su`, `rt` | `server_uuid`, `retired_at_ms` |
| `cas_epoch` | `nwe` | `next_writer_epoch` |
| `cas_mount_lease` | `su`, `we`, `hn`, `pid`, `sat`, `seq`, `eat`, `ma`, `fen`, `write_attempt_id` | `server_uuid`, `writer_epoch`, `hostname`, `pid`, `started_at_ms`, `seq`, `expires_at_ms`, `min_active`, `gc_fenced`, `write_attempt_id` |

The value encodings do not change. In particular, full-range counters that are currently decimal JSON
strings remain strings, bounded values remain numbers, and IDs remain lowercase fixed-width hex.

## Ref checkpoint {#ref-checkpoint}

`cas_ref_ckpt` is a small strict singleton. Its optional pairs become:

| Current | Generation 11 |
|---|---|
| `le` | `life_epoch` |
| `cte`, `cts` | `committed_epoch`, `committed_seq` |
| `cse`, `css` | `snapshot_epoch`, `snapshot_seq` |
| `lse`, `lss` | `seal_epoch`, `seal_seq` |

The existing both-or-neither rules for each `RefTxnId` pair remain unchanged.

## Ref transaction log {#ref-transaction-log}

The once-per-object metadata line changes as follows:

| Current | Generation 11 |
|---|---|
| `ns` | `namespace` |
| `we`, `rs` | `txn_epoch`, `txn_seq` |
| `!pse`, `!pss` | `!prev_epoch`, `!prev_seq` |

The `!prev_epoch` and `!prev_seq` keys retain the both-or-neither grammar and remain critical INV-2
chain evidence.

Operation rows are high-cardinality and use semantic compact names:

| Current | Generation 11 |
|---|---|
| `op` | `op` |
| `obk`, `orn`, `ome`, `omb`, `omo` | `old_kind`, `old_ref`, `old_epoch`, `old_build`, `old_ord` |
| `nbk`, `nrn`, `nme`, `nmb`, `nmo` | `new_kind`, `new_ref`, `new_epoch`, `new_build`, `new_ord` |
| `rn`, `me`, `mb`, `mo`, `ts` in `set_published_at` | `ref`, `epoch`, `build`, `ord`, `published_ms` |

Operation values such as `owner_transition`, `set_published_at`, and `epoch_seal`, and owner-kind
values `committed` and `precommit`, are already descriptive and do not change.

## Ref snapshot {#ref-snapshot}

The once-per-object metadata line becomes:

| Current | Generation 11 |
|---|---|
| `ns` | `namespace` |
| `we`, `rs` | `snapshot_epoch`, `snapshot_seq` |
| `lc` | `lifecycle` |

Repeated rows become:

| Current | Generation 11 |
|---|---|
| `k` | `kind` |
| `rn` | `ref` |
| `me`, `mb`, `mo` | `epoch`, `build`, `ord` |
| `ts` | `published_ms` |

The abbreviated row-tag values also change: `c` becomes `committed`, and `p` becomes `precommit`.

The reader-only retired fields `rte`, `rts`, and `pl` remain explicit rejection sentinels. They are
not live generation-11 fields, are never written, and are not aliases. Keeping their rejection paths
prevents a malformed current-generation object from silently reintroducing retired semantics through
the tolerant reader.

## Part manifest {#part-manifest}

The once-per-object descriptor line becomes:

| Current | Generation 11 |
|---|---|
| `me`, `mb`, `mo` | `epoch`, `build`, `ord` |
| `ns` | `root_namespace` |
| `pd` | `payload_digest` |

Repeated entry rows become:

| Current | Generation 11 |
|---|---|
| `p` | `path` |
| `pm` | `place` |
| `ha`, `h` | `algo`, `digest` |
| `sz` for blob entries | `size` |
| `il` for inline entries | `size` |

Both placements therefore use one `size` key. For a blob it remains the raw blob byte count; for an
inline entry it remains the following payload length. The placement word determines which case is
valid, exactly as it determines whether `sz` or `il` is valid today.

The payload-zone banner changes from `il=<n>` to `size=<n>`. It is rebuilt from the decoded path and
size and compared byte-for-byte as before. Placement values `inline` and `blob`, raw payload framing,
entry ordering, and the `n` trailer do not change.

## GC outcome log {#gc-outcome-log}

Each repeated outcome row becomes:

| Current | Generation 11 |
|---|---|
| `k` | `kind` |
| `ha`, `h` | `algo`, `digest` |
| `tt`, `tv` | `token_type`, `token` |
| `oc` | `outcome` |

The existing full-word values `blob`, `deleted`, `absent`, `replaced`, and `spared` do not change.

## Ref catalog {#ref-catalog}

`cas_ref_catalog` can grow to many raw rows, so each row uses compact semantic context:

| Current | Generation 11 |
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
server-root ID. The existing creator/state and removal/state pairing rules remain unchanged.

## Source-edge run {#source-edge-run}

The `cas_run` header retains `type`, `v`, and `kind`; the existing `source_edge` kind value also
remains unchanged. Repeated rows become:

| Current | Generation 11 |
|---|---|
| `b` | `ref` |
| `s` | `src` |
| `m` | `mark` |
| `pend` | `pending` |
| `tt`, `tv` | `token_type`, `token` |
| `sz` | `size` |
| `cr` | `round` |
| `mc` | `confirmed` |

Marker values `edge`, `zero`, and `condemned` remain unchanged. The serialized `ref` value remains
the algorithm byte followed by the digest, so lexical `(ref, src)` ordering continues to equal the
binary `(algorithm, digest, source_id)` ordering required by the streaming merge.

## Fold seal {#fold-seal}

The once-per-object metadata line changes from `g` and `pg` to `generation` and
`parent_generation`.

Every repeated record uses `kind` instead of `k`. The record tags change as follows:

| Current | Generation 11 |
|---|---|
| `rfl` | `ref_life` |
| `btr` | `blob_run` |
| `cnd` | `condemned` |

A `blob_run` row becomes:

| Current | Generation 11 |
|---|---|
| `key` | `key` |
| `ck` | `checksum` |
| `shard` | `shard` |
| `gen` | `generation` |

A `ref_life` row becomes:

| Current | Generation 11 |
|---|---|
| `life` | `life` |
| `cls` | `class` |
| `lfe`, `lfs` | `fold_epoch`, `fold_seq` |
| `hr` | `hold` |
| `hpe`, `hps` | `hold_epoch`, `hold_seq` |
| `hrc` | `retries` |
| `hnr` | `retry_round` |
| `rte`, `rts` | `remove_epoch`, `remove_seq` |

A `condemned` summary row becomes:

| Current | Generation 11 |
|---|---|
| `shard` | `shard` |
| `ct` | `condemned` |
| `pt` | `pending` |
| `ocr` | `oldest_round` |

The context supplied by `kind` makes `class`, `hold`, `retries`, `condemned`, `pending`, and
`oldest_round` unambiguous without repeating the full C++ nesting in every raw row. All ordering,
closed-set, both-or-neither, hold, cleanup-evidence, and shard-total validation remains unchanged.

## Decompressed-byte accounting {#decompressed-byte-accounting}

Compression policy is not used to decide whether a repeated key may be long. The relevant quantity is
the number of key and tag bytes emitted after decompression. The generation-11 mappings deliberately
remain substantially smaller than exact member names.

The following deltas count only key and changed record-tag bytes; JSON punctuation and values are
unchanged:

| Repeated record | Generation-11 increase |
|---|---:|
| active `cas_run` row | 7 bytes |
| condemned `cas_run` row | 33 bytes |
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
generation-11 encoders rather than using hand-maintained byte constants.

Implementation verification records before/after encode and decode throughput for representative
large `cas_run`, `RefSnapshot`, and `PartManifest` inputs. These measurements are review evidence,
not a timing assertion in CI. Exact encoded-byte deltas and boundary sizes are pinned in deterministic
unit tests.

## Compatibility and generation floor {#compatibility-and-generation-floor}

This is a breaking generation-11 change to every live codec. Implementation must:

- set `G_BUILD` to 11 and introduce a named generation constant for the semantic-key change;
- append `{11, 11}` to the immutable change-point history of all 17 registered formats:
  `Blob`, `BlobMeta`, `PoolMeta`, `RefLog`, `RefSnapshot`, `RefCkpt`, `RefCatalog`,
  `GcMaintenanceState`, `PartManifest`, `RunFile`, `FoldSeal`, `GcState`, `GcHeartbeat`,
  `GcOutcomes`, `Owner`, `ServerEpoch`, and `MountLease`;
- leave the reserved `Roster` identifier without traits or a codec;
- raise the pool-wide backward floor enforced by `decodePoolMeta` from generation 10 to generation
  11 and describe the semantic-wire-key boundary in its exception;
- stamp every newly encoded object with `v:11` through `currentCompatibilityVersion`;
- keep the normal forward gate, so an older reader rejects `v:11` with
  `UNKNOWN_FORMAT_VERSION` before reading a body.

A generation-11 reader never mounts a generation-10 pool: `_pool_meta` is checked before other pool
objects are interpreted. There is no supported mixed-generation state. CAS is pre-release, so the
operator recreates the pool rather than migrating persisted objects.

Readers recognize only generation-11 live spellings. They do not accept old keys as aliases. In a
tolerant format, an old optional spelling has the same behavior as any unknown ordinary key: it may
be skipped, but it never populates the renamed field. Required-field validation and strict formats
continue to reject incomplete or unknown shapes according to their existing policies.

## Codec structure {#codec-structure}

The implementation remains a mechanical codec change rather than a new schema subsystem:

- `CasTextFormat` retains `type`, `v`, `n`, and the existing `!` handling;
- `CasWireVocab` owns the shared `BlobRef`, `Token`, and `ManifestRef` spellings;
- `CasRefWireVocab` continues to own value representation and validation for `RefTxnId`, while each
  containing format supplies contextual keys such as `txn_epoch`, `snapshot_epoch`, or
  `committed_epoch`;
- each codec keeps its format-local record names and tag values next to its writer and reader;
- no map lookup, schema object, heap allocation, or runtime branch is added to select key names.

Writer order remains canonical. Reader strictness remains exactly as registered in `CasFormat`.
Unknown critical fields continue to fail before strict/tolerant handling, and exception taxonomy does
not change.

## Test strategy {#test-strategy}

The implementation updates every exact-byte fixture and adds coverage in five layers.

First, each of the 17 registered formats receives or retains a canonical encode/decode golden that
pins the generation-11 header, field order, key spelling, tag spelling, and value representation.
Goldens remain inline with the codec unit tests; no generated golden-update command is introduced.

Second, `cas_format_test_battery` must cover the same set of `FormatId` values as the live traits
registry. The current omissions for `RefCkpt`, `GcMaintenanceState`, and `RunFile` are closed, and a
set-equality assertion prevents another registered codec from silently missing the common battery.

Third, compatibility tests pin:

- generation-10 `_pool_meta` is rejected before its body is interpreted;
- generation-11 objects use only new live spellings;
- representative old spellings are not aliases;
- unknown ordinary fields retain strict/tolerant behavior;
- unknown `!` fields still raise `UNKNOWN_FORMAT_VERSION`;
- `!prev_epoch` and `!prev_seq` retain both-or-neither validation.

Fourth, blob-envelope boundary tests construct maximum-width mandatory values and assert:

- `blob_header_len = 240` succeeds with an empty truncated `ref`;
- `blob_header_len = 256` succeeds and permits exactly 16 escaped `ref` bytes at the mandatory
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

`Formats/README.md` replaces “keys 2–5 chars” with the generation-11 naming rule:

- descriptive names for once-per-object metadata;
- semantic compact words for repeated records;
- the explicitly budgeted blob descriptor vocabulary;
- fixed framing `type`, `v`, and `n`;
- `!` as the must-understand prefix.

The README object examples and per-format comments are updated to generation-11 spellings in the same
commit as the codecs. The backlog item at `wire-keys-full-words` is resolved by pointing to this
design and recording that exact full member names were deliberately rejected for repeated records
and the fixed blob descriptor.

## Rejected alternatives {#rejected-alternatives}

### Exact C++ member names everywhere {#exact-cpp-member-names-everywhere}

This maximizes local readability but makes the fixed blob descriptor exceed 256 bytes, inflates raw
record streams, increases decompression and parser traffic, and changes effective record capacity
under uncompressed byte caps. It also leaks incidental C++ nesting into a wire contract.

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
branches without a protocol need. Generation 11 has one canonical spelling per containing format.

## Acceptance criteria {#acceptance-criteria}

The change is complete when:

- every production writer and corresponding reader uses the generation-11 tables in this document;
- every live `FormatId` has a generation-11 breaking change point;
- generation-10 pools fail at the pool floor and no old key alias exists;
- a maximum-width production blob descriptor fits 240 bytes and a default descriptor fits 256 bytes
  with the documented `ref` budget;
- common, codec, corruption, byte-budget, and exact-encoding unit tests pass for all 17 formats;
- raw assertions in integration tests and `utils/ca-soak` use generation-11 spellings;
- `Formats/README.md`, codec comments, and the backlog no longer claim a universal 2–5-character or
  exact-full-member-name convention;
- local before/after measurements report the decompressed bytes and encode/decode throughput of the
  representative high-cardinality formats for review.
