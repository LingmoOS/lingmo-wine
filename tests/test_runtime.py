#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

MODULE = Path(__file__).resolve().parents[1] / 'runtime/lingmo_wine.py'
spec = importlib.util.spec_from_file_location('lingmo_wine', MODULE)
wine = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wine)


class RuntimeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.store = wine.Store(self.root / 'data with spaces')
        self.key = self.store.create('Test environment')
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.trace = self.root / 'trace.jsonl'
        mock = '''#!/usr/bin/python3
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
with open(os.environ['TRACE'], 'a') as out:
    out.write(json.dumps({'command': name, 'args': sys.argv[1:], 'prefix': os.environ.get('WINEPREFIX'),
                          'arch': os.environ.get('WINEARCH'), 'cwd': os.getcwd()}) + chr(10))
if os.environ.get('FAIL_COMMAND') == name:
    sys.exit(7)
if name == 'wineboot' and not os.environ.get('NO_PREFIX'):
    prefix = pathlib.Path(os.environ['WINEPREFIX'])
    prefix.mkdir(parents=True, exist_ok=True)
    (prefix / 'system.reg').touch()
'''
        for command in ('wine', 'wineboot', 'wineserver', 'winecfg'):
            file = self.bin / command
            file.write_text(mock)
            file.chmod(0o755)
        self.environment = patch.dict(os.environ, PATH=str(self.bin), TRACE=str(self.trace))
        self.environment.start()
        self.exe = self.root / 'setup with "quotes" $(touch BAD).EXE'
        self.exe.touch()

    def tearDown(self):
        self.environment.stop()
        self.temp.cleanup()

    def commands(self):
        return [json.loads(line) for line in self.trace.read_text().splitlines()]

    def test_first_run_initializes_then_runs_without_shell(self):
        self.store.execute(self.key, 'run', str(self.exe))
        records = self.commands()
        self.assertEqual([r['command'] for r in records], ['wineboot', 'wineserver', 'wine', 'wineserver'])
        self.assertEqual(records[2]['args'], [str(self.exe)])
        self.assertEqual(records[2]['cwd'], str(self.exe.parent))
        self.assertFalse((self.root / 'BAD').exists())
        self.assertTrue((self.store.environment(self.key) / 'ready').exists())
        self.store.execute(self.key, 'run', str(self.exe))
        self.assertEqual(len([r for r in self.commands() if r['command'] == 'wineboot']), 1)

    def test_msi_uses_msiexec(self):
        msi = self.root / 'App Setup.msi'; msi.touch()
        self.store.execute(self.key, 'run', str(msi))
        self.assertEqual(self.commands()[2]['args'], ['msiexec', '/i', str(msi)])

    def test_prefix_isolation_and_no_inherited_override(self):
        other = self.store.create('Other')
        with patch.dict(os.environ, WINEPREFIX='/wrong', WINEARCH='win32', WINESERVER='/wrong/server'):
            self.store.execute(self.key, 'run', str(self.exe))
            self.store.execute(other, 'run', str(self.exe))
        prefixes = {r['prefix'] for r in self.commands()}
        self.assertEqual(len(prefixes), 2)
        self.assertNotIn('/wrong', prefixes)
        self.assertTrue(all(r['arch'] == 'win64' for r in self.commands()))

    def test_stop_targets_only_selected_prefix(self):
        self.store.execute(self.key, 'initialize')
        other = self.store.create('Other')
        self.store.execute(other, 'stop')
        before = len(self.commands())
        with self.store.lock(self.key):
            self.store.execute(self.key, 'stop')
        self.assertEqual(len(self.commands()), before + 1)
        self.assertEqual(self.commands()[-1]['args'], ['-k'])
        self.assertEqual(self.commands()[-1]['prefix'], str(self.store.environment(self.key) / 'prefix'))

    def test_concurrent_operation_rejected(self):
        with self.store.lock(self.key):
            with self.assertRaises(wine.WineError):
                self.store.execute(self.key, 'initialize')

    def test_failed_initialization_is_retryable(self):
        with patch.dict(os.environ, FAIL_COMMAND='wineboot'):
            with self.assertRaises(wine.WineError): self.store.execute(self.key, 'initialize')
        self.assertFalse((self.store.environment(self.key) / 'ready').exists())
        self.store.execute(self.key, 'initialize')
        self.assertTrue((self.store.environment(self.key) / 'ready').exists())

    def test_partial_initialization_is_not_ready(self):
        with patch.dict(os.environ, NO_PREFIX='1'):
            with self.assertRaises(wine.WineError): self.store.execute(self.key, 'initialize')
        self.assertFalse((self.store.environment(self.key) / 'ready').exists())

    def test_missing_wine_has_actionable_error(self):
        with patch.dict(os.environ, PATH=str(self.root / 'missing')):
            with self.assertRaisesRegex(wine.WineError, 'not installed'):
                self.store.execute(self.key, 'initialize')

    def test_shortcut_exec_contains_only_id(self):
        app = self.store.register(self.key, 'Quoted " App %f', str(self.exe))
        desktop = (self.store.data / 'applications' / ('lingmo-wine-' + app + '.desktop')).read_text()
        self.assertIn('Exec=lingmo-wine --launch ' + app + '\n', desktop)
        self.assertNotIn(str(self.exe), desktop)
        self.assertEqual(self.store.application(app)['path'], str(self.exe))
        self.assertEqual(len(self.store.listing()['applications']), 1)

    def test_invalid_inputs_do_not_run_commands(self):
        for key in ('../escape', '/', '-k', ''):
            with self.assertRaises(wine.WineError): self.store.environment(key)
        for name in ('', 'Name\nExec=bad', 'x' * 161):
            with self.assertRaises(wine.WineError): self.store.create(name)
        file = self.root / 'program.sh'; file.touch()
        with self.assertRaises(wine.WineError): self.store.execute(self.key, 'run', str(file))
        self.assertFalse(self.trace.exists())

    def test_symlink_prefix_rejected(self):
        (self.store.environment(self.key) / 'prefix').symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(wine.WineError): self.store.execute(self.key, 'initialize')

    def test_configure_uninstall_and_nonzero_exit(self):
        self.store.execute(self.key, 'configure')
        self.assertEqual(self.commands()[2]['command'], 'winecfg')
        self.store.execute(self.key, 'uninstall')
        self.assertEqual(self.commands()[-2]['args'], ['uninstaller'])
        with patch.dict(os.environ, FAIL_COMMAND='wine'):
            with self.assertRaisesRegex(wine.WineError, 'code 7'):
                self.store.execute(self.key, 'run', str(self.exe))


if __name__ == '__main__':
    unittest.main()
