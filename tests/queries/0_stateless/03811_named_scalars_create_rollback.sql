-- Tags: no-parallel
SET allow_experimental_named_scalars = 1;
--
-- CREATE NAMED SCALAR runs the initial SELECT before publishing the
-- definition. If the SELECT throws, the CREATE must roll back cleanly:
-- no definition is durable, no scalar appears in the catalog, and a
-- subsequent CREATE with the same name on a working SELECT must
-- succeed (i.e. the orphaned value blob from the failed first attempt
-- did not block reuse of that name).

DROP NAMED SCALAR IF EXISTS cv_rollback;

-- Initial-evaluate failure: CREATE throws and leaves nothing behind.
CREATE NAMED SCALAR cv_rollback AS SELECT throwIf(1, 'eval_failed'); -- { serverError FUNCTION_THROW_IF_VALUE_IS_NON_ZERO }

-- The catalog must be empty for this name.
SELECT count() FROM system.named_scalars WHERE name = 'cv_rollback';

-- Looking the scalar up by name reports "not found", not "no value".
SELECT getNamedScalar('cv_rollback'); -- { serverError NAMED_SCALAR_NOT_FOUND }

-- Retry with a working SELECT must succeed: the orphan value blob
-- from the first attempt has been cleaned up, and even if it wasn't,
-- the new attempt mints a fresh UUID so the slot does not collide.
CREATE NAMED SCALAR cv_rollback AS SELECT toUInt32(7);
SELECT getNamedScalar('cv_rollback');

DROP NAMED SCALAR cv_rollback;
