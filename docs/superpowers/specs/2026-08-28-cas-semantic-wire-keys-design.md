---
description: 'Semantic ("sufficiently full") CAS persisted field names, shipped as a generation reset, with an asymmetric C++ member-naming rule'
sidebar_label: 'CAS semantic wire keys'
sidebar_position: 10
slug: /superpowers/specs/cas-semantic-wire-keys
title: 'CAS semantic wire keys design'
doc_type: 'guide'
---

# CAS semantic wire keys design {#cas-semantic-wire-keys-design}

Revision 13 (2026-08-29). Three adjudications shape the design: keys must be *sufficiently full
for understanding*, not exact C++ member names; there are no existing installations, so the change
ships as a **generation reset**, not a generation bump; and C++ members follow an **asymmetric
rule** — a member may be fuller than its wire key, never more cryptic. The exact-full-name
alternative was independently implemented in Altinity PR #2288 and rejected for this design (see
rejected alternatives). The per-revision history is at the end of the document.

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
  fuller wherever the wire is budget-compressed;
- every vocabulary item has a single production carrier — a key constant, an enum wire table, or a
  single word constant for non-enum words — and goldens stay an independent byte oracle that never
  reads those carriers.

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
- guarantee that no C++ member is more cryptic than its wire key;
- make spelling drift impossible for all code that reads the carriers — writer, reader, and
  introspection read each vocabulary item from one place — with write-side bypass made loud (the
  `WireKey` type) and read-side discipline held by goldens and review, rather than pretending
  bypass is impossible, while tests remain an independent proof of the wire contract.

The following are not goals:

- mirroring C++ nesting or member names mechanically;
- changing JSON into nested objects;
- changing numeric, string, hexadecimal, or enum representations except the record-tag and `class`
  words and the `algos_used` array listed below;
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
  any new initialism member alongside a readable key. The rule is absolute rather than contextual,
  and it covers the members of every struct and class — codec-local collector structs included.
  Ordinary function-local variables are not members and are out of scope (the same spirit applies,
  but the rule does not police them).
- The audit against this design finds four renames to make. In the public types, the wire key
  `key_generation` out-explains `RunRef::generation`, so that member is renamed `key_generation`.
  In the codec-local collectors, whose members currently mirror the old wire spellings, the
  `ManifestFields` members `me`/`mb`/`mo` (one copy each in the ref-log and ref-snapshot codecs)
  become `epoch`/`build`/`ord`, and the `BindingFields` members `bk`/`rn`/`mf` become
  `kind`/`ref`/`manifest_fields` — otherwise the renamed wire would out-explain the very structs
  that parse it. One further rename follows a wire decision rather than this audit:
  `MountLease::min_active` becomes `min_active_build_sequence`, tracking its renamed key, and
  lands with the wire cut (phase 2) rather than with the audit-driven four (phase 1). No other
  member rename is required; beyond these the rule is a review gate for future codecs.
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
these flat representations. Keys that exist in bare, `old_`, and `new_` forms are not assembled
from a prefix at write time — that would leave the composed spelling without a single carrier while
the reader compares full literals. Instead they are declared as bundles of full-key constants:

```cpp
struct ManifestRefWireKeys { WireKey epoch, build, ord; };
constexpr ManifestRefWireKeys kBareManifestRefKeys{WireKey{"epoch"}, WireKey{"build"}, WireKey{"ord"}};
constexpr ManifestRefWireKeys kOldManifestRefKeys{WireKey{"old_epoch"}, WireKey{"old_build"}, WireKey{"old_ord"}};
constexpr ManifestRefWireKeys kNewManifestRefKeys{WireKey{"new_epoch"}, WireKey{"new_build"}, WireKey{"new_ord"}};
```

