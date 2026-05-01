-- Tags: no-parallel
SET allow_experimental_named_scalars = 1;

DROP NAMED SCALAR IF EXISTS cv_test;
DROP NAMED SCALAR IF EXISTS cv_ref;
DROP NAMED SCALAR IF EXISTS cv_big;
DROP NAMED SCALAR IF EXISTS cv_fixed;
DROP NAMED SCALAR IF EXISTS cv_const;
DROP NAMED SCALAR IF EXISTS cv_local;

CREATE NAMED SCALAR cv_test AS SELECT toUInt32(1);
SELECT getNamedScalar('cv_test');

SELECT name, kind, value, type, has_value, current_value_is_valid
FROM system.named_scalars
WHERE name = 'cv_test'
ORDER BY name;

CREATE OR REPLACE NAMED SCALAR cv_test AS SELECT toUInt32(2);
SELECT getNamedScalar('cv_test');

-- OR REPLACE is a clean swap: incompatible types are fine, the new SELECT
-- result is whatever lives there now.
CREATE OR REPLACE NAMED SCALAR cv_test AS SELECT 'x';
SELECT getNamedScalar('cv_test');
SELECT toTypeName(getNamedScalar('cv_test'));

CREATE NAMED SCALAR cv_ref AS SELECT getNamedScalar('cv_test'); -- {serverError BAD_ARGUMENTS}
CREATE NAMED SCALAR cv_ref AS SELECT getNamedScalarOrDefault('cv_test', toUInt32(0)); -- {serverError BAD_ARGUMENTS}
CREATE NAMED SCALAR cv_ref SQL SECURITY INVOKER AS SELECT toUInt32(1); -- {serverError BAD_ARGUMENTS}
CREATE NAMED SCALAR cv_ref SQL SECURITY NONE AS SELECT toUInt32(1); -- {serverError BAD_ARGUMENTS}
SELECT getNamedScalar('missing'); -- {serverError NAMED_SCALAR_NOT_FOUND}

CREATE NAMED SCALAR cv_test AS SELECT toUInt32(3); -- {serverError NAMED_SCALAR_ALREADY_EXISTS}
CREATE NAMED SCALAR IF NOT EXISTS cv_test AS SELECT toUInt32(4);
SELECT getNamedScalar('cv_test');

DROP NAMED SCALAR cv_test;
DROP NAMED SCALAR cv_test; -- {serverError NAMED_SCALAR_NOT_FOUND}
DROP NAMED SCALAR IF EXISTS cv_test;

CREATE NAMED SCALAR cv_big AS SELECT repeat('x', 2 * 1024 * 1024); -- {serverError TOO_LARGE_STRING_SIZE}

-- Existence check must fire before the expression is evaluated so that both
-- `IF NOT EXISTS` and duplicate CREATE are no-ops / clean errors even when
-- the new expression is invalid.
CREATE NAMED SCALAR cv_fixed AS SELECT toUInt32(1);
CREATE NAMED SCALAR IF NOT EXISTS cv_fixed AS (SELECT * FROM nonexistent_table);
SELECT getNamedScalar('cv_fixed');

CREATE NAMED SCALAR cv_fixed AS (SELECT * FROM nonexistent_table); -- {serverError NAMED_SCALAR_ALREADY_EXISTS}
SELECT getNamedScalar('cv_fixed');
DROP NAMED SCALAR cv_fixed;

-- Server named_scalars always persist their value; constant expressions are allowed.
CREATE NAMED SCALAR cv_const AS SELECT 42;
SELECT getNamedScalar('cv_const');
DROP NAMED SCALAR cv_const;

-- LOCAL is an explicit cache-kind modifier, equivalent to the default local cache.
CREATE LOCAL NAMED SCALAR cv_local AS SELECT toUInt16(7);
SELECT getNamedScalar('cv_local');
DROP NAMED SCALAR cv_local;
