#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasSingleWriterSlot.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

SingleWriterSlot::SingleWriterSlot(
    BackendPtr backend_, String key_, std::string_view slot_name_, std::string_view terminal_verb_,
    std::string_view logger_name_)
    : backend(std::move(backend_))
    , key(std::move(key_))
    , slot_name(slot_name_)
    , terminal_verb(terminal_verb_)
    , log(getLogger(String(logger_name_)))
{
}

SingleWriterSlot::~SingleWriterSlot()
{
    /// Stop the renewal thread only — deliberately NO terminal op. Destruction without a terminal op
    /// is the crash path: the slot object persists, its seq stops advancing, and full GC observes the
    /// frozen seq.
    stopBackground();
}

void SingleWriterSlot::recordWrite(uint64_t new_seq, const Token & token)
{
    seq = new_seq;
    last_token = token;
    last_renew_time = std::chrono::steady_clock::now();
}

void SingleWriterSlot::doStart()
{
    /// Compute the per-call payload BEFORE taking state_mutex: a subclass callback (the watermark's
    /// min_active hook) may reach into the Store's own lock, so we never hold state_mutex across it.
    const RenewPayload payload = prepareRenew();

    std::lock_guard lock(state_mutex);
    if (dead)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS {}: start after {} on key '{}'", slot_name, terminal_verb, key);
    if (seq != 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS {}: already started on key '{}'", slot_name, key);

    const String body = encodeBody(/*seq=*/1, payload);
    const Token token = claim(body);
    recordWrite(/*new_seq=*/1, token);
}

void SingleWriterSlot::renewOnce()
{
    /// Compute the per-call payload BEFORE taking state_mutex (see doStart): never hold state_mutex
    /// across the subclass callback.
    const RenewPayload payload = prepareRenew();

    std::lock_guard lock(state_mutex);
    if (dead)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS {}: renew after {} on key '{}'", slot_name, terminal_verb, key);
    if (seq == 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS {}: renew before start on key '{}'", slot_name, key);

    const String body = encodeBody(seq + 1, payload);
    const PutResult res = backend->putOverwrite(key, body, last_token);
    if (res.outcome != PutOutcome::Done)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS {}: key '{}' was touched by a foreign writer — failing closed, never re-minting", slot_name, key);

    recordWrite(seq + 1, res.token);
}

void SingleWriterSlot::doTerminate()
{
    /// Join the renewal thread before taking the state lock, so no renewal races the terminal op.
    stopBackground();

    std::lock_guard lock(state_mutex);
    if (dead)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS {}: double {} on key '{}'", slot_name, terminal_verb, key);
    if (seq == 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS {}: {} before start on key '{}'", slot_name, terminal_verb, key);

    /// Dead regardless of what terminate does below: we attempted the terminal op, the keeper must
    /// never renew this key again.
    dead = true;
    terminate();
}

void SingleWriterSlot::startBackground(std::chrono::milliseconds period)
{
    /// After a thread-side renewal failure the loop returns (see backgroundLoop) but the thread
    /// handle stays joinable, so a subsequent startBackground throws "already running" until
    /// stopBackground is called. Intentional fail-closed: we never silently re-arm renewal after it
    /// has failed.
    std::lock_guard lock(background_mutex);
    if (thread.joinable())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS {}: background renewal is already running for key '{}'", slot_name, key);
    stop_requested = false;
    thread = ThreadFromGlobalPool([this, period] { backgroundLoop(period); });
}

void SingleWriterSlot::stopBackground()
{
    ThreadFromGlobalPool to_join;
    {
        std::lock_guard lock(background_mutex);
        if (!thread.joinable())
            return;
        stop_requested = true;
        wakeup.notify_all();
        to_join = std::move(thread);
    }
    to_join.join();
}

std::chrono::steady_clock::time_point SingleWriterSlot::lastRenewTime() const
{
    std::lock_guard lock(state_mutex);
    return last_renew_time;
}

void SingleWriterSlot::backgroundLoop(std::chrono::milliseconds period)
{
    /// A failed renewal is logged and stops the loop, so lastRenewTime (and the slot's seq) stop
    /// advancing and GC observes the frozen seq. No retry, no re-mint.
    std::unique_lock lock(background_mutex);
    while (!stop_requested)
    {
        if (wakeup.wait_for(lock, period, [this] { return stop_requested; }))
            break;

        lock.unlock();
        try
        {
            renewOnce();
        }
        catch (...)
        {
            tryLogCurrentException(
                log, fmt::format("CAS {}: background renewal failed, the {} stops advancing", slot_name, slot_name));
            return;
        }
        lock.lock();
    }
}

}
