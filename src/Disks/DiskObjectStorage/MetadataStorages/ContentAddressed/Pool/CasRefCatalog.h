#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCatalogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.h>
#include <functional>
#include <optional>

namespace DB::Cas
{

/// The `cas/ref_catalog` object (spec INV-3) as seen from the pool side: reading the current
/// catalog, and the generic token-CAS retry primitive every lifecycle transition rides. This class
/// builds ONLY that primitive -- the actual lifecycle steps (the three-conditional-write creation
/// sequence, the removal terminal-record-then-entry-delete sequence) are later tasks' job, built ON
/// TOP of `casUpdate`/`casAdmitEntry`.
class CasRefCatalog
{
public:
    /// The catalog snapshot as read from the backend: the decoded object plus the token an update
    /// must present to `casPut`. `token == std::nullopt` means the object is ABSENT -- a pool that
    /// has never admitted a namespace has never had a durable `cas/ref_catalog`, mirroring the
    /// bootstrap contract of every other token-CAS singleton (`_pool_meta`, `gc/state`).
    struct Snapshot
    {
        RefCatalog catalog;
        std::optional<Token> token;
    };

    /// Reads and decodes the current catalog. Absent key -> an empty catalog with `token = nullopt`.
    static Snapshot read(Backend & backend, const Layout & layout);

    /// Stage B (Task 4-C): the REAL catalog life for `ns` if a `Live`/`Removing` entry names it, else
    /// the Stage-A sentinel (`NamespaceLifeId::stageATransition`). For non-production discovery-path
    /// readers -- `recoverRefTableDetailed`, fsck's stream/oracle walk, `CasOrphanManifestSweep`'s
    /// active-key set -- which must find whatever a mounted writer actually wrote (a catalog-minted
    /// incarnation, since Task 4-C's production birth wiring) while staying correct for the raw-fixture
    /// tests that seed ref-log content directly and never touch the catalog at all: for THOSE
    /// namespaces the sentinel fallback is not a guess, it is the only other identity the fixture could
    /// have keyed its objects at (`cas_test_helpers.h`'s `casAdmitEntry` pins the same constant when it
    /// does admit one). `Creating` is excluded exactly as `Gc::discoverUniverse` excludes it: no
    /// publication can exist under an entry still being created, so there is nothing to resolve to.
    ///
    /// NOT for the mounted writer's own open path -- that is `CasRefLedger::resolveNamespaceLife`, which
    /// MINTS a life when none exists rather than falling back to a shared sentinel (a production writer
    /// must never key a genuine first birth at a namespace-independent constant).
    static NamespaceLifeId resolveLifeOrSentinel(Backend & backend, const Layout & layout, const RootNamespace & ns);

    /// The generic token-CAS retry loop shared by every catalog mutation, mirroring
    /// `PoolMeta::admitOrValidate`'s loop: read the current snapshot, apply `mutate` to obtain the
    /// CANDIDATE next catalog, `casPut` it against the observed token (`std::nullopt` create-if-absent
    /// when the object does not exist yet), and on `Conflict` re-read and re-apply `mutate` to the
    /// FRESH snapshot -- never re-encoding the stale candidate. `mutate` must return a canonically
    /// ordered, grammar-valid candidate; `encodeRefCatalog` (called internally) enforces that.
    ///
    /// Bounded (the same live-lock brake `publishCkpt`/`allocateWriterEpoch` use on their own
    /// contended token-CAS singletons): after 100 conflicting attempts it gives up and raises the
    /// typed retryable error `throwCasWriteRetryLater`, naming the key and the attempt count, rather
    /// than spinning forever against a pathologically busy catalog.
    ///
    /// A re-read that finds the object genuinely ABSENT after it was previously observed present is
    /// NOT treated as a fresh bootstrap: `Backend::get` returns `nullopt` only for authoritative
    /// absence, so an existing-then-vanished catalog is a real concurrent delete (or a lying store),
    /// and silently falling back to an empty catalog would let the next attempt's create-if-absent
    /// `casPut` replace EVERY other namespace's entry with whatever this one mutation produced. This
    /// raises `LOGICAL_ERROR` instead, mirroring `PoolMeta::admitOrValidate`'s identical fail-closed
    /// reaction to the same observation.
    ///
    /// This primitive runs NO admission check: Constraint 13 (removal is never refused) means
    /// whether a candidate must clear the additive predicate is the CALLER's decision, not this
    /// loop's. A caller mutating an entry's state without growing the catalog (a removal transition)
    /// uses this directly.
    ///
    /// THE FENCE OBLIGATION (Task 3 carry-over from the Task 2 review): this loop has no fence
    /// parameter and performs no fence check of its own -- `publishCkpt`'s "AFTER the read, BEFORE the
    /// CAS, on every attempt" discipline has no equivalent built in here. The seam a fenced caller
    /// MUST use is `mutate` itself: it runs, fresh, after EVERY read this loop performs (the very
    /// first one and every one after a `Conflict`), immediately before the candidate it returns is
    /// encoded and `casPut`. A caller that needs its own write fenced (any catalog mutation minted
    /// under a mount incarnation -- which is every one Task 3 onward adds) MUST throw from inside
    /// `mutate`, checking on EVERY invocation, not once before calling `casUpdate`: checking once
    /// before the call fences against the read this loop is *about* to perform, not the one it just
    /// did, and a `Conflict` retry performs an entirely new read `mutate` is never told about except
    /// by being called again. `completeCreation`'s own `mutate` (below) is the first production
    /// caller to ride this seam, and does so by wrapping its `check_fence_or_throw` call at the top of
    /// its `mutate`, exactly where `publishCkpt` places the identical check.
    static RefCatalog casUpdate(
        Backend & backend, const Layout & layout,
        const std::function<RefCatalog(const RefCatalog &)> & mutate);

