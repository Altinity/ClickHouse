# Export Partition Disaster Recovery Tests

This test suite validates the disaster recovery capabilities of the EXPORT PARTITION operation in ClickHouse.

## Test Scenarios

1. **Basic Server Kill** - Tests simple crash during export with recovery
2. **Network Delays** - Tests crash during network-delayed operations  
3. **Multiple Crashes** - Tests resilience across multiple crash/restart cycles
4. **Large Datasets** - Tests recovery with substantial data that ensures slow operations
5. **State Persistence** - Tests that recovery state/checkpoints survive crashes
6. **Concurrent Operations** - Tests crashes during multiple simultaneous exports

## Running Tests

```bash
# Run all disaster recovery tests
pytest -v test_export_partition_disaster_recovery/

# Run specific test
pytest -v test_export_partition_disaster_recovery/test.py::TestExportPartitionDisasterRecovery::test_export_partition_with_server_kill_basic

# Run with detailed logging
pytest -v -s test_export_partition_disaster_recovery/
```

## Test Infrastructure

- Uses `BrokenS3` mock to simulate slow S3 operations
- Uses `PartitionManager` for network delay simulation
- Uses `kill -9` to simulate hard server crashes
- Validates data integrity and operation resumability after recovery
- Tests various data sizes and partition configurations

## Requirements

- ClickHouse with S3 support
- MinIO integration test environment  
- `stay_alive=True` cluster configuration for crash testing

## Why No Zookeeper?

These tests focus on **single-node disaster recovery** for export partition operations on plain MergeTree tables. Zookeeper is only needed for:
- ReplicatedMergeTree coordination
- Multi-node distributed operations
- Cross-replica synchronization

Since we're testing crash recovery of individual export operations, no coordination is required.
