#pragma once

#include <filesystem>

#if defined(OS_LINUX)
/// I think it is possible to mount the cgroups hierarchy somewhere else (e.g. when in containers).
/// /sys/fs/cgroup was still symlinked to the actual mount in the cases that I have seen.
static inline const std::filesystem::path default_cgroups_mount = "/sys/fs/cgroup";
#endif

/// Is cgroups v2 enabled on the system?
bool cgroupsV2Enabled();

/// Is the memory controller of cgroups v2 enabled on the system?
/// Assumes that cgroupsV2Enabled() is enabled.
bool cgroupsV2MemoryControllerEnabled();

/// Detects which cgroup the process belong and returns the path to it in sysfs (for cgroups v2).
/// Returns an empty path if the cgroup cannot be determined.
/// Assumes that cgroupsV2Enabled() is enabled.
std::filesystem::path cgroupV2PathOfProcess();
