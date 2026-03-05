-- Tags: no-random-settings

-- It is not allowed to have experimental settings enabled by default.

-- However, some settings in the experimental tier are meant to control another experimental feature, and then they can be enabled as long as the feature itself is disabled.
-- These are in the exceptions list inside NOT IN.
<<<<<<< HEAD
SELECT name, value FROM system.settings WHERE tier = 'Experimental' AND type = 'Bool' AND value != '0' AND name NOT IN ('throw_on_unsupported_query_inside_transaction', 'ai_function_throw_on_error', 'ai_function_throw_on_quota_exceeded');
=======
SELECT name, value FROM system.settings WHERE tier = 'Experimental' AND type = 'Bool' AND value != '0' AND name NOT IN (
  'throw_on_unsupported_query_inside_transaction',
  'allow_experimental_export_merge_tree_part',
-- turned ON for Altinity Antalya builds specifically
  'allow_experimental_iceberg_read_optimization'
);
>>>>>>> 05010e84270 (Merge pull request #1414 from Altinity/frontport/antalya-26.1/rendezvous_hashing)
SELECT name, value FROM system.merge_tree_settings WHERE tier = 'Experimental' AND type = 'Bool' AND value != '0' AND name NOT IN ('remove_rolled_back_parts_immediately');