    /// Admits exactly ONE new namespace into the catalog under INV-3's two-predicate gate, inserting
    /// `entry` at its canonical (ns-sorted) position and running the SAME bounded `casUpdate` retry
    /// loop. Takes the entry to insert rather than an arbitrary mutation, by design: an admission
    /// entry point that accepted a free-form candidate could be handed a REMOVAL by a future caller
    /// that reads as correct, silently reopening Constraint 13 (removal is never refused) behind a
    /// name that says "admitting". A namespace `entry.ns` already carries an entry is a bug in the
    /// caller (Task 3's creation lifecycle owns checking that first) and surfaces as
    /// `encodeRefCatalog`'s own canonical-order/no-duplicate grammar check, inside
    /// `checkCatalogAdmission`.
    static RefCatalog casAdmitEntry(Backend & backend, const Layout & layout, const CatalogEntry & entry);

    /// === Task 3: the §3 creation lifecycle, built on the two primitives above ===

    /// Outcome of the two-step tail every creation attempt ends in (`_ckpt` publish + `Creating ->
    /// Live` CAS) -- shared by a fresh `createNamespace` and a reconciler that just adopted a stalled
    /// entry via `reconcileStaleCreator`, since both resume from the identical point (an OBSERVED
    /// `Creating` entry with a live creator identity that is now THIS caller's own).
    enum class NamespaceCreationOutcome : uint8_t
    {
        Live,        /// the entry reached `Live`; `_ckpt` is durable with this creator's `writer_epoch`
                      /// as `life_epoch` (spec INV-4: the genesis epoch, recorded nowhere else).
        FencedOut,   /// this caller's OWN admitted generation moved before the `_ckpt` publish or the
                      /// `Creating -> Live` CAS. Nothing more was written; the caller's own mount
                      /// incarnation is gone, so it cannot be the one to retry.
        Superseded,  /// the catalog entry no longer equals what this caller observed -- a concurrent
                      /// reconciler stole it, or a race already carried it to `Live`/`Removing`. Nothing
                      /// was written; a DIFFERENT actor now owns whatever happens to this namespace next.
    };

    /// Outcome of `reconcileStaleCreator` alone (see below) -- kept distinct from
    /// `NamespaceCreationOutcome` because the two refusal reasons here are not interchangeable with
    /// "fenced" / "superseded": one is "not yet permitted" (retry later, unconditionally on the SAME
    /// entry), the other is "someone else already moved this entry" (retrying against the SAME
    /// `observed` value can never succeed; the caller must re-read first).
    enum class ReconcileCreatorOutcome : uint8_t
    {
        Reconciled,          /// `creator` is now `new_creator`; the caller may proceed as if it had
                              /// just run step 1 itself, over the SAME (unchanged) incarnation.
        CreatorFenceStillLive,  /// `is_creator_fence_terminal` said no -- the stalled creator might
                              /// still complete this itself. Not written; retry later against a FRESH
                              /// terminality read, not immediately.
        EntryChanged,         /// the catalog's current entry for `observed.ns` no longer equals
                              /// `observed` -- token-exactness failed. Not written; the caller must
                              /// re-read the catalog before trying again.
    };

