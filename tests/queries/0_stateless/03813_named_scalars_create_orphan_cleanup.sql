-- Tags: no-parallel
SET allow_experimental_named_scalars = 1;
-- A second CREATE on an existing name (without OR REPLACE) must throw
-- and not leave the value backend in a state that prevents future
-- CREATE/DROP from working. (Hard to verify orphan cleanup directly
-- via SQL — this test mostly ensures end-to-end resilience.)

DROP NAMED SCALAR IF EXISTS cv_orphan;

CREATE NAMED SCALAR cv_orphan AS SELECT toUInt32(1);
CREATE NAMED SCALAR cv_orphan AS SELECT toUInt32(2); -- { serverError NAMED_SCALAR_ALREADY_EXISTS }
SELECT getNamedScalar('cv_orphan');

-- DROP and recreate works.
DROP NAMED SCALAR cv_orphan;
CREATE NAMED SCALAR cv_orphan AS SELECT toUInt32(99);
SELECT getNamedScalar('cv_orphan');

DROP NAMED SCALAR cv_orphan;
