from soak.pool import parse_mc_ls


def test_parse_mc_ls_sums_files():
    out = "\n".join([
        '{"type":"file","key":"soak_pool/blobs/aa","size":100}',
        '{"type":"file","key":"soak_pool/blobs/bb","size":250}',
        '{"type":"folder","key":"soak_pool/trees/"}',
        '{"type":"file","key":"soak_pool/heads/h1","size":50}',
    ])
    objs, total = parse_mc_ls(out)
    assert objs == 3
    assert total == 400


def test_parse_mc_ls_skips_folders_and_blanks():
    out = '\n{"type":"folder","key":"x/"}\n\n{"type":"file","key":"y","size":7}\n'
    assert parse_mc_ls(out) == (1, 7)


def test_parse_mc_ls_tolerates_garbage_lines():
    out = 'not-json\n{"type":"file","key":"y","size":7}\n{bad json}'
    assert parse_mc_ls(out) == (1, 7)


def test_parse_mc_ls_empty():
    assert parse_mc_ls("") == (0, 0)


def test_parse_mc_ls_entry_without_type_but_with_size():
    # mc variants may omit `type` for files; a numeric size still counts.
    out = '{"key":"soak_pool/blobs/aa","size":42}'
    assert parse_mc_ls(out) == (1, 42)
