import os
import shutil
import tempfile
import unittest
import xml.etree.ElementTree as ET

from yardstick.config import POLICIES, write_config

ENDPOINT = 'http://127.0.0.1:19121'
DISK_KINDS = ('cas', 's3', 's3plain')


def load_disks(run_dir):
    root = ET.parse(os.path.join(run_dir, 'etc', 'config.d', 'yardstick.xml')).getroot()
    disks = root.find('storage_configuration/disks')
    return root, {d.tag: d for d in disks}


class ConfigTest(unittest.TestCase):
    def write(self, policy, gc=False, prefix='test/run/x/k1'):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        return d, write_config(d, policy, ENDPOINT, prefix, gc)

    def test_one_disk_per_policy(self):
        for policy in POLICIES:
            with self.subTest(policy=policy):
                d, _ = self.write(policy)
                _, disks = load_disks(d)
                base = policy.replace('_cached', '')
                self.assertEqual(sorted(set(disks) & set(DISK_KINDS)), [base])
                self.assertIn('default', disks)
                cached = policy.endswith('_cached')
                self.assertEqual(policy in disks and policy != base, cached)
                self.assertEqual(len(disks), 2 + cached)
                if cached:
                    self.assertEqual(disks[policy].findtext('disk'), base)
                    self.assertEqual(disks[policy].findtext('type'), 'cache')

    def test_policy_points_at_the_outermost_disk(self):
        for policy in POLICIES:
            with self.subTest(policy=policy):
                d, _ = self.write(policy)
                root, _ = load_disks(d)
                pol = root.find('storage_configuration/policies')
                self.assertEqual([p.tag for p in pol], [policy])
                self.assertEqual(pol.find(policy + '/volumes/main/disk').text, policy)

    def test_endpoint_carries_prefix(self):
        for policy in POLICIES:
            with self.subTest(policy=policy):
                d, _ = self.write(policy, prefix='test/run/p/k7')
                _, disks = load_disks(d)
                base = policy.replace('_cached', '')
                self.assertEqual(disks[base].findtext('endpoint'), ENDPOINT + '/test/run/p/k7/')
                self.assertEqual(disks[base].findtext('access_key_id'), 'clickhouse')
                self.assertEqual(disks[base].findtext('secret_access_key'), 'clickhouse')
                self.assertEqual(disks[base].findtext('http_keep_alive_timeout'), '30')
                self.assertEqual(disks[base].findtext('http_keep_alive_max_requests'), '10000')

    def test_cas_gc_flag_follows_argument(self):
        for gc, want in ((False, '0'), (True, '1')):
            d, _ = self.write('cas', gc=gc)
            _, disks = load_disks(d)
            self.assertEqual(disks['cas'].findtext('cas_gc_enabled'), want)
            self.assertEqual(disks['cas'].findtext('cas_server_root_id'), 'yardstick')

    def test_s3_has_metadata_path_under_run_dir(self):
        d, _ = self.write('s3')
        _, disks = load_disks(d)
        self.assertTrue(disks['s3'].findtext('metadata_path').startswith(d))

    def test_cache_wrapper_block_on_every_cached_policy(self):
        for policy in ('cas_cached', 's3_cached', 's3plain_cached'):
            with self.subTest(policy=policy):
                d, _ = self.write(policy)
                _, disks = load_disks(d)
                base = policy.replace('_cached', '')
                got = {ch.tag: (ch.text or '').strip() for ch in disks[policy]}
                self.assertEqual(got, {
                    'type': 'cache', 'disk': base, 'path': base + '_cache/', 'max_size': '1Gi',
                    'cache_on_write_operations': '1', 'background_download_threads': '0',
                    'background_download_queue_size_limit': '0', 'load_metadata_asynchronously': '0'})

    def test_disk_bodies_verbatim(self):
        d, _ = self.write('cas', gc=True)
        _, disks = load_disks(d)
        got = {ch.tag: (ch.text or '').strip() for ch in disks['cas']}
        self.assertEqual(got['type'], 'object_storage')
        self.assertEqual(got['object_storage_type'], 's3')
        self.assertEqual(got['metadata_type'], 'cas')
        self.assertEqual(got['cas_scratch_path'], 'cas_scratch/')
        self.assertEqual(got['cas_gc_interval_sec'], '20')
        d, _ = self.write('s3')
        _, disks = load_disks(d)
        self.assertEqual(disks['s3'].findtext('type'), 's3')
        d, _ = self.write('s3plain')
        _, disks = load_disks(d)
        self.assertEqual(disks['s3plain'].findtext('type'), 'object_storage')
        self.assertEqual(disks['s3plain'].findtext('object_storage_type'), 's3')
        self.assertEqual(disks['s3plain'].findtext('metadata_type'), 'plain_rewritable')

    def test_hash_ignores_run_dir_and_prefix(self):
        for policy in POLICIES:
            with self.subTest(policy=policy):
                _, h1 = self.write(policy, prefix='test/run1/x/k1')
                _, h2 = self.write(policy, prefix='test/run2/y/k9')
                self.assertEqual(len(h1), 64)
                self.assertEqual(h1, h2)

    def test_hash_changes_with_gc_policy_and_endpoint(self):
        d, h = self.write('cas', gc=False)
        self.assertNotEqual(h, write_config(d, 'cas', ENDPOINT, 'test/run/x/k1', True))
        self.assertNotEqual(h, write_config(d, 'cas', 'http://127.0.0.1:19122', 'test/run/x/k1', False))
        self.assertNotEqual(h, write_config(d, 'cas_cached', ENDPOINT, 'test/run/x/k1', False))
        self.assertEqual(h, write_config(d, 'cas', ENDPOINT, 'test/run/x/k1', False))

    def test_relative_run_dir_becomes_absolute(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        old = os.getcwd()
        os.chdir(d)
        self.addCleanup(os.chdir, old)
        write_config('rel', 's3', ENDPOINT, 'test/p', False)
        _, disks = load_disks(os.path.join(d, 'rel'))
        self.assertEqual(disks['s3'].findtext('metadata_path'), os.path.realpath(os.path.join(d, 'rel')) + '/s3_meta/')

    def test_base_files_written(self):
        d, _ = self.write('s3')
        for f in ('config.xml', 'users.xml'):
            ET.parse(os.path.join(d, 'etc', f))
        for sub in ('data', 'tmp', 'uf', 'caches'):
            self.assertTrue(os.path.isdir(os.path.join(d, sub)))

    def test_tables_load_before_the_server_answers(self):
        # A cold reset drops caches right after `SELECT 1` answers; an asynchronous table load would refill them.
        d, _ = self.write('s3')
        root = ET.parse(os.path.join(d, 'etc', 'config.xml')).getroot()
        self.assertEqual(root.findtext('async_load_databases'), 'false')

    def test_unknown_policy_rejected(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        with self.assertRaises(ValueError):
            write_config(d, 'nope', ENDPOINT, 'p', False)


if __name__ == '__main__':
    unittest.main()
