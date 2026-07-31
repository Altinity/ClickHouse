# Task 5 alternative: make removal monotone with one transient catalog state

## Status

This is a proposed replacement for the current Task 5 design. It deliberately
optimizes for:

- the smallest state machine that is safe across process stops, stale GC
  leaders, and arbitrarily many later GC rounds;
- locally checkable rules instead of temporal arguments spanning the catalog,
  `gc/state`, and a fold seal;
- one authoritative meaning for each persisted object;
- removal of tests which ask the implementation to distinguish physically
  indistinguishable states.

It is not yet an amendment to the authoritative specification.

## Decision in one paragraph

Add one transient catalog state, `RemovalReady`, between `Removing` and entry
deletion. A namespace enters `RemovalReady` only after its terminal record has
folded and its life-scoped cleanup item is durably `Completed`. GC never walks
or creates a cursor for a `RemovalReady` entry. A later seal therefore prunes
the old name-keyed cursor monotonically: no subsequent round can reintroduce
it while the entry still exists. Once a durable seal contains neither that
cleanup item nor that cursor, the removal driver exact-deletes `_ckpt` and
exact-CAS-deletes the same `RemovalReady` catalog entry. Cleanup work always
uses the `NamespaceLifeId` captured at deposition and never re-derives an
incarnation from the catalog.

This keeps the current name-keyed cursor format. It adds one lifecycle word and
one catalog transition instead of changing every cursor producer, consumer,
codec, fsck path, REBUILD path, and sweep lookup to a life-keyed cursor.

## Why the previous entry-delete-last formulation is insufficient

The current proposed invariant says that a seal with the namespace cursor
pruned must be durable before entry deletion. That observation is not stable.
After such a seal, but before the entry is deleted, the catalog still contains
a `Removing` row. A process stop can leave it there indefinitely. Any later GC
round which walks every `Live` or `Removing` catalog entry can add the
name-keyed cursor again.

The removal driver then has no safe and live choice:

- trusting the historical pruned seal can delete the entry while the current
  seal carries a cursor;
- requiring the current seal to be pruned can wait forever, because every new
  round is allowed to add the cursor again.

`RemovalReady` removes this oscillation. It is a durable, catalog-resident
statement that the namespace must never be folded again. Once the state exists,
cursor absence is monotone.

## Catalog states

The catalog has four transient/active states:

```text
Creating -> Live -> Removing -> RemovalReady -> absent
```

Their meanings are:

- `Creating`: creation is fenced and `_ckpt` is not yet installed as a live
  namespace. Publication is impossible.
- `Live`: normal positive ownership and namespace-file writes are allowed.
- `Removing`: new positive ownership is forbidden. The terminal record may be
  pending, folded, or undergoing the bounded cleanup pass.
- `RemovalReady`: the terminal record is folded and the cleanup item reached a
  durable `Completed` state. No writer publication, recovery, ref fold, or new
  cursor is permitted. The only legal mutation is final removal of this exact
  row.

`RemovalReady` is not a permanent `Retired` tombstone. It exists only during an
in-flight removal, is counted by the same catalog and fold-seal admission
reservation as the other states, and is deleted after cursor retirement.
Consequently the catalog remains O(active namespaces plus in-flight removals),
not O(historical namespace births).

Creation of the same name receives the same typed retry-later response for both
`Removing` and `RemovalReady`.

## Ordered lifecycle

### 1. Start removal

The owning mounted writer performs an exact catalog transition:

```text
Live -> Removing
```

The comparison covers the complete observed `CatalogEntry`, including its
incarnation. `Removing` forbids new positive ownership.

### 2. Append the terminal record

Only the owning writer, or a successor which has claimed and fenced the same
server root, may append `RemoveNamespace`.

GC never invents or appends this record. If GC observes `Removing` without a
terminal record for the configured number of rounds, it reports the stuck
removal and leaves it unchanged.

### 3. Fold and deposit cleanup

When GC folds the terminal record, it deposits:

