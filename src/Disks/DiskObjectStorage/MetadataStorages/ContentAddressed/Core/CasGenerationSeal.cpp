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
        e->set_shard(r->shard);
        e->set_generation(r->generation);
    }
}

void readRuns(const google::protobuf::RepeatedPtrField<Proto::RunRefProto> & field, std::vector<RunRef> & runs)
{
    for (const auto & e : field)
        runs.push_back(RunRef{.key = e.key(),
            .checksum = (UInt128(e.checksum_hi()) << 64) | UInt128(e.checksum_lo()),
            .shard = e.shard(),
            .generation = e.generation()});
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
        e->set_last_folded_ref_epoch(cov.last_folded_ref_id.writer_epoch);
        e->set_last_folded_ref_sequence(cov.last_folded_ref_id.ref_sequence);
    }
    addRuns(msg.mutable_blob_target_runs(), seal.blob_target_runs);
    addRuns(msg.mutable_part_manifest_cleanup(), seal.part_manifest_cleanup);

    for (const auto & [shard, summary] : seal.condemned_summary)   /// std::map => sorted by shard
    {
        auto * e = msg.add_condemned_summary();
        e->set_shard(shard);
        e->set_condemned_total(summary.condemned_total);
        e->set_pending_total(summary.pending_total);
        e->set_oldest_nonpending_condemn_round(summary.oldest_nonpending_condemn_round);
    }

    for (const auto & [key, item] : seal.ns_cleanup_items)   /// std::map => sorted by key => deterministic
    {
        auto * e = msg.add_ns_cleanup_items();
        e->set_ns(item.ns.string());
        e->set_remove_txn_epoch(item.remove_txn_id.writer_epoch);
        e->set_remove_txn_sequence(item.remove_txn_id.ref_sequence);
        e->set_state(static_cast<uint32_t>(item.state));
    }

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
    {
        const uint32_t token_type_raw = e.folded_token_type();
        if (token_type_raw != static_cast<uint32_t>(TokenType::ETag)
            && token_type_raw != static_cast<uint32_t>(TokenType::Generation)
            && token_type_raw != static_cast<uint32_t>(TokenType::Emulated))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS fold seal: unknown folded token type {}", token_type_raw);
        seal.per_ns_shard[e.key()] = ShardCoverage{
            .classification = static_cast<uint8_t>(e.classification()),
            .folded_token = Token{e.folded_token_value(), static_cast<TokenType>(token_type_raw)},
            .last_folded_ref_id = RefTxnId{e.last_folded_ref_epoch(), e.last_folded_ref_sequence()}};
    }
    readRuns(msg.blob_target_runs(), seal.blob_target_runs);
    readRuns(msg.part_manifest_cleanup(), seal.part_manifest_cleanup);
    for (const auto & e : msg.condemned_summary())
        seal.condemned_summary[e.shard()] = CondemnedSummary{
            .condemned_total = e.condemned_total(),
            .pending_total = e.pending_total(),
            .oldest_nonpending_condemn_round = e.oldest_nonpending_condemn_round()};
    for (const auto & e : msg.ns_cleanup_items())
    {
        const uint32_t state_raw = e.state();
        if (state_raw != static_cast<uint32_t>(RefNsCleanupState::Pending)
            && state_raw != static_cast<uint32_t>(RefNsCleanupState::Completed))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS fold seal: unknown ref-namespace cleanup state {}", state_raw);
        const RefTxnId remove_txn_id{e.remove_txn_epoch(), e.remove_txn_sequence()};
        const String key = e.ns() + "\n" + renderRefTxnId(remove_txn_id);
        seal.ns_cleanup_items[key] = RefNsCleanupItem{
            .ns = RootNamespace{e.ns()},
            .remove_txn_id = remove_txn_id,
            .state = static_cast<RefNsCleanupState>(state_raw)};
    }
    return seal;
}

}
