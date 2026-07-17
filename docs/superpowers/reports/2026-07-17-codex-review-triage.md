# Triage of the 2026-07-17 codex AI review (31 findings)

Input: `docs/superpowers/reports/20260717_codex_review.md` (static review of the whole CAS
branch diff from merge base `2ed6626a25e`, C++ only, `src/Disks/tests/**` excluded).
Method per the approved plan: contract-level findings traced inline by the controller;
every concurrency finding handed to a dedicated adversarial verifier subagent tasked to
REFUTE it and build the exact interleaving (14 verifiers: №1,2,3,4,5,6,7,8,9,10,11,18+19,
20+22, 26). Design-level findings are collected in §4 for the user's decision — no code
changed by this triage.

Verdicts: `REAL-FIX` (defect confirmed, fix in the follow-up wave) · `REAL-DEFER`
(confirmed, tracked in BACKLOG, not this wave) · `BY-DESIGN` (intended behavior — add a
code comment so the next reviewer does not re-report) · `FALSE-POSITIVE` (claim refuted —
comment only where the code genuinely misleads) · `DESIGN-DECISION` (user call).

## 1. Verdict table {#verdicts}

| № | Finding (short) | Verdict | Key evidence |
|---|---|---|---|
| 1 | Conditional S3 copy falls back to unconditional | PARTIAL → REAL-FIX (severity down from blocker) | §3.1 |
| 2 | Condemned-object resurrect PUT unconditional | REFUTED at harm level → BY-DESIGN comment | §3.2 |
| 3 | HEAD+GET identity straddle | REFUTED → comment only | §3.3 |
| 4 | Swallowed condemn-marker write vs adopt | CONFIRMED → REAL-FIX (top severity of the review) | §3.4 |
| 5 | Pre-CAS retention prune vs failed state CAS | CONFIRMED → REAL-FIX (wedged GC, ~2-line union) | §3.5 |
| 6 | Relink sender-to-receiver gap without pin | CONFIRMED → REAL-FIX (known: pin spec not wired) | §3.6 |
| 7 | Advertised relink pool ≠ reservation disk | CONFIRMED (both) → REAL-FIX (byte-fallback on mismatch) | §3.7 |
| 8 | Scheduler/cas_store shutdown races | CONFIRMED (UAF sub-claim) → REAL-FIX | §3.8 |
| 9 | Decommission deletes successor control objects | CONFIRMED → REAL-FIX (epoch monotonicity at stake) | §3.9 |
| 10 | Event-sink assignment/read data race | PARTIAL → REAL-FIX (TSan hygiene; crash-grade refuted) | §3.10 |
| 11 | Namespace drop misses unregistered build | PARTIAL → REAL-DEFER (narrow metadata leak; reviewer's fix wouldn't close it) | §3.11 |
| 12 | Directory ops mutate durable refs at call time | SPLIT: immediacy = BY-DESIGN; narrow sub-claim = REAL-FIX | §2.12 |
| 13 | Throwing telemetry after durable publish | REAL-FIX (hardening) | §2.13 |
| 14 | CityHash128 default without byte verify | DESIGN-DECISION | §4.14 |
| 15 | Local/NFS multi-server only INFO | DESIGN-DECISION | §4.15 |
| 16 | `ReaderExecutor` bypasses `file_view` window | REAL-FIX (confirmed inline) | §2.16 |
| 17 | Unknown GCS versioning fails open | DESIGN-DECISION (documented WARNING, not silent) | §4.17 |
| 18 | Emulated list token dialect mismatch | CONFIRMED → REAL-FIX (fail-safe leak + phantom delete accounting) | §3.18 |
| 19 | Conditional mutations send only `Token.value` | CONFIRMED-as-coded, harm LOW → REAL-FIX (hardening) | §3.18 |
| 19c | NEW (out-of-review): emu-restart seq-token collision | CONFIRMED → REAL-FIX (latent local-CA data loss) | §3.18 |
| 20 | Decoders accept incomplete/duplicate/unbound records | SPLIT: (a) PARTIAL hygiene-fix, (b) REFUTED, (c) CONFIRMED → REAL-FIX | §3.20 |
| 21 | Version narrowed before validation | REAL-FIX (trivial, confirmed inline) | §2.21 |
| 22 | Zero-valued GC config (spin / `gcs=0`) | CONFIRMED (all three) → REAL-FIX (config → permanent GC wedge) | §3.22 |
| 23 | `truncateFile` silent no-op | REAL-FIX (trivial) | §2.23 |
| 24 | `unlinkFile` ignores `if_exists` contract | REAL-FIX (minor) | §2.24 |
| 25 | Commit retry after rollback skips `published` entries | REAL-FIX (terminal-txn hardening) | §2.25 |
| 26 | Verbatim append CAS retry stale payload | PARTIAL → REAL-DEFER (latent; no concurrent appender exists) | §3.26 |
| 27 | Rename treats existing destination as replay | BY-DESIGN (documented single-writer re-drive) | §2.27 |
| 28 | Build-seq watermark pinned on ctor throw | REAL-FIX (scope guard) | §2.28 |
| 29 | No experimental-feature gate | DESIGN-DECISION | §4.29 |
| 30 | `system.content_addressed_mounts` swallows errors | DESIGN-DECISION (recommend error column) | §4.30 |
| 31 | `staging_backend=s3` silently falls back to local | DESIGN-DECISION | §4.31 |

