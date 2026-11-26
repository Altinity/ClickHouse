#pragma once

#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace DB::Cas::tests
{

/// Owns a test directory and removes it recursively when destroyed, but only in the process that
/// created it: a death-test child inherits a copy of every guard and must not delete what the parent
/// still uses. The removal reports no error, so a failed cleanup cannot mask the test's own result.
class ScopedTestDirectory
{
public:
    explicit ScopedTestDirectory(std::filesystem::path path_) : path(std::move(path_)), owner_pid(::getpid()) {}
    ScopedTestDirectory(const ScopedTestDirectory &) = delete;
    ScopedTestDirectory & operator=(const ScopedTestDirectory &) = delete;

    ~ScopedTestDirectory()
    {
        if (::getpid() != owner_pid)
            return;
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    const std::filesystem::path & getPath() const { return path; }

private:
    std::filesystem::path path;
    pid_t owner_pid;
};

/// Hands `storage` out under a deleter that destroys it first and then removes `root`, so the
/// directory lives exactly as long as the last owner of the storage.
template <typename Storage>
std::shared_ptr<Storage> ownedByDirectory(std::shared_ptr<Storage> storage, std::filesystem::path root)
{
    auto * raw = storage.get();
    return std::shared_ptr<Storage>(
        raw,
        [directory = std::make_shared<ScopedTestDirectory>(std::move(root)), keep = std::move(storage)](Storage *) mutable
        {
            keep.reset();
            directory.reset();
        });
}

}
