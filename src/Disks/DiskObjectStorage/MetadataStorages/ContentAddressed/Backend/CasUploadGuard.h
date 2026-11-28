#pragma once
#include <Common/IThrottler.h>
#include <base/types.h>

#include <functional>

namespace DB::Cas
{

/// A throttler for one request body that also runs `stop` around every `throttle` call, so the sender notices a
/// stopped owner at each send-budget refill. The amount and `max_block_ns` reach `bandwidth` unchanged.
/// Returns `bandwidth` itself, possibly null, when `stop` is empty. With a stop and no `bandwidth`, `throttle`
/// runs the stop and returns false, and the limits report as absent.
ThrottlerPtr makeUploadGuard(ThrottlerPtr bandwidth, std::function<void()> stop);

/// Binds one object key to the throttler of its body on this thread, for backend code that builds the write settings
/// of that body and has no parameter to receive it. Scopes nest: the innermost binding is the only one visible.
class BodyGuardScope
{
public:
    BodyGuardScope(String key, ThrottlerPtr throttler);
    ~BodyGuardScope();

    BodyGuardScope(const BodyGuardScope &) = delete;
    BodyGuardScope & operator=(const BodyGuardScope &) = delete;

    /// The throttler bound to exactly `key` on this thread, null for any other key or outside a scope.
    static ThrottlerPtr forKey(const String & key);

private:
    struct Binding
    {
        String key;
        ThrottlerPtr throttler;
    };

    Binding saved;
};

}
