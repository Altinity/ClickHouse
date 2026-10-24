"""Report arithmetic for the CAS yardstick: counter aggregates, statistics, TSV files, tables.

Requests are counted with the disk-level DiskS3* counters: the generic S3* counters overlap them,
and some CAS paths increment only the disk-level ones."""
import statistics
from collections import OrderedDict

AGGREGATES = {
    'GET': ['DiskS3GetObject'],
    'PUT': ['DiskS3PutObject', 'DiskS3UploadPart', 'DiskS3CreateMultipartUpload', 'DiskS3CompleteMultipartUpload'],
    'HEAD': ['DiskS3HeadObject'],
    'LIST': ['DiskS3ListObjects'],
    'DELETE': ['DiskS3DeleteObjects'],
    'COPY': ['DiskS3CopyObject', 'DiskS3UploadPartCopy'],
    'ABORT': ['DiskS3AbortMultipartUpload'],
    'bytes_read': ['ReadBufferFromS3Bytes'],
    'bytes_written': ['WriteBufferFromS3Bytes'],
}
CAS_DIAGNOSTICS = ['CASRequestAttempt', 'CASRootGet', 'CASRootPut', 'CASBlobPut', 'CASBlobHead', 'CASBlobHeadMiss',
                   'CASManifestPut', 'CASManifestGet', 'CASOtherGet', 'CASHotKeyQueueWaitMicroseconds',
                   'CASRefQueueWaitMicroseconds']
S3_ERROR_COUNTERS = ['S3ReadRequestsErrors', 'S3WriteRequestsErrors',
                     'DiskS3ReadRequestRetryableErrors', 'DiskS3WriteRequestRetryableErrors']
UNAVAILABLE = 'unavailable'
REQUEST_METRICS = ['GET', 'PUT', 'HEAD', 'LIST', 'DELETE', 'COPY', 'ABORT']
RATIO_METRICS = ['wall', 'GET', 'PUT', 'bytes_read', 'bytes_written']
RATIO_COLUMNS = ['wall', 'GET', 'PUT', 'bytes_r', 'bytes_w']
RATIO_PAIRS = [('cas', 's3'), ('cas', 's3plain'), ('cas_cached', 's3_cached'), ('cas_cached', 's3plain_cached')]
BYTES_METRICS = ['bytes_read', 'bytes_written']
COMPAT_FIELDS = ['workload_hash', 'scale', 'counts', 'gc', 'repeats', 'config_hash', 'build_type', 'store',
                 'store_version']
FAILED = 'FAILED'

SAMPLE_COLUMNS = ['policy', 'card', 'phase', 'repeat', 'metric', 'value', 'unit_count']
SUMMARY_COLUMNS = ['policy', 'card', 'phase', 'metric', 'median', 'min', 'max', 'repeats', 'per_unit', 'mark']
_TEXT_COLUMNS = {'policy', 'card', 'phase', 'metric', 'mark'}
TABLE_COLUMNS = ['wall', 'median_op_ms', 'p90_op_ms', 'GET', 'PUT', 'HEAD', 'LIST', 'DELETE', 'COPY', 'MiB_r', 'MiB_w', 'gc_rounds']


def delta(before, after):
    return {k: after[k] - before[k] for k in after if k in before}


def aggregate(d, present):
    out = {}
    for metric, counters in AGGREGATES.items():
        out[metric] = sum(d.get(c, 0) for c in counters) if all(c in present for c in counters) else UNAVAILABLE
    return out


def summarize(samples):
    return statistics.median(samples), min(samples), max(samples)


def _ratios(num, den):
    """(median, min, max) of per-repeat ratios, or the text '0 → n' when any denominator is zero."""
    if len(num) != len(den) or not num:
        raise ValueError(f'ratio needs equal, non-empty samples: {len(num)} vs {len(den)}')
    if any(d == 0 for d in den):
        return '0 → ' + _plain(statistics.median(num))
    return summarize([n / d for n, d in zip(num, den)])


def mark_requests(samples):
    return '~' if len(set(samples)) > 1 else ''


def mark_spread(samples):
    lo, hi = min(samples), max(samples)
    return 'noisy' if (lo == 0 < hi) or (lo > 0 and hi / lo > 1.2) else ''


def per_unit(value, unit_count):  # callers store '' instead when unit_count is 0
    return value / unit_count