```text
CleanupItem {
    life: NamespaceLifeId,
    remove_txn_id: RefTxnId,
    state: Pending
}
```

`life` is captured from the catalog snapshot used by that fold. It is persisted
in the item. Every resumed or stale cleanup pass uses this value directly.
Re-reading the catalog to choose a cleanup incarnation is forbidden.

The cursor may remain name-keyed because the catalog still prevents rebirth at
this point.

### 4. Run one bounded cleanup attempt

A committed `Pending` item runs one bounded, suppression-aware, best-effort
cleanup attempt.

The attempt:

- acts only when destructive work is allowed;
- uses exact-token deletion;
- treats `NotFound` and token mismatch as retained/deferred work;
- never turns a LIST omission into a safety conclusion;
- marks the item `Completed` after the bounded attempt has been durably
  accounted for.

The pass does not LIST and delete the shared manifest namespace prefix.
`RemoveNamespace` already names every removed owner explicitly. The pass
re-reads that exact terminal record through the deposited
`NamespaceLifeId`/`remove_txn_id`, derives each `ManifestId`, and exact-deletes
those manifest bodies after their decrements are durable. The terminal record
is retained until the item becomes `Completed`. If it cannot be read or
validated, the item remains `Pending` and the round fails closed; it never
substitutes an empty manifest set.

This gives manifest cleanup a deterministic finite work set without changing
manifest keys and without trusting LIST completeness. A manifest key cannot
alias a later build because its mount-global build identity is never reused.
Unrelated pre-existing orphan manifests remain the ordinary orphan sweep's
responsibility; a manifest which that sweep never lists can leak storage, but
it is not turned into a lifecycle precondition.

Other physical residue is not a condition for rebirth:

- ref objects and `_files` are life-qualified and unreachable from a later
  life;
- covered ref logs and snapshots are deleted from deterministic cursor and
  checkpoint ranges;
- `_files` cleanup is a best-effort LIST under the deposited life prefix, so an
  omission can leak bytes but cannot affect visibility or rebirth.

This explicitly changes the current `namespaceManifestsPhysicallyEmpty`
assumption. A LIST result cannot prove physical emptiness on the stores this
design is intended to tolerate, so it must not gate a lifecycle transition.
Terminal-owned manifests are instead known exactly from the terminal record.

### 5. Make cursor retirement monotone

After a fold seal containing the matching life-scoped cleanup item in
`Completed` is durable, the GC owner performs an exact catalog CAS:

```text
Removing -> RemovalReady
```

This transition uses the `Completed` item itself as positive evidence. It does
not infer completion merely from the absence of an item.

This detail closes the process-stop window:

- if the seal is durable but the catalog CAS did not happen, the next round
  still sees `Removing` plus the durable `Completed` item and retries the CAS;
- if the catalog CAS happened, every later round sees `RemovalReady` and is
  forbidden from adding the namespace to `walk_targets`.

### 6. Retire the cleanup item and cursor

A round whose catalog cut contains `RemovalReady`:

- does not probe or fold the namespace;
- does not copy or recreate its cursor;
- removes the matching `Completed` cleanup item from the next seal;
- omits the namespace's shard-0 cursor from the next seal.

Both absences become durable together in the adopted seal. Repeating any
number of later rounds preserves both absences because `RemovalReady` is not a
fold target.

### 7. Delete `_ckpt` and the catalog entry

The removal driver reads the current adopted seal and the current catalog. It
continues only if:

- the exact catalog row is still the expected `RemovalReady` row;
- the matching cleanup item is absent;
- the name-keyed shard-0 cursor is absent.

It then:

1. exact-deletes the old life's `_ckpt`;
2. exact-CAS-deletes the complete observed `RemovalReady` catalog row.

A stop after `_ckpt` deletion is resumed by the owning writer's next mount or
the existing dead-root/decommission path. Neither path re-creates `_ckpt`.

The same-name retry can now create a fresh row with a fresh incarnation.

## Stale cleanup safety without `_cleanup`

The persisted `_cleanup` marker can be deleted because no stale pass performs
a namespace-wide delete against keys a new life can create:

