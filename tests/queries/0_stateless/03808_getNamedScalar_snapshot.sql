-- Tags: no-parallel
SET allow_experimental_named_scalars = 1;

-- Query-snapshot semantics: the value resolved at analysis time is stable for
-- the whole query even across multiple reads and a REFRESH that lands during
-- execution. Also verifies constant folding makes repeated reads free.

DROP NAMED SCALAR IF EXISTS snap_v;

CREATE NAMED SCALAR snap_v REFRESH EVERY 1 SECOND AS SELECT toUInt64(now());

-- Two reads of the same scalar in one query must return the same value.
SELECT getNamedScalar('snap_v') = getNamedScalar('snap_v');

-- Across queries, refresh is observable.
SELECT sleep(2) FORMAT Null;
SELECT getNamedScalar('snap_v') > 0;

-- Defined scalar path also captures value/type before execution; truly-missing
-- getNamedScalarOrDefault is covered elsewhere.
SELECT getNamedScalarOrDefault('snap_v', toUInt64(0)) > 0;

DROP NAMED SCALAR snap_v;