def _parse_number(s):
    if s == '':
        return ''
    try:
        return int(s)
    except ValueError:
        pass
    try:
        return float(s)
    except ValueError:
        return s


def _format(v):
    if not isinstance(v, str):
        return repr(v)
    if '\t' in v or '\n' in v:
        raise ValueError(f'tab or newline in TSV value: {v!r}')
    return v


def _write_tsv(path, columns, rows):
    with open(path, 'w', encoding='utf-8') as f:
        f.write('\t'.join(columns) + '\n')
        for r in rows:
            f.write('\t'.join(_format(r[c]) for c in columns) + '\n')


def _read_tsv(path, columns):
    with open(path, encoding='utf-8') as f:
        lines = f.read().split('\n')
    header = lines[0].split('\t')
    if header != columns:
        raise ValueError(f'{path}: unexpected header {header}')
    rows = []
    for line in lines[1:]:
        if line == '':
            continue
        cells = line.split('\t')
        rows.append({c: (v if c in _TEXT_COLUMNS else _parse_number(v)) for c, v in zip(columns, cells)})
    return rows


def write_samples(path, rows):
    _write_tsv(path, SAMPLE_COLUMNS, rows)


def read_samples(path):
    return _read_tsv(path, SAMPLE_COLUMNS)


def write_summary(path, summary):
    _write_tsv(path, SUMMARY_COLUMNS, summary)


def read_summary(path):
    return _read_tsv(path, SUMMARY_COLUMNS)


def _ratio_rows(rows, pairs, strict):
    by = {}
    for r in rows:
        by.setdefault((r['policy'], r['card'], r['phase']), {}).setdefault(r['metric'], {})[r['repeat']] = r['value']
    out = []
    for (policy, card, phase), metrics in by.items():
        for num_policy, den_policy in pairs:
            if policy != num_policy or (den_policy, card, phase) not in by:
                continue
            other = by[(den_policy, card, phase)]
            if 'status' in metrics or 'status' in other:
                continue
            for m in RATIO_METRICS:
                if m not in metrics or m not in other:
                    continue
                if set(metrics[m]) != set(other[m]):
                    if not strict:
                        continue
                    raise ValueError(f'repeat sets differ for {card}/{phase}/{m}: {num_policy} vs {den_policy}')
                repeats = sorted(metrics[m])
                num, den = [metrics[m][i] for i in repeats], [other[m][i] for i in repeats]
                if any(isinstance(v, str) for v in num + den):
                    continue
                r = _ratios(num, den)
                lo_hi = (r, r) if isinstance(r, str) else r[1:]
                out.append(dict(policy=f'{num_policy}/{den_policy}', card=card, phase=phase,
                                metric=f'ratio:{num_policy}/{den_policy}:{m}', median=r if isinstance(r, str) else r[0],
                                min=lo_hi[0], max=lo_hi[1], repeats=len(repeats), per_unit='', mark=''))
    return out


def _failed_passes(rows):
    """(policy, card, phase) -> 'FAILED (n of K repeats ok)' for every pass with a FAILED repeat."""
    repeats, failed = {}, {}
    for r in rows:
        key = (r['policy'], r['card'], r['phase'])
        repeats.setdefault(key, set()).add(r['repeat'])
        if r['metric'] == 'status' and r['value'] == FAILED:
            failed.setdefault(key, set()).add(r['repeat'])
    return {key: f'{FAILED} ({len(repeats[key]) - len(ks)} of {len(repeats[key])} repeats ok)'
            for key, ks in failed.items()}