`writeManifestRefFields` takes a bundle rather than a prefix, and the analogous binding bundle
carries `old_kind`/`old_ref` (and `new_` counterparts) for `writeBindingFields`; the readers compare
against the same constants. There is no string composition, no allocation, and no runtime naming
mode.

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
are two hand-maintained switches in `CasBlobEnvelopeFormat.cpp`. Every component of the worst case
is then a compile-time constant (key lengths, 34 bytes per quoted `hex128`, 20 digits per u64, 10
per u32, the table's longest word, the `ref` framing, the brace, and the newline), so the mandatory
worst case is a constexpr expression guarded in the same change as the wire cut:

```cpp
static_assert(mandatory_descriptor_worst_case <= kMinBlobHeaderLen - 1);
```

A future longer provenance word then fails to compile instead of overflowing minimum-configured
pools. The boundary test stays as the independent half of the proof: it feeds maximum-width values
through the real encoder and checks that the formula matches the bytes — 239 mandatory bytes, a
240-byte header encodes, a 256-byte header leaves 17 escaped `ref` bytes, and the payload offset is
exact. The compiler proves the formula; the test proves the formula describes the encoder.

For the `static_assert` to be possible at all, `kMinBlobHeaderLen` needs one compile-time owner:
today it is a file-local constant inside `CasPoolMetaFormat.cpp`, while the formula, the key
constants, and the `ProvenanceOp` table naturally live with the envelope codec. The floor moves to
a dependency-light envelope-limits header that both `encodeEnvelopeHeader` and
`validatePoolBlobHeaderLen` read, and the `static_assert` sits next to the formula. Without the
shared owner an implementation would almost inevitably duplicate the number 240 and the proof would
guard a copy instead of the constant the pool validator enforces. The
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
| `cas_mount_lease` | `su`, `we`, `hn`, `pid`, `sat`, `seq`, `eat`, `ma`, `fen`, `write_attempt_id` | `server_uuid`, `writer_epoch`, `hostname`, `pid`, `started_at_ms`, `seq`, `expires_at_ms`, `min_active_build_sequence`, `gc_fenced`, `write_attempt_id` |

The mount lease keeps `pid` (an established fragment: the OS process id) and `seq` — the object
*is* the lease, so a `lease_seq` spelling would restate its own container; both stay per the
context rule. `ma` becomes `min_active_build_sequence`: the singleton descriptive rule outranks
brevity here, and a bare `min_active` under-names what is specifically a build-sequence floor. The
member follows (`MountLease::min_active` becomes `min_active_build_sequence`), and its `UINT64_MAX`
clean-farewell sentinel remains part of the field contract, documented at the codec.

The value encodings do not change, with one exception. `algos_used` stops being a comma-joined list
inside a JSON string and becomes a JSON array of algo words (`["ch128","sha256"]`): the hand-rolled
mini-grammar inside a JSON value disappears, `jq` reads it natively, and the cost is single-digit
bytes in a tiny singleton. This adds one small string-array primitive to `CasTextFormat`, used by
exactly this field. Everything else holds: full-range counters that are currently decimal JSON
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
member `RunRef::generation` is renamed `key_generation` in the same change (see the member rule).

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
`hold->reason`. The `class` values become words — `absent`, `unchanged`, `folded`, `clamped`,
taken verbatim from the documented semantics at `CasFoldSealFormat.h` (0 means absent, 1 unchanged,
2 folded through the observed cursor, 4 clamped below the ref-log cursor) — closing the last field
a raw-object reader needed the sources for. The capacity calculation that gated this counts the
whole rename, not just the word: an old maximum-width base `ref_life` row is 120 bytes and the
renamed row is 149–152 (22 bytes of keys and tags plus 7–10 for the word), so the 256 MiB seal
cap's worst case moves from about 2.24M to about 1.77M namespace lives — roughly a fifth lower and
still orders of magnitude beyond any realistic population — and the fold-seal reservation helpers
measure through the real encoder, so admission adjusts itself. The C++ side gains the matching
typed enum, and it is deliberately dense: `CoverageClass { Absent = 0, Unchanged = 1, Folded = 2,
Clamped = 3 }`. The old byte value 4 belongs to the numeric wire being deleted, not to any other
contract — nothing outside this JSON persists the raw classification byte — so the enum satisfies
the tables' density requirement and the indexed `toWord`, and every `== 0` / `== 4` comparison in
GC and sweep logic becomes a typed comparison (the member keeps the name `classification`, since
`class` is a keyword). The set stays closed exactly as before — an unknown word is
`CORRUPTED_DATA` — and the wide-integer truncation hazard the header documents cannot occur with
words.

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
- `class`: `absent`, `unchanged`, `folded`, `clamped`;
- `place`: `inline`, `blob`; run header `kind`: `source_edge`; blob descriptor `op`: `other`,
  `insert`, `merge`, `mutation`, `attach`, `repack`;
- object `type` values: exactly the seventeen registry type strings in `CasFormat.cpp`.

## Canonical examples {#canonical-examples}

Shapes as an operator sees them. Identifier values are illustrative and abbreviated with `…`; real
`hex128` values are 32 characters.

An active and a condemned `cas_run` row between the run header and trailer:

```json
{"type":"cas_run","v":1,"kind":"source_edge"}
{"ref":"01aa…","src":"0000…0007","mark":"edge"}
{"ref":"01cc…","src":"0000…0009","mark":"condemned","pending":false,"token_type":"etag","token":"e-42","size":4096,"condemn_round":"7","confirmed":true}
{"n":2}
```

The three `ref_life` fold-seal variants — base, hold-bearing, and with cleanup evidence:

```json
{"kind":"ref_life","life":"0000…1234","class":"folded","fold_epoch":"7","fold_seq":"11"}
{"kind":"ref_life","life":"0000…1234","class":"clamped","fold_epoch":"3","fold_seq":"4","hold_reason":"manifest_body_missing","hold_epoch":"5","hold_seq":"6","retries":2,"retry_round":"8"}
{"kind":"ref_life","life":"0000…1234","class":"folded","fold_epoch":"9","fold_seq":"12","remove_epoch":"9","remove_seq":"10"}
```

A creating-state ref-catalog entry:

```json
{"kind":"entry","ns":"srv1/db/table@cas@","state":"creating","life":"0000…0001","creator":"srv1","creator_epoch":"5","creator_fence":"2"}
```

And what a codec looks like under the carriers and helpers — the `cas_blob_meta` writer and reader
(a sketch, not normative code):

```cpp
namespace BlobMetaWire
{
    constexpr WireKey state{"state"};
    constexpr WireKey condemn_round{"condemn_round"};
    constexpr WireKey size{"size"};
}

writeWordField(out, BlobMetaWire::state, meta_states.toWord(meta.state), first);
writeU64StringField(out, BlobMetaWire::condemn_round, meta.condemn_round, first);
writeU64StringField(out, BlobMetaWire::size, meta.size, first);

while (r.nextKey(key))
{
    if (key == BlobMetaWire::state)
    {
        m.state = meta_states.fromWord(r.readString(), "blob meta");
        saw_state = true;
    }
    else if (key == BlobMetaWire::condemn_round)
        m.condemn_round = r.readU64String();
    else if (key == BlobMetaWire::size)
        m.size = r.readU64String();
    else
        r.skipUnknown(key);
}
if (!saw_state)
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS blob meta: missing state");
```

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
| base `ref_life` fold-seal row | 22 bytes of keys and tags, plus 7–10 for the `class` word |
| hold-bearing `ref_life` additions | 33 bytes |
| cleanup-evidence `ref_life` additions | 16 bytes |
| `blob_run` fold-seal row | 25 bytes |
| `condemned` fold-seal summary row | 30 bytes |

These are accepted readability costs. Exact-member-name alternatives are rejected because they add
another tens of bytes per row, particularly for `source_id`, `delete_pending`,
`marker_confirmed`, and nested manifest-reference fields.

Numeric object and line caps do not increase. Consequently, a canonical object near a current
uncompressed byte limit may admit fewer records after the rename. This is intentional: increasing a
cap would increase whole-read memory and line-allocation exposure merely to compensate for spelling.
Admission estimators and fold-seal reservation helpers must continue measuring through the real
encoders rather than using hand-maintained byte constants.

Implementation verification records before/after encode and decode throughput and records per
second for representative large `cas_run`, `RefSnapshot`, `PartManifest`, `FoldSeal` (with a
realistic distribution of `ref_life` row variants), and `RefCatalog` inputs, together with the
maximum record count under each object cap before and after, and — for `.zst` formats — the stored
bytes alongside the decompressed bytes. These measurements are review evidence,
not a timing assertion in CI. Exact encoded-byte deltas and boundary sizes are pinned in deterministic
unit tests. The dominant performance risk of the whole change is the longer keys themselves — extra
bytes written, compressed, decompressed, and key-compared — which these measurements cover; the enum
tables must not add to it, so the review also inspects the generated assembly of the hot `toWord`
instances (the `cas_run` marker first) and of the hot match helpers (`cas_run` token matching and
`PartManifest` blob matching), with decode throughput staying the primary evidence. No timing
assertion is added to CI.

## Generation reset {#compatibility-and-generation-floor}

There are no existing installations and no persisted data that must survive, so the breaking change
ships as a reset, not a bump. Implementation must:

- set `G_BUILD` to 1; `currentCompatibilityVersion` and `currentWriterVersion` stamp every newly
  encoded object with `v:1`;
- collapse the change-point history of all 17 registered formats (and the reserved `Roster` row) to
  one shared `BASELINE` constant holding `{1, 1}`, and delete the named legacy generation constants
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

## Vocabulary evolution {#vocabulary-evolution}

The reset erases the history, not the rules for the next change. What a change costs is determined
by what an old tolerant reader does with it:

| Change | Old tolerant reader | Contract |
|---|---|---|
| new *semantically ignorable* optional ordinary key — omission preserves exactly the safe old semantics, and no writer depends on old readers honoring it | skips it | may be additive: no `v` bump; best-effort on mutable objects until the floor rises. An optional key that affects fencing, retention, or deletion is not ignorable — route it through `!` or a breaking change |
| new key in a strict format | rejects (`CORRUPTED_DATA`) | breaking: bump plus change point |
| new enum or tag word | rejects as corruption (`fromWord` fails closed) | breaking: bump plus change point. Append-only declarations such as `hold_reason` promise only that values are never renumbered or reused — not that appending is compatible |
| new required field, or new meaning of an existing value | may misread the object | breaking: bump, change point, and a pool-floor raise |
| new `!` key | `UNKNOWN_FORMAT_VERSION` on exactly the objects that carry it | additive-critical: objects without the key stay readable, objects with it fail closed. This does not replace a bump — use `!` when old readers must not misread new objects, and a bump plus floor when old builds must be fenced from the pool entirely |

The codec-author checklist for any vocabulary change, in one place:

1. extend the enum wire table or the word constant (the set-equality assert will not let a table
   lag its enum);
2. pick the row above; for a breaking row, bump `G_BUILD`, append the change point, and raise the
   pool floor when old builds must be fenced;
3. update the goldens and the negative fixtures;
4. re-check the byte budgets: the descriptor `static_assert` and the reservation helpers recompute
   themselves, the delta table in this document does not;
5. confirm introspection renders the new value through the shared table;
6. update the `Formats/README.md` row and this document's tables in the same commit.

## Codec structure {#codec-structure}

The implementation remains a mechanical codec change rather than a new schema subsystem, but every
vocabulary item gains a single production carrier:

- **Key constants.** Each wire key is one format-local `constexpr std::string_view` (shared fields
  in `CasWireVocab`, format-local ones next to their codec), read by both the writer and the
  reader, so a writer/reader spelling divergence is impossible by construction. Critical keys carry
  the `!` prefix inside the literal (`"!prev_epoch"`), making criticality part of the single
  spelling. Keys existing in bare/`old_`/`new_` forms are declared as full-key bundles (see the
  shared vocabulary section) — never assembled from a prefix at write time. The key parameter of
  `writeKey` and of every field helper is a `WireKey` — a thin strong type with an explicit
  constexpr constructor — so a raw string literal cannot be passed silently; an inline
  `WireKey{"..."}` at a call site still compiles, but it is visually loud and review rejects it.
  The honest scope of the guarantee: for codecs that read the carriers, writer/reader divergence
  is impossible; nothing physically prevents a future codec from bypassing them. `WireKey` makes
  the write-side bypass conspicuous; on the read side a literal comparison still compiles
  silently, so reader carrier discipline is held by the codec structure, the goldens, and review —
  a symmetric read-key type was considered and judged excess machinery. There is no central schema
  registry; locality is preserved.
- **Enum wire tables.** The `ProvenanceOp` decision generalizes to every enum-backed persisted
  word vocabulary **whose entire enum domain is serializable**: `ProvenanceOp`, `TokenType`,
  `HoldReason`, `OutcomeKind`, `MetaState`, `NsState`, `RefOpKind`, `RefOwnerKind`, `BlobHashAlgo`
  (whose word functions are today split between `CasBlobDigest.cpp` and `CasWireVocab.cpp` — a
  live instance of the drift class this layer removes), `ObjectKind`, the placement words, and the
  new run-marker enum. Each moves from its switch-plus-if pair into one constexpr table whose
  coverage is proven by compile-time **set equality with `magic_enum::enum_values<E>()`** — every
  declared enumerator appears exactly once and nothing else does. (Size-plus-uniqueness is not
  enough: an invalid casted value would satisfy both while an enumerator goes missing. Set
  equality is the actual replacement for the `-Wswitch` guarantee the switches provided.) The
  table also proves two-way uniqueness of words and gives fail-closed `fromWord`
  (`CORRUPTED_DATA` on an unknown word; the defensive `toWord` throw stays against out-of-range
  enum values). `magic_enum` may back these asserts in `.cpp` files and tests only, never in
  production headers. Access is asymmetric by design: all these enums are dense, so `toWord` —
  which sits on the encode hot path (`cas_run` renders a marker word on every row) — is a direct
  indexed lookup (bounds check, index arithmetic from the first entry's underlying value, one
  defensive identity guard), never a scan; `fromWord` stays a linear pass over the handful of
  words, matching today's `if` chain. The compile-time asserts therefore additionally prove
  density and sorted-by-underlying-value order, making the index arithmetic valid by construction.
  No maps, no hashing, no allocation on either path; the exception paths stay cold. One enum may
  also carry several word contracts, and the wire table owns only the persisted spelling:
  `BlobHashAlgo`'s persisted `ch128` and its configuration spelling `cityhash128`
  (`ContentAddressedSettings`) are different contracts and are not merged into one table. Two
  enum-adjacent cases are deliberately outside the table rule: `RefLifecycle`, whose wire domain
  is a strict subset of the enum — `Removed` must never be serialized, so a table entry for it
  would either open the wire to a forbidden state or break the coverage proof; and `FormatId`,
  whose type strings already belong to the `TRAITS` registry rather than a separate wire table.
  (The fold-seal classification, numeric in earlier revisions, becomes a word vocabulary and joins
  the tables — see the fold seal section.)
- **Non-enum words.** Words with no backing fully-serializable enum — the record tags `entry`,
  `ref_life`, `blob_run`, `condemned`, and single-value vocabularies such as `live` and
  `source_edge` — are single constexpr word constants shared by writer and reader (the snapshot
  row tags `committed` and `precommit` render through the `RefOwnerKind` table rather than a
  parallel spelling). `live` additionally keeps the explicit `lifecycle == RefLifecycle::Live`
  check, since the enum's `Removed` value must never reach the wire. The run marker — today three
  raw `char` constants — becomes a typed enum over the same pinned byte values `0x00`, `0x01`,
  `0x02` (the bytes also persist in the in-degree payload representation, so they are part of the
  contract): an invalid marker becomes unrepresentable, and the marker's word table joins the
  standard set-equality rule above.
- **Field write helpers.** Each writer line becomes one call that names its encoding —
  `writeWordField`, `writeStringField`, `writeU64StringField`, `writeNumberField`,
  `writeHex128Field`, `writeBoolField` — collapsing today's `writeKey`-plus-value pairs so a
  codec's writer reads as the format's field list in canonical order. `writeWordField` takes a
  wire-table word; `writeStringField` takes an arbitrary escaped string — paths, namespaces,
  hostnames, ref names, token values, cursors — because spelling an open string through the word
  helper would misstate its contract. Two writers stay codec-owned, outside the field-helper rule:
  the blob envelope's frozen truncated-`ref` writer (its escaper and budget arithmetic are part of
  the envelope contract) and the raw payload zones. There is deliberately no type-overloaded
  `writeField`: the encoding rule is range-driven, not type-driven (`seq` is a decimal string and
  `expires_at_ms` a number, both `uint64_t`), so the encoding must be named at the call site — an
  overload would pick one silently. The helpers are thin inline forwarders over the existing
  writer primitives: no allocation, no added branching, codegen-equivalent to the pairs they
  replace, and the `cas_run` row writer is the hot path on which that equivalence must hold.
