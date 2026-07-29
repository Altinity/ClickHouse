"""Scenario cards. Importing this package registers every scenario via the @register decorator."""

from . import s01_s02_huge_blob  # noqa: F401
from . import s03_s05_scale  # noqa: F401
from . import s06_s08_manifest_parts  # noqa: F401
from . import s09_s11_mutations  # noqa: F401
from . import s12_s14_faults  # noqa: F401
from . import s15_s18_shards_lifecycle  # noqa: F401
from . import s19_s22_clone_fetch  # noqa: F401
from . import s23_s27_misc  # noqa: F401
from . import s28_s33_corner  # noqa: F401
from . import s34_s35_d1_churn  # noqa: F401
from . import s36_s37_disk_move  # noqa: F401
from . import s38_late_put_injection  # noqa: F401
from . import s39_lease_fault_tolerance  # noqa: F401
from . import s40_insert_dedup_outage  # noqa: F401
from . import s41_wide_insert_baseline  # noqa: F401
from . import s42_alloc_faults  # noqa: F401
from . import s43_same_uuid_recreation  # noqa: F401
