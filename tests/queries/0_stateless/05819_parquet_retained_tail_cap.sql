-- Tags: no-fasttest
-- The footer tail retained by the v3 reader is capped (`Prefetcher::max_retained_tail_size`, 2 MiB). This file is about
-- 8 MiB, and the explicit `input_format_parquet_footer_read_size` of 16 MiB reads all of it, so the retained part starts
-- in the middle of the file: the ranges that lie before it are read from the file, and the ranges inside it are served
-- from memory. The results must be the same as for the values that were written.

insert into function file(currentDatabase() || '_cap_05819.parquet')
    select cityHash64(number) as x from numbers(1000000)
    settings engine_file_truncate_on_insert = 1, output_format_parquet_row_group_size = 1000000, output_format_parquet_data_page_size = 65536;

select count(), sum(x) = (select sum(cityHash64(number)) from numbers(1000000))
from file(currentDatabase() || '_cap_05819.parquet', Parquet, 'x UInt64')
settings storage_file_read_method = 'pread', input_format_parquet_footer_read_size = 16777216;

select count() = (select count() from numbers(1000000) where cityHash64(number) % 3 = 0),
       sum(x) = (select sum(cityHash64(number)) from numbers(1000000) where cityHash64(number) % 3 = 0)
from file(currentDatabase() || '_cap_05819.parquet', Parquet, 'x UInt64')
where x % 3 = 0
settings storage_file_read_method = 'pread', input_format_parquet_footer_read_size = 16777216;