- **Paired match-and-build helpers for shared value types.** `CasWireVocab`'s three write helpers
  gain read counterparts with a two-stage contract. `matchBlobRefFields`, `matchTokenFields`, and
  `matchManifestRefFields(key, reader, bundle, collector)` each consume exactly one recognized
  field into a `Fields` collector and return whether the key was theirs — the codec's read loop
  stays in charge, and no single field can validate the group (the digest width needs both `algo`
  and `digest`, in any key order). At the point its grammar requires the group, the codec calls
  `fields.build(what)`, and `build` owns everything that needs the whole group: group
  requiredness, word parsing, the digest-width validation before `fromHex` (today copied into each
  codec — the copy-forgets-the-check failure mode disappears), the `CORRUPTED_DATA` taxonomy, and
  independence from key order. Requiredness thus remains an explicit call in the codec's grammar,
  at the variant point that demands it. This generalizes the collector-plus-`build` pattern the
  ref codecs already use (`ManifestFields::build`). `TokenFields::build` requires both
  `token_type` and `token`: today `GcOutcomes` tolerates a missing token value and reads it as
  empty while `cas_run` requires both — that divergence is adjudicated as a `GcOutcomes` reader
  bug, since `writeTokenFields` has always emitted both fields, so the unified requirement rejects
  only shapes no writer ever produced. The tightening is a deliberate, separately-landed change
  with its own negative test — never a silent side effect of the mechanical helper move — and is
  sequenced with the wire cut. One bundle feeds both directions, so the shared flat types are
  symmetric. The match helpers are defined inline next to the key bundles:
  on the hot decode paths they must add no function call, allocation, or branch beyond the
  comparisons the inline chains make today; no stored callables, no maps.