## 2. Inline verdicts — reasoning {#inline}

### 2.12 Directory ops (№12) — SPLIT {#f12}
The reviewer's headline ("record intents in the overlay, apply at commit") attacks the
architecture itself: CA deliberately runs `removeDirectory`/`moveDirectory` against durable
refs at call time. This is the everything-immediate model: `renameParts` is the commit
point; a parts-transaction rollback COMPENSATES via new operations over committed state
(the same model the acked-loss fix formalized). MergeTree's own compensation paths
(`rollbackPartsToTemporaryState`, outdated-part cleanup) run over committed disk state on
every disk. Verdict for the immediacy claim: BY-DESIGN — add a contract comment at
`ContentAddressedTransaction::removeDirectory`/`moveDirectory` explaining call-time
durability + compensation, so it stops being re-reported.
The NARROW sub-claim is real: `removeDirectory` clears `content_removed` marks but leaves
`st.entries` staged (`ContentAddressedTransaction.cpp`, `removeDirectory`), so a
transaction that stages files for a part and then `removeDirectory`s that part BEFORE
commit still publishes it in `publishStaging`. Low reachability (no known MergeTree
sequence does create-then-remove-then-commit in one disk txn), but fail-closed hygiene:
`removeDirectory` must also discard the staged entries (and abandon the build) for that
ref. REAL-FIX (small).

### 2.13 Throwing telemetry after durable publish (№13) — REAL-FIX (hardening) {#f13}
Confirmed: `Pool::emitEvent` (`CasPool.h:564`) invokes the sink unprotected; the
`BuildPublish` emission in `PartWriteTxn` promote and emissions in
`ContentAddressedTransaction::commit` run AFTER the durable ref append. A throwing sink
(system-log queue push under OOM) turns a durably-succeeded operation into a caller-visible
failure. Mitigations already present: promote has an idempotent re-promote guard, and
`commit` has compensating unpublish, so the failure is CONSISTENT (retry converges or the
ref is dropped) — not state corruption. Still, telemetry must never override the outcome
of a durable operation: wrap the post-durable emission sites (or `emitEvent` itself) in
catch-log-continue. REAL-FIX, small, no behavior change on the success path.

### 2.16 `ReaderExecutor` bypasses the CAS payload window (№16) — REAL-FIX {#f16}
Confirmed inline. `ReadPipeline::tryBuildReaderExecutor` (`src/IO/ReadPipeline.cpp:203`)
falls back to the legacy chain for `distributed_cache || memory_cache ||
filesystem_caches || decryption_stages || async_prefetch` — but NOT for `file_view`, and
the executor branch returns `PipelineReadBuffer(executor)` without ever applying Stage 6.
A CA read populates `file_view` (`DiskObjectStorage.cpp:919-923`: payload window inside
the blob, skipping the `CHCA` envelope header). Reachable with `use_reader_executor=1`
(default 0, `Settings.cpp:758`) + `remote_filesystem_read_method='read'` (kills the
async-prefetch fallback) + no caches: the executor then reads raw blob bytes — envelope
header exposed, offsets shifted → wrong results. Fix: add `file_view` to the fallback
condition (fail-closed, one line); implementing the window inside the executor can come
later. Gated by an off-by-default experimental setting, hence not a release blocker, but
it is exactly the class of silent wrong-results we never leave open.

### 2.21 Version narrowing (№21) — REAL-FIX (trivial) {#f21}
Confirmed at all three cited sites (`CasTextFormat.cpp:292`,
`CasRecordStreamFormat.cpp:134`, `CasBlobEnvelopeFormat.cpp:189`):
`static_cast<uint32_t>(r.readU64Number())` truncates before `checkCompatibility`, so
`4294967299` decodes as version `3` and passes. No real writer produces such values —
this fires only on corruption/hostile bytes, where the whole point of the check is to fail
closed. Fix: a `readU32Number` helper (range-check then cast) used by all header readers.