    /// The full, fresh §3 sequence for a namespace that carries NO catalog entry yet: mints a random
    /// nonzero incarnation (spec: "fresh_random_128"), runs step 1 (`casAdmitEntry` inserting `{ns,
    /// Creating, incarnation, creator}`), then steps 2+3 via `completeCreation` below.
    ///
    /// Per the Task 2 review's own note on `casAdmitEntry` ("a namespace `entry.ns` already carries an
    /// entry is a bug in the caller -- Task 3's creation lifecycle owns checking that first"): this
    /// function reads the catalog FIRST and throws `LOGICAL_ERROR` naming the existing entry's state if
    /// `ns` already has one, rather than handing `casAdmitEntry` a doomed insert and letting its own
    /// grammar check report a confusing duplicate-namespace message. A namespace already `Creating` is
    /// not this function's problem to solve -- that is exactly what `reconcileStaleCreator` +
    /// `completeCreation` are for; a namespace already `Live`/`Removing` is a caller bug (recreating an
    /// existing name is Task 5's -- removal's -- business, not creation's).
    static NamespaceCreationOutcome createNamespace(
        Backend & backend, const Layout & layout, const RootNamespace & ns, const CreatorFence & creator,
        uint64_t admitted_generation, const std::function<void(uint64_t)> & check_fence_or_throw,
        const CkptDeadline & deadline);

    /// Steps 2 (`_ckpt` publish) + 3 (`Creating -> Live` CAS) alone, given an entry the caller already
    /// owns as `observed` -- either the entry `createNamespace`'s own step 1 just inserted, or one a
    /// caller just reconciled onto itself via `reconcileStaleCreator`. Exposed separately (rather than
    /// folded invisibly into `createNamespace`) because reconciliation resumes exactly HERE, never
    /// re-running step 1.
    ///
    /// Step 2: `publishCkpt` with a contribution carrying `observed.creator->writer_epoch` as
    /// `life_epoch` -- INV-4's genesis record; `observed.creator` must be present (i.e. `observed.state
    /// == Creating`), enforced with `LOGICAL_ERROR` since a caller reaching here with anything else is
    /// this module's own bug, not a race. A `FencedOut` from `publishCkpt` ends the attempt here.
    ///
    /// Step 3: `CasRefCatalog::casUpdate`'s `mutate` is the fence re-check point (see the class-level
    /// note below) -- `check_fence_or_throw(admitted_generation)` runs FIRST, on every fresh read this
    /// retry loop performs, exactly like `publishCkpt`'s own re-check; a throw from it is caught and
    /// reported as `FencedOut`, nothing else. ONLY THEN is the fresh entry for `observed.ns` compared
    /// against `observed` by FULL VALUE equality (`CatalogEntry::operator==`) -- the value-CAS that
    /// plays the role `publishCkpt`'s object token plays for `_ckpt`, since one catalog object holds
    /// every namespace's entry and there is no separate per-entry token to CAS against. A mismatch
    /// (stolen by a concurrent reconciler, or already carried to `Live`/`Removing`) is `Superseded`,
    /// caught before any CAS is attempted -- not a retry against fresh state, because retrying here
    /// would mean re-deciding against an entry that is no longer `observed`, which is precisely what
    /// token-exactness forbids. Ordering the fence check before the entry check is deliberate (mirrors
    /// `publishCkpt`); a caller that manages to make BOTH stale sees `FencedOut`, not `Superseded` --
    /// both are truthful refusals of a CAS that was never sent.
    static NamespaceCreationOutcome completeCreation(
        Backend & backend, const Layout & layout, const CatalogEntry & observed,
        uint64_t admitted_generation, const std::function<void(uint64_t)> & check_fence_or_throw,
        const CkptDeadline & deadline);

