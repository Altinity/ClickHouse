#include "config.h"

#if USE_AVRO

#include <gtest/gtest.h>

#include <Common/Exception.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/MetadataGenerator.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergWrites.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/SnapshotSummary.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>

using namespace DB;
using namespace DB::Iceberg;

namespace
{

/// Build minimal metadata suitable for snapshot generation.
Poco::JSON::Object::Ptr makeMetadataForReplace()
{
    auto metadata = Poco::JSON::Object::Ptr(new Poco::JSON::Object);
    metadata->set(f_format_version, 2);
    metadata->set(f_current_schema_id, 0);
    metadata->set(f_last_column_id, 1);
    metadata->set(f_default_spec_id, 0);
    metadata->set(f_last_sequence_number, Int64(2));
    metadata->set(f_table_uuid, "test-uuid-1234");

    auto schemas = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    auto schema = Poco::JSON::Object::Ptr(new Poco::JSON::Object);
    schema->set(f_schema_id, 0);
    schema->set(f_type, "struct");
    auto fields = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    auto field = Poco::JSON::Object::Ptr(new Poco::JSON::Object);
    field->set(f_id, 1);
    field->set(f_name, "x");
    field->set(f_required, true);
    field->set(f_type, "int");
    fields->add(field);
    schema->set(f_fields, fields);
    schemas->add(schema);
    metadata->set(f_schemas, schemas);

    /// Partition specs.
    auto specs = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    auto spec = Poco::JSON::Object::Ptr(new Poco::JSON::Object);
    spec->set(f_spec_id, 0);
    spec->set(f_fields, Poco::JSON::Array::Ptr(new Poco::JSON::Array));
    specs->add(spec);
    metadata->set(f_partition_specs, specs);

    /// Create a parent snapshot with known totals.
    auto snapshots = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    auto parent_snapshot = Poco::JSON::Object::Ptr(new Poco::JSON::Object);
    parent_snapshot->set(f_metadata_snapshot_id, Int64(100));
    parent_snapshot->set(f_timestamp_ms, Int64(1000));
    parent_snapshot->set(f_metadata_sequence_number, Int64(1));
    parent_snapshot->set(f_manifest_list, "s3://bucket/metadata/snap-100-0.avro");

    auto parent_summary = Poco::JSON::Object::Ptr(new Poco::JSON::Object);
    parent_summary->set(f_operation, f_append);
    parent_summary->set(f_total_records, "1000");
    parent_summary->set(f_total_files_size, "50000");
    parent_summary->set(f_total_data_files, "10");
    parent_summary->set(f_total_delete_files, "0");
    parent_summary->set(f_total_position_deletes, "0");
    parent_summary->set(f_total_equality_deletes, "0");
    parent_snapshot->set(f_summary, parent_summary);

    snapshots->add(parent_snapshot);
    metadata->set(f_snapshots, snapshots);
    metadata->set(f_current_snapshot_id, Int64(100));

    return metadata;
}

}


TEST(IcebergBinPackRewrite, ReplaceSnapshotSummaryCounters)
{
    auto metadata = makeMetadataForReplace();
    MetadataGenerator gen(metadata);

    FileNamesGenerator file_gen("s3://bucket/table/", false, CompressionMethod::None, "Parquet");
    file_gen.setVersion(2);

    auto metadata_path = file_gen.generateMetadataPathWithInfo();

    auto result = gen.generateReplaceSnapshot(
        file_gen,
        metadata_path.path,
        /*parent_snapshot_id=*/100,
        /*added_data_files=*/2,
        /*added_records=*/1000,
        /*added_files_size=*/40000,
        /*removed_data_files=*/8,
        /*removed_records=*/800,
        /*removed_files_size=*/35000,
        /*num_partitions=*/3);

    ASSERT_NE(result.snapshot, nullptr);

    auto summary = result.snapshot->getObject(f_summary);
    ASSERT_NE(summary, nullptr);

    /// Operation must be `replace`.
    EXPECT_EQ(summary->getValue<String>(f_operation), f_replace);

    /// Added counters.
    EXPECT_EQ(summary->getValue<String>(f_added_data_files), "2");
    EXPECT_EQ(summary->getValue<String>(f_added_records), "1000");
    EXPECT_EQ(summary->getValue<String>(f_added_files_size), "40000");

    /// Removed counters.
    EXPECT_EQ(summary->getValue<String>(f_deleted_data_files), "8");
    EXPECT_EQ(summary->getValue<String>(f_removed_data_files), "8");
    EXPECT_EQ(summary->getValue<String>(f_deleted_records), "800");
    EXPECT_EQ(summary->getValue<String>(f_removed_files_size), "35000");

    /// Partition count.
    EXPECT_EQ(summary->getValue<String>(f_changed_partition_count), "3");

    /// Total-* counters: parent + (added - removed).
    /// total_records: 1000 + (1000 - 800) = 1200
    EXPECT_EQ(summary->getValue<String>(f_total_records), "1200");
    /// total_files_size: 50000 + (40000 - 35000) = 55000
    EXPECT_EQ(summary->getValue<String>(f_total_files_size), "55000");
    /// total_data_files: 10 + (2 - 8) = 4
    EXPECT_EQ(summary->getValue<String>(f_total_data_files), "4");
}


