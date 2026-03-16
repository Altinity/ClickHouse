SET allow_experimental_object_storage_table_engine = 1;
SET allow_experimental_insert_into_iceberg = 1;

DROP TABLE IF EXISTS test_iceberg_truncate;
CREATE TABLE test_iceberg_truncate (id Int32, val String) ENGINE = IcebergLocal('test_iceberg_truncate');

INSERT INTO test_iceberg_truncate VALUES (1, 'Test1'), (2, 'Test2');
SELECT count() FROM test_iceberg_truncate;
TRUNCATE TABLE test_iceberg_truncate;
SELECT count() FROM test_iceberg_truncate;

DROP TABLE test_iceberg_truncate;
