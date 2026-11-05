-- Tags: no-fasttest, no-random-settings
-- no-fasttest: needs the `Parquet` format, which is not built in fasttest.
-- no-random-settings: the last statement checks that `compatibility` = '26.6' restores 65536.
-- Tests `input_format_parquet_footer_read_size`: the v3 reader sizes the initial footer read
-- adaptively when the setting is 0 (default), and uses the explicit value otherwise. Every
-- value - the adaptive default, a huge fixed read that covers the whole footer in one go, and a
-- tiny fixed read that forces the second (metadata_size + 8 > initial_read_size) read path - must
-- produce identical, correct results.

insert into function file(currentDatabase() || '_data_05058.parquet')
    select number as x, toString(number) as s from numbers(10000)
    settings engine_file_truncate_on_insert = 1;

-- Adaptive (default 0): 1% clamped to [128 KiB, 2 MiB].
select count(), sum(x), min(s), max(s) from file(currentDatabase() || '_data_05058.parquet', auto, 'x UInt64, s String')
    settings input_format_parquet_footer_read_size = 0;

-- Explicit large read: footer certainly fits in the initial read.
select count(), sum(x), min(s), max(s) from file(currentDatabase() || '_data_05058.parquet', auto, 'x UInt64, s String')
    settings input_format_parquet_footer_read_size = 8388608;

-- Explicit tiny read: forces the second read to fetch the rest of the footer.
select count(), sum(x), min(s), max(s) from file(currentDatabase() || '_data_05058.parquet', auto, 'x UInt64, s String')
    settings input_format_parquet_footer_read_size = 16;

-- Explicit value larger than the file: clamped to the file size, still correct.
select count(), sum(x), min(s), max(s) from file(currentDatabase() || '_data_05058.parquet', auto, 'x UInt64, s String')
    settings input_format_parquet_footer_read_size = 1073741824;

-- Explicit value below the 8-byte trailer: bumped up to 8, no out-of-bounds read.
select count(), sum(x), min(s), max(s) from file(currentDatabase() || '_data_05058.parquet', auto, 'x UInt64, s String')
    settings input_format_parquet_footer_read_size = 1;

-- Compatibility contract: the setting is new in 26.6.4 Antalya. `SET compatibility` to 26.6 must
-- restore the previous fixed 64 KiB footer read, not the new adaptive default (0).
set compatibility = '26.6';
select getSetting('input_format_parquet_footer_read_size');
