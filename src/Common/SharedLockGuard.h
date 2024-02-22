// SharedLockGuard was introduced to master in https://github.com/ClickHouse/ClickHouse/pull/55278
// since that PR also brings other features, we do not cherry-pick it directly but rather cut out only required part.
// https://github.com/ClickHouse/ClickHouse/blob/04272fa481353c6c197a79c13656af9ed210e163/src/Common/SharedLockGuard.h
// (master on 25-11-2023)

#pragma once

#include <base/defines.h>

namespace DB
{

/** SharedLockGuard provide RAII-style locking mechanism for acquiring shared ownership of the implementation
  * of the SharedLockable concept (for example std::shared_mutex or ContextSharedMutex) supplied as the
  * constructor argument. Think of it as std::lock_guard which locks shared.
  *
  * On construction it acquires shared ownership using `lock_shared` method.
  * On destruction shared ownership is released using `unlock_shared` method.
  */
template <typename Mutex>
class TSA_SCOPED_LOCKABLE SharedLockGuard
{
public:
    explicit SharedLockGuard(Mutex & mutex_) TSA_ACQUIRE_SHARED(mutex_) : mutex(mutex_) { mutex_.lock_shared(); }

    ~SharedLockGuard() TSA_RELEASE() { mutex.unlock_shared(); }

private:
    Mutex & mutex;
};

}
