#include <Disks/tests/cas_killable_query.h>

#include <Common/Stopwatch.h>
#include <Interpreters/Context.h>

namespace DB::Cas::tests
{

CasKillableQuery::CasKillableQuery(ContextPtr global)
    : context(Context::createCopy(global))
{
    context->setCurrentQueryId("");
    group = std::make_shared<ThreadGroup>(context, 0);
    attachCurrentThread();
    /// The process list binds the query to the group of the inserting thread, so attach first.
    entry = context->getProcessList().insert(
        /*query_=*/"", /*normalized_query_hash=*/0, /*ast=*/nullptr, context,
        /*watch_start_nanoseconds=*/clock_gettime_ns(), /*is_internal=*/true);
    status = entry->getQueryStatus();
    context->setProcessListElement(status);
}

CasKillableQuery::~CasKillableQuery()
{
    /// `ThreadStatus` checks on destruction that an attached query's context is still alive.
    if (group && CurrentThread::getGroup() == group)
        CurrentThread::detachFromGroupIfNotDetached();
    context->setProcessListElement(nullptr);
    entry.reset();
}

void CasKillableQuery::attachCurrentThread()
{
    CurrentThread::attachToGroup(group);
}

void CasKillableQuery::kill(std::exception_ptr cause)
{
    status->cancelQuery(CancelReason::CANCELLED_BY_USER, cause);
}

}
