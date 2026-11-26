#!/usr/bin/env python3
"""Small isolated CAS fault experiment. All mutations target this Compose project."""
import argparse
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parent
COMPOSE = ['docker', 'compose', '-f', str(ROOT / 'compose.yaml')]
EVENTS = ('CASMountRenewalRetries', 'CASMountRenewalRecovered', 'CASMountRenewalDeadlineExceeded',
          'CASMountLeaseLost', 'CASRemountSucceeded', 'CASRemountFailed')


def command(*args, timeout=40, input=None):
    result = subprocess.run(COMPOSE + list(args), input=input, text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(result.stderr[-1600:])
    return result.stdout.strip()


def sql(role, query, timeout=20, user=None):
    assert role in ('writer', 'reader')
    args = ['exec', '-T', role, 'clickhouse-client']
    if user:
        args += ['--user', user]
    return command(*args, '--query', query, timeout=timeout)


def control(role, path, data=None):
    code = """import json,sys,urllib.request
body = sys.argv[2].encode() if sys.argv[2] != 'null' else None
req = urllib.request.Request('http://127.0.0.1:8474' + sys.argv[1],data=body,headers={'Content-Type':'application/json'})
with urllib.request.urlopen(req,timeout=5) as r: print(r.read().decode())"""
    return json.loads(command('exec','-T',role+'-proxy','python3','-c',code,path,json.dumps(data)))


def wait(probe, seconds=120):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        try:
            last = probe()
            if last:
                return last
        except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
            last = str(error)
        time.sleep(1)
    raise RuntimeError(f'Condition did not hold in {seconds}s: {last}')


def counters(role):
    events = sql(role, 'SELECT event,value FROM system.events WHERE event IN (' +
                 ','.join(repr(e) for e in EVENTS) + ') FORMAT JSONEachRow')
    return {r['event']: int(r['value']) for r in map(json.loads, events.splitlines())}


def snapshot(role):
    mounts = sql(role, "SELECT state,lifecycle,server_uuid,writer_epoch FROM system.cas_mounts "
                 f"WHERE server_root_id='{role}' FORMAT JSONEachRow")
    return {'mount': [json.loads(x) for x in mounts.splitlines()], 'events': counters(role)}


def record(name, data):
    line = json.dumps({'test': name, 'at': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), **data})
    print(line, flush=True)
    with (ROOT / '.lab' / 'results.jsonl').open('a') as out:
        out.write(line + '\n')


def initialize():
    # Sign a local bucket creation request with public TEST credentials. No AWS SDK/env credentials.
    create_bucket = r'''
import datetime,hashlib,hmac,http.client
host='store:11121'; now=datetime.datetime.now(datetime.timezone.utc)
date=now.strftime('%Y%m%d'); stamp=now.strftime('%Y%m%dT%H%M%SZ')
digest=hashlib.sha256(b'').hexdigest(); headers=f'host:{host}\nx-amz-content-sha256:{digest}\nx-amz-date:{stamp}\n'
names='host;x-amz-content-sha256;x-amz-date'; scope=f'{date}/us-east-1/s3/aws4_request'
canonical=f'PUT\n/test\n\n{headers}\n{names}\n{digest}'
sign=lambda key,msg:hmac.new(key,msg.encode(),hashlib.sha256).digest()
key=sign(sign(sign(sign(b'AWS4clickhouse',date),'us-east-1'),'s3'),'aws4_request')
message=f'AWS4-HMAC-SHA256\n{stamp}\n{scope}\n'+hashlib.sha256(canonical.encode()).hexdigest()
signature=sign(key,message).hex()
c=http.client.HTTPConnection(host,timeout=10)
c.request('PUT','/test',body=b'',headers={'Host':host,'x-amz-content-sha256':digest,'x-amz-date':stamp,'Authorization':f'AWS4-HMAC-SHA256 Credential=clickhouse/{scope}, SignedHeaders={names}, Signature={signature}'})
r=c.getresponse();body=r.read().decode();assert r.status in (200,409),(r.status,body);print(r.status)
'''
    wait(lambda: command('exec', '-T', 'writer-proxy', 'python3', '-', input=create_bucket) in ('200', '409'), 60)
    command('up', '-d', 'writer', 'reader', timeout=120)
    for role in ('writer', 'reader'):
        wait(lambda: sql(role, 'SELECT 1') == '1')
        sql(role, f"CREATE DATABASE IF NOT EXISTS lab ENGINE=Replicated('/cas-lab/database','1','{role}')")
    sql('writer', "CREATE TABLE IF NOT EXISTS lab.events (id UInt64, tenant UInt8, version UInt64) "
        "ENGINE=ReplicatedMergeTree ORDER BY (tenant,id,version) SETTINGS storage_policy='shared_s3'")
    sql('writer', 'INSERT INTO lab.events VALUES (1,1,1),(2,2,1),(3,1,1)')
    wait(lambda: sql('reader', 'SELECT count() FROM lab.events') == '3')
    record('baseline', {'writer': sql('writer', 'SELECT * FROM lab.events ORDER BY id'),
                       'reader': sql('reader', 'SELECT * FROM lab.events ORDER BY id')})


