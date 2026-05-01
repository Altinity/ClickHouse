-- Tags: no-parallel
SET allow_experimental_named_scalars = 1;
-- CREATE OR REPLACE that changes cache kind must be rejected.
-- Without Keeper, CREATE SHARED throws SHARED_NAMED_SCALARS_NOT_CONFIGURED before
-- reaching the kind-change check. We verify the LOCAL → SHARED path fires the right
-- error (BAD_ARGUMENTS) when the kind-change check can run (i.e. before ensureCreatable).
-- In a no-Keeper environment ensureCreatable fires first; the BAD_ARGUMENTS path is
-- exercised by integration tests with Keeper configured.

DROP NAMED SCALAR IF EXISTS cv_kind_local;
DROP NAMED SCALAR IF EXISTS cv_kind_shared;

-- Local→Shared: without Keeper the node rejects SHARED before evaluating kind-change.
-- Both BAD_ARGUMENTS and SHARED_NAMED_SCALARS_NOT_CONFIGURED are acceptable here.
CREATE LOCAL NAMED SCALAR cv_kind_local AS SELECT toUInt32(1);
CREATE OR REPLACE SHARED NAMED SCALAR cv_kind_local AS SELECT toUInt32(2); -- { serverError BAD_ARGUMENTS,SHARED_NAMED_SCALARS_NOT_CONFIGURED }

-- Original definition is intact after rejection.
SELECT getNamedScalar('cv_kind_local');
SELECT toTypeName(getNamedScalar('cv_kind_local'));

DROP NAMED SCALAR cv_kind_local;
