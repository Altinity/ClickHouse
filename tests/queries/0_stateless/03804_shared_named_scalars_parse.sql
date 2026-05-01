-- Tags: no-parallel
SET allow_experimental_named_scalars = 1;

-- SHARED is a CREATE-time cache-kind modifier and requires
-- <named_scalar_definitions_zookeeper_path>. Without it, CREATE returns a clear
-- configuration error rather than a generic / confusing diagnostic.
CREATE SHARED NAMED SCALAR foo AS SELECT 1; -- {serverError SHARED_NAMED_SCALARS_NOT_CONFIGURED}
CREATE OR REPLACE SHARED NAMED SCALAR foo AS SELECT toUInt64(5); -- {serverError SHARED_NAMED_SCALARS_NOT_CONFIGURED}
CREATE SHARED NAMED SCALAR IF NOT EXISTS foo REFRESH EVERY 1 MINUTE AS SELECT now(); -- {serverError SHARED_NAMED_SCALARS_NOT_CONFIGURED}
DROP SHARED NAMED SCALAR foo; -- {clientError SYNTAX_ERROR}
DROP SHARED NAMED SCALAR IF EXISTS foo; -- {clientError SYNTAX_ERROR}

-- Interpreter rejects ON CLUSTER for SHARED (Keeper already distributes).
CREATE SHARED NAMED SCALAR foo ON CLUSTER x AS SELECT 1; -- {serverError SYNTAX_ERROR}
DROP SHARED NAMED SCALAR foo ON CLUSTER x; -- {clientError SYNTAX_ERROR}

-- TEMPORARY kind is not supported.
CREATE TEMPORARY NAMED SCALAR t AS SELECT 1; -- {clientError SYNTAX_ERROR}

-- Bare identifier is required for scalar names: no compound (dotted) names.
CREATE NAMED SCALAR foo.bar AS SELECT 1; -- {clientError SYNTAX_ERROR}
DROP NAMED SCALAR foo.bar; -- {clientError SYNTAX_ERROR}

-- LOCAL is accepted as an explicit cache-kind modifier.
CREATE LOCAL NAMED SCALAR local_foo AS SELECT 1;
DROP NAMED SCALAR local_foo;
