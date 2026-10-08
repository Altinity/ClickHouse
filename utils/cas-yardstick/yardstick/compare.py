"""Compare a run against a reference run of the same shape."""
import os

from . import report as R

CURRENT = 'CURRENT'


def reference_dir(tool_dir: str) -> str:
    """The run directory named by `reference/CURRENT` (relative to `reference/`)."""
    path = os.path.join(tool_dir, 'reference', CURRENT)
    with open(path, encoding='utf-8') as f:
        name = f.read().strip()
    if not name:
        raise ValueError(f'{path} is empty: no reference has been set')
    return os.path.join(tool_dir, 'reference', name)


def compare_runs(run_dir: str, ref_dir: str) -> int:
    """Print the comparison, or only `incompatible: <field>` lines; 1 when incompatible."""
    diff = R.compatible(R.read_manifest(os.path.join(run_dir, 'manifest.tsv')),
                        R.read_manifest(os.path.join(ref_dir, 'manifest.tsv')))
    if diff:
        for field in diff:
            print(f'incompatible: {field}')
        return 1
    print(R.render_compare(R.read_summary(os.path.join(run_dir, 'summary.tsv')),
                           R.read_summary(os.path.join(ref_dir, 'summary.tsv'))), end='')
    return 0