### 2.23 `truncateFile` no-op (№23) — REAL-FIX (trivial) {#f23}
Confirmed: silent success while bytes remain. No caller found anywhere in
`src/Storages/MergeTree`/`src/Backups` (only the `IDisk` virtual). Since the operation is
believed unreachable on CA, a `NOT_IMPLEMENTED` throw is strictly safer than a silent
no-op: if some path DOES truncate, we want the loud failure, not readable stale bytes.

### 2.24 `unlinkFile` ignores `if_exists` (№24) — REAL-FIX (minor) {#f24}
Confirmed: both flags discarded (`ContentAddressedTransaction.cpp`, `unlinkFile`), and the
routing is syntactic, so unlinking a nonexistent committed file stages a removal mark that
publish resolves as a no-op — silent success where the non-`ifExists` API promises
`FILE_DOESNT_EXIST`. Harm is contract fidelity, not data. Fix: existence check against
staged entries + committed manifest for the non-`if_exists` variant.

### 2.25 Retry of a failed commit (№25) — REAL-FIX (terminal-txn hardening) {#f25}
Confirmed mechanism: the compensating rollback in `commit` drops created refs but leaves
each `PartStaging::published = true`, so a hypothetical re-`commit` of the same object
would skip them and report success. No caller retries the same
`IDiskTransaction` object today (a failed INSERT/merge builds a NEW transaction), so this
is latent — but "failed transactions are terminal" should be enforced, not assumed: set a
`failed` flag in the catch and throw `LOGICAL_ERROR` on any later `commit`/`tryCommit`.

### 2.27 Rename replay heuristic (№27) — BY-DESIGN {#f27}
The `source absent && destination present → success` branch (`moveFile`, both the
table-verbatim and mountpoint branches) is documented in place: verbatim renames run under
the SINGLE-WRITER contract (only the owning server renames its own mutation entries), and
destination names derive deterministically from source names
(`tmp_mutation_N.txt → mutation_N.txt`), so "unrelated destination present" has no
producer; the branch exists to make a re-driven rename idempotent, matching the
ENOENT-tolerant re-drive of a POSIX rename. The reviewer's "first-time move to an
unrelated existing destination" requires a caller that never existed. Action: none needed
beyond the comment that is already there; optionally name the reviewer confusion in it.

### 2.28 Watermark pinned on construction failure (№28) — REAL-FIX (scope guard) {#f28}
Confirmed: `Pool::beginPartWrite` (`CasPool.cpp:761-776`) registers the build seq in the
active set (`mount_runtime.allocateBuildSeq()`) BEFORE `make_shared<PartWriteTxn>`; a
throw from the allocation or the constructor's event emission leaves the seq registered
with no owner, permanently pinning `minActive` — the GC retention floor — until remount.
Probability is tiny (bad_alloc / throwing sink), damage is unbounded retention growth.
Fix: scope guard dismissed after successful construction+registration, exactly as the
reviewer suggests.

## 3. Verifier verdicts (concurrency findings) {#verifier}

### 3.1 Conditional copy fallback (№1) — PARTIAL, REAL-FIX (blocker severity refuted) {#v1}
Mechanism confirmed: inside `CopyFileHelper` both the single-op `AccessDenied` path
(`copyS3File.cpp:742-758`) and the multipart `ACCESS_DENIED` path (`:808-815`) drop to
`fallback_method` = `copyDataToS3File`, which has NO `if_none_match` parameter — the
precondition is silently discarded; the fallback also loses `out_dest_etag`, so CAS then
records an EMPTY incarnation token (`promoteStaged`, `CasObjectStorageBackend.cpp:882`).
Severity nuances the reviewer missed: (i) `copyObjectConditional` already fails closed for
`allow_native_copy=false` (`S3ObjectStorage.cpp:770`); (ii) the mount-time
`probeConditionalCopy` exercises exactly the single-op path and disables S3-native promote
on any backend where CopyObject falls back — so the common path is probe-guarded; (iii)
content-addressing means the overwrite writes byte-identical payload — the damage is
token-machinery breakage (empty token → exact-token GC delete mismatch → leak), not data
corruption. THE REAL GAP: real blobs >32 MiB take the MULTIPART copy path the probe never
exercises; an `ACCESS_DENIED` there silently degrades. Fix: refuse `fallback_method`
whenever `if_none_match.has_value()` (rethrow instead), and either extend the probe to a
multipart-size object or force single-op copy for conditional promotes.

