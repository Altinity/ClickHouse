-- Tags: no-fasttest
SET allow_local_data_lakes = 1;

CREATE TABLE t0 (c0 Nullable(Int)) ENGINE = IcebergLocal('/file0') PARTITION BY (`c0.null` IS NULL);  -- { serverError BAD_ARGUMENTS }