def build_summary(rows, pairs=RATIO_PAIRS, strict=True):
    """Median, min and max per (policy, card, phase, metric), then the paired ratio rows.
    A pass with any FAILED repeat keeps only its status row: survivors of a failed pass are not a sample.
    `strict` = False drops the ratios of policies whose repeat sets differ instead of raising."""
    failed = _failed_passes(rows)
    groups = OrderedDict()
    for r in rows:
        if (r['policy'], r['card'], r['phase']) in failed:
            r = dict(r, metric='status', value=failed[(r['policy'], r['card'], r['phase'])])
        groups.setdefault((r['policy'], r['card'], r['phase'], r['metric']), []).append(r)
    out = []
    for (policy, card, phase, metric), rs in groups.items():
        key = dict(policy=policy, card=card, phase=phase, metric=metric)
        values = [r['value'] for r in rs]
        text = [v for v in values if isinstance(v, str)]
        if text:  # a text value (status, unavailable) in any repeat replaces the whole cell
            repeats = len({r['repeat'] for r in rs})
            out.append(dict(key, median=text[0], min=text[0], max=text[0], repeats=repeats, per_unit='', mark=''))
            continue
        units = {r['unit_count'] for r in rs}
        if len(units) != 1:
            raise ValueError(f'unit_count differs within {key}: {sorted(units)}')
        median, lo, hi = summarize(values)
        if metric in REQUEST_METRICS:
            mark = mark_requests(values)
        elif metric == 'wall' or metric in BYTES_METRICS:
            mark = mark_spread(values)
        else:
            mark = ''
        unit = units.pop()
        out.append(dict(key, median=median, min=lo, max=hi, repeats=len(rs),
                        per_unit=per_unit(median, unit) if unit else '', mark=mark))
    return out + _ratio_rows(rows, pairs, strict)


def _index(summary):
    return {(r['policy'], r['card'], r['phase'], r['metric']): r for r in summary}


def _plain(v):
    return f'{v:.6f}'.rstrip('0').rstrip('.')


def _cards(summary):
    return list(OrderedDict(((r['card'], r['phase']), None) for r in summary))


def _num(v, metric):
    if metric in ('wall', 'median_op_ms', 'p90_op_ms'):
        return f'{v:.2f}'
    if metric in ('MiB_r', 'MiB_w'):
        return f'{v:.1f}'
    return str(int(v)) if v == int(v) else f'{v:.2f}'


def _cell(idx, policy, card, phase, metric):
    status = idx.get((policy, card, phase, 'status'))
    if status is not None:
        return status['median']
    if metric == 'gc_rounds':
        return _gc_cell(idx, policy, card, phase)
    source = {'MiB_r': 'bytes_read', 'MiB_w': 'bytes_written'}.get(metric, metric)
    r = idx.get((policy, card, phase, source))
    if r is None:
        return '-'
    if isinstance(r['median'], str):
        return r['median']
    scale = (1 << 20) if source != metric else 1
    text = _range(r, metric, scale)
    return text + ('' if r['mark'] in ('', 'noisy') else r['mark']) + (' noisy' if r['mark'] == 'noisy' else '')


def _range(r, metric, scale=1):
    text = _num(r['median'] / scale, metric)
    if r['min'] != r['max']:
        text += f' ({_num(r["min"] / scale, metric)}..{_num(r["max"] / scale, metric)})'
    return text


def _gc_cell(idx, policy, card, phase):
    """`N, M overlapping`: N rounds began and ended inside the window, M only overlapped it."""
    inside = idx.get((policy, card, phase, 'gc_rounds'))
    over = idx.get((policy, card, phase, 'gc_rounds_overlapping'))
    if inside is None or over is None:
        return '-'
    return f'{_range(inside, "gc_rounds")}, {_range(over, "gc_rounds")} overlapping'


def _ratio_cell(idx, num_policy, den_policy, card, phase, metric):
    r = idx.get((f'{num_policy}/{den_policy}', card, phase, f'ratio:{num_policy}/{den_policy}:{metric}'))
    if r is None:
        return '-'
    if isinstance(r['median'], str):
        return r['median']
    return f'{r["median"]:.2f} ({r["min"]:.2f}..{r["max"]:.2f})'


def _grid(rows):
    widths = [max(len(r[i]) for r in rows) for i in range(len(rows[0]))]
    return '\n'.join('  '.join(c.ljust(w) for c, w in zip(r, widths)).rstrip() for r in rows)


def _columns(policy):
    return TABLE_COLUMNS if policy.startswith('cas') else [c for c in TABLE_COLUMNS if c != 'gc_rounds']


def render_header(manifest, summary):
    """Run parameters, the GC label, and every metric the binary could not measure."""
    lines = [f'scale {manifest.get("scale")}, repeats {manifest.get("repeats")}, gc {manifest.get("gc")}']
    if manifest.get('gc') == 'off':
        lines.append('GC-disabled process window: CAS runs with cas_gc_enabled = 0; mount renewal still runs')
    missing = [' '.join((r['policy'], r['card'], r['phase'], r['metric'])) for r in summary
               if r['median'] == UNAVAILABLE]
    lines.append('unavailable: ' + (', '.join(missing) if missing else 'none'))
    return '\n'.join(lines) + '\n\n'


