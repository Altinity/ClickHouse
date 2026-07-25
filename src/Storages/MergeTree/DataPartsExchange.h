#pragma once

#include <Storages/MergeTree/MergeTreePartInfo.h>
#include <Interpreters/InterserverIOHandler.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/IStorage_fwd.h>
#include <IO/HashingWriteBuffer.h>
#include <IO/copyData.h>
#include <IO/ConnectionTimeouts.h>
#include <Common/Throttler.h>
#include <Common/ActionBlocker.h>
#include <IO/ReadBuffer.h>


namespace zkutil
{
    class ZooKeeper;
    using ZooKeeperPtr = std::shared_ptr<ZooKeeper>;
}

namespace DB
{

class StorageReplicatedMergeTree;
class ReadWriteBufferFromHTTP;

/// Declared by `ContentAddressedExchange.h` (the narrow content-addressed seam). Opaque-enum-declared
/// here so this header stays free of content-addressed includes; the definition must keep the same
/// underlying type.
enum class CasConfirmAnswer : uint8_t;

namespace DataPartsExchange
{

/** Service for sending parts from the table *ReplicatedMergeTree.
  */
class Service final : public InterserverIOEndpoint
{
public:
    explicit Service(StorageReplicatedMergeTree & data_);

    Service(const Service &) = delete;
    Service & operator=(const Service &) = delete;

    std::string getId(const std::string & node_id) const override;
    void processQuery(const HTMLForm & params, ReadBufferPtr body, WriteBuffer & out, HTTPServerResponse & response) override;

private:
    /// CAS fetch-by-relink, publish-then-confirm: answer one relink confirm token — "is `manifest_ref_text`
    /// still exactly what `ref_name` names here?" — for a receiver that has already made its own `+1`
    /// durable and may promote only on `Yes`. Everything content-addressed is behind
    /// `IContentAddressedExchange`; what has to live here is what only the storage can see: which of this
    /// table's disks is entitled to answer (`ownsNamespace` under a matching pool UUID, exactly one match
    /// or `Unknown`), and gate 0, the part-anchored filter over this table's parts set. Never throws, and
    /// `No` is not knowledge — see `CasConfirmAnswer`.
    /// The confirm action's handler: decode the peer's token, resolve it, and set the answer cookie.
    /// Exactly two answers cross the wire — proven, and not proven — because only `Yes` authorizes
    /// anything and `No` is not knowledge (see `CasConfirmAnswer`). Never throws: an unparsable token
    /// is one more unproven answer, not an error the receiver would have to classify.
    void answerContentAddressedConfirm(const String & token_text, HTTPServerResponse & response) const;

    CasConfirmAnswer resolveContentAddressedConfirm(
        const String & pool_uuid,
        const String & server_root_id,
        const String & root_namespace,
        const String & ref_name,
        const String & part_name,
        const String & manifest_ref_text) const;

    MergeTreeData::DataPartPtr findPart(const String & name);

    MergeTreeData::DataPart::Checksums sendPartFromDisk(
        const MergeTreeData::DataPartPtr & part,
        WriteBuffer & out,
        int client_protocol_version,
        bool from_remote_disk,
        bool send_projections);

    /// StorageReplicatedMergeTree::shutdown() waits for all parts exchange handlers to finish,
    /// so Service will never access dangling reference to storage
    StorageReplicatedMergeTree & data;
    LoggerPtr log;
};

/** Client for getting the parts from the table *MergeTree.
  */
class Fetcher final : private boost::noncopyable
{
public:
    explicit Fetcher(StorageReplicatedMergeTree & data_);

    /// Downloads a part to tmp_directory. If to_detached - downloads to the `detached` directory.
    std::pair<MergeTreeData::MutableDataPartPtr, scope_guard> fetchSelectedPart(
        const StorageMetadataPtr & metadata_snapshot,
        ContextPtr context,
        const String & part_name,
        const String & zookeeper_name,
        const String & replica_path,
        const String & host,
        int port,
        const ConnectionTimeouts & timeouts,
        const String & user,
        const String & password,
        const String & interserver_scheme,
        ThrottlerPtr throttler,
        bool to_detached = false,
        const String & tmp_prefix_ = "",
        std::optional<CurrentlySubmergingEmergingTagger> * tagger_ptr = nullptr,
        bool try_zero_copy = true,
        DiskPtr dest_disk = nullptr);

    /// You need to stop the data transfer.
    ActionBlocker blocker;

private:
    using OutputBufferGetter = std::function<std::unique_ptr<WriteBufferFromFileBase>(IDataPartStorage &, const String &, size_t)>;

    void downloadBaseOrProjectionPartToDisk(
        const String & replica_path,
        const MutableDataPartStoragePtr & data_part_storage,
        ReadWriteBufferFromHTTP & in,
        OutputBufferGetter output_buffer_getter,
        MergeTreeData::DataPart::Checksums & checksums,
        ThrottlerPtr throttler,
        bool sync) const;

    MergeTreeData::MutableDataPartPtr downloadPartToDisk(
        const String & part_name,
        const String & replica_path,
        bool to_detached,
        const String & tmp_prefix_,
        DiskPtr disk,
        bool to_remote_disk,
        ReadWriteBufferFromHTTP & in,
        OutputBufferGetter output_buffer_getter,
        size_t projections,
        ThrottlerPtr throttler,
        bool sync);

    /// CAS replication 2b — fetch-by-relink (spec §4). Build a part WITHOUT downloading any bytes by
    /// publishing this server's own ref to the blobs already in the shared content-addressed pool. Stages
    /// the ref under the tmp-fetch dir (so the caller's renameTempPartAndReplace re-keys it to the final
    /// part name, exactly as for a byte-fetched part), loads the part from the shared manifest, and
    /// returns it. Returns nullptr if the relink is not possible (the transferred manifest's blobs are
    /// not resolvable in this pool — missing/condemned), in which case the caller falls back to a byte fetch.
    /// Self-contained (all-tree task 7): the transferred manifest alone is enough to rebuild the part — no
    /// separate uuid/metadata_version wire fields to reconstruct as a sidecar.
    MergeTreeData::MutableDataPartPtr relinkPartToDisk(
        const String & part_name,
        const String & tmp_prefix,
        DiskPtr disk,
        const String & sender_manifest_bytes);

    MergeTreeData::MutableDataPartPtr downloadPartToDiskRemoteMeta(
       const String & part_name,
       const String & replica_path,
       bool to_detached,
       const String & tmp_prefix_,
       DiskPtr disk,
       ReadWriteBufferFromHTTP & in,
       size_t projections,
       MergeTreeData::DataPart::Checksums & checksums,
       ThrottlerPtr throttler);

    StorageReplicatedMergeTree & data;
    LoggerPtr log;
};

}

}
