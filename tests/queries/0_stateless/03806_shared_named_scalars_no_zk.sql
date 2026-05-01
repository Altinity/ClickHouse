-- Tags: no-parallel
SET allow_experimental_named_scalars = 1;

-- SHARED cache creates produce a clear configuration error when
-- <named_scalar_definitions_zookeeper_path> is not set.
CREATE SHARED NAMED SCALAR z AS SELECT toUInt64(1); -- {serverError SHARED_NAMED_SCALARS_NOT_CONFIGURED}
CREATE OR REPLACE SHARED NAMED SCALAR z AS SELECT toUInt64(1); -- {serverError SHARED_NAMED_SCALARS_NOT_CONFIGURED}
CREATE SHARED NAMED SCALAR IF NOT EXISTS z AS SELECT toUInt64(1); -- {serverError SHARED_NAMED_SCALARS_NOT_CONFIGURED}
DROP SHARED NAMED SCALAR z; -- {clientError SYNTAX_ERROR}
DROP NAMED SCALAR z; -- {serverError NAMED_SCALAR_NOT_FOUND}
DROP NAMED SCALAR IF EXISTS z;

-- SYSTEM REFRESH for a never-created scalar is an in-memory "not found"
-- error, so no Keeper configuration is required to recognise it.
SYSTEM REFRESH NAMED SCALAR never_existed; -- {serverError NAMED_SCALAR_NOT_FOUND}
SYSTEM STOP NAMED SCALAR REFRESHES never_existed; -- {serverError NAMED_SCALAR_NOT_FOUND}
SYSTEM START NAMED SCALAR REFRESHES never_existed; -- {serverError NAMED_SCALAR_NOT_FOUND}

-- SYSTEM REFRESH TEMPORARY NAMED SCALAR is grammar-level forbidden (no such grammar).
SYSTEM REFRESH TEMPORARY NAMED SCALAR anything; -- {clientError SYNTAX_ERROR}
