-- Regression guard: no Antalya protocol string may reach `system.query_log` (see
-- `Core/AntalyaProtocol.h`). Only the `ServerHello` carries the marker, and the client strips it, so
-- `client_name` is exactly what an upstream server would store.
--
-- `remote(host, <table function>)` is the one path that sends `query_kind = INITIAL_QUERY`, so the
-- Query packet carries its own `client_name` and `validate_tcp_client_information` compares it
-- against the Hello's. Marking the Hello would fail this query with `CLIENT_INFO_DOES_NOT_MATCH`.

SELECT count() FROM remote('127.0.0.2', numbers(10))
SETTINGS log_queries = 1, log_comment = '05053_antalya_protocol_marker';

SYSTEM FLUSH LOGS query_log;

-- The worker runs in `default` rather than in the test database, so its row is reached through the
-- initiating query's id. Filtering on `log_comment` alone would also pick up an earlier run of this
-- test against a different build, which is exactly what the upgrade check does. `IN` rather than a
-- scalar subquery: the worker's row is also `is_initial_query`, so a second match must narrow the
-- result rather than throw.
SELECT DISTINCT client_name
FROM system.query_log
WHERE event_date >= yesterday()
  AND type = 'QueryFinish'
  AND initial_query_id IN
  (
      SELECT query_id
      FROM system.query_log
      WHERE event_date >= yesterday()
        AND current_database = currentDatabase()
        AND log_comment = '05053_antalya_protocol_marker'
        AND type = 'QueryFinish'
        AND is_initial_query
  )
ORDER BY 1;
