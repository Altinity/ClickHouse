#pragma once
#include <base/types.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace DB::Cas
{

class Pool;

struct MigratePoolOptions
{
    /// Unset: the larger of 120 s and the disk's `cas_request_window_ms`, since a request a stopped member sent may land
    /// that late.
    std::optional<uint64_t> grace_ms;
    /// An intent only for a body a marker or a GC row condemns, not for every body. Ignored on a pool below
    /// generation 3, whose markers prove nothing.
    bool trust_clean_markers = false;
    bool dry_run = false;
    /// Stop once the floor is 4. The markers stay for a rerun or `cas-fsck --fix legacy-markers`.
    bool keep_markers = false;
    /// Requests in flight in M4, M5 and the marker removal; 0 takes the disk's `gc_io_concurrency`.
    size_t concurrency = 0;
    uint64_t poll_step_ms = 1'000;
    /// Test only: throws `ABORTED` after step N (1..9).
    std::optional<size_t> stop_after_step_for_test;
};

/// Bounds `--grace-seconds` so the conversion to milliseconds cannot wrap to a short grace.
constexpr uint64_t kMaxMigrateGraceSeconds = 7 * 24 * 3600;

/// `seconds` in milliseconds; above `kMaxMigrateGraceSeconds` it is refused with `BAD_ARGUMENTS`.
uint64_t migrateGraceMs(uint64_t seconds);

/// What each step of one run handled.
struct MigratePoolReport
{
    uint64_t floor_read = 0;
    uint64_t probes_run = 0;                /// M1
    uint64_t slots_read = 0;                /// M2
    uint64_t condemned_rows = 0;            /// M3
    uint64_t markers_read = 0;              /// M4
    uint64_t bodies_listed = 0;             /// M4
    uint64_t intents_confirmed = 0;         /// M4 and M5
    uint64_t control_objects_written = 0;   /// M6: the raise to floor 3 and the `gc/state` rewrite that landed
    uint64_t slots_reread = 0;              /// M7
    bool floor_raised = false;              /// M8 sent the write that raised the floor to 4
    uint64_t legacy_markers_deleted = 0;    /// M9
    uint64_t legacy_markers_changed = 0;    /// M9: replaced between their `HEAD` and the delete, and left
    bool migrated = false;                  /// the floor is 4, raised by this run or an earlier one
    bool completed = false;                 /// migrated, and the marker removal ran to its end
};

/// Converts a stopped pool from blob markers to deletion intents (`clickhouse-disks cas-migrate-pool`): steps M1 to
/// M8, each idempotent, so a run that stops anywhere is completed by a rerun with the same options. `pool` comes
/// from `Pool::openForMigration`. Every refusal throws and names what the operator must do. Nothing is deleted
/// before the floor is raised to 4. Then M9 removes the legacy markers, which no server of generation 4 reads, so
/// servers may start before it; a rerun on a migrated pool runs M9 only. `dry_run` reads and sends no write or
/// delete, at any floor. `progress` receives a line per step and one before each write that cannot be undone.
MigratePoolReport migratePool(Pool & pool, const MigratePoolOptions & options,
                              const std::function<void(std::string_view)> & progress);

/// The summary line of `cas-migrate-pool`: every count of `report` as `<field>=<value>`, in field order, without a
/// trailing newline.
String formatMigratePoolSummary(const MigratePoolReport & report);

}