### 3.2 Resurrect overwrite (№2) — REFUTED at harm level, BY-DESIGN comment {#v2}
The reviewer conflated two paths. The non-staging displacement path is ALREADY
`If-Match`-bound to the observed condemned token (`CasPartWriteTxn.cpp:637` →
`putOverwrite` sets `write_if_match`) — the suggested remediation is implemented there.
The S3-native-staging `resurrectStaged` (`CasObjectStorageBackend.cpp:887`) is genuinely
unconditional, and that is safe by three independent structural properties: (i)
content-addressed key ⇒ byte-identical payload across incarnations (an overwrite rotates
only envelope/token); (ii) adopted token VALUES never gate promote — deps are
tokenless-on-ref, only `has_value()` is consulted (`CasPartWriteTxn.cpp:238,248`); (iii)
fresh-tag semantics make every queued exact-token GC delete of a prior incarnation
mismatch — the resurrection cannot be killed by a stale delete. Worst case is the
already-documented benign orphan leak. Action: add the verifier's by-design comment at
`resurrectStaged` (an `If-Match` would only save a redundant re-upload, not prevent loss).

### 3.4 Swallowed condemn-marker write (№4) — CONFIRMED, REAL-FIX (top severity) {#v4}
The refutation attempt failed; the marker is load-bearing and its write can be lost
silently. Chain: `scheduleMetaJob` (`CasGc.cpp:183`) swallows ALL exceptions from
`writeCondemnedMeta` (`:876`, supersede `:513`) while the round commits the retired
`(hash, token)` regardless; a writer reusing that content sees absent/Clean meta
(`CasPartWriteTxn.cpp:292`) and ADOPTS THE SAME TOKEN (no re-upload, `:349-363`) — so the
exact-token delete defense (`CasGc.cpp:372`) matches and deletes a blob with a live
committed edge → dangling manifest. EDGE-BEFORE-OBSERVE does not close it: it orders the
writer's edge before the writer's OWN observation, not before GC's once-per-fold discovery
LIST — a writer landing in the [discovery-LIST, deleteExact] window of the graduation
round is invisible to the redelete's in-degree recount (`CasBlobInDegree.cpp:401-420`).
The GC's own safety comment (`CasGc.cpp:91-95`) assumes the marker was durably written —
a swallowed write breaks the premise. WORSE: disaster-recovery rebuild (`CasGc.cpp:2043`)
NEVER writes condemn markers at all, making the hazard systematic after a rebuild, not a
race. Fix direction (verifier option (a), cleanest): gate graduation/redelete on a
CONFIRMED durable Condemned meta for the exact (hash, token) — absent marker ⇒ carry the
entry to the next round (fail-safe delay) instead of deleting; rebuild must publish
markers before its entries can graduate. Note the standing GC rule (never throw on 404
during fold) is untouched — this gates a DELETE on missing evidence, it does not throw.

### 3.5 Pre-CAS generation prune (№5) — CONFIRMED, REAL-FIX (~2 lines) {#v5}
`pruneSupersededGenerations` (`CasGc.cpp:573`, physical delete `:1670`) runs PRE-CAS keyed
on `referenced_generations` built ONLY from the PROPOSED seal (`:570-572`); the parent
(adopted) seal's runs (`:341-343`) are never consulted. Two concurrent leaders whose fold
windows straddle a ref commit: loser flushes shard s to a new generation and prunes the
old one; winner carried the old generation's run verbatim and WINS the state CAS → the
adopted seal references a physically deleted run → every next fold throws
`CORRUPTED_DATA` in `PriorEdgeCursor` (`CasBlobInDegree.cpp:273-280`) → GC permanently
wedged (user data untouched; reclamation halted, leak grows). Violates the round's own
stated pre-CAS invariant (`CasGc.cpp:240-242`: destructive pre-CAS actions justified by
PREVIOUSLY PUBLISHED state only). Fix: seed `referenced_generations` with the UNION of
proposed + parent seal generations before pruning; the existing post-CAS hand-off still
reclaims parent generations once a committed round moves off them.

### 3.6 Relink handoff gap (№6) — CONFIRMED, REAL-FIX (wire the specced pin) {#v6}
My working assumption that the fetch-handoff retention pin had landed was WRONG — it is a
committed SPEC, not wired code. The gap is self-documented at
`ContentAddressedMetadataStorage.cpp:1335-1343`: the sender is fire-and-forget
(`DataPartsExchange.cpp:255-280` streams manifest bytes and releases the source part), so
if the receiver's `precommitAdd` edge-PUT stalls across ≥2 GC folds while the source goes
Outdated and the blob has no other ref, the blob is reclaimed → dangling committed
manifest (fsck-detected, not silent). Token-CHANGE recoveries are covered by the GC
`deleteExact` liveness re-check; only this same-token tail remains. Narrow conjunction,
detectable damage — but structurally real and acknowledged in-code. Fix: implement the
sender-created build-owned epoch-floor handoff pin per the existing spec (pin cleanup
rides the heartbeat `min_active` floor); interim alternative: gate relink off to the byte
path until the pin lands.

