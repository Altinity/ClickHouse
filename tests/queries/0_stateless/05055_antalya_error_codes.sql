SELECT name, code FROM system.errors
WHERE name IN ('CATALOG_NAMESPACE_DISABLED', 'PENDING_MUTATIONS_NOT_ALLOWED',
    'EXPORT_PARTITION_ALREADY_EXPORTED', 'PARTITION_EXPORT_FAILED',
    'CAS_WRITE_UNATTRIBUTED', 'CAS_DELETE_MARKER') AND remote = 0
ORDER BY code SETTINGS system_events_show_zero_values = 1;

SELECT errorCodeToName(number) FROM numbers(10001, 6);
SELECT errorCodeToName(toInt32(-1)), errorCodeToName(10000), errorCodeToName(10007), errorCodeToName(2147483647);

SET allow_custom_error_code_in_throwif = 1;
SELECT throwIf(1, 'Antalya namespace regression', toInt32(10001)); -- { serverError CATALOG_NAMESPACE_DISABLED }
SELECT throwIf(1, 'Antalya namespace regression', toInt32(10006)); -- { serverError CAS_DELETE_MARKER }
SELECT throwIf(1, 'Unknown Antalya code', toInt32(10007)); -- { serverError 10007 }