- `CasInspect` renders enum values through the same tables, so introspection cannot print `Merge`
  where the wire says `merge`. `system.cas_*` column names are deliberately NOT coupled to wire
  keys: the SQL surface and the persisted format have different compatibility contracts.
- `CasTextFormat` retains `type`, `v`, `n`, and the existing `!` handling; `CasRefWireVocab`
  continues to own value representation and validation for `RefTxnId`, while each containing format
  supplies contextual keys such as `txn_epoch`, `snapshot_epoch`, or `committed_epoch`;
- after the reset, all change-point rows reference one shared `BASELINE` constant instead of
  per-format copies of the same `{1, 1}` history;
- no map lookup, schema object, heap allocation, or runtime branch is added to select key names.

What deliberately stays hand-written is the grammar: variant-dependent requiredness
(`published_ms` on committed rows only, the condemned-row sextet), both-or-neither pairs, record
ordering, and payload zones remain explicit C++ in the readers. Of the schema triple
required/optional/critical, only criticality survives into the carriers — via the `!` prefix in the
key literal; the rest is validation logic, written once and readable. The line between a helper and
a framework is fixed: a match helper consumes one known field and returns whether it matched; it
never owns the read loop, never decides the unknown-key policy, and never tracks requiredness. The
collector's `build` is where group requiredness is validated, and the codec keeps that requirement
visible by explicitly invoking `build` at the variant point that demands it. Reader dispatch
tables, stored callables, and fluent writer builders are out.