### 3.10 Event-sink race (№10) — PARTIAL, REAL-FIX (hygiene) {#v10}
Formal data race confirmed (plain `std::function` member, `CasPool.h:597`; subcomponents
hold it BY REFERENCE; `setEventSink` at `ContentAddressedMetadataStorage.cpp:483` runs
after `Pool::open` already spawned the renewal thread at `CasPool.cpp:499`). Crash-grade
severity REFUTED: the renewal thread's first possible sink read is ≥ one renew period
(default 10 s) after start and only on the renewal-failure path, while the write completes
microseconds after open returns — no wall-clock overlap. TSan WILL flag it (relevant to
the R6 sanitizer pass). Fix: thread the sink through `PoolConfig`/`Pool::open` so it is
installed before any thread starts (immutable-after-open); subcomponents already reference
the member, nothing else changes.

### 3.3 HEAD+GET identity straddle (№3) — REFUTED, comment only {#v3}
The ordering saves it: HEAD precedes GET, so the returned token is never NEWER than the
bytes — a mixed pair is always (bytes_newer, token_older), and every token consumer uses
the token as a conditional precondition (`casPut`/`putOverwrite`/`deleteExact`) that fails
closed EXACTLY when a post-HEAD replacement occurred. A stale token costs a retry, never
commits a mixed pair. Caller sweep (all safe): blob bodies are content-addressed
(byte-identical across incarnations, token discarded); mutable control objects
(gc/state, mount lease, epoch, pool meta, blob meta) are read-modify-CAS loops; write-once
objects validate on decode. The size sub-concern is moot: every `get()` call site reads
WHOLE objects, and `readObjectRanged`'s whole-read path drains to EOF ignoring
`known_size` for slicing. Action: add the verifier's one-paragraph comment at
`ObjectStorageBackend::get` (extends the existing deletion-race comment to
replacement/identity).

### 3.7 Relink pool ≠ reservation disk (№7) — CONFIRMED (both sub-claims), REAL-FIX {#v7}
Pool UUID is compared exactly ONCE — on the SENDER against the advertised pool
(`DataPartsExchange.cpp:261`); the receiver advertises the FIRST CA disk's pool when
`disk == nullptr` (`:556`), then reserves independently (`:639-689`) and commits against
the reservation-chosen disk with no pool re-check (`:752`). Non-CA reservation →
`LOGICAL_ERROR` BEFORE the byte-fallback lambda is reachable (availability wedge on
retry). CA disk in a DIFFERENT pool → `adoptPartFromManifest` publishes by manifest trust
without leaf-presence probes (`ContentAddressedMetadataStorage.cpp:1303-1306,1345`) → a
dangling committed manifest in pool B (fsck-detected, not silent). Requires an unusual
policy (two pools, or CA+local, with `disk == nullptr` at fetch) — nothing forbids it.
Fix (cheap, complete): after `reservation->getDisk()`, compare the chosen disk's pool UUID
to the advertised one; on mismatch OR non-CA disk take `fall_back_to_byte_fetch()`.

### 3.8 Shutdown races (№8) — CONFIRMED (raw-pointer UAF), REAL-FIX {#v8}
Sub-claim 1 CONFIRMED, crash-grade, narrow: `runGarbageCollectionRoundNow`
(`ContentAddressedMetadataStorage.cpp:359-368`) snapshots `gc_scheduler.get()` under the
mutex, unlocks, then derefs — `shutdown` (`:541-543`) can `reset()` between unlock and
deref (admin `SYSTEM ... GC` racing disk teardown). Note the reviewer got one detail
backwards: `gcHealth` holds the mutex for the whole call and is the one already-safe path.
Sub-claim 2 PARTIAL: `cas_store` unlocked read vs unlocked reset is a `shared_ptr` data
race (TSan-grade), but every consumer copies by value so the Pool pointee always outlives
— no pointee UAF; `part_access` (unique_ptr, same shape) COULD be a real UAF. Sub-claim 3
benign (post-shutdown lazy creation leaks a scheduler in a sliver window; header already
acknowledges). Fix: hold the mutex across `runOneRoundNow` (as `gcHealth` does) or make
the scheduler a `shared_ptr` snapshot; add a shutdown flag under the same lock rejecting
lazy creation; one lifecycle lock (or shared_ptr snapshots) for `cas_store`/`part_access`.
Relevant to the upcoming R6 TSan pass.