def short_fault(role):
    before = snapshot(role)
    control(role, '/config', {'reset': True})
    control(role, '/config', {'rate': 1.0, 'modes': ['503'], 'methods': ['PUT'],
            'path_substring': f'/test/shared_pool/gc/server-roots/{role}/mount',
            'remaining_faults': 1, 'seed': 801})
    try:
        wait(lambda: control(role, '/stats').get('faults', 0) == 1, 40)
        wait(lambda: snapshot(role)['events'].get('CASMountRenewalRecovered',0) > before['events'].get('CASMountRenewalRecovered',0), 40)
        after = snapshot(role)
        assert after['events'].get('CASMountLeaseLost',0) == before['events'].get('CASMountLeaseLost',0)
        record('short_503', {'role': role, 'before': before, 'after': after,
                            'rows': sql(role, 'SELECT count() FROM lab.events')})
    finally:
        control(role, '/config', {'reset': True})


def long_fault(role):
    before = snapshot(role)
    control(role, '/config', {'reset': True})
    control(role, '/config', {'rate': 1.0, 'modes': ['503'], 'methods': ['GET','PUT','HEAD'],
            'path_substring': f'/test/shared_pool/gc/server-roots/{role}/mount', 'seed': 802})
    try:
        wait(lambda: counters(role).get('CASMountLeaseLost',0) > before['events'].get('CASMountLeaseLost',0), 70)
        record('lease_lost', {'role':role, 'events':counters(role), 'faults':control(role,'/stats')['faults']})
    finally:
        control(role, '/config', {'reset': True})
    wait(lambda: counters(role).get('CASRemountSucceeded',0) > before['events'].get('CASRemountSucceeded',0), 120)
    # A fresh row proves replication still works, not just a cached count.
    next_id = int(sql('writer', 'SELECT max(id) FROM lab.events')) + 1
    sql('writer', f'INSERT INTO lab.events VALUES ({next_id},2,2)')
    read = wait(lambda: sql('reader', f'SELECT count() FROM lab.events WHERE id={next_id}') == '1', 90)
    record('long_503_recovered', {'role':role, 'snapshot':snapshot(role), 'fresh_row_visible':read})


def loading_fault(role, expect_recovery=False):
    before_rows = sql(role, 'SELECT * FROM lab.events ORDER BY id')
    before_uuid = sql(role, "SELECT uuid FROM system.tables WHERE database='lab' AND name='events'")
    # Hold table loading on ref-log reads, then interrupt the pool until its lease expires.
    # After removing the fault, distinguish mount recovery from loader recovery.
    command('stop', role, timeout=60)
    control(role, '/config', {'reset': True})
    control(role, '/config', {'rate':1.0,'modes':['503'],'methods':['GET'],
                            'path_substring':'/test/shared_pool/cas/ns/', 'seed':803})
    try:
        command('start', role, timeout=60)
        wait(lambda: sql(role, 'SELECT 1')=='1', 90)
        control(role, '/config', {'rate':1.0,'modes':['503'],'methods':['GET','PUT','HEAD'],
                                'path_substring':'/test/shared_pool/', 'seed':803})
        wait(lambda: counters(role).get('CASMountLeaseLost',0)>0, 70)
        # Keep the same fault duration regardless of the loader outcome. Waiting
        # for FAILED would make the corrected implementation impossible to test.
        time.sleep(5)
        record('loading_fault_observed', {'role':role,'faults':control(role,'/stats')['faults']})
    finally:
        control(role, '/config', {'reset': True})
    wait(lambda: counters(role).get('CASRemountSucceeded',0)>0, 120)
    if expect_recovery:
        wait(lambda: sql(role, 'SELECT * FROM lab.events ORDER BY id') == before_rows, 90)
        assert sql(role, "SELECT count() FROM system.asynchronous_loader WHERE status IN ('FAILED','CANCELED')") == '0'
        next_id = int(sql('writer', 'SELECT max(id) FROM lab.events')) + 1
        sql('writer', f'INSERT INTO lab.events VALUES ({next_id},2,2)')
        wait(lambda: sql('reader', f'SELECT count() FROM lab.events WHERE id={next_id}') == '1', 90)
        writer_rows = sql('writer', 'SELECT * FROM lab.events ORDER BY id')
        reader_rows = sql('reader', 'SELECT * FROM lab.events ORDER BY id')
        assert writer_rows == reader_rows == before_rows + f'\n{next_id}\t2\t2'
        assert sql(role, "SELECT uuid FROM system.tables WHERE database='lab' AND name='events'") == before_uuid
        record('loading_recovered_without_restart', {'role':role, 'snapshot':snapshot(role),
               'table_uuid':before_uuid, 'writer_rows':writer_rows, 'reader_rows':reader_rows})
        return
    # Capture the persisted failure after storage is available, without trying to repair it.
    states = []
    for _ in range(6):
        states.append(sql(role,"SELECT status,count() FROM system.asynchronous_loader GROUP BY status FORMAT JSONEachRow"))
        time.sleep(5)
    try:
        rows = sql(role, 'SELECT * FROM lab.events ORDER BY id')
        error = None
    except RuntimeError as failure:
        rows = None
        error = str(failure)
    record('after_loading_fault', {'role':role,'snapshot':snapshot(role),
                                  'loader_samples':states,'rows':rows,'error':error})
    assert error and 'ASYNC_LOAD_WAIT_FAILED' in error and 'NETWORK_ERROR' in error, 'Expected cached CAS load failure was not reproduced'
    assert all(m['lifecycle'] == 'live' for m in snapshot(role)['mount']), 'Storage itself has not recovered'


