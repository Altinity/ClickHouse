"""Server configuration for one disk under test per process."""

import hashlib
import os
import shutil

POLICIES = ['cas', 's3', 's3plain', 'cas_cached', 's3_cached', 's3plain_cached']

ETC_SRC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'etc')

ACCESS = '''<access_key_id>clickhouse</access_key_id>
        <secret_access_key>clickhouse</secret_access_key>
        <http_keep_alive_timeout>30</http_keep_alive_timeout>
        <http_keep_alive_max_requests>10000</http_keep_alive_max_requests>'''

# {endpoint}, {gc}, {meta} are filled in by write_config.
POLICY_DISK = {
    'cas': '''<type>object_storage</type>
        <object_storage_type>s3</object_storage_type>
        <metadata_type>cas</metadata_type>
        <endpoint>{endpoint}</endpoint>
        ''' + ACCESS + '''
        <cas_server_root_id>yardstick</cas_server_root_id>
        <cas_scratch_path>cas_scratch/</cas_scratch_path>
        <cas_gc_enabled>{gc}</cas_gc_enabled>
        <cas_gc_interval_sec>20</cas_gc_interval_sec>''',
    's3': '''<type>s3</type>
        <endpoint>{endpoint}</endpoint>
        ''' + ACCESS + '''
        <metadata_path>{meta}</metadata_path>''',
    's3plain': '''<type>object_storage</type>
        <object_storage_type>s3</object_storage_type>
        <metadata_type>plain_rewritable</metadata_type>
        <endpoint>{endpoint}</endpoint>
        ''' + ACCESS,
}

CACHE_DISK = '''<type>cache</type>
        <disk>{disk}</disk>
        <path>{disk}_cache/</path>
        <max_size>1Gi</max_size>
        <cache_on_write_operations>1</cache_on_write_operations>
        <background_download_threads>0</background_download_threads>
        <background_download_queue_size_limit>0</background_download_queue_size_limit>
        <load_metadata_asynchronously>0</load_metadata_asynchronously>'''

TEMPLATE = '''<clickhouse>
  <path>{run_dir}/data/</path>
  <tmp_path>{run_dir}/tmp/</tmp_path>
  <user_files_path>{run_dir}/uf/</user_files_path>
  <format_schema_path>{run_dir}/fs/</format_schema_path>
  <storage_configuration>
    <disks>
      <default/>
      <{disk}>
        {disk_body}
      </{disk}>{cache_block}
    </disks>
    <policies>
      <{policy}>
        <volumes>
          <main>
            <disk>{outer}</disk>
          </main>
        </volumes>
      </{policy}>
    </policies>
  </storage_configuration>
</clickhouse>
'''


def write_config(run_dir: str, policy: str, endpoint: str, prefix: str, gc_enabled: bool) -> str:
    """Write the server config under run_dir/etc; return the config hash.

    The hash covers the config files with run_dir and the bucket prefix masked,
    so passes of the same configuration hash equal.
    """
    if policy not in POLICIES:
        raise ValueError(f'unknown policy {policy!r}')
    run_dir = os.path.abspath(run_dir)
    disk = policy.replace('_cached', '')
    cached = policy.endswith('_cached')
    etc = os.path.join(run_dir, 'etc')
    os.makedirs(os.path.join(etc, 'config.d'), exist_ok=True)
    for sub in ('data', 'tmp', 'uf', 'caches'):
        os.makedirs(os.path.join(run_dir, sub), exist_ok=True)
    for name in ('config.xml', 'users.xml'):
        shutil.copyfile(os.path.join(ETC_SRC, name), os.path.join(etc, name))

    base = endpoint.rstrip('/')
    full_endpoint = f"{base}/{prefix.strip('/')}/"
    body = POLICY_DISK[disk].format(
        endpoint=full_endpoint,
        gc='1' if gc_enabled else '0',
        meta=os.path.join(run_dir, 's3_meta') + '/',
    )
    cache_block = ''
    if cached:
        cache_block = f'\n      <{policy}>\n        ' + CACHE_DISK.format(disk=disk) + f'\n      </{policy}>'
    text = TEMPLATE.format(run_dir=run_dir, disk=disk, disk_body=body, cache_block=cache_block,
                           policy=policy, outer=policy if cached else disk)
    with open(os.path.join(etc, 'config.d', 'yardstick.xml'), 'w') as f:
        f.write(text)

    # The prefixed endpoint is masked before run_dir: either may contain the other's text.
    masked = text.replace(full_endpoint, f'{base}/<PREFIX>/').replace(run_dir, '<RUN_DIR>')
    digest = hashlib.sha256()
    for name in ('config.xml', 'users.xml'):
        with open(os.path.join(etc, name), 'rb') as f:
            digest.update(f.read())
    digest.update(masked.encode())
    return digest.hexdigest()
