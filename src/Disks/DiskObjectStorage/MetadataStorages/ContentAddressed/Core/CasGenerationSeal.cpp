#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
/// Included by basename via clickhouse_cas_proto's SYSTEM include dir so the generated header's
/// reserved identifiers don't trip -Weverything -Werror.
#include <cas_format.pb.h>
#include <Common/Exception.h>
#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

namespace Proto = ::clickhouse::cas::format;

namespace
{

void addRuns(google::protobuf::RepeatedPtrField<Proto::RunRefProto> * field, const std::vector<RunRef> & runs)
{
    std::vector<const RunRef *> sorted;
    sorted.reserve(runs.size());
    for (const auto & r : runs)
        sorted.push_back(&r);
    std::sort(sorted.begin(), sorted.end(), [](const RunRef * a, const RunRef * b) { return a->key < b->key; });
    for (const RunRef * r : sorted)
    {
        auto * e = field->Add();
        e->set_key(r->key);
        e->set_checksum_hi(static_cast<uint64_t>(r->checksum >> 64));
        e->set_checksum_lo(static_cast<uint64_t>(r->checksum));
    }
}

void readRuns(const google::protobuf::RepeatedPtrField<Proto::RunRefProto> & field, std::vector<RunRef> & runs)
{
    for (const auto & e : field)
        runs.push_back(RunRef{.key = e.key(),
            .checksum = (UInt128(e.checksum_hi()) << 64) | UInt128(e.checksum_lo())});
}

void addCoverage(google::protobuf::RepeatedPtrField<Proto::FoldShardCoverageProto> * field,
                 const std::map<String, ShardCoverage> & cov)
{
    for (const auto & [key, c] : cov)   /// std::map => sorted => deterministic
    {
        auto * e = field->Add();
        e->set_key(key);
        e->set_classification(c.classification);
        e->set_folded_token_type(static_cast<uint32_t>(c.folded_token.type));
        e->set_folded_token_value(c.folded_token.value);
        e->set_folded_cursor(c.folded_cursor);
        e->set_incarnation_writer_epoch(c.incarnation.writer_epoch);
        e->set_incarnation_build_sequence(c.incarnation.build_sequence);
    }
}

void readCoverage(const google::protobuf::RepeatedPtrField<Proto::FoldShardCoverageProto> & field,
                  std::map<String, ShardCoverage> & cov)
{
    for (const auto & e : field)
        cov[e.key()] = ShardCoverage{
            .classification = static_cast<uint8_t>(e.classification()),
            .folded_token = Token{e.folded_token_value(), static_cast<TokenType>(e.folded_token_type())},
            .folded_cursor = e.folded_cursor(),
            .incarnation = ShardIncarnation{e.incarnation_writer_epoch(), e.incarnation_build_sequence()}};
}

}

String encodeFoldSeal(const CasFoldSeal & seal)
{
    Proto::FoldSealProto msg;
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::FoldSeal));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_generation(seal.generation);
    msg.set_parent_generation(seal.parent_generation);

    for (const auto & [key, cov] : seal.per_ns_shard)   /// std::map => sorted => deterministic
    {
        auto * e = msg.add_per_ns_shard();
        e->set_key(key);
        e->set_classification(cov.classification);
        e->set_folded_token_type(static_cast<uint32_t>(cov.folded_token.type));
        e->set_folded_token_value(cov.folded_token.value);
        e->set_folded_cursor(cov.folded_cursor);
        e->set_incarnation_writer_epoch(cov.incarnation.writer_epoch);
        e->set_incarnation_build_sequence(cov.incarnation.build_sequence);
    }
    addRuns(msg.mutable_blob_target_runs(), seal.blob_target_runs);
    addRuns(msg.mutable_part_manifest_cleanup(), seal.part_manifest_cleanup);

    return msg.SerializeAsString();
}

CasFoldSeal decodeFoldSeal(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: empty object");

    Proto::FoldSealProto msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: protobuf parse failed");
    if (msg.header().magic() != magicFor(FormatId::FoldSeal))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS fold seal: bad magic (got 0x{:08x}, expected 0x{:08x})",
            msg.header().magic(), magicFor(FormatId::FoldSeal));
    checkCompatibility(msg.header().compatibility_version(), "fold seal");

    CasFoldSeal seal;
    seal.generation = msg.generation();
    seal.parent_generation = msg.parent_generation();
    for (const auto & e : msg.per_ns_shard())
        seal.per_ns_shard[e.key()] = ShardCoverage{
            .classification = static_cast<uint8_t>(e.classification()),
            .folded_token = Token{e.folded_token_value(), static_cast<TokenType>(e.folded_token_type())},
            .folded_cursor = e.folded_cursor(),
            .incarnation = ShardIncarnation{e.incarnation_writer_epoch(), e.incarnation_build_sequence()}};
    readRuns(msg.blob_target_runs(), seal.blob_target_runs);
    readRuns(msg.part_manifest_cleanup(), seal.part_manifest_cleanup);
    return seal;
}

String encodeCompletionSeal(const CasCompletionSeal & seal)
{
    Proto::CompletionSealProto msg;
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::CompletionSeal));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_generation(seal.generation);
    for (const auto & [key, version] : seal.fence_positions)   /// std::map => sorted
    {
        auto * e = msg.add_fence_positions();
        e->set_key(key);
        e->set_version(version);
    }
    addRuns(msg.mutable_delete_outcomes(), seal.delete_outcomes);
    for (const auto & [key, version] : seal.trim_cursors)
    {
        auto * e = msg.add_trim_cursors();
        e->set_key(key);
        e->set_version(version);
    }
    msg.set_adoptable(seal.adoptable);
    addRuns(msg.mutable_blob_target_runs(), seal.blob_target_runs);   /// M2
    addCoverage(msg.mutable_folded_cursors(), seal.folded_cursors);   /// M1

    return msg.SerializeAsString();
}

CasCompletionSeal decodeCompletionSeal(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS completion seal: empty object");

    Proto::CompletionSealProto msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS completion seal: protobuf parse failed");
    if (msg.header().magic() != magicFor(FormatId::CompletionSeal))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS completion seal: bad magic (got 0x{:08x}, expected 0x{:08x})",
            msg.header().magic(), magicFor(FormatId::CompletionSeal));
    checkCompatibility(msg.header().compatibility_version(), "completion seal");

    CasCompletionSeal seal;
    seal.generation = msg.generation();
    for (const auto & e : msg.fence_positions())
        seal.fence_positions[e.key()] = e.version();
    readRuns(msg.delete_outcomes(), seal.delete_outcomes);
    for (const auto & e : msg.trim_cursors())
        seal.trim_cursors[e.key()] = e.version();
    seal.adoptable = msg.adoptable();
    readRuns(msg.blob_target_runs(), seal.blob_target_runs);   /// M2
    readCoverage(msg.folded_cursors(), seal.folded_cursors);   /// M1
    return seal;
}

}