TEST(IcebergBinPackRewrite, ReplaceSnapshotSequenceNumberIncremented)
{
    auto metadata = makeMetadataForReplace();
    MetadataGenerator gen(metadata);

    FileNamesGenerator file_gen("s3://bucket/table/", false, CompressionMethod::None, "Parquet");
    file_gen.setVersion(2);

    auto metadata_path = file_gen.generateMetadataPathWithInfo();

    auto result = gen.generateReplaceSnapshot(
        file_gen,
        metadata_path.path,
        /*parent_snapshot_id=*/100,
        /*added_data_files=*/1,
        /*added_records=*/500,
        /*added_files_size=*/20000,
        /*removed_data_files=*/5,
        /*removed_records=*/500,
        /*removed_files_size=*/25000,
        /*num_partitions=*/1);

    /// The new snapshot's sequence number must be > parent's (which was 2).
    EXPECT_GT(result.snapshot->getValue<Int64>(f_metadata_sequence_number), 2);
    /// metadata.last-sequence-number must also advance.
    EXPECT_GT(metadata->getValue<Int64>(f_last_sequence_number), 2);
}


TEST(IcebergBinPackRewrite, DataFileEntryLineageStatusOverride)
{
    /// When status_override is set, the entry should use that status.
    DataFileEntryLineage lineage;
    lineage.added_snapshot_id = 42;
    lineage.sequence_number = 1;
    lineage.file_sequence_number = 1;
    lineage.status_override = ManifestEntryStatus::DELETED;

    EXPECT_EQ(lineage.status_override.value(), ManifestEntryStatus::DELETED);
}


TEST(IcebergBinPackRewrite, DataFileEntryLineageNoOverrideDefaultsToExisting)
{
    /// When status_override is not set but lineage is present, the entry status
    /// should be EXISTING (handled by generateManifestFile logic, but we test the struct).
    DataFileEntryLineage lineage;
    lineage.added_snapshot_id = 42;
    lineage.sequence_number = 1;
    lineage.file_sequence_number = 1;

    EXPECT_FALSE(lineage.status_override.has_value());
}


TEST(IcebergBinPackRewrite, SnapshotSummaryReplaceOperationCounters)
{
    /// Build a SnapshotSummary with a replace update and verify totals.
    SnapshotSummaryTotals parent_totals{
        .records = 1000,
        .files_size = 50000,
        .data_files = 10,
        .delete_files = 0,
        .position_deletes = 0,
        .equality_deletes = 0};

    SnapshotSummary summary(
        SnapshotSummaryUpdateReplace{
            .added_files = 3,
            .added_records = 800,
            .added_files_size = 30000,
            .deleted_data_files = 7,
            .removed_records = 700,
            .removed_files_size = 28000,
            .num_partitions = 2},
        parent_totals);

    EXPECT_EQ(summary.getOperation(), SnapshotSummaryOperation::REPLACE);

    auto totals = summary.getTotals();
    /// total_records: 1000 + 800 - 700 = 1100
    EXPECT_EQ(totals.records, 1100);
    /// total_files_size: 50000 + 30000 - 28000 = 52000
    EXPECT_EQ(totals.files_size, 52000);
    /// total_data_files: 10 + 3 - 7 = 6
    EXPECT_EQ(totals.data_files, 6);
}

#endif
