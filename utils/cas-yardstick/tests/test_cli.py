import importlib.util
import io
import os
import unittest
from unittest import mock

from yardstick.cards import CARDS
from yardstick.config import POLICIES

_PATH = os.path.join(os.path.dirname(__file__), '..', 'yardstick.py')
REPO_ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
_spec = importlib.util.spec_from_file_location('yardstick_cli', _PATH)
CLI = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(CLI)


def parsed(*argv):
    """Arguments that `main` hands to `run`, with `run` stubbed out."""
    seen = {}

    def stub(args):
        seen['args'] = args
        return 0

    with mock.patch.object(CLI, 'run', stub), mock.patch('sys.argv', ['yardstick.py', 'run', '--binary', 'b', *argv]):
        CLI.main()
    return seen['args']


class ProfileTest(unittest.TestCase):
    def test_default_is_every_card_in_registry_order(self):
        a = parsed()
        self.assertEqual(a.cards, list(CARDS))
        self.assertEqual((a.scale, a.repeats, a.policies), (1.0, 3, list(POLICIES)))

    def test_quick_profile(self):
        a = parsed('--quick')
        self.assertEqual(a.cards, list(CARDS))
        self.assertEqual((a.scale, a.repeats), (0.25, 1))
        self.assertEqual(a.policies, ['cas', 's3', 's3plain'])

    def test_smoke_profile(self):
        a = parsed('--smoke')
        self.assertEqual((a.cards, a.scale, a.repeats, a.policies), (list(CARDS), 0.01, 1, list(POLICIES)))

    def test_profiles_refuse_explicit_values(self):
        for flag in ('--smoke', '--quick'):
            for extra in (['--cards', 'cycle'], ['--scale', '1'], ['--repeats', '2'], ['--policies', 'cas']):
                with self.subTest(flag=flag, extra=extra), self.assertRaises(SystemExit):
                    parsed(flag, *extra)

    def test_duplicate_selections_are_rejected(self):
        for extra in (['--cards', 'cycle,cycle'], ['--policies', 'cas,s3,cas']):
            with self.subTest(extra=extra), self.assertRaises(SystemExit), \
                    mock.patch('sys.stderr', new_callable=io.StringIO):
                parsed(*extra)

    def test_default_runs_dir_does_not_depend_on_the_cwd(self):
        self.assertEqual(parsed().runs_dir, os.path.join(REPO_ROOT, 'tmp', 'cas-yardstick', 'runs'))

    def test_smoke_exit_code_survives_an_incompatible_compare(self):
        compared = []
        with mock.patch.object(CLI, 'run', lambda args: setattr(args, 'run_root', 'x') or 2), \
                mock.patch.object(CLI, 'compare_runs', lambda *a: compared.append(a) or 1), \
                mock.patch.object(CLI, '_reference', lambda explicit: 'ref'), \
                mock.patch('sys.argv', ['yardstick.py', 'run', '--binary', 'b', '--smoke', '--compare']):
            self.assertEqual(CLI.main(), 2)
        self.assertEqual(compared, [('x', 'ref')])

    def test_smoke_and_quick_exclude_each_other(self):
        with self.assertRaises(SystemExit):
            parsed('--smoke', '--quick')


if __name__ == '__main__':
    unittest.main()
