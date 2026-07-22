#pragma once
#include <Common/ThreadPool_fwd.h>
#include <cstddef>

namespace DB::Cas
{

/// Process-wide, dedicated thread pool for CAS part-commit work. Task 5 (the CAS parallel-write-path
/// plan, docs/superpowers/sdd) dispatches per-part commit jobs onto this pool; this task (Task 4) only
/// adds the pool itself -- `commit()` stays sequential until Task 5 wires it in.
///
/// Deliberately DISJOINT from `getIOThreadPool()` (`IO/SharedThreadPools.h`) and from
/// `Context::getThreadPoolWriter()` (the `REMOTE_FS_WRITE_THREAD_POOL` `WriteBufferFromS3` multiparts
/// onto): a commit worker blocks in `WriteBufferFromS3::finalize` waiting on the writer pool, so if the
/// worker thread itself were drawn from the writer pool, that wait would deadlock one level down the
/// moment the pool is saturated. This pool owns its own `ThreadPool` instance -- never aliasing either
/// of those two -- so a commit worker can safely block waiting on the writer pool without starving its
/// own pool's ability to make progress.
///
/// Sized from the server setting `cas_commit_pool_size` (default 100, `Core/ServerSettings.cpp`), wired
/// from server startup via `initializeCasCommitThreadPool` (mirrors `getIOThreadPool().initialize(...)`
/// in `programs/server/Server.cpp`). Throws `LOGICAL_ERROR` if called before `initializeCasCommitThreadPool`
/// has run -- mirrors `StaticThreadPool::get()` (`IO/SharedThreadPools.cpp`). There is deliberately no
/// lazy self-initializing fallback: a caller that reaches this before server startup wired the pool
/// (or a test that forgot to call `initializeCasCommitThreadPool` first) is a bug, and a fallback to a
/// hardcoded default would make `cas_commit_pool_size` a silent no-op.
ThreadPool & getCasCommitThreadPool();

/// Called once from server startup to size the pool from `cas_commit_pool_size`. Throws `LOGICAL_ERROR`
/// if called a second time -- mirrors `StaticThreadPool::initialize()` (`IO/SharedThreadPools.cpp`).
void initializeCasCommitThreadPool(size_t max_threads, size_t max_free_threads, size_t queue_size);

/// Called once from server teardown (right next to `StaticThreadPool::shutdownAll()`, before
/// `GlobalThreadPool::instance().shutdown()`) so any workers this pool spun up can't outlive the global
/// pool. Safe to call even if the pool was never initialized (e.g. a unit test process, or a server
/// that failed to start before `initializeCasCommitThreadPool` ran).
void shutdownCasCommitThreadPool();

}