Writer order remains canonical. Reader strictness remains exactly as registered in `CasFormat`.
Unknown critical fields continue to fail before strict/tolerant handling, and exception taxonomy does
not change.

## Implementation shape {#implementation-shape}

The change lands in three phases, so the mechanical part stops being hundreds of independent
replacements: first the writers and readers are bound to one vocabulary, then the vocabulary
changes in one place.

1. **Behavior-preserving preparation.** Introduce the key constants with the OLD spellings —
   including the full-key bundles for the `old_`/`new_` families — and move every writer and reader
   onto them; introduce the enum wire tables keeping the current words; introduce the per-encoding
   field write helpers and the shared-type match helpers; perform the four audit-driven member
   renames; close the battery omissions and add the registry set-equality assertion. Goldens stay
   byte-identical throughout — a green suite is the proof that preparation (helpers included)
   changed no wire byte.
2. **Atomic wire cut.** Flip the constant values and the record-tag words, reset `G_BUILD` and the
   change-point history to the shared `BASELINE`, update the literal goldens and negative fixtures,
   keep the sentinels and both pool gates, and update `Formats/README.md` in the same change. The
   descriptor `static_assert` lands in this phase — the cut must not compile if it overflows the
   floor. The `TokenFields` requiredness unification (the `GcOutcomes` tightening) lands here as
   its own explicit change with its negative test, and `MountLease::min_active` is renamed to
   `min_active_build_sequence` together with its wire key — the fifth member rename tracks the cut,
   not the audit.