- ref objects and `_files` use the deposited `NamespaceLifeId`, so a stale
  pass can reach only the old prefix;
- terminal-owned manifests are selected by exact `ManifestId` from the old
  terminal record, not by a shared-prefix LIST;
- manifest build identities are never reused, and deletion is exact-token.

A fresh catalog/life/fence validation still runs immediately before every
destructive delete, as required by the general destructive-cleanup contract.
It is defense in depth rather than the only thing separating an old prefix
pass from a new life.

Capture-at-deposition is still mandatory. The existing stale-leader TLA
sabotage demonstrates that re-deriving the incarnation after rebirth targets
the new life directly.

## Catalog authority and entry-less objects

The runtime must stop trying to distinguish these two object-store states:

```text
legal removal:  catalog entry absent, old canonical life objects survive
fabricated loss: catalog entry absent, the same canonical life objects survive
```

They are physically indistinguishable after `_cleanup` and the cleanup item
are gone. No anomaly classifier can accept the first and reject the second
without another permanent evidence object.

The simple rule is:

- a canonical life-qualified object whose life is not the current catalog
  life is inert debris;
- a canonical cursor for a namespace absent from the catalog is retired
  metadata and is omitted from the next seal;
- malformed or unparseable keys remain anomalies;
- an absent or undecodable catalog object in a Stage-B pool is corruption and
  stops creation, removal, and GC.

The final rule requires changing `CasRefCatalog::read`: once the Stage-B pool
format is initialized, a missing `cas/ref_catalog` must not be interpreted as
a virgin empty catalog. Pool creation writes the empty catalog before marking
the new format ready. No new marker object is needed; the existing pool format
version is the bootstrap boundary.

An individual catalog entry cannot disappear because of a partial object-store
write: the catalog is one atomically read and token-CAS-written object. Invalid
entry removal is therefore prevented and tested at the catalog mutation API,
not inferred later from ambiguous debris.

## Capacity bound

The transient state preserves a simple bound:

- every `Pending` or `Completed` cleanup item still has one catalog row;
- rebirth cannot create a second removal for the name before that row is
  deleted;
- every name contributes at most one namespace cleanup item and one shard-0
  cursor to the current seal;
- `RemovalReady` remains charged until both rows disappear.

The admission reservation must separately cover non-per-namespace seal data,
including `blob_target_runs` and `condemned_summary`. Entry ordering does not
make those rows disappear, so they must not be hidden inside the per-entry
argument.

The exact pre-PUT encoded-size check remains the final gate.

## Retry latency

The minimal implementation accepts and documents the retry window. It does not
add a DROP-driven targeted fold, a CREATE-side helper, a new worker, or a
special synchronous GC path.

For a name in `Removing` or `RemovalReady`, creation returns a typed retryable
error which states that it is waiting for terminal fold, cleanup completion,
and cursor retirement.

If UUID-less DROP plus CREATE latency later proves operationally unacceptable,
a bounded CREATE-side assist can be added without changing this state machine.
It is intentionally not part of the correctness baseline.

## Required tests

### Catalog state machine

- only `Live -> Removing -> RemovalReady -> absent` is accepted;
- `RemovalReady` cannot return to `Removing`, `Live`, or `Creating`;
- exact-CAS mismatch leaves the catalog byte-identical;
- a same-name create is retryable during both removal states;
- after absence, the same name creates a fresh incarnation.

### Monotone cursor retirement

- stop after the seal which completed cleanup but before the
  `Removing -> RemovalReady` CAS; restart and finish;
- stop after `RemovalReady` but before the pruning round; run several GC
  rounds and prove none reintroduces the cursor;
- stop after the pruned seal but before `_ckpt` deletion; run several GC
  rounds and prove the cursor stays absent;
- stop after `_ckpt` deletion; remount and delete only the exact
  `RemovalReady` entry without re-creating `_ckpt`;
- reuse low sequence numbers in the same writer epoch and prove the new life
  folds from its own beginning.

### Stale cleanup

