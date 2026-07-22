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
/// in `programs/server/Server.cpp`). A caller that never wires the explicit initialize call (unit
/// tests, `clickhouse-local`-style tools) still gets a working pool: the first `getCasCommitThreadPool()`
/// call self-initializes with the compiled-in default size, exactly like `StaticThreadPool::
/// initializeWithDefaultSettingsIfNotInitialized()` covers callers of `getIOThreadPool()` that skip the
/// explicit server-startup `initialize()`.
ThreadPool & getCasCommitThreadPool();

/// Called once from server startup to size the pool from `cas_commit_pool_size`. A no-op (never
/// re-sizes, never throws) if the pool was already constructed by an earlier call -- either an earlier
/// call to this function, or a lazy self-initialization triggered by `getCasCommitThreadPool()` itself.
void initializeCasCommitThreadPool(size_t max_threads, size_t max_free_threads, size_t queue_size);

}
