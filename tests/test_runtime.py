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

    def test_logs_are_bounded_and_allow_missing_log(self):
        self.assertEqual(self.store.read_log(self.key), '')
        path = self.store.logs / (self.key + '.log')
        path.write_bytes(b'x' * 100000 + b'last line')
        content = self.store.read_log(self.key)
        self.assertEqual(len(content), 65536)
        self.assertTrue(content.endswith('last line'))
        with self.assertRaises(wine.WineError): self.store.read_log('../outside')

    def test_duplicate_registration_updates_same_launcher(self):
        first = self.store.register(self.key, 'First name', str(self.exe))
        second = self.store.register(self.key, 'New name', str(self.exe))
        self.assertEqual(first, second)
        self.assertEqual(len(self.store.listing()['applications']), 1)
        self.assertIn('Name=New name', self.store.desktop_path(first).read_text())

    def test_edit_preserves_id_and_environment(self):
        app = self.store.register(self.key, 'App', str(self.exe))
        replacement = self.root / 'New App.exe'; replacement.touch()
        self.store.edit_application(app, 'Renamed', str(replacement))
        self.assertEqual(self.store.application(app), {
            'id': app, 'environment': self.key, 'name': 'Renamed', 'path': str(replacement)})
        self.assertIn('Exec=lingmo-wine --launch ' + app, self.store.desktop_path(app).read_text())
        with self.assertRaises(wine.WineError):
            self.store.edit_application(app, 'bad\nExec=bad', str(self.exe))
        self.assertEqual(self.store.application(app)['name'], 'Renamed')

    def test_remove_and_restore_preserves_installed_files(self):
        app = self.store.register(self.key, 'Keep program', str(self.exe))
        self.store.remove_application(app)
        self.assertFalse(self.store.desktop_path(app).exists())
        self.assertEqual(self.store.listing()['applications'], [])
        self.assertTrue(self.exe.exists())
        self.assertTrue(self.store.environment(self.key).exists())
        self.store.restore_application(app)
        self.assertTrue(self.store.desktop_path(app).is_file())
        self.assertEqual(self.store.application(app)['name'], 'Keep program')
        with self.assertRaises(FileNotFoundError):
            self.store.restore_application(app)

    def test_busy_environment_rejects_registration_changes(self):
        app = self.store.register(self.key, 'App', str(self.exe))
        with self.store.lock(self.key):
            for action in (lambda: self.store.register(self.key, 'new', str(self.exe)),
                           lambda: self.store.edit_application(app, 'new', str(self.exe)),
                           lambda: self.store.remove_application(app),
                           lambda: self.store.rename_environment(self.key, 'new')):
                with self.assertRaises(wine.WineError): action()
        self.assertEqual(self.store.application(app)['name'], 'App')

    def test_rename_and_ready_status(self):
        self.store.rename_environment(self.key, 'Office')
        self.assertEqual(self.store.metadata(self.key)['name'], 'Office')
        env = self.store.listing()['environments'][0]
        self.assertFalse(env['ready'])
        self.assertEqual(env['programDirectory'], str(self.store.environment(self.key)))
        self.store.execute(self.key, 'initialize')
        self.assertTrue(self.store.listing()['environments'][0]['ready'])
        (self.store.environment(self.key) / 'prefix/system.reg').unlink()
        self.assertFalse(self.store.listing()['environments'][0]['ready'])
        self.store.execute(self.key, 'initialize')
        self.assertTrue(self.store.listing()['environments'][0]['ready'])

    def test_shortcut_write_failure_rolls_back_metadata(self):
        app = self.store.register(self.key, 'Original', str(self.exe))
        target = self.store.desktop_path(app)
        target.unlink(); target.mkdir()
        with self.assertRaises(OSError): self.store.edit_application(app, 'Changed', str(self.exe))
        self.assertEqual(self.store.application(app)['name'], 'Original')

    def test_mismatched_metadata_is_not_listed(self):
        app = self.store.register(self.key, 'App', str(self.exe))
        path = self.store.apps / (app + '.json')
        item = json.loads(path.read_text()); item['id'] = '0' * 32
        path.write_text(json.dumps(item))
        self.assertEqual(self.store.listing()['applications'], [])
        with self.assertRaises(wine.WineError): self.store.remove_application(app)

    def test_double_click_creates_and_reuses_environment(self):
        self.store.open_file(str(self.exe))
        records = self.commands()
        first = records[2]['prefix']
        self.store.open_file(str(self.exe))
        self.assertEqual(self.commands()[-2]['prefix'], first)
        self.assertEqual(sum(r['command'] == 'wineboot' for r in self.commands()), 1)
        self.assertEqual(self.commands()[2]['args'], [str(self.exe)])

    def test_double_click_msi_installs_automatically(self):
        msi = self.root / 'Setup.MSI'; msi.touch()
        self.store.open_file(str(msi))
        self.assertEqual(self.commands()[2]['args'], ['msiexec', '/i', str(msi)])

    def test_installed_exe_keeps_original_prefix(self):
        self.store.execute(self.key, 'initialize')
        installed = self.store.environment(self.key) / 'prefix/drive_c/Program Files/App.exe'
        installed.parent.mkdir(parents=True); installed.touch()
        self.store.open_file(str(installed))
        self.assertEqual(self.commands()[-2]['prefix'], str(self.store.environment(self.key) / 'prefix'))
        self.assertEqual(len(self.store.listing()['environments']), 1)

    def test_registered_exe_uses_registered_environment(self):
        self.store.register(self.key, 'App', str(self.exe))
        self.store.open_file(str(self.exe))
        self.assertEqual(self.commands()[2]['prefix'], str(self.store.environment(self.key) / 'prefix'))

    def test_missing_runtime_does_not_allocate_automatic_environment(self):
        before = len(self.store.listing()['environments'])
        with patch.dict(os.environ, PATH=str(self.root / 'missing')):
            with self.assertRaises(wine.WineError): self.store.open_file(str(self.exe))
        self.assertEqual(len(self.store.listing()['environments']), before)

    @unittest.skipUnless(os.environ.get('LINGMO_WINE_TEST_BINARY'), 'native executable not configured')
    def test_desktop_entry_launches_without_a_display(self):
        import subprocess
        for arguments in ([str(self.exe)], ['--open', str(self.exe)]):
            with patch.dict(os.environ, XDG_DATA_HOME=str(self.store.data), WAYLAND_DISPLAY='nonexistent'):
                result = subprocess.run([os.environ['LINGMO_WINE_TEST_BINARY'], *arguments],
                                        capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sum(r['command'] == 'wineboot' for r in self.commands()), 1)


if __name__ == '__main__':
    unittest.main()
