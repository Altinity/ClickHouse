-- Tags: no-random-settings

-- It is not allowed to have experimental settings enabled by default.

-- However, some settings in the experimental tier are meant to control another experimental feature, and then they can be enabled as long as the feature itself is disabled.
-- These are in the exceptions list inside NOT IN.
<<<<<<< HEAD
SELECT name, value FROM system.settings WHERE tier = 'Experimental' AND type = 'Bool' AND value != '0' AND name NOT IN ('throw_on_unsupported_query_inside_transaction', 'ai_function_throw_on_error', 'ai_function_throw_on_quota_exceeded', 'time_series_prefer_recent_samples_table');
=======
SELECT name, value FROM system.settings WHERE tier = 'Experimental' AND type = 'Bool' AND value != '0' AND name NOT IN (
  'throw_on_unsupported_query_inside_transaction',
  'ai_function_throw_on_error',
  'ai_function_throw_on_quota_exceeded',
-- turned ON for Altinity Antalya builds specifically
  'allow_experimental_iceberg_read_optimization'
);
>>>>>>> f7a9d3433b2 (Merge pull request #2145 from Altinity/feature/antalya-26.6/auto-grp-pr-1687)
SELECT name, value FROM system.merge_tree_settings WHERE tier = 'Experimental' AND type = 'Bool' AND value != '0' AND name NOT IN ('remove_rolled_back_parts_immediately');