    /// Stale-`Creating` reconciliation (spec INV-3: "stalled creators occupy entries until
    /// fence-terminal reconciliation"; TLA Task 3 obligation 1: "the call-site is where
    /// token-exactness is enforced"). `observed` must be a `Creating` entry this caller read a moment
    /// ago (`LOGICAL_ERROR` otherwise -- a caller mistake, not a race). Refuses, WITHOUT writing
    /// anything, unless BOTH hold against a FRESH catalog read:
    ///   - `is_creator_fence_terminal(*observed.creator)` -- injected rather than reaching into
    ///     `CasServerRoot` directly, so this module (and its tests) stay independent of the mount-lease
    ///     machinery; the real predicate a production caller wires in is `isCreatorFenceTerminal`
    ///     (`Pool/CasServerRoot.h`, called as `isCreatorFenceTerminal(backend, layout,
    ///     fence.server_root_id, fence.writer_epoch)` -- it takes those two scalars, not the whole
    ///     `CreatorFence`, so the mount layer stays independent of the ref-catalog format), built from
    ///     `writer_epoch` plus the mount-terminality certificates
    ///     `probeNonTerminalMountSlots`/`computeHeartbeatFloor` already use -- NEVER from
    ///     `CreatorFence::fence_generation`. That field IS persisted (Task 2 serializes it into the
    ///     catalog entry), so it reaches the object store fine; what it is NOT is comparable across
    ///     actors: it mirrors `CasMountRuntime::fence_generation`, an in-process atomic that each mount
    ///     bumps from its OWN zero on every open, so a different actor's counter (or the SAME actor's
    ///     after a restart) starts over at the same values and answers a different question than "is
    ///     the incarnation that minted this entry still alive";
    ///   - the catalog's CURRENT entry for `observed.ns` still equals `observed` exactly
    ///     (token-exactness: a concurrent reconciler, or the original creator finishing on its own,
    ///     invalidates this immediately).
    /// On success, CASes `creator` to `new_creator` -- `state` and `incarnation` are UNCHANGED, so the
    /// caller resumes with `completeCreation(backend, layout, {..., .creator = new_creator}, ...)` over
    /// the SAME incarnation, never a fresh one (rebirth under a fresh incarnation is Task 5/removal's
    /// business, not a live reconciliation's).
    static ReconcileCreatorOutcome reconcileStaleCreator(
        Backend & backend, const Layout & layout, const CatalogEntry & observed,
        const CreatorFence & new_creator,
        const std::function<bool(const CreatorFence &)> & is_creator_fence_terminal);

    /// Spec §3: "`Creating` forbids publication -- no ref writes admitted while the entry is
    /// Creating." Throws `throwCasWriteRetryLater`'s class (transient: `Creating` resolves once the
    /// creator finishes or is reconciled away) if `catalog`'s entry for `ns` is `Creating`; a no-op for
    /// every other case -- no entry, `Live`, or `Removing` -- since this is ONLY the birth-lifecycle
    /// gate on the catalog's own `Creating` state, never a general existence/removal check (that role
    /// moves onto the catalog in Task 4/Task 6). Takes an already-read `RefCatalog` rather than
    /// `Backend`/`Layout`, so a caller that is about to append anyway (and so already holds a fresh
    /// read for its OWN purposes) pays no second GET here.
    ///
    /// NOT YET ENFORCED ON THE PRODUCTION REF-WRITE PATH -- state this plainly rather than let the
    /// spec sentence above read as a claim the tree already satisfies. `CasRefLedger::commitRefChunk`
    /// (today's `RefOpKind::NamespaceBirth` writer) does not call this function and consults no catalog
    /// state before a ref-log `PUT`: adding a per-write catalog GET to that path would add a protocol
    /// step to ref writes, which the standing veto on protocol-step additions forbids regardless of
    /// cost. The refusal therefore has to ride on wherever existence/discovery lives, which is Task 4's
    /// catalog-backed universe (plan `0cf11354aa0`, "Task 4 owns closing the Creating-forbids-publication
    /// gap on the production path" -- a task step, not a citation). Until that lands, a namespace sitting
    /// in `Creating` does not actually block a concurrent production ref write to the same name; this
    /// function is exercised today only by this module's own tests and by any FUTURE caller Task 4 adds.
    static void checkPublicationAdmittedOrThrow(const RefCatalog & catalog, const RootNamespace & ns);
};

}
