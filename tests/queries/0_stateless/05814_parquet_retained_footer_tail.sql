-- Tags: no-fasttest
-- The Column Index and the Offset Index of a Parquet file are just below the footer, so they are inside the tail
-- that the v3 reader reads to parse the footer. The reader keeps that tail and serves these ranges from it, instead
-- of reading them again. This test checks that the retained tail is used when the footer fits into the tail read and
-- is not used when it does not, and that the results are the same either way.
-- Every query reads its own copy of the file, because a cached footer would skip reading the footer at all.

insert into function file(currentDatabase() || '_src_05814.parquet')
    select number as x, toString(number) as s from numbers(100000)
    settings engine_file_truncate_on_insert = 1, output_format_parquet_write_page_index = 1,
             output_format_parquet_data_page_size = 4096, output_format_parquet_row_group_size = 50000;
insert into function file(currentDatabase() || '_a_05814.parquet') select * from file(currentDatabase() || '_src_05814.parquet')
    settings engine_file_truncate_on_insert = 1, output_format_parquet_write_page_index = 1,
             output_format_parquet_data_page_size = 4096, output_format_parquet_row_group_size = 50000;
insert into function file(currentDatabase() || '_b_05814.parquet') select * from file(currentDatabase() || '_src_05814.parquet')
    settings engine_file_truncate_on_insert = 1, output_format_parquet_write_page_index = 1,
             output_format_parquet_data_page_size = 4096, output_format_parquet_row_group_size = 50000;
insert into function file(currentDatabase() || '_c_05814.parquet') select * from file(currentDatabase() || '_src_05814.parquet')
    settings engine_file_truncate_on_insert = 1, output_format_parquet_write_page_index = 1,
             output_format_parquet_data_page_size = 4096, output_format_parquet_row_group_size = 50000;
insert into function file(currentDatabase() || '_d_05814.parquet') select * from file(currentDatabase() || '_src_05814.parquet')
    settings engine_file_truncate_on_insert = 1, output_format_parquet_write_page_index = 1,
             output_format_parquet_data_page_size = 4096, output_format_parquet_row_group_size = 50000;

-- Reads without a filter use the Offset Index.
select count(), sum(x), min(s), max(s) from file(currentDatabase() || '_a_05814.parquet', Parquet, 'x UInt64, s String')
    settings storage_file_read_method = 'pread', log_comment = '05814_default';
-- Page filter push-down uses the Column Index too.
select count(), sum(x) from file(currentDatabase() || '_b_05814.parquet', Parquet, 'x UInt64, s String')
    where x between 70000 and 70100
    settings storage_file_read_method = 'pread', input_format_parquet_page_filter_push_down = 1, log_comment = '05814_filter';
-- The tail covers 16 bytes only: the footer needs the second read, nothing is retained.
select count(), sum(x), min(s), max(s) from file(currentDatabase() || '_c_05814.parquet', Parquet, 'x UInt64, s String')
    settings storage_file_read_method = 'pread', input_format_parquet_footer_read_size = 16, log_comment = '05814_small_tail';
select count(), sum(x) from file(currentDatabase() || '_d_05814.parquet', Parquet, 'x UInt64, s String')
    where x between 70000 and 70100
    settings storage_file_read_method = 'pread', input_format_parquet_page_filter_push_down = 1,
             input_format_parquet_footer_read_size = 16, log_comment = '05814_small_tail_filter';

system flush logs query_log;
select
    log_comment,
    ProfileEvents['ParquetMetadataCacheMisses'] as footer_reads,
    ProfileEvents['ParquetPrefetcherServedFromRetainedTail'] > 0 as served_from_retained_tail,
    ProfileEvents['ParquetFooterSecondRead'] as footer_second_reads
from system.query_log
where event_date >= yesterday() and current_database = currentDatabase() and type = 'QueryFinish'
      and log_comment like '05814\_%' and query not like 'insert%'
order by log_comment;
