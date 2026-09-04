#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <optional>

namespace DB::Cas
{

/// What a sequential walk needs from whoever fetches its objects. `take` returns the object (or
/// nullopt when absent) and is the only call that decides anything; `hint` may start fetching a key
/// the walk will take later; `discard` drops a hint the walk will never take. A walk that hints
/// nothing and takes everything in order behaves exactly like one that reads inline: the reader is a
/// cache of results, never of decisions.
class KeyReader
{
public:
    virtual ~KeyReader() = default;
    virtual void hint(const String & key) = 0;
    virtual std::optional<Object> take(const String & key) = 0;
    virtual void discard(const String & key) = 0;
    /// Hinted and not yet taken.
    virtual size_t pending() const = 0;
    /// How many hints a walk keeps outstanding; 0 means "do not hint".
    virtual size_t window() const = 0;
};

/// The sequential reader: every take is one inline read on the caller's operation.
class InlineKeyReader final : public KeyReader
{
public:
    explicit InlineKeyReader(CasOperation & op_) : op(op_) {}
    void hint(const String &) override {}
    std::optional<Object> take(const String & key) override { return op.read(key, Retry::standard()); }
    void discard(const String &) override {}
    size_t pending() const override { return 0; }
    size_t window() const override { return 0; }

private:
    CasOperation & op;
};

}