3. **Proof and measurement.** Cross-check the constexpr worst case against the real encoder, run
   the byte-delta and throughput measurements, and sweep the raw assertions in integration tests
   and `utils/ca-soak`.

## Test strategy {#test-strategy}

The implementation updates every exact-byte fixture and adds coverage in five layers.

First, each of the 17 registered formats receives or retains a canonical encode/decode golden that
pins the post-reset header, field order, key spelling, tag spelling, and value representation.
Goldens remain inline with the codec unit tests; no generated golden-update command is introduced.
Goldens spell their bytes literally and must not be constructed from the production key constants
or enum tables: a golden built from the carriers would compare the encoder to itself and could not
catch a wrongly chosen name. (The envelope battery already states this principle for itself; it
becomes format-wide.)

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
- the known-field sentinels still reject `pl` on rows and `rte`/`rts` on the snapshot meta line;
- a `GcOutcomes` record missing `token` is rejected with `CORRUPTED_DATA` — the negative test of
  the deliberate requiredness tightening.

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
duplicating a second byte formula. Value-set tests iterate each enum wire table — every entry
round-trips through `toWord` and `fromWord`, and the word list is pinned against the closed sets in
this document.

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

### Declarative field schema {#declarative-field-schema}

A compile-time table of `field → key → required/optional/critical` looks like the natural next step
after key constants, but requiredness here is variant-dependent (`published_ms` is required on
committed rows and forbidden on precommit ones; the condemned sextet exists only under
`mark: condemned`; `old_`/`new_` obey a group grammar; `!prev_epoch`/`!prev_seq` are positional),
so expressing it declaratively means inventing conditional schemas, pair groups, ordering rules,
and payload transitions — a DSL beside which the hand-written validation would still exist, turning
the schema into a second source of truth. The grammar stays explicit C++. Of the schema triple,
only criticality survives into the carriers, via the `!` prefix inside the key literal.

