#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace DB::Cas
{

/// Thread-safe, token-enforcing in-memory `Backend` implementation used by CAS tests.
///
/// All successful writes mint a monotonically increasing token (`TokenType::Emulated`).
/// Tokens NEVER repeat across the lifetime of a backend instance.
///
/// The backend also exposes fault-injection controls for probe tests and CAS correctness tests:
///   - `setHoldDeletes` / `landPendingDelete`: simulate async/delayed conditional deletes
///   - `failNextCasPut`:                      inject a one-shot conflict
///   - `setEnforceTokens(false)`:             mimic a "dumb" backend that ignores token checks
///   - `setSimulateDeleteMarkers`:            mimic S3 versioning-enabled buckets
///
/// Not `final`: tests subclass it to distort single behaviors (e.g. clamp list page size to force
/// pagination) while delegating everything else to this base.
class InMemoryBackend : public Backend
{
public:
    InMemoryBackend() = default;

    /// Unhide the base overloads this class's own declarations would otherwise shadow: the legacy
    /// `head`/`list`/`getStream`/`putIfAbsent`/`casPut` names, and the omitted-`Range`/`ObjectMeta`
    /// conveniences.
    using Backend::casPut;
    using Backend::getStream;
    using Backend::head;
    using Backend::list;
    using Backend::putIfAbsent;

    // ---- Backend interface ----

    /// Returns the stored bytes and the key's current incarnation value, or `nullopt` when absent.
    std::optional<Raw> read(const String & key, TransportAccess & access) override;

    /// Returns the current size and incarnation value without materializing the body.
    std::optional<RawMeta> head(const String & key, TransportAccess & access) override;

    /// Lists up to `limit` keys under `prefix` in map order. `cursor` is the last key from the
    /// previous page; `next_cursor` is set only when more matching keys remain.
    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override;

    /// Removes exactly the incarnation named by `expected_value`, or queues that check for a later
    /// `landPendingDelete` when delete holding is enabled. A queued delete is reported as removed,
    /// but its expected value is rechecked when it is landed.
    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override;

    /// Creates the key when `expected_value` is empty, or replaces the incarnation it names. A
    /// refused precondition leaves the store unchanged. Value enforcement can be disabled with
    /// `setEnforceTokens` to model a backend that incorrectly ignores the condition.
    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override;

    /// A forward-only reader over a private copy of the bytes; null when the key is absent.
    std::unique_ptr<ReadBuffer> stream(const String & key, TransportAccess & access) override;

    /// Publishes either `[fresh_envelope][payload]` or the complete staged bytes as one atomic
    /// in-memory replacement. Streaming sources are fully validated before the destination changes.
    void publish(const BlobPublishRequest & request, TransportAccess & access) override;

    /// This backend mints its own emulated values.
    Dialect dialect() const override { return Dialect::Emulated; }

    /// The in-memory backend mints a monotonic value it surfaces through `list` — TRUE.
    bool supportsListTokens() const override { return true; }

    /// Whatever `setRefreshCredentialsResult` last configured; FALSE by default, so a test that has
    /// not opted in models a backend with no refresh mechanism.
    bool refreshCredentials() override;

    /// Returns a forward-only stream over the requested byte window, or `nullopt` when the key is absent.
    /// The in-memory implementation copies the window into an owning read buffer while holding the
    /// backend lock, so the returned stream remains independent of later backend mutations.
    std::optional<GetStreamResult> getStream(const String & key, Range range) override;

    /// ---- Two legacy verbs, overridden ONLY so each write knob keeps its verb identity ----
    ///
    /// A knob is armed against a VERB, but the keyed `write` cannot see which verb its caller used, so
    /// the base forwarder would let `failNextCasPut` fire on a `putIfAbsent` and
    /// `injectAmbiguousPutIfAbsent` on a create-shaped `casPut`. Each of these consumes only the knob
    /// named for it, and neither reaches the keyed primitive -- so, unlike every other legacy verb, an
    /// override of `write` in a SUBCLASS of this backend does not intercept these two. Deleted with the
    /// rest of the legacy surface at the lock, and the exception goes with them.
    PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta) override;
    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                     const ObjectMeta & meta) override;

    // ---- Fault-injection controls ----

    /// When true, `remove` validates and enqueues deletes rather than applying them immediately.
    /// The caller sees `Removed` (the send was accepted), but the object remains until
    /// `landPendingDelete`, where the expected value is checked again.
    void setHoldDeletes(bool hold);

    /// Returns the number of currently held deletes.
    size_t pendingDeletes() const;

    /// Applies and removes the held delete at index `i`. The expected value is evaluated against the
    /// current object at land time; the queue entry is removed whether the result is `Mismatch` or
    /// `Removed`. An invalid index returns `NotFound`.
    DeleteOutcome landPendingDelete(size_t i);

    /// Injects a one-shot artificial refusal on the next write of `key`, CREATING OR REPLACING. Both,
    /// because the store cannot tell a create-if-absent `casPut` from a `putIfAbsent` and the knob is
    /// armed against conditional writes of both shapes -- a GC lease acquire creates its object, and
    /// a test that arms this knob for it is testing exactly that create losing its condition.
    void failNextCasPut(const String & key);

    /// Injects a one-shot AMBIGUOUS outcome on the next CREATING write of `key` (a write with no
    /// expected value): instead of attempting it, that call throws a plain (non-`DB::Exception`)
    /// exception -- classified `Unresolved`, never `DefiniteFailure`, by
    /// `classifyConditionalWriteResult` regardless of build flags -- and the store is left exactly as
    /// it was. Models a request whose own HTTP attempt outcome is lost (a timeout, a dropped
    /// connection) rather than a clean refusal, for tests that must exercise the "ambiguous attempt,
    /// resolve before deciding" path without a live network. One-shot, mirroring `failNextCasPut`'s
    /// contract: consumed by the first matching write, whether the key was already present or not.
    void injectAmbiguousPutIfAbsent(const String & key);

    /// The other ambiguity, and the only one that can prove a resolve read settles a commit: the next
    /// write of `key` IS APPLIED and then throws a plain (non-`DB::Exception`) exception, so the object
    /// is durable and its incarnation was never returned. Consumed by the keyed `write` alone -- it
    /// names no legacy verb, so no legacy verb consumes it. One-shot.
    void injectAmbiguousLandedWrite(const String & key);

    /// Enables or disables value checks for remove and replace. Disabling checks models a backend
    /// that reports every expected value as matching.
    void setEnforceTokens(bool enforce);

    /// When true, a successful `remove` answers `DeleteMarker`, modelling a versioned S3 bucket whose
    /// delete creates a marker instead of reclaiming the current object.
    void setSimulateDeleteMarkers(bool simulate);

    /// What `refreshCredentials` answers: TRUE models a storage that installed fresh credentials.
    void setRefreshCredentialsResult(bool result);

    /// How many times `refreshCredentials` has been called on this backend.
    size_t refreshCredentialsCalls() const;

    /// The next `write` naming `key` throws `error` instead of applying it, and the store is left
    /// exactly as it was. Each arming is consumed by one write, so arming twice fails two consecutive
    /// attempts of the same call. An `exception_ptr` rather than a concrete type because a caller
    /// classifies a failed attempt by its exception CLASS, and the classes worth exercising span
    /// `S3Exception`, `DB::Exception`, `Poco::Exception` and plain `std::exception`.
    void failNextWriteWith(const String & key, std::exception_ptr error);
    /// The read-side siblings, for the read loop's own classification. `read` and `head` are armed
    /// separately because the two resolve loops differ in exactly which of them they issue: a
    /// presence-only caller must be able to fail its HEAD without a body read stealing the arming.
    void failNextReadWith(const String & key, std::exception_ptr error);
    void failNextHeadWith(const String & key, std::exception_ptr error);

    /// Runs before a write of `key` is applied, with no backend lock held -- so a hook may itself read
    /// and write this backend, which is what it exists for: a hook that replaces `key` models a
    /// permanently hot key whose incarnation moves under every attempt. A hook that writes the same
    /// key re-enters this callback, so a hook must guard its own recursion.
    void onBeforeWrite(const String & key, std::function<void()> hook);
    /// Runs after a write of `key` is durable and BEFORE its value is returned, with no backend lock
    /// held -- the point at which a fact outside the store can change while a write is in flight.
    void onWriteCommitted(const String & key, std::function<void()> hook);