- deposit cleanup for incarnation 1, stall the actor, complete removal, create
  incarnation 2, write ref objects, `_files`, and manifests, then resume the
  stale actor; every incarnation-2 byte survives;
- omit terminal-owned manifests from every LIST result and prove cleanup still
  reads their identities from the exact terminal record and deletes only those
  exact old keys;
- remove or corrupt the terminal record before completion and prove the item
  remains `Pending` and destruction is suppressed;
- sabotage capture-at-deposition by re-deriving from the catalog and require
  the test/model to fail;
- change an object's token between LIST and deletion and prove it is retained;
- suppress destructive work and prove no cleanup delete occurs;
- feed an unparseable key and prove anomaly-and-continue behavior.

### Catalog authority

- canonical objects from a deleted life do not raise an un-cataloged anomaly
  and cannot enter the fold;
- malformed keys still raise an anomaly;
- a missing or undecodable catalog in an initialized Stage-B pool fails
  closed;
- a never-opened read or removal performs zero catalog mutations, asserted
  from the operation journal rather than only from the final catalog bytes.

### Reader behavior

- plant old-life `_files`, prevent their physical cleanup, and prove logical
  reads return absent during `Removing`, during `RemovalReady`, and after entry
  deletion;
- keep a warm `RefTableRuntime` across deletion and rebirth and prove both
  life-assignment paths use the new incarnation without relying on LRU
  eviction.

## What this proposal removes from Task 5

- incarnation-scoped fold cursors;
- prune-before-entry-delete as a non-monotone temporal assumption;
- permanent `Retired` history;
- the `_cleanup` object class;
- the `Removed` snapshot as a completion artifact if its only remaining role
  is to authorize cleanup already represented by the durable `Completed`
  item;
- un-cataloged-removal evidence discrimination;
- enumeration of every possible `walk_targets` carrier;
- stale-resume-versus-reborn tests which try to prove the race impossible;
- eager targeted folding and other latency-specific workers.

It retains the one stale-resume test that matters: a stale actor exists, but
its deposited life and per-delete validation make it harmless.

## Comparison with life-scoped cursors

Making cursors life-scoped is safe and conceptually uniform, but it changes:

- the fold-seal wire grammar;
- every cursor key builder/parser;
- GC intake and carried-hold identity;
- REBUILD and fsck;
- manifest sweep coverage lookup;
- capacity tests and all cursor fixtures.

It also does not by itself solve cleanup-item capacity or the non-life-qualified
manifest pass.

`RemovalReady` changes one catalog enum, its codec, one catalog transition, and
the GC universe predicate. It makes name-keyed cursor retirement monotone and
keeps the existing cursor representation. For the stated goal—minimum moving
parts and straightforward crash recovery—it is the smaller design.

## Rejected simpler-looking alternatives

### Delete the entry immediately after folding the terminal

This permits multiple cleanup items for repeated lives of one name and breaks
the fold-seal reservation bound. It also requires every deferred state to be
life-keyed and independently garbage-collected.

### Keep a permanent `Retired` row

This is simple locally but grows the catalog with historical namespace births,
eventually exhausts its object cap, and breaks supported exact-name reuse.

### Keep only the historical pruned-seal observation

This is the current race: another round can recreate the cursor while the
catalog entry remains `Removing`.

### Keep `_cleanup` as permanent removal evidence

A marker which must outlive every possibly stalled actor is unbounded. Deleting
it recreates the same stale-leader race it was introduced to prevent.

### Infer damage from entry-less debris

Legal debris and a fabricated missing entry have the same observable shape.
Adding more classifier branches cannot create information which is not
persisted.

## Recommendation

Replace the current Task 5 ordering with the transient `RemovalReady` protocol,
retain capture-at-deposition, make the Stage-B catalog mandatory after pool
bootstrap, and classify canonical non-current-life objects as inert debris.

Accept the retry latency in the first implementation. Do not add cursor
re-keying, a permanent tombstone, another evidence marker, or an eager GC
worker unless measurements later establish a concrete need.