### Generated goldens and a golden key scanner {#generated-goldens-and-a-golden-key-scanner}

Generating goldens from the production carriers is rejected outright: goldens are the independent
wire oracle, and an oracle derived from the thing it checks pins nothing. A repository-wide golden
key scanner (walk every golden, assert each key belongs to the declared vocabulary and each
declared key appears somewhere) is deferred, not adopted: to walk real objects it would need to
understand zstd framing, NDJSON, the padded blob header, the `PartManifest` payload zone, mutually
exclusive record variants, and test-only keys — a second, incomplete parser. Full-coverage literal
goldens, the registry/battery set-equality, and per-table round-trip tests provide nearly the same
protection more simply. Because the scanner is deferred, no enumerable per-format key arrays are
introduced either — scaffolding without a consumer.

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
  descriptor fits 256 bytes with a 17-byte `ref` budget; the mandatory worst case is a constexpr
  expression over the shared `ProvenanceOp` wire-word table, guarded by `static_assert` against
  `kMinBlobHeaderLen`, and the boundary test independently confirms the formula against the real
  encoder;
- every wire key is read by its writer and reader from one constexpr constant — the
  bare/`old_`/`new_` families through full-key bundles, never prefix assembly; every fully
  serializable enum-backed vocabulary — the run marker included, as a typed enum over the pinned
  bytes `0x00`/`0x01`/`0x02` — lives in one wire table whose values are proven set-equal to
  `magic_enum::enum_values<E>()` with two-way word uniqueness, density, and ordering statically
  asserted, so that `toWord` is a direct indexed lookup (`magic_enum` confined to `.cpp`
  files and tests); `RefLifecycle` and `FormatId` stay outside the table rule (`live` is a word
  constant guarded by the explicit `Live` check), the fold-seal `class` values are the four words
  from the documented semantics, `algos_used` is a JSON array, and configuration spellings are
  never merged into wire tables;
  non-enum tag and single-value words are single constexpr constants; `CasInspect` renders enum
  values through the tables; `kMinBlobHeaderLen` has one compile-time owner shared by the envelope
  encoder and `validatePoolBlobHeaderLen`; and all change points reference one shared `BASELINE`;
