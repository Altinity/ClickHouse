#include <benchmark/benchmark.h>

#include <Common/PODArray.h>
#include <IO/WriteBufferFromVector.h>
#include <IO/WriteHelpers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>

/// Pure measurement, no pass/fail assertions -- see the ca-gc-rebuild BACKLOG.md entries
/// "OPTIMIZATION OPPORTUNITY -- ref-ledger JSON encoding writes byte-by-byte" and
/// "OPTIMIZATION OPPORTUNITY -- admits() re-encodes the WHOLE ref table once per state-growing
/// op" for the investigation these benchmarks measure. Build with `-DENABLE_BENCHMARKS=ON` and
/// run the resulting `benchmark_cas_ref_protocol` binary directly; never wired into `ninja test`
/// or CI.

using namespace DB::Cas;

namespace
{

/// A ref-ledger key shape as actually written on the wire: table_uuid + database + table + part_name.
constexpr std::string_view kSafeKeyLikeString
    = "eeeb74a2-606a-4ee9-840a-1aac7b5ac25b_ca_stress_default_part_20260719_0_89811_538";

RefLogTxn makeSamplePromoteTxn()
{
    RefLogTxn txn;
    txn.ns = "roots/ca_soak_ch1";
    txn.txn_id = RefTxnId{1, 12345};

    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, "20260719_0_89811_538_89818", ManifestRef{1, 1, 999999}};
    op.new_binding = RefOwnerBinding{RefOwnerKind::Committed, "20260719_0_89811_538_89818", ManifestRef{1, 1, 999999}};
    txn.ops.push_back(op);
    return txn;
}

/// A synthetic committed-ref table of `n` rows, plus one pending precommit ready to promote --
/// exactly the shape `admits()` previews on every state-growing ref op.
RefTableState makeSyntheticState(size_t n)
{
    RefTableState state;
    state.lifecycle = RefLifecycle::Live;
    state.greatest_applied = RefTxnId{1, 1};   /// snapshotOf() rejects an all-zero snapshot_id
    for (size_t i = 0; i < n; ++i)
    {
        RefCommittedRow row;
        row.ref_name = "part_" + std::to_string(i) + "_20260719_0_1000_1";
        row.manifest_ref = ManifestRef{1, 1, static_cast<uint32_t>(i + 1)};
        state.committed.emplace(row.ref_name, row);
    }
    state.committed.materialize();
    state.precommits.emplace("new_part_x", ManifestRef{1, 1, 999999});
    return state;
}

}

/// Floor comparison: writeJSONString's per-character escaping loop (WriteHelpers.h) on a string
/// that needs no escaping at all (a real ref-ledger key shape) vs a raw bulk write of the same
/// bytes. See BM_RawBulkWriteSafe below for the delta.
static void BM_WriteJSONStringSafe(benchmark::State & state)
{
    DB::FormatSettings settings;
    DB::PODArray<char> buf;
    for (auto _ : state)
    {
        buf.clear();
        DB::WriteBufferFromVector<DB::PODArray<char>> out(buf);
        DB::writeJSONString(kSafeKeyLikeString, out, settings);
        benchmark::DoNotOptimize(buf.data());
    }
}
BENCHMARK(BM_WriteJSONStringSafe);

static void BM_RawBulkWriteSafe(benchmark::State & state)
{
    DB::PODArray<char> buf;
    for (auto _ : state)
    {
        buf.clear();
        DB::WriteBufferFromVector<DB::PODArray<char>> out(buf);
        DB::writeChar('"', out);
        out.write(kSafeKeyLikeString.data(), kSafeKeyLikeString.size());
        DB::writeChar('"', out);
        benchmark::DoNotOptimize(buf.data());
    }
}
BENCHMARK(BM_RawBulkWriteSafe);

/// Absolute cost of encoding one ref-log transaction (a single promote op) with the current
/// field-by-field WriteBuffer-call implementation -- no "after" to diff against yet, this is the
/// baseline a rewrite (batched/templated assembly, see the BACKLOG entry) would need to beat.
static void BM_EncodeRefLogTxn(benchmark::State & state)
{
    const RefLogTxn txn = makeSamplePromoteTxn();
    for (auto _ : state)
        benchmark::DoNotOptimize(encodeRefLogTxn(txn));
}
BENCHMARK(BM_EncodeRefLogTxn);

/// admits() re-derives and re-encodes the WHOLE committed-ref snapshot on every call
/// (CasRefProtocol.cpp) -- this should show close to linear (O(N)) growth with table size.
/// ->Complexity() has Google Benchmark fit and print the empirical big-O across the range.
static void BM_Admits(benchmark::State & state)
{
    const size_t n = static_cast<size_t>(state.range(0));
    const RefTableState table = makeSyntheticState(n);
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, "new_part_x", ManifestRef{1, 1, 999999}};
    op.new_binding = RefOwnerBinding{RefOwnerKind::Committed, "new_part_x", ManifestRef{1, 1, 999999}};

    for (auto _ : state)
        benchmark::DoNotOptimize(admits(table, op, 1ull << 40, 1ull << 40));

    state.SetComplexityN(static_cast<int64_t>(n));
}
BENCHMARK(BM_Admits)->RangeMultiplier(10)->Range(100, 100000)->Complexity();

BENCHMARK_MAIN();