def render_table(summary, policies_uncached, policies_cached, units=None):
    """Rows are card x phase; one column group per policy, then one ratio group per pair named by the ratio rows.
    `units` maps a card to the unit its per-unit line names."""
    idx = _index(summary)
    blocks = []
    for title, policies in (('uncached', policies_uncached), ('cached', policies_cached)):
        cards = _cards([r for r in summary if r['policy'] in policies])
        if not policies or not cards:
            continue
        named = OrderedDict((tuple(r['policy'].split('/')), None) for r in summary if r['metric'].startswith('ratio:'))
        pairs = [(n, d) for n, d in named if n in policies and d in policies]
        top = ['']
        sub = ['card/phase']
        for p in policies:
            top += [p] + [''] * (len(_columns(p)) - 1)
            sub += _columns(p)
        for n, d in pairs:
            top += [f'{n}/{d}'] + [''] * (len(RATIO_COLUMNS) - 1)
            sub += RATIO_COLUMNS
        grid = [top, sub]
        for card, phase in cards:
            row = [f'{card}/{phase}']
            for p in policies:
                row += [_cell(idx, p, card, phase, c) for c in _columns(p)]
            for n, d in pairs:
                row += [_ratio_cell(idx, n, d, card, phase, m) for m in RATIO_METRICS]
            grid.append(row)
        lines = [title, _grid(grid)]
        for card, phase in cards:
            for p in policies:
                unit = [f'{m}={_plain(idx[(p, card, phase, m)]["per_unit"])}' for m in ['wall'] + REQUEST_METRICS
                        if (p, card, phase, m) in idx and isinstance(idx[(p, card, phase, m)]['per_unit'], float)]
                if unit:
                    lines.append(f'per {(units or {}).get(card, "unit")} {card}/{phase} {p}: ' + ' '.join(unit))
        blocks.append('\n'.join(lines))
    return '\n\n'.join(blocks) + '\n'


def render_diagnostics(summary):
    idx = _index(summary)
    policies = list(OrderedDict((r['policy'], None) for r in summary
                                if r['policy'].startswith('cas') and '/' not in r['policy']))
    lines = []
    for card, phase in _cards(summary):
        for p in policies:
            for m in CAS_DIAGNOSTICS + S3_ERROR_COUNTERS:
                r = idx.get((p, card, phase, m))
                if r is not None:
                    lines.append(f'{p} {card} {phase} {m}: {r["median"]} ({r["min"]}..{r["max"]})')
    return '\n'.join(lines) + '\n'


def write_manifest(path, fields):
    _write_tsv(path, ['field', 'value'], [dict(field=k, value=str(v)) for k, v in fields.items()])


def read_manifest(path):
    with open(path, encoding='utf-8') as f:
        lines = f.read().split('\n')
    if lines[0] != 'field\tvalue':
        raise ValueError(f'{path}: unexpected header {lines[0]!r}')
    return dict(line.split('\t', 1) for line in lines[1:] if line)


def compatible(m1, m2):
    return [f for f in COMPAT_FIELDS if f not in m1 or f not in m2 or m1[f] != m2[f]]


def render_compare(cur_summary, ref_summary):
    ref = _index(ref_summary)
    cur = _index(cur_summary)
    lines = []
    for k, r in cur.items():
        name = ' '.join(k)
        o = ref.get(k)
        if o is None:
            lines.append(f'{name}: no reference !')
            continue
        head = f'{name}: cur {r["median"]} ({r["min"]}..{r["max"]}) vs ref {o["median"]} ({o["min"]}..{o["max"]})'
        if isinstance(r['median'], str) or isinstance(o['median'], str):
            lines.append(head + ('' if r['median'] == o['median'] else ' !'))
            continue
        pct = (r['median'] - o['median']) / o['median'] * 100 if o['median'] else None
        text = ' n/a' if pct is None else f' {pct:+.1f}%'
        flag = (k[3] == 'wall' and pct is not None and abs(pct) > 10) or \
               (k[3] in REQUEST_METRICS and r['median'] != o['median'])
        lines.append(head + text + (' !' if flag else ''))
    lines += [f'{" ".join(k)}: no current !' for k in ref if k not in cur]
    return '\n'.join(lines) + '\n'