private:
    /// Complete in-memory incarnation state for one key. All fields are read or modified while
    /// `mutex_` is held; replacing `token` marks a new incarnation even when the bytes are unchanged.
    struct Object
    {
        String bytes;
        Token token;
    };

    /// Value captured when a held delete is queued. It is intentionally checked again at land time so
    /// a replacement between send and land produces `Mismatch` rather than deleting the new object.
    struct PendingDelete
    {
        String key;
        Token token;
    };

    /// Mints the next process-local token. Tokens are strictly increasing and never reused by this
    /// backend instance, which also makes token equality a safe content-cache identity check in tests.
    Token mintToken();

    /// Applies an exact-value delete while `mutex_` is already held. Used by immediate deletes and by
    /// `landPendingDelete` after its queue entry has been removed.
    DeleteOutcome applyDelete(const String & key, const Token & token);

    using ArmedFailures = std::map<String, std::vector<std::exception_ptr>>;
    using Hooks = std::map<String, std::function<void()>>;

    /// Which of the verb-scoped write knobs one call may consume.
    enum class WriteKnobs : uint8_t
    {
        All,                    /// the keyed `write`: the one caller every knob is armed against
        AmbiguousPutIfAbsent,   /// legacy `putIfAbsent`
        FailNextCasPut,         /// legacy `casPut`, either form
    };

    /// Consumes and returns the next failure armed for `key`, or null when none is.
    std::exception_ptr takeArmedFailure(ArmedFailures & armed, const String & key);
    /// Consumes the landed-then-lost arming for `key`, if there is one.
    bool takeAmbiguousLandedWrite(const String & key);
    /// A copy of the hook registered for `key`, taken under the lock so the caller can run it without
    /// one.
    std::function<void()> hookFor(const Hooks & hooks, const String & key) const;
    /// One write, whichever verb asked for it: armed failure, hooks, the store mutation, and exactly
    /// the knobs `knobs` allows.
    std::expected<String, RawConflict> applyWrite(const String & key, const String & bytes,
                                                  const std::optional<String> & expected_value, WriteKnobs knobs);
    /// The part of `applyWrite` that touches the store, run with `mutex_` held.
    std::expected<String, RawConflict> writeUnderLock(const String & key, const String & bytes,
                                                      const std::optional<String> & expected_value, WriteKnobs knobs);

    mutable std::mutex mutex_;
    std::map<String, Object> store_;
    uint64_t token_seq_ = 0;

    // Fault-injection state. These fields are protected by `mutex_` just like `store_`.
    bool hold_deletes_ = false;
    std::vector<PendingDelete> pending_deletes_;
    std::set<String> fail_next_cas_;
    std::set<String> ambiguous_put_keys_;
    std::set<String> ambiguous_landed_keys_;
    bool enforce_tokens_ = true;
    bool simulate_delete_markers_ = false;
    bool refresh_credentials_result_ = false;
    size_t refresh_credentials_calls_ = 0;
    ArmedFailures write_failures_;
    ArmedFailures read_failures_;
    ArmedFailures head_failures_;
    Hooks before_write_hooks_;
    Hooks write_committed_hooks_;
};

}