- goldens spell their bytes literally and reference no production carrier;
- writers use the per-encoding field helpers including `writeStringField` (no type-overloaded
  `writeField` exists; the envelope `ref` writer and the payload zones stay codec-owned), the
  three shared value types follow the match-plus-`build` contract — `build` owning group
  requiredness and the digest-width check — with the match helpers inline and adding no call,
  allocation, or branch on hot decode paths, and no reader dispatch table, stored-callable, or
  fluent-builder machinery exists;
- no C++ member is more cryptic than its wire key, including the five member renames this
  requires: `RunRef::generation` to `key_generation`, both `ManifestFields` collectors'
  `me`/`mb`/`mo` to `epoch`/`build`/`ord`, `BindingFields`' `bk`/`rn`/`mf` to
  `kind`/`ref`/`manifest_fields`, and `MountLease::min_active` to `min_active_build_sequence`
  (following its wire key);
- common, codec, corruption, byte-budget, and exact-encoding unit tests pass for all 17 formats,
  and the battery covers exactly the registry;
- raw assertions in integration tests and `utils/ca-soak` use the new spellings;
- `Formats/README.md`, codec comments, the `CasPoolMetaFormat.cpp` worst-case table, and the
  backlog no longer claim a universal 2–5-character or exact-full-member-name convention;
- local before/after measurements report the decompressed and (for `.zst` formats) stored bytes,
  encode/decode throughput, records per second, and the maximum record count under each cap for
  `cas_run`, `RefSnapshot`, `PartManifest`, `FoldSeal` with realistic row-variant distributions,
  and `RefCatalog`; the generated assembly of the hot `toWord` instances and of the hot match
  helpers (`cas_run` token matching, `PartManifest` blob matching) is reviewed — with no timing
  assertion in CI.

## Revision history {#revision-history}

- Revision 1 (2026-08-28): initial semantic-vocabulary design, framed as a generation-11 bump.
- Revision 2 (2026-08-28): the three adjudications — sufficiently-full keys, generation reset,
  asymmetric member rule.
- Revision 3 (2026-08-29): sentinels kept as forbidden-field guards, pool-gate tests, absolute
  member rule, value sets completed, `ProvenanceOp` constexpr table.
- Revision 4: member-rule audit extended to codec-local collector structs.
- Revision 5: constructive-enforcement layer; declarative schemas, generated goldens, and a golden
  key scanner rejected.
- Revision 6: full-key bundles, set-equality coverage proof, exact table/constant split, one owner
  for `kMinBlobHeaderLen`.
- Revision 7: table rule scoped to fully-serializable enum domains; run marker settled as a typed
  enum.
- Revision 8: performance profile — indexed `toWord`, linear `fromWord`, persisted versus
  configuration spellings separated.
- Revision 9: codec readability layer — field write helpers, match helpers, the
  helper-versus-framework boundary.
- Revision 10: `writeStringField`, the match-plus-`build` collector contract, codegen-neutral
  match helpers.
- Revision 11: `TokenFields::build` requires both fields (the `GcOutcomes` tightening); boundary
  wording fixed.
- Revision 12: `class` becomes words with the capacity calculation done; fold seal and catalog
  join the deltas and measurements (stored bytes, records per second, capacity under caps); the
  vocabulary-evolution matrix and codec-author checklist; `WireKey` and the honestly-scoped
  by-construction claim; `algos_used` becomes a JSON array; `min_active` becomes
  `min_active_build_sequence`; canonical examples added; the revision history moved here.
- Revision 13: `CoverageClass` settled as a dense enum (`Clamped = 3`; the old byte 4 dies with
  the numeric wire); the capacity arithmetic corrected to count the whole rename (120 → 149–152
  bytes per row, about 2.24M → 1.77M lives); the additive-evolution row narrowed to semantically
  ignorable keys; `WireKey` propagated into the bundles and its guarantee split honestly between
  the write and read sides; the fifth member rename sequenced into phase 2.
