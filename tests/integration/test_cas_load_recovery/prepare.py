#!/usr/bin/env python3
"""Download the pinned upstream fault proxy and generate disposable lab configs."""
from pathlib import Path
import hashlib
import urllib.request

ROOT = Path(__file__).resolve().parent
LAB = ROOT / '.lab'
LAB.mkdir(exist_ok=True)
URL = ('https://raw.githubusercontent.com/Altinity/ClickHouse/v26.6.4.20001.altinityantalya/'
       'tests/integration/test_cas_mount_renewal_retry/s3_fault_proxy.py')
proxy = urllib.request.urlopen(URL, timeout=30).read()
assert hashlib.sha256(proxy).hexdigest() == 'e4a2b0d7051d08b5c34c255aafe3e63f3dc3463f4bc67693cccd3ab6fd5f8b04'
(LAB / 's3_fault_proxy.py').write_bytes(proxy)
(LAB / 'keeper.xml').write_text('''<clickhouse>
<logger><level>warning</level><console>true</console></logger>
<listen_host>0.0.0.0</listen_host>
<keeper_server><tcp_port>9181</tcp_port><server_id>1</server_id>
<log_storage_path>/var/lib/clickhouse-keeper/log</log_storage_path>
<snapshot_storage_path>/var/lib/clickhouse-keeper/snapshots</snapshot_storage_path>
<raft_configuration><server><id>1</id><hostname>keeper</hostname><port>9234</port></server></raft_configuration>
</keeper_server></clickhouse>''')
for role in ('writer', 'reader'):
    cache = ('''<shared_s3_cache><type>cache</type><disk>shared_s3</disk>
<path>/var/lib/clickhouse/cache/</path><max_size>134217728</max_size></shared_s3_cache>'''
             if role == 'reader' else '')
    disk = 'shared_s3_cache' if role == 'reader' else 'shared_s3'
    (LAB / f'{role}.xml').write_text(f'''<clickhouse>
<logger><level>information</level><console>true</console></logger>
<listen_host>0.0.0.0</listen_host>
<max_server_memory_usage>1073741824</max_server_memory_usage>
<async_load_databases>true</async_load_databases>
<interserver_http_host>{role}</interserver_http_host>
<macros><replica>{role}</replica><shard>1</shard></macros>
<zookeeper><node><host>keeper</host><port>9181</port></node></zookeeper>
<cas_log><database>system</database><table>cas_log</table><flush_interval_milliseconds>1000</flush_interval_milliseconds></cas_log>
<storage_configuration><disks><shared_s3>
<type>object_storage</type><object_storage_type>s3</object_storage_type><metadata_type>cas</metadata_type>
<endpoint>http://{role}-proxy:11121/test/shared_pool/</endpoint>
<access_key_id>clickhouse</access_key_id><secret_access_key>clickhouse</secret_access_key>
<cas_server_root_id>{role}</cas_server_root_id>
<connect_timeout_ms>1000</connect_timeout_ms>
<cas_mount_lease_ttl_ms>30000</cas_mount_lease_ttl_ms><cas_mount_renew_period_ms>10000</cas_mount_renew_period_ms>
<cas_attempt_timeout_ms>5000</cas_attempt_timeout_ms><cas_lease_safety_margin_ms>2000</cas_lease_safety_margin_ms>
</shared_s3>{cache}</disks>
<policies><shared_s3><volumes><main><disk>{disk}</disk></main></volumes></shared_s3></policies>
</storage_configuration></clickhouse>''')
print('Prepared isolated configs; no production credentials or endpoints.')
