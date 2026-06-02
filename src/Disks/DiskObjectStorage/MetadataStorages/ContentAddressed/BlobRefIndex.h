#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Footer.h>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>

namespace DB::ContentAddressed
{

/// Seam (B9): the delta refcount over content-addressed object keys.
/// M1 ships InMemoryBlobRefIndex; a RocksDB-backed impl plugs in here unchanged.
class IBlobRefIndex
{
public:
    virtual ~IBlobRefIndex() = default;
    virtual void addPart(const std::string & part_id, const Footer & footer) = 0;
    virtual void removePart(const std::string & part_id, const Footer & footer) = 0;
    virtual int64_t refcount(const std::string & blob_key) const = 0;
    virtual std::set<std::string> unreferenced() const = 0;
};

class InMemoryBlobRefIndex : public IBlobRefIndex
{
public:
    void addPart(const std::string & part_id, const Footer & footer) override;
    void removePart(const std::string & part_id, const Footer & footer) override;
    int64_t refcount(const std::string & blob_key) const override;
    std::set<std::string> unreferenced() const override;

private:
    std::unordered_map<std::string, int64_t> counts;
    std::set<std::string> applied_parts; /// idempotency guard for add/remove
};

}
