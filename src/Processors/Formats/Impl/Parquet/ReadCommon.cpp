#include <Processors/Formats/Impl/Parquet/ReadCommon.h>

#include <Common/futex.h>
#include <Formats/FormatParserSharedResources.h>

namespace DB::Parquet
{

SharedResourcesExt::Limits SharedResourcesExt::getLimitsPerReader(const FormatParserSharedResources & parser_shared_resources, double fraction)
{
    const SharedResourcesExt & ext = *static_cast<const SharedResourcesExt *>(parser_shared_resources.opaque.get());
    size_t n = parser_shared_resources.num_streams.load(std::memory_order_relaxed);
    /// Up to `max_concurrent_readers_per_stream` readers can be alive per stream at once (e.g.
    /// `object_storage_max_files_to_prefetch` priming several files ahead). Without this factor,
    /// each of them would compute its budget as if it alone owned its stream's whole share,
    /// oversubscribing memory/parsing-thread budget by that factor once more than one is primed.
    n *= std::max<size_t>(parser_shared_resources.max_concurrent_readers_per_stream, 1);
    fraction /= static_cast<double>(std::max(n, size_t(1)));
    return Limits {
        .memory_low_watermark = size_t(ext.total_memory_low_watermark * fraction),
        .memory_high_watermark = size_t(ext.total_memory_high_watermark * fraction),
        .parsing_threads = size_t(std::max(std::lround(parser_shared_resources.parsing_runner.getMaxThreads() * fraction + .5), 1l))};
}

#ifdef OS_LINUX

bool CompletionNotification::check() const
{
    return val.load(std::memory_order_acquire) == NOTIFIED;
}

void CompletionNotification::wait()
{
    UInt32 n = val.load(std::memory_order_acquire);
    if (n == NOTIFIED)
        return; // fast path
    if (n == EMPTY)
    {
        if (!val.compare_exchange_strong(n, WAITING))
        {
            if (n == NOTIFIED)
                return;
            chassert(n == WAITING);
        }
    }
    while (true)
    {
        futexWait(&val, WAITING);
        n = val.load();
        if (n == NOTIFIED)
            return;
        chassert(n == WAITING);
    }
}

void CompletionNotification::notify()
{
    UInt32 n = val.exchange(NOTIFIED);
    /// If there were no wait() calls before the notify() call, avoid the syscall.
    if (n == WAITING)
        futexWake(&val, INT32_MAX);
}

#else

bool CompletionNotification::check() const
{
    return notified.load();
}

void CompletionNotification::wait()
{
    if (!check())
        future.wait();
}

void CompletionNotification::notify()
{
    if (!notified.exchange(true))
        promise.set_value();
}

#endif

}
