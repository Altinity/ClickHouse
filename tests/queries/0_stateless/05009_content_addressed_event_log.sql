-- Tags: no-fasttest
-- ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

-- Default-off contract for `system.content_addressed_log`: the per-event content-addressed audit log is
-- opt-in (it mirrors `system.content_addressed_garbage_collection_log`). It is only created and populated
-- when a `<content_addressed_log>` config section is present. The stateless test config does not enable it,
-- so even after we exercise a content-addressed disk end-to-end (INSERT, OPTIMIZE, DROP), the table must not
-- exist and querying it must fail with `UNKNOWN_TABLE` — no log is created or written behind our back.

DROP TABLE IF EXISTS t_cas_event_log;

CREATE TABLE t_cas_event_log (a UInt64, s String)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '05009_content_addressed_event_log',
    path = '05009_content_addressed_event_log_pool/');

-- Exercise the content-addressed write/merge path: this is exactly the work that, when the log is enabled,
-- would emit put/reuse/ref events. With the log unconfigured, none of it may conjure the table into being.
INSERT INTO t_cas_event_log SELECT number, toString(number % 7) FROM numbers(1000);
INSERT INTO t_cas_event_log SELECT number, toString(number % 7) FROM numbers(1000, 1000);
OPTIMIZE TABLE t_cas_event_log FINAL;

SELECT 'rows', count() FROM t_cas_event_log;

-- Flushing the (unconfigured) log is a harmless no-op and must not create the table either.
SYSTEM FLUSH LOGS content_addressed_log;

-- Default-off assertion #1: the table simply does not exist.
EXISTS TABLE system.content_addressed_log;

-- Default-off assertion #2: querying it surfaces UNKNOWN_TABLE rather than an empty result set.
SELECT count() FROM system.content_addressed_log; -- { serverError UNKNOWN_TABLE }

DROP TABLE t_cas_event_log;
SELECT 'ok';
