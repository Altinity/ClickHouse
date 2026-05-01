-- Tags: no-parallel
SET allow_experimental_named_scalars = 1;

DROP NAMED SCALAR IF EXISTS cv_defined;

-- Local scope (no SHARED modifier) + getNamedScalarOrDefault.
CREATE NAMED SCALAR cv_defined AS SELECT toUInt32(7);

SELECT getNamedScalarOrDefault('cv_defined', toUInt32(0));
SELECT getNamedScalarOrDefault('missing', toUInt32(42));
SELECT getNamedScalarOrDefault('missing', 'fallback');
SELECT getNamedScalarOrDefault('missing', NULL) IS NULL;

-- OrDefault falls through to the default for a missing scalar.
SELECT getNamedScalarOrDefault('missing', toUInt32(11));

DROP NAMED SCALAR cv_defined;
