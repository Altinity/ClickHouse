-- No Antalya protocol string may reach `system.query_log`: only the `ServerHello` carries the
-- marker, and the client strips it. See `Core/AntalyaProtocol.h`.

SELECT count() FROM remote('127.0.0.2', numbers(10))
SETTINGS log_queries = 1, log_comment = '05053_antalya_protocol_marker';

SYSTEM FLUSH LOGS query_log;

-- The worker runs in `default` rather than in the test database, so its row is reached through the
-- initiating query's id. `IN` rather than a scalar subquery: the worker's row is also
-- `is_initial_query`, so a second match must narrow the result rather than throw.
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
