#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""On-demand Wine environment runner. No shell commands, root service or downloads."""
import argparse
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid


class WineError(Exception):
    pass


def identifier(value):
    if not isinstance(value, str) or not re.fullmatch(r'[a-f0-9]{32}', value):
        raise WineError('Invalid environment or application ID.')
    return value


def text(value):
    if not isinstance(value, str) or not value.strip() or len(value) > 160 or any(ord(c) < 32 for c in value):
        raise WineError('Enter a name of 1–160 characters without control characters.')
    return value.strip()


def save(path, data):
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        with temporary.open('x', encoding='utf-8') as out:
            json.dump(data, out, ensure_ascii=False, indent=2)
            out.flush()
            os.fsync(out.fileno())
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def desktop_value(value):
    return value.replace(chr(92), chr(92) * 2).replace(chr(10), chr(92) + 'n').replace(chr(13), chr(92) + 'r')


class Store:
    def __init__(self, data=None):
        self.data = Path(data or os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share'))).absolute()
        self.root = self.data / 'lingmo/wine'
        self.environments = self.root / 'environments'
        self.apps = self.root / 'applications'
        self.logs = self.root / 'logs'
        for directory in (self.environments, self.apps, self.logs):
            directory.mkdir(parents=True, exist_ok=True, mode=0o700)

    def environment(self, key):
        path = self.environments / identifier(key)
        if path.is_symlink() or not path.is_dir():
            raise WineError('Environment not found.')
        return path

    def metadata(self, key):
        item = json.loads((self.environment(key) / 'environment.json').read_text())
        if not isinstance(item, dict) or item.get('id') != key:
            raise WineError('Invalid environment metadata.')
        text(item.get('name'))
        return item

    def create(self, name):
        key = uuid.uuid4().hex
        path = self.environments / key
        name = text(name)
        path.mkdir(mode=0o700)
        save(path / 'environment.json', {'id': key, 'name': text(name), 'arch': 'win64'})
        return key

    def listing(self):
        environments = []
        for path in sorted(self.environments.iterdir()):
            try:
                item = self.metadata(path.name)
                item['ready'] = (path / 'ready').is_file() and (path / 'prefix/system.reg').is_file()
                item['directory'] = str(path)
                item['programDirectory'] = str(path / 'prefix/drive_c') if item['ready'] else str(path)
                environments.append(item)
            except (OSError, ValueError, WineError, KeyError, TypeError):
                continue
        apps = []
        for path in sorted(self.apps.glob('*.json')):
            try:
                apps.append(self.application(path.stem))
            except (OSError, ValueError, WineError, KeyError, TypeError):
                continue
        return {'environments': environments, 'applications': apps,
                'wineAvailable': all(shutil.which(p) for p in ('wine', 'wineboot', 'wineserver')),
                'logDirectory': str(self.logs)}

    def application(self, key):
        item = json.loads((self.apps / (identifier(key) + '.json')).read_text())
        if not isinstance(item, dict) or item.get('id') != key:
            raise WineError('Invalid application metadata.')
        self.environment(item['environment'])
        identifier(item['id'])
        text(item['name'])
        if not isinstance(item['path'], str):
            raise WineError('Invalid application path.')
        return item

    @contextmanager
    def lock(self, key):
        # One owner across manager windows and launcher entries. Stop deliberately
        # takes no lock: it must work while a Wine program owns this lock.
        path = self.environment(key) / 'operation.lock'
        with path.open('a') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise WineError('This environment is busy. Close its applications or stop it first.')
            yield

    def wine_environment(self, key):
        path = self.environment(key)
        prefix = path / 'prefix'
        if prefix.is_symlink():
            raise WineError('An environment prefix must not be a symbolic link.')
        env = dict(os.environ)
        # Keep the desktop/session variables, but do not let a caller redirect
        # this managed environment to ~/.wine or an incompatible architecture.
        for name in ('WINEPREFIX', 'WINEARCH', 'WINESERVER', 'WINELOADER', 'WINEDLLPATH'):
            env.pop(name, None)
        env.update(WINEPREFIX=str(prefix), WINEARCH='win64', WINEDEBUG='-all')
        return env

    @staticmethod
    def program(name):
        path = shutil.which(name)
        if not path:
            raise WineError(f'{name} is not installed. Install the Wine runtime packages first.')
        return path

    def command(self, args, env, log, cwd=None, timeout=None):
        log.write((chr(10) + json.dumps(args, ensure_ascii=False) + chr(10)).encode())
        log.flush()
        try:
            result = subprocess.run(args, env=env, cwd=cwd, stdout=log, stderr=log,
                                    stdin=subprocess.DEVNULL, timeout=timeout, check=False)
        except subprocess.TimeoutExpired:
            subprocess.run([self.program('wineserver'), '-k'], env=env,
                           stdout=log, stderr=log, timeout=10, check=False)
            raise WineError('Wine initialization timed out. See the environment log.')
        if result.returncode:
            raise WineError(f'Wine exited with code {result.returncode}. See the environment log.')

    def initialize(self, key, env, log):
        directory = self.environment(key)
        if (directory / 'ready').is_file() and (directory / 'prefix/system.reg').is_file():
            return
        print('Initializing the Windows environment…', flush=True)
        self.command([self.program('wineboot'), '--init'], env, log, timeout=120)
        self.command([self.program('wineserver'), '-w'], env, log, timeout=120)
        # Failed/partial initialization remains retryable.
        if not (directory / 'prefix/system.reg').is_file():
            raise WineError('Wine did not finish creating the environment.')
        (directory / 'ready').touch(mode=0o600)

    def execute(self, key, operation, path=None):
        self.environment(key)
        if operation == 'stop':
            # Avoid spawning a server or creating a prefix when nothing exists.
            if not (self.environment(key) / 'prefix/system.reg').is_file():
                return
            with (self.logs / (key + '-stop.log')).open('ab') as log:
                self.command([self.program('wineserver'), '-k'], self.wine_environment(key), log, timeout=10)
            return
        if operation not in ('run', 'configure', 'uninstall', 'initialize'):
            raise WineError('Unknown operation.')
        file = self.executable(path) if operation == 'run' else None
        with self.lock(key):
            env = self.wine_environment(key)
            filename = self.logs / (key + '.log')
            # Bound growth across launches while retaining the previous run.
            if filename.exists() and filename.stat().st_size > 4 * 1024 * 1024:
                filename.replace(filename.with_suffix('.previous.log'))
            with filename.open('ab') as log:
                self.initialize(key, env, log)
                if operation == 'initialize':
                    return
                if operation == 'configure':
                    args = [self.program('winecfg')]
                elif operation == 'uninstall':
                    args = [self.program('wine'), 'uninstaller']
                elif file.suffix.lower() == '.msi':
                    args = [self.program('wine'), 'msiexec', '/i', str(file)]
                else:
                    args = [self.program('wine'), str(file)]
                print('Running in ' + self.metadata(key)['name'] + '. Log: ' + str(filename), flush=True)
                self.command(args, env, log, cwd=str(file.parent) if file else None)
                # Installers may hand off to another Windows process. Keep the
                # operation alive until the environment's server has drained.
                self.command([self.program('wineserver'), '-w'], env, log)
        print('Finished.', flush=True)

    @staticmethod
    def executable(path):
        file = Path(path).expanduser().resolve(strict=True)
        if not file.is_file() or file.suffix.lower() not in ('.exe', '.msi'):
            raise WineError('Select a local EXE or MSI file.')
        return file

    def automatic_environment(self, path):
        file = self.executable(path)
        # Choose by prefix location before registration: an installed EXE
        # must stay in the prefix where its installer wrote the registry.
        try:
            relative = file.relative_to(self.environments)
        except ValueError:
            relative = None
        if relative and len(relative.parts) > 2 and relative.parts[1] == 'prefix':
            key = identifier(relative.parts[0])
            self.environment(key)
            return key
        # Serialize automatic environment creation across simultaneous opens.
        with (self.root / 'automatic.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            for item in self.listing()['applications']:
                if item['path'] == str(file):
                    return item['environment']
            for item in self.listing()['environments']:
                if item.get('source') == str(file):
                    return item['id']
            key = self.create(file.stem[:160] or 'Windows application')
            metadata = self.metadata(key)
            metadata['source'] = str(file)
            save(self.environment(key) / 'environment.json', metadata)
            return key

    def open_file(self, path):
        # Check the runtime before allocating a prefix for an unusable launch.
        for name in ('wine', 'wineboot', 'wineserver'):
            self.program(name)
        file = self.executable(path)
        self.execute(self.automatic_environment(file), 'run', str(file))

    def rename_environment(self, key, name):
        name = text(name)
        with self.lock(key):
            item = self.metadata(key)
            item['name'] = name
            save(self.environment(key) / 'environment.json', item)

    def read_log(self, key):
        self.environment(key)
        path = self.logs / (identifier(key) + '.log')
        if not path.exists():
            return ''
        with path.open('rb') as stream:
            stream.seek(0, os.SEEK_END)
            size = stream.tell()
            stream.seek(max(0, size - 64 * 1024))
            return stream.read(64 * 1024).decode('utf-8', errors='replace')

    def desktop_path(self, app):
        return self.data / 'applications' / ('lingmo-wine-' + identifier(app) + '.desktop')

    def write_application(self, item):
        app = identifier(item['id'])
        directory = self.data / 'applications'
        directory.mkdir(parents=True, exist_ok=True)
        desktop = chr(10).join(['[Desktop Entry]', 'Type=Application',
                    'Name=' + desktop_value(item['name']), 'Exec=lingmo-wine --launch ' + app,
                    'Icon=application-x-executable', 'Terminal=false', 'Categories=Utility;', ''])
        target = self.desktop_path(app)
        temporary = target.with_name(target.name + '.' + uuid.uuid4().hex + '.tmp')
        metadata = self.apps / (app + '.json')
        previous = json.loads(metadata.read_text()) if metadata.exists() else None
        try:
            with temporary.open('x', encoding='utf-8') as output:
                output.write(desktop)
                output.flush()
                os.fsync(output.fileno())
            save(metadata, item)
            try:
                temporary.replace(target)
            except OSError:
                if previous is None:
                    metadata.unlink(missing_ok=True)
                else:
                    save(metadata, previous)
                raise
        finally:
            temporary.unlink(missing_ok=True)

    def register(self, key, name, path):
        file = self.executable(path)
        if file.suffix.lower() != '.exe':
            raise WineError('Choose the installed application EXE, not an MSI installer.')
        name = text(name)
        with self.lock(key):
            # Registering the same installed program again updates its entry,
            # preserving launcher pins instead of creating duplicates.
            existing = next((item for item in self.listing()['applications']
                             if item['environment'] == key and item['path'] == str(file)), None)
            app = existing['id'] if existing else uuid.uuid4().hex
            self.write_application({'id': app, 'name': name, 'environment': key, 'path': str(file)})
            return app

    def edit_application(self, app, name, path):
        item = self.application(app)
        file = self.executable(path)
        if file.suffix.lower() != '.exe':
            raise WineError('Choose an EXE file for the application.')
        name = text(name)
        with self.lock(item['environment']):
            item.update(name=name, path=str(file))
            self.write_application(item)

    def remove_application(self, app):
        item = self.application(app)
        with self.lock(item['environment']):
            # Only our launcher and its metadata are removed. Installed files,
            # prefixes and user documents are never deleted by this operation.
            metadata = self.apps / (identifier(app) + '.json')
            backup = self.root / 'removed-applications'
            backup.mkdir(mode=0o700, exist_ok=True)
            save(backup / (app + '.json'), item)
            self.desktop_path(app).unlink(missing_ok=True)
            metadata.unlink()

    def restore_application(self, app):
        backup = self.root / 'removed-applications' / (identifier(app) + '.json')
        item = json.loads(backup.read_text())
        if not isinstance(item, dict) or item.get('id') != app:
            raise WineError('Invalid application backup.')
        text(item.get('name'))
        self.executable(item['path'])
        with self.lock(item['environment']):
            if (self.apps / (app + '.json')).exists():
                raise WineError('Application already exists.')
            self.write_application(item)
            backup.unlink()


def main():
    parser = argparse.ArgumentParser(description='Lingmo Wine environment helper')
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('list')
    sub.add_parser('open').add_argument('path')
    create = sub.add_parser('create'); create.add_argument('name')
    for name in ('initialize', 'configure', 'uninstall', 'stop', 'log'):
        command = sub.add_parser(name); command.add_argument('environment')
    run = sub.add_parser('run'); run.add_argument('environment'); run.add_argument('path')
    register = sub.add_parser('register')
    register.add_argument('environment'); register.add_argument('name'); register.add_argument('path')
    launch = sub.add_parser('launch'); launch.add_argument('application')
    rename = sub.add_parser('rename'); rename.add_argument('environment'); rename.add_argument('name')
    edit = sub.add_parser('edit'); edit.add_argument('application'); edit.add_argument('name'); edit.add_argument('path')
    for action in ('remove', 'restore'):
        sub.add_parser(action).add_argument('application')
    args = parser.parse_args()
    os.umask(0o077)
    # Wine must always run as the desktop user.
    if os.geteuid() == 0:
        parser.exit(1, 'Run Windows applications as a regular user, not root.' + chr(10))
    try:
        store = Store()
        if args.command == 'list':
            print(json.dumps(store.listing(), ensure_ascii=False))
        elif args.command == 'open':
            store.open_file(args.path)
        elif args.command == 'create':
            print(json.dumps({'id': store.create(args.name)}))
        elif args.command == 'register':
            print(json.dumps({'id': store.register(args.environment, args.name, args.path)}))
        elif args.command == 'log':
            print(store.read_log(args.environment), end='')
        elif args.command == 'rename':
            store.rename_environment(args.environment, args.name)
        elif args.command == 'edit':
            store.edit_application(args.application, args.name, args.path)
        elif args.command == 'remove':
            store.remove_application(args.application)
        elif args.command == 'restore':
            store.restore_application(args.application)
        elif args.command == 'launch':
            app = store.application(args.application)
            store.execute(app['environment'], 'run', app['path'])
        else:
            store.execute(args.environment, args.command, getattr(args, 'path', None))
    except (WineError, OSError, ValueError, KeyError, TypeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