### 3.9 Decommission vs successor (№9) — CONFIRMED, REAL-FIX {#v9}
The slot deletes RE-READ tokens AFTER `admin.reset()` released the impersonated mount
lease (`CasDecommission.cpp:158-183`) — and the teardown farewell is precisely what makes
the slot instantly reclaimable by a returning victim. A successor mounting in the
reset→get gap gets its FRESH epoch/mount tokens read and exact-deleted; `DeleteOutcome` at
`:172` is discarded, so the report still says `slot_removed=1`. Damage: deleted live mount
lease (recoverable via self-remount), deleted owner anchor (fail-closed `CORRUPTED_DATA`
on next mount → ca-fsck), and — the serious one — deleted durable-monotone epoch counter:
a later incarnation re-mints `writer_epoch` from scratch, violating epoch monotonicity
(the invariant that keeps stale prior-incarnation state untrusted). Fix: inspect every
`DeleteOutcome` (abort the tail on non-Deleted), and fence the tail deletes to the exact
terminated objects decommission itself authored/observed (farewell mount token; epoch
value observed under the claim; re-verify the mount token immediately before the owner
delete) so any successor reclaim fails the tail closed.

### 3.11 Namespace drop vs unregistered build (№11) — PARTIAL, REAL-DEFER {#v11}
The allocate/register window is real (`CasPool.cpp:772-777`; the drop sweep snapshots only
`inflight_builds`), but the sweep is a best-effort optimization, not the authority — the
append lane rejects `NamespaceBirth` on a non-Live namespace unless the GC cleanup marker
for that removal is published (`CasPartWriteTxn.cpp:846-866`), which requires the
namespace to be PHYSICALLY EMPTY. The only resurrection path needs the drop AND full GC
reclaim to complete while the build is parked pre-registration, after which the late build
legitimately passes the marker gate: worst case a reborn Live-but-ownerless EMPTY
ref-table — a small, non-self-healing METADATA leak (GC never sweeps Live namespaces).
The reviewer's atomic-registration fix would NOT close it (the same TOCTOU exists for
registered builds between the `cancelled` check and the append; cancellation is
deliberately retry-later). Disposition: LOW, BACKLOG entry with the verifier's remediation
options (GC backstop for empty ownerless Live namespaces, or a namespace generation in
the birth-time marker gate).

### 3.26 Verbatim append lost update (№26) — PARTIAL, REAL-DEFER + comment {#v26}
Mechanism real: the Append base is read once at buffer-open and frozen
(`ContentAddressedTransaction.cpp:695-702`), and the CAS loop in
`CasPlainObjects.cpp:26-40` re-reads the TOKEN on conflict but retries with the frozen
payload — the exact fresh-token/stale-payload lost-update shape. Reachability REFUTED
today: the only production appender is the mutation-entry CSN write
(`MergeTreeMutationEntry::writeCSN`) — one append, per-mutation-unique key, under the
per-table single-writer lease; no second appender exists to lose. Disposition: document
the single-appender invariant at the `casPutObject` boundary (comment) + BACKLOG note to
implement `casAppendObject` (re-read base inside the loop) before any future concurrent
appender appears.

### 3.20 Decoder laxity (№20) — SPLIT {#v20}
(a) Missing identity fields: PARTIAL — `decodeMountLease`/`decodeGcHeartbeat` indeed
require NO field (asymmetric with `decodeOwner`/`decodeServerEpoch`/`decodeGcState`, which
throw on missing identity), BUT every safety-critical consumer fails closed on a zeroed
identity: mount adopt compares uuid/epoch and throws; heartbeat fencing is
token-STABILITY-based; the GC lease steal needs a DOUBLE frozen signal (lease tuple AND hb
pair) across a full observation window. Hygiene fix (worth taking): require identity
fields in both decoders, matching the siblings — a truncated control object should be
CORRUPTED_DATA, not a zeroed struct.
(b) Duplicate keys: REFUTED outright — `JsonObjectReader::nextKey`
(`CasTextFormat.cpp:162-164`) throws `CORRUPTED_DATA` on any repeated key in BOTH
strictness modes. No last-wins path exists.
(c) Fold seal unbound to its key: CONFIRMED gap — `readFoldSeal` (`CasGc.cpp:1692`) never
checks the decoded `generation`/`parent_generation` against the requested key, unlike
`decodeRefTableSnapshot`'s ns/id cross-check. A misplaced seal would corrupt the fold
CURSOR (`last_folded_ref_id`): re-fold = over-pin/leak, skip = premature condemn = data
loss. No writer path misplaces one today (deterministic key, single writer), so
exploitability is corruption-only — but the fix is cheap and mirrors the snapshot decoder:
pass expected generation into `decodeFoldSeal`, throw on mismatch. REAL-FIX.

