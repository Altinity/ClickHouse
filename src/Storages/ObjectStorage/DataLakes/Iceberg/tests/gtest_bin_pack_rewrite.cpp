#include "config.h"

#if USE_AVRO

#include <gtest/gtest.h>

#include <Common/Exception.h>
#include <Common/tests/gtest_global_context.h>
#include <DataTypes/DataTypesNumber.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <Processors/Formats/Impl/AvroRowInputFormat.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergPath.h>
#include <Storages/ObjectStorage/Utils.h>
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

Poco::JSON::Object::Ptr makeReplaceSnapshot(Poco::JSON::Object::Ptr metadata)
{
    MetadataGenerator gen(metadata);
    FileNamesGenerator file_gen("s3://bucket/table/", false, CompressionMethod::None, "Parquet");
    file_gen.setVersion(2);
    auto metadata_path = file_gen.generateMetadataPathWithInfo();
    return gen.generateReplaceSnapshot(file_gen, metadata_path.path, 100, 1, 30, 300, 3, 30, 300, 1).snapshot;
}

std::vector<avro::GenericDatum> readAvroRecords(const String & data)
{
    ReadBufferFromString in(data);
    auto reader_base = std::make_unique<avro::DataFileReaderBase>(
        std::make_unique<AvroInputStreamReadBufferAdapter>(in), MAX_AVRO_SCHEMA_DEPTH);
    avro::DataFileReader<avro::GenericDatum> reader(std::move(reader_base));

    std::vector<avro::GenericDatum> records;
    avro::GenericDatum datum(reader.readerSchema());
    while (reader.read(datum))
        records.push_back(datum);
    return records;
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

    ASSERT_NE(result.snapshot.get(), nullptr);

    auto summary = result.snapshot->getObject(f_summary);
    ASSERT_NE(summary.get(), nullptr);

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


TEST(IcebergBinPackRewrite, ReplaceSnapshotRejectsFormatVersion3)
{
    auto metadata = makeMetadataForReplace();
    metadata->set(f_format_version, 3);
    MetadataGenerator gen(metadata);

    FileNamesGenerator file_gen("s3://bucket/table/", false, CompressionMethod::None, "Parquet");
    file_gen.setVersion(2);
    auto metadata_path = file_gen.generateMetadataPathWithInfo();

    EXPECT_THROW(
        gen.generateReplaceSnapshot(file_gen, metadata_path.path, 100, 1, 10, 100, 2, 10, 100, 1),
        DB::Exception);
}


TEST(IcebergBinPackRewrite, ManifestEntriesStatusAndSnapshotId)
{
    auto metadata = makeMetadataForReplace();
    auto snapshot = makeReplaceSnapshot(metadata);
    const Int64 new_snapshot_id = snapshot->getValue<Int64>(f_metadata_snapshot_id);
    const Int64 new_sequence_number = snapshot->getValue<Int64>(f_metadata_sequence_number);

    DataFileEntryLineage deleted;
    deleted.added_snapshot_id = 42;
    deleted.sequence_number = 1;
    deleted.file_sequence_number = 1;
    deleted.status_override = ManifestEntryStatus::DELETED;

    DataFileEntryLineage existing;
    existing.added_snapshot_id = 42;
    existing.sequence_number = 1;
    existing.file_sequence_number = 1;

    auto sample_block = std::make_shared<const Block>(
        Block{ColumnWithTypeAndName(std::make_shared<DataTypeInt32>(), "x")});

    WriteBufferFromOwnString buf;
    generateManifestFile(
        metadata,
        {},
        {},
        {},
        {IcebergPathFromMetadata::deserialize("s3://bucket/table/data/a.parquet"),
         IcebergPathFromMetadata::deserialize("s3://bucket/table/data/b.parquet")},
        {10, 20},
        {100, 200},
        std::nullopt,
        sample_block,
        snapshot,
        "PARQUET",
        metadata->getArray(f_partition_specs)->getObject(0),
        0,
        buf,
        FileContentType::DATA,
        std::nullopt,
        {},
        {},
        {},
        {},
        {deleted, existing});
    buf.finalize();

    auto entries = readAvroRecords(buf.str());
    ASSERT_EQ(entries.size(), 2);

    /// The spec defines `snapshot_id` of a DELETED entry as the snapshot that deleted the file.
    const auto & deleted_entry = entries[0].value<avro::GenericRecord>();
    EXPECT_EQ(deleted_entry.field(f_status).value<Int32>(), static_cast<Int32>(ManifestEntryStatus::DELETED));
    EXPECT_EQ(deleted_entry.field(f_snapshot_id).value<Int64>(), new_snapshot_id);
    EXPECT_EQ(deleted_entry.field(f_sequence_number).value<Int64>(), 1);

    /// An EXISTING entry keeps the snapshot that added the file.
    const auto & existing_entry = entries[1].value<avro::GenericRecord>();
    EXPECT_EQ(existing_entry.field(f_status).value<Int32>(), static_cast<Int32>(ManifestEntryStatus::EXISTING));
    EXPECT_EQ(existing_entry.field(f_snapshot_id).value<Int64>(), 42);
    EXPECT_EQ(existing_entry.field(f_sequence_number).value<Int64>(), 1);
    EXPECT_NE(new_sequence_number, 1);
}


TEST(IcebergBinPackRewrite, ManifestListCountsMatchEntryStatuses)
{
    auto metadata = makeMetadataForReplace();
    auto snapshot = makeReplaceSnapshot(metadata);
    const Int64 new_sequence_number = snapshot->getValue<Int64>(f_metadata_sequence_number);

    ManifestListEntryExistingCounts deleted_counts;
    deleted_counts.min_sequence_number = 1;
    deleted_counts.deleted_files_count = 3;
    deleted_counts.deleted_rows_count = 30;

    ManifestListEntryExistingCounts added_counts;
    added_counts.min_sequence_number = new_sequence_number;
    added_counts.added_files_count = 1;
    added_counts.added_rows_count = 30;

    ManifestListEntryExistingCounts kept_counts;
    kept_counts.min_sequence_number = 1;
    kept_counts.existing_files_count = 2;
    kept_counts.existing_rows_count = 50;

    IcebergPathResolver path_resolver("s3://bucket/table", "table");
    SecondaryStorages secondary_storages;
    WriteBufferFromOwnString buf;
    generateManifestList(
        path_resolver,
        metadata,
        /* object_storage */ nullptr,
        secondary_storages,
        getContext().context,
        {IcebergPathFromMetadata::deserialize("s3://bucket/table/metadata/deleted.avro"),
         IcebergPathFromMetadata::deserialize("s3://bucket/table/metadata/added.avro"),
         IcebergPathFromMetadata::deserialize("s3://bucket/table/metadata/kept.avro")},
        snapshot,
        {1000, 1000, 1000},
        buf,
        FileContentType::DATA,
        /* use_previous_snapshots */ false,
        {},
        {deleted_counts, added_counts, kept_counts});

    auto entries = readAvroRecords(buf.str());
    ASSERT_EQ(entries.size(), 3);

    auto check = [](const avro::GenericDatum & datum, const ManifestListEntryExistingCounts & expected)
    {
        const auto & entry = datum.value<avro::GenericRecord>();
        EXPECT_EQ(entry.field(f_added_files_count).value<Int32>(), expected.added_files_count);
        EXPECT_EQ(entry.field(f_existing_files_count).value<Int32>(), expected.existing_files_count);
        EXPECT_EQ(entry.field(f_deleted_files_count).value<Int32>(), expected.deleted_files_count);
        EXPECT_EQ(entry.field(f_added_rows_count).value<Int64>(), expected.added_rows_count);
        EXPECT_EQ(entry.field(f_existing_rows_count).value<Int64>(), expected.existing_rows_count);
        EXPECT_EQ(entry.field(f_deleted_rows_count).value<Int64>(), expected.deleted_rows_count);
        EXPECT_EQ(entry.field(f_min_sequence_number).value<Int64>(), expected.min_sequence_number);
    };
    check(entries[0], deleted_counts);
    check(entries[1], added_counts);
    check(entries[2], kept_counts);
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