def rls_isolation(role):
    expected = {}
    for tenant in (1, 2):
        user = f'lab_tenant_{tenant}'
        sql(role, f'CREATE USER IF NOT EXISTS {user}')
        sql(role, f'GRANT SELECT ON lab.events TO {user}')
        sql(role, f'CREATE ROW POLICY IF NOT EXISTS tenant_{tenant} ON lab.events USING tenant = {tenant} TO {user}')
        expected[user] = sql(role, f'SELECT id FROM lab.events WHERE tenant={tenant} ORDER BY id')
        assert expected[user], f'RLS fixture has no visible baseline for tenant {tenant}'
    assert set(expected['lab_tenant_1'].splitlines()).isdisjoint(expected['lab_tenant_2'].splitlines())
    # Same SQL and alternating users exercise the reader filesystem cache.
    # Result caching is separate and is not enabled by our storage-cache rollout.
    for _ in range(3):
        for user, rows in expected.items():
            actual = sql(role, 'SELECT id FROM lab.events ORDER BY id SETTINGS use_query_cache=0', user=user)
            assert actual == rows, (user, actual, rows)
    try:
        sql(role, 'ALTER ROW POLICY tenant_1 ON lab.events USING 1', user='lab_tenant_1')
    except RuntimeError as error:
        assert 'ACCESS_DENIED' in str(error), str(error)
    else:
        raise AssertionError('Restricted user changed its row policy')
    record('rls_same_query_isolated', {'role': role, 'visible_ids': expected, 'policy_change_denied': True})


def terminal_fault(role):
    sql(role, 'SYSTEM CAS FORGET shared_s3')
    try:
        sql(role, 'SELECT * FROM lab.events')
    except RuntimeError as error:
        assert 'INVALID_STATE' in str(error), str(error)
        record('terminal_refusal_preserved', {'role':role, 'snapshot':snapshot(role), 'error':str(error)})
        return
    raise AssertionError('Forgotten disk unexpectedly served the table')


def shutdown_fault(role):
    command('stop', role, timeout=60)
    control(role, '/config', {'reset':True})
    control(role, '/config', {'rate':1.0,'modes':['503'],'methods':['GET'],
                            'path_substring':'/test/shared_pool/cas/ns/', 'seed':804})
    try:
        command('start', role, timeout=60)
        wait(lambda: sql(role, 'SELECT 1') == '1', 90)
        control(role, '/config', {'rate':1.0,'modes':['503'],'methods':['GET','PUT','HEAD'],
                                'path_substring':'/test/shared_pool/', 'seed':804})
        wait(lambda: counters(role).get('CASMountLeaseLost',0)>0,70)
        time.sleep(5)
        started = time.monotonic()
        command('stop', '--timeout', '30', role, timeout=40)
        container = command('ps', '-a', '-q', role)
        status = subprocess.check_output(['docker','inspect','--format','{{.State.ExitCode}}',container],text=True).strip()
        record('shutdown_during_fault', {'role':role,'seconds':round(time.monotonic()-started,2),'exit_code':status})
        assert status == '0', 'Server did not shut down cleanly (137 would mean forced kill)'
    finally:
        control(role, '/config', {'reset':True})


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('test', choices=['init','short','long','loading','terminal','shutdown','rls','snapshot'])
    p.add_argument('--role', choices=['writer','reader'], default='reader')
    p.add_argument('--expect-recovery', action='store_true', help='Require automatic recovery in the loading test')
    a=p.parse_args()
    try:
        if a.test=='init': initialize()
        elif a.test=='short': short_fault(a.role)
        elif a.test=='long': long_fault(a.role)
        elif a.test=='loading': loading_fault(a.role, a.expect_recovery)
        elif a.test=='rls': rls_isolation(a.role)
        elif a.test=='terminal': terminal_fault(a.role)
        elif a.test=='shutdown': shutdown_fault(a.role)
        else: record('snapshot', {'role':a.role, **snapshot(a.role)})
    except Exception as error:
        record(a.test+'_failed', {'role':a.role,'error':str(error)})
        raise