### 3.22 Zero-valued GC config (№22) — CONFIRMED (all three), REAL-FIX {#v22}
(a) No bounds validation at the factory: `gc_interval_sec` (default 60) and `gc_shards`
(default 1) flow unchecked into `PoolConfig` (`MetadataStorageFactory.cpp:247,272`).
(b) `gc_interval_sec=0` → `wake.wait_for(lock, 0)` returns immediately → GC rounds
back-to-back at max rate (the HEARTBEAT interval is floored at 50ms, the round loop is
not — `CasGcScheduler.cpp:40,207`).
(c) `gc_shards=0` → first-ever lease acquire encodes `gcs=0` (the `chassert` at
`CasGcStateFormat.cpp:22` is release-inert) → every later `decodeGcState` throws
`gc_shards must be >= 1` → `gc/state` PERMANENTLY unreadable → GC wedged until rebuild;
plus `% gc_shards` mod-by-zero hazard in the free shard functions (`CasGcShardPlan`).
Fix: validate both bounds in the factory (throw `BAD_ARGUMENTS`, fail closed before pool
open) + replace the write-site `chassert` with a runtime exception. A plain-XML config
value reaching a permanent GC wedge is exactly the fail-open class we eliminate.

### 3.18 Emulated tokens (№18, №19 + the new 19c) — CONFIRMED family, one root fix {#v18}
№18 CONFIRMED: emu `get`/`head`/mutations mint `Token{seq, Emulated}`, while `list`
surfaces `tokenForList(etag) = Token{mtime_ns, ETag}` — `Token::operator==` compares type
AND value, so a list-derived token can NEVER satisfy an emulated expectation. Local
storage populates the etag, so the head-fallback never fires. Consumers of listed tokens
(GC namespace-cleanup deletes, `deletePrefixWholesale` for generation prune/hand-off,
orphan sweep, decommission drain) therefore always get `TokenMismatch`: a fail-SAFE leak,
never a wrong delete — and `deletePrefixWholesale` counts `deleted` unconditionally, so
the accounting reports phantom success while gc-gen objects leak. Blob content
reclamation is HEAD-token-based and unaffected (why local soaks still reclaim).
№19 CONFIRMED-as-coded (value-only on the wire), harm LOW: a wrong-type token also needs a
value collision across non-overlapping dialect value spaces on a fixed-dialect pool.
Hardening: reject mismatched `Token.type` locally before any conditional op.
**19c (out-of-review, found while verifying): CONFIRMED latent DATA LOSS in local-CA
across restart.** `emu_seq` is a plain in-process counter initialized to 0 — NOT persisted
and NOT seeded from the file etag, although both the header and inline comments CLAIM
etag-seeding (doc/code drift is the root cause). GC persists condemn tokens (type+value)
in retired runs/fold seals and replays them for the redelete after restarts; after a
restart the counter re-mints small values, so a persisted `Token{"5", Emulated}` can
textually collide with a resurrected live incarnation's fresh token for the SAME key →
`deleteExact` matches → wrong delete of a referenced body. The exact-token resurrection
protection (fresh token defeats stale deletes) holds on native ETag stores but is BROKEN
in emu-across-restart. Local-CA is a supported deployment that restarts and runs GC.
Root fix (closes №18 + 19c together, and matches what the comments already claim):
seed emu tokens from the local etag (mtime-ns) instead of a counter — a resurrected body
has a newer mtime → stale persisted delete-tokens miss; list/head token values then unify.

## 6. Final synthesis {#synthesis}

All 14 verifier reports in; scoreboard against the reviewer's 31: 1 blocker → downgraded
to real-but-narrow; of 16 high: 5 confirmed (№4,5,6,7,9), 3 partial, 3 refuted at harm
level, 5 design-decisions; of 14 major: most confirmed as small contract/hygiene fixes,
2 refuted (№20b, №27), 2 deferred (№11, №26). One NEW finding (19c) emerged from
verification and outranks most of the review.

FIX WAVE — proposed order (all fail-closed, mostly small):
1. **№4** condemn-marker load-bearing (GC deletes vs same-token adopt; rebuild markers) —
   the one reachable shared-pool data-loss class; medium fix, TLA+ touch likely.
2. **19c** emu token etag-seeding (latent local-CA data loss across restart) — small fix,
   also collapses №18.
3. **№5** parent∪proposed prune union (GC permanent wedge) — ~2 lines.
4. **№22** GC config bounds (XML → permanent wedge) — small.
5. **№9** decommission tail fencing + DeleteOutcome checks — small/medium.
6. **№7** pool-UUID recheck after reservation → byte-fallback — small.
7. **№16** `file_view` in the executor fallback condition — 1 line.
8. **№1** no unconditional fallback when `if_none_match` set (+ multipart probe) — small.
9. **№8** scheduler UAF lock scope + lifecycle flag — small; pairs with **№10** sink
   install-before-open (both TSan-relevant, do before R6).
10. Contract/hygiene batch: №12-narrow (removeDirectory drops staged entries), №13
    (noexcept post-durable telemetry), №20a (decoder identity fields), №20c (fold-seal
    key binding), №21 (readU32), №23 (truncate throw), №24 (if_exists), №25 (terminal
    failed txn), №28 (build-seq scope guard), №19 (token-type local check).
