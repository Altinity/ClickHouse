#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <optional>
#include <set>

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

/// Hints the ref-log ids of `first`'s epoch, from `first` upward, while the reader has window, the id
/// is within the committed frontier, and the id is in `listed` -- the ids the caller's LIST of this
/// life's stream returned. Ids within an epoch are dense, so the first unlisted id ends the run: it is
/// either past the epoch's seal, where no id ever exists (a walk learns where the seal is only by
/// decoding it, so arithmetic alone would hint a window of keys that are not there), or younger than
/// the LIST, which the walk then takes inline. Either way the listing decides only WHAT is fetched
/// early, never what the walk reads or decides.
void hintRefLogsWithinEpoch(KeyReader & reader, const Layout & layout, const NamespaceLifeId & life,
                            RefTxnId first, const RefTxnId & committed_through, const std::set<RefTxnId> & listed);

/// Drops every hint of `first`'s epoch from `first` up to one window: called when a walk crosses an
/// epoch or leaves it early, so the window is free for whatever it reads next. Discarding an unhinted
/// key is a no-op, so over-asking by a window is harmless.
void discardRefLogHintsOfEpoch(KeyReader & reader, const Layout & layout, const NamespaceLifeId & life,
                               RefTxnId first, const RefTxnId & committed_through);

}