DEFER (BACKLOG): №6 handoff pin (spec exists — schedule as its own task), №11 (GC
backstop for empty ownerless Live namespaces), №26 (`casAppendObject`).
COMMENT WAVE: №2 (resurrectStaged), №3 (get HEAD/GET ordering), №12 (call-time
durability contract), №26/№27 (single-appender/single-writer invariants).
DESIGN DECISIONS awaiting the user: §4 (№14, №15, №17, №29, №30, №31).

## 4. Design decisions — for the user {#design}

### 4.14 Blob-hash default `CityHash128` (№14) {#f14}
The reviewer wants a cryptographic default or mandatory byte-verify for shared pools. The
project position (2026-07-14, hash-equality adversary model): re-hashing is the identity
primitive, no skip-read shortcuts exist, and `blob_hash='sha256'` is fully supported and
pool-authoritative for deployments whose threat model includes hostile writers sharing a
pool. `CityHash128` stays the default because same-pool writers are same-trust-domain
replicas (exactly like today's `ReplicatedMergeTree` interserver trust), and a chosen-
collision attacker inside that trust domain already has direct write access to every
object. RECOMMENDATION: keep the default; add the threat-model paragraph as a comment at
the `blob_hash` config parse site + a sentence in the CAS doc set. Decision: keep default
/ flip default to sha256 / force sha256 when pool is multi-writer?

### 4.15 Shared local pool only INFO (№15) {#f15}
The INFO (not WARNING) level is itself deliberate and documented in place
(`ContentAddressedMetadataStorage.cpp:400`): WARNING would be forwarded to clients at
stateless-test default `send_logs_level=warning` and fail every CA-over-local test. The
reviewer wants fail-closed startup unless single-server ownership is provable — but
single-server ownership of a local path is NOT provable from inside the process, so the
only honest fail-close is "refuse local mode entirely unless an unsafe-mode flag is set",
which breaks the entire local-CA lane (tests, dev). RECOMMENDATION: keep as-is for the
pre-release fork; revisit with a `single_server=true` attestation config if/when local
pools become a supported production topology. Decision needed.

### 4.17 Unknown GCS bucket-versioning proceeds (№17) {#f17}
Not silent — an explicit WARNING with operator guidance, and the code comment records the
trade-off (a mount refusal on an unverifiable check, e.g. missing `GetBucketVersioning`
permission, is too aggressive; confirmed-Enabled DOES fail). The damage on a wrong guess
is unreclaimed storage (cost), not data loss. RECOMMENDATION: keep, optionally add
`strict_versioning_check=true` opt-in. Decision needed.

### 4.29 No experimental gate on `metadata_type=content_addressed` (№29) {#f29}
True by construction — the fork never added one. For upstreaming or any user-facing
release an `allow_experimental_*`-style gate (default off) + `SettingsChangesHistory`
entry is standard and cheap. For the fork's own soak/CI lanes the gate is friction.
RECOMMENDATION: add the gate when the upstream PR is prepared, not now. Decision needed.

### 4.30 `system.content_addressed_mounts` partial results (№30) {#f30}
Confirmed: per-disk failures `continue` (`StorageSystemContentAddressedMounts.cpp:120`) —
"disk not started yet" is legitimately not an error, but a listing failure on a STARTED
disk currently yields silent omission, which during an incident reads as "no mounts".
RECOMMENDATION (small, worth taking): keep the not-started skip, add an
error/status column (or at least a row with the exception message) for started disks whose
listing fails. Decision: take into the fix wave?

### 4.31 `staging_backend=s3` silent fallback to local (№31) {#f31}
Confirmed: a failed conditional-copy probe logs INFO and continues with local staging
(`ContentAddressedMetadataStorage.cpp:485-500`). The code frames this as fail-close
("local staging remains fully functional") — but the user EXPLICITLY opted into S3
staging, and the silent downgrade can unexpectedly consume local disk (the reviewer's
point) and hides a capability misconfiguration. This contradicts the explicit-config
faithfulness we enforce elsewhere. RECOMMENDATION: fail the mount when an EXPLICIT
`staging_backend=s3` cannot be honored (keep auto-fallback only if we ever add
`staging_backend=auto`). Decision needed.

## 5. Comment-only actions {#comments}

For every BY-DESIGN/FALSE-POSITIVE verdict the fix wave adds a short in-place comment
stating the invariant and why the apparent issue is intended — the goal is that the next
external review stops re-reporting them. Sites (so far): `removeDirectory`/`moveDirectory`
call-time durability (№12), `moveFile` re-drive branch (№27, already documented — extend
one line), plus whatever the verifier reports refute in §3.
