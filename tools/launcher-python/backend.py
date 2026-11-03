"""Native launcher operations; the Tk thread only receives data/events."""
import copy
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import threading
import urllib.error
import urllib.request
import uuid
import graphics_prep
import launch_options
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'local_build'))
import aircraft_hd

BASE = 'https://github.com/saintiron82/macross30-releases/releases/latest/download/'
METADATA_BASE = 'https://raw.githubusercontent.com/saintiron82/macross30-releases/main/channels/stable/'
LOCALES = ('ko-KR', 'en-US', 'ja-JP')
VERSION = '0.3.8'
REPORT_ENDPOINT = 'https://macross30-log-relay.saintiron82.workers.dev'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
EXTERNAL_PROCESS_LOCK = threading.Lock()


def external_popen(*args, **kwargs):
    """Do not pass PyInstaller's private runtime to external Windows tools."""
    env = dict(kwargs.pop('env', os.environ))
    bundled = getattr(sys, '_MEIPASS', None)
    if bundled:
        base = os.path.normcase(os.path.abspath(bundled))
        env['PATH'] = os.pathsep.join(part for part in env.get('PATH', '').split(os.pathsep)
                                    if not (os.path.normcase(os.path.abspath(part)) == base or
                                            os.path.normcase(os.path.abspath(part)).startswith(base + os.sep)))
    kwargs['env'] = env
    if sys.platform != 'win32':
        return subprocess.Popen(*args, **kwargs)
    with EXTERNAL_PROCESS_LOCK:
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.GetDllDirectoryW.argtypes = (ctypes.c_uint32, ctypes.c_wchar_p)
        kernel.SetDllDirectoryW.argtypes = (ctypes.c_wchar_p,)
        kernel.SetDllDirectoryW.restype = ctypes.c_int
        size = kernel.GetDllDirectoryW(0, None)
        prior = ctypes.create_unicode_buffer(size + 1)
        kernel.GetDllDirectoryW(len(prior), prior)
        if not kernel.SetDllDirectoryW(None):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            return subprocess.Popen(*args, **kwargs)
        finally:
            if not kernel.SetDllDirectoryW(prior.value or None):
                raise ctypes.WinError(ctypes.get_last_error())


def sha(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def external_run(args, input=None, timeout=None, **kwargs):
    if input is not None:kwargs['stdin']=subprocess.PIPE
    process=external_popen(args,**kwargs)
    try:stdout,stderr=process.communicate(input=input,timeout=timeout)
    except BaseException:
        process.kill();process.wait();raise
    return subprocess.CompletedProcess(args,process.returncode,stdout,stderr)


def localized(value, locale, fallback='ko'):
    return value if isinstance(value, str) else value.get(locale) or value.get(fallback) or value.get('en', '')


def child(root, relative):
    root = Path(root).absolute()
    path = root / relative
    if not path.absolute().is_relative_to(root) or '..' in Path(relative).parts or Path(relative).is_absolute():
        raise ValueError('Unsafe installation path')
    for parent in (path, *path.parents):
        if parent.is_symlink() or getattr(parent, 'is_junction', lambda: False)():
            raise ValueError('Installation path contains a link')
    return path


def version_from(row):
    match = re.search(r'-(\d+\.\d+\.\d+)\.zip$', row.get('file', ''))
    return match[1] if match else ''


def update_plan(installed, locale):
    return {'core': installed.get('runtime_status') != 2,
            'language': locale != 'ja-JP' and installed.get('language_status') != 2}


class Backend:
    def __init__(self, bridge, config_path, emit=lambda event: None, confirm=lambda data: False,
                 choose_iso=lambda: None, opener=None):
        self.bridge_path = Path(bridge)
        self.config_path = Path(config_path)
        self.emit, self.confirm, self.choose_iso = emit, confirm, choose_iso
        self.opener = opener or urllib.request.urlopen
        self.busy = self.running = False
        self.manifest = None
        self.installed = {}
        self.status = None
        self.stale = True
        self.checked_at = None
        self.last_log = None
        self.logs = []
        self.config = {'folder': str(Path(os.environ.get('LOCALAPPDATA', tempfile.gettempdir())) / 'Macross30'),
                       'iso': '', 'disc_key': '', 'game_language': 'ja-JP', 'ui_language': 'ko'}
        self.iso_info=None
        self.iso_hash_cache={}
        self.prepared_iso=None
        self.graphics_prep = None
        self.launch_aborted = False
        for path in (self.config_path, self.config_path.with_name('settings.json')):
            try:
                self.config.update(json.loads(path.read_text(encoding='utf-8-sig')))
                break
            except (OSError, ValueError):
                pass
        if self.config['game_language'] not in LOCALES:
            self.config['game_language'] = 'ja-JP'
        if self.config['ui_language'] not in ('ko', 'en', 'ja'):
            self.config['ui_language'] = 'ko'
        try:
            self.status = self.validate_status(json.loads(self.config_path.with_name('status-cache.json').read_text(encoding='utf-8-sig')))
        except (OSError, ValueError):
            pass

    def event(self, kind, **data):
        self.emit(copy.deepcopy({'type': kind, **data}))

    def message(self, key, **data):
        self.logs.append(time.strftime('%H:%M:%S') + ' ' + key + ' ' + str(data))
        self.logs = self.logs[-1500:]
        self.event('progress', key=key, **data)

    def save(self):
        self.config_path.parent.mkdir(parents=True, exist_ok=True)
        pending = self.config_path.with_suffix('.pending')
        pending.write_text(json.dumps(self.config, ensure_ascii=False), encoding='utf-8')
        os.replace(pending, self.config_path)

    def load_launch_options(self):
        path = child(self.config['folder'], 'play/settings.ini')
        if not path.parent.is_dir():
            raise FileNotFoundError(str(path.parent))
        raw = launch_options.read(path)
        available = bool((self.manifest or {}).get('features', {}).get('hd_textures', {}).get('available'))
        return {'path': str(path), 'original': raw, 'values': launch_options.parse(raw or b''), 'hd_available': available}

    def save_launch_options(self, document, changes):
        if self.busy or self.running:
            raise RuntimeError('Stop the game and wait for launcher operations before saving.')
        path = child(self.config['folder'], 'play/settings.ini')
        if str(path) != document['path']:
            raise ValueError('Installation folder changed; reopen Launch options.')
        # Also detects games started outside this launcher; never terminates them.
        self.bridge('stopped')
        if 'hd_textures' in changes and not document.get('hd_available'):
            changes = {key: value for key, value in changes.items() if key != 'hd_textures'}
        launch_options.save(path, document['original'], changes)

    def export_game_logs(self, save_directory=None):
        root = Path(self.config['folder'])
        collector = root / 'tools/collect-logs.ps1'
        if not collector.is_file():
            raise FileNotFoundError(str(collector))
        command = [str(Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'),
                   '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', str(collector), '-NoOpen']
        if save_directory:
            command += ['-SaveDirectory', save_directory]
        pointer = root / 'play/last-session.json'
        if pointer.is_file():
            session = json.loads(pointer.read_text(encoding='utf-8'))['session']
            if not re.fullmatch(r'[a-f0-9]{32}', session):
                raise ValueError('Invalid session identity')
            command += ['-Session', session]
        process = external_popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, creationflags=NO_WINDOW)
        output, _ = process.communicate(timeout=180)
        if process.returncode:
            raise RuntimeError(output.decode('utf-8', errors='replace'))
        reports = root / 'bug-reports'
        archive = max(reports.glob('Macross30-logs-*.zip'), key=lambda p: p.stat().st_mtime_ns)
        self.logs.append('Bug report ZIP: ' + str(archive))
        os.startfile(str(reports))
        return archive

    def preserve_session(self, exit_code):
        """Pin the finished play before another launch/update can replace its files."""
        root = Path(self.config['folder'])
        play = child(root, 'play')
        session = uuid.uuid4().hex
        target = child(root, 'play/sessions/' + session)
        target.mkdir(parents=True)
        names = {'stdout.log', 'stderr.log', 'process.json', 'exit.json', 'settings.ini', 'keymap.ini', 'padmap.ini'}
        names.update(p.name for p in play.glob('diagnostics*.log'))
        for name in names:
            path = play / name
            if path.is_file():
                shutil.copyfile(path, target / name)
        metadata = {'session': session, 'exit_code': exit_code, 'launcher_version': VERSION,
                    'game_version': self.installed.get('installed_version'),
                    'language': self.config['game_language'], 'ended': time.time()}
        (target / 'session.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
        pending = play / 'last-session.pending'
        pending.write_text(json.dumps(metadata), encoding='utf-8')
        os.replace(pending, play / 'last-session.json')
        self.event('session-ended', **metadata)
        return metadata

    def last_session(self):
        try:
            return json.loads((Path(self.config['folder'])/'play/last-session.json').read_text(encoding='utf-8'))
        except (OSError, ValueError):
            return None

    def prepare_report(self, save_slot=None, description='', test=False):
        from report_client import prepare
        if self.running:
            raise RuntimeError('Wait for the game to exit before sending its last play logs')
        if not self.last_session():
            try:
                exit_info = json.loads((Path(self.config['folder'])/'play/exit.json').read_text(encoding='utf-8-sig'))
            except (OSError,ValueError):
                raise RuntimeError('No completed play session is available') from None
            self.preserve_session(int(exit_info['exit_code']))
        report = prepare(self.config['folder'],save_slot,description,test)
        self.event('report-prepared', report=report)
        return report

    def send_report(self, report, cancelled=None):
        from report_client import send
        result = send(report, REPORT_ENDPOINT, cancelled)
        self.event('report-result', report=result)
        return result

    def bridge(self, operation, **extra):
        request = {'op': operation, 'root': self.config['folder'], 'locale': self.config['game_language'], **extra}
        with tempfile.TemporaryDirectory(prefix='macross-request-') as work:
            path = Path(work) / 'request.json'
            path.write_text(json.dumps(request), encoding='utf-8')
            process = external_popen([str(self.bridge_path), str(path)], stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, encoding='utf-8-sig', errors='replace',
                                     creationflags=NO_WINDOW)
            stdout, stderr = process.communicate()
            result = subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)
            try:
                value = json.loads(result.stdout.strip().splitlines()[-1])
            except (ValueError, IndexError) as error:
                raise RuntimeError((result.stderr or result.stdout or 'Installer helper failed')[-8000:]) from error
            if not value.get('ok'):
                raise RuntimeError(value.get('error', 'Installer helper failed'))
            return value['result']

    def get(self, name):
        if name in ('BOOT-INSTALL.json', 'STATUS.json', 'PYTHON-LAUNCHER.json', 'LAUNCHER.json'):
            url = METADATA_BASE + name
        else:
            version = re.search(r'-(\d+\.\d+\.\d+)\.(?:zip|exe)$', name)
            if not version:
                raise ValueError('Invalid release asset name')
            url = 'https://github.com/saintiron82/macross30-releases/releases/download/v'+version[1]+'/'+name
        request = urllib.request.Request(url + '?check=' + str(time.time_ns()),
                                         headers={'User-Agent': 'Macross30Launcher/' + VERSION, 'Cache-Control': 'no-cache'})
        return self.opener(request, timeout=25)

    def json(self, name):
        with self.get(name) as response:
            data = response.read(256 * 1024 + 1)
        if len(data) > 256 * 1024:
            raise ValueError('Metadata too large')
        return json.loads(data.decode('utf-8-sig'))

    @staticmethod
    def validate_status(status):
        if not isinstance(status, dict):
            raise ValueError('Invalid service status')
        if status.get('schema_version') != 1 or not isinstance(status.get('closed'), bool):
            raise ValueError('Invalid service status')
        if status.get('default_language') not in ('ko', 'en', 'ja'):
            raise ValueError('Invalid default language')
        for name in ('announcements', 'patch_notes'):
            if not isinstance(status.get(name), list) or len(status[name]) > 500:
                raise ValueError('Invalid release content')
            for entry in status[name]:
                if not isinstance(entry.get('id'), str):
                    raise ValueError('Missing content identity')
                for field in ('title', 'body'):
                    if not isinstance(entry.get(field), dict) or any(k not in ('ko', 'en', 'ja') or not isinstance(v, str) or len(v) > 16000 for k, v in entry[field].items()):
                        raise ValueError('Invalid translated content')
        return status

    def check_status(self):
        try:
            self.status = self.validate_status(self.json('STATUS.json'))
            self.stale = False
            self.config_path.parent.mkdir(parents=True, exist_ok=True)
            self.config_path.with_name('status-cache.json').write_text(json.dumps(self.status), encoding='utf-8')
        except (OSError, ValueError):
            self.stale = True

    def inspect(self):
        self.installed = self.bridge('inspect', manifest=self.manifest)
        return self.installed

    def snapshot(self):
        return copy.deepcopy({'config': self.config, 'manifest': self.manifest, 'installed': self.installed,
                              'status': self.status, 'stale': self.stale, 'busy': self.busy,
                              'running': self.running, 'checked_at': self.checked_at, 'version': VERSION,
                              'last_session': self.last_session()})

    def check(self):
        self.event('checking', value=True)
        try:
            self.check_status()
            manifest = self.json('BOOT-INSTALL.json')
            self.bridge('validate', manifest=manifest)
            self.manifest = manifest
            self.checked_at = time.time()
            if not self.running:
                self.inspect()
            self.event('state', state=self.snapshot())
        finally:
            self.event('checking', value=False)
        return self.snapshot()

    def settings(self, **changes):
        if self.busy or self.running:
            raise RuntimeError('Wait for the current operation')
        for key, value in changes.items():
            if key not in ('folder', 'iso', 'disc_key', 'game_language', 'ui_language') or not isinstance(value, str) or '\0' in value:
                raise ValueError('Invalid setting')
            if key == 'game_language' and value not in LOCALES or key == 'ui_language' and value not in ('ko', 'en', 'ja'):
                raise ValueError('Unsupported language')
        self.config.update(changes)
        self.save()
        self.inspect()
        self.event('state', state=self.snapshot())

    def iso_tools(self):
        return self.bridge_path.parent/'iso-tools' if getattr(sys,'frozen',False) else Path(__file__).resolve().parents[2]/'.tools/iso-tools'

    def inspect_iso(self):
        import iso_service
        self.message('isoHashing')
        fingerprint=iso_service.fingerprint(self.config['iso']) if self.config['iso'] else None
        info=iso_service.analyze(self.config['iso'],self.config.get('disc_key',''),self.iso_tools(),external_run,
                                 known_hash=self.iso_hash_cache.get(fingerprint))
        if fingerprint:self.iso_hash_cache={fingerprint:info['sha1']}
        self.iso_info=info
        self.event('iso-status',info=info)
        return info

    def require_disc(self,info):
        from i18n import TEXT
        words=TEXT[self.config['ui_language']]
        if info['kind']=='ready':return
        if info['kind']!='encrypted':raise ValueError(words['isoUnsupported'])
        if info['key_status']!='matched':
            key={'missing':'isoKeyMissing','invalid-format':'isoKeyInvalid','mismatch':'isoKeyMismatch'}[info['key_status']]
            raise ValueError(words[key]+'\nSHA-1: '+info['sha1'])

    def prepare_disc(self,program):
        import iso_service
        from i18n import TEXT
        words=TEXT[self.config['ui_language']]
        source=self.config['iso']
        key_path=self.config.get('disc_key','')
        marker=(iso_service.fingerprint(source),iso_service.fingerprint(key_path) if key_path and Path(key_path).is_file() else key_path)
        if self.prepared_iso and self.prepared_iso['marker']==marker:
            try:
                if iso_service.fingerprint(self.prepared_iso['path'])==self.prepared_iso['fingerprint']:
                    return self.prepared_iso['path']
            except OSError:pass
        info=self.inspect_iso()
        if info['kind']=='ready':return source
        self.require_disc(info)
        root=Path(self.config['folder'])
        tools=child(root,'.local-build/tools')
        rpcs3=tools/'rpcs3/rpcs3.exe'
        env=dict(os.environ);env['PATH']=str(Path(program)/'bin')+os.pathsep+env.get('PATH','')
        def run(args,**kwargs):return external_run(args,env=env,**kwargs)
        def popen(args,**kwargs):return external_popen(args,env=env,**kwargs)
        if not rpcs3.is_file():
            self.message('originalPrepare')
            powershell=str(Path(os.environ.get('SystemRoot','C:/Windows'))/'System32/WindowsPowerShell/v1.0/powershell.exe')
            result=run([powershell,'-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',
                        str(Path(program)/'tools/local_build/setup_tools.ps1'),'-Root',str(tools),'-SkipCompiler'],
                       stdout=subprocess.PIPE,stderr=subprocess.STDOUT,stdin=subprocess.DEVNULL)
            if result.returncode:raise ValueError(words['isoToolFailed'])
        cache=child(root,'.local-build/disc-iso');cache.mkdir(parents=True,exist_ok=True)
        target=child(cache,info['sha1']+'.iso');receipt=target.with_suffix('.json')
        if target.is_file() and receipt.is_file():
            record=json.loads(receipt.read_text())
            if record.get('source_sha1')==info['sha1'] and sha(target)==record.get('sha256'):
                self.prepared_iso=dict(marker=marker,path=str(target),fingerprint=iso_service.fingerprint(target))
                return str(target)
            raise ValueError(words['isoCacheInvalid'])
        def progress(data):
            self.message('isoDecrypting' if data['stage']=='decrypting' else 'isoVerifying')
            if data.get('total'):self.event('transfer',bytes=data['bytes'],total=data['total'],percent=data['bytes']*100//data['total'])
        self.message('isoDecrypting')
        try:
            result=iso_service.prepare(source,target,self.config['disc_key'],self.iso_tools(),rpcs3,progress=progress,
                                       runner=run,popen=popen,verifier=lambda path,tool:iso_service.verify_elf(path,tool,run))
        except Exception as error:raise ValueError(words['isoPrepareFailed']+'\n'+str(error)) from error
        result['source_sha1']=info['sha1'];receipt.write_text(json.dumps(result,indent=2))
        self.prepared_iso=dict(marker=marker,path=str(target),fingerprint=iso_service.fingerprint(target))
        return str(target)

    def download(self, row, launcher=False):
        pattern = r'Macross30Launcher-\d+\.\d+\.\d+\.exe' if launcher else r'Macross30-(?:program|language-(?:ko-KR|en-US))-\d+\.\d+\.\d+\.zip'
        if not re.fullmatch(pattern, row.get('file', '')) or not re.fullmatch(r'[a-f0-9]{64}', row.get('sha256', '')) or type(row.get('bytes')) is not int or not 0 < row['bytes'] <= 1024 ** 3:
            raise ValueError('Invalid artifact identity')
        root = self.config_path.parent if launcher else Path(self.config['folder'])
        cache = child(root, 'launcher-cache' if launcher else '.local-build/downloads')
        cache.mkdir(parents=True, exist_ok=True)
        target = child(cache, row['sha256'] + ('.exe' if launcher else '.zip'))
        if target.is_file() and target.stat().st_size == row['bytes'] and sha(target) == row['sha256']:
            self.message('cacheReuse')
            return target
        pending = child(cache, target.name + '.partial')
        count, last, digest = 0, 0, hashlib.sha256()
        try:
            with self.get(row['file']) as response, pending.open('wb') as output:
                while data := response.read(256 * 1024):
                    count += len(data)
                    if count > row['bytes']:
                        raise ValueError('Download exceeds declared size')
                    digest.update(data)
                    output.write(data)
                    if time.monotonic() - last > .15:
                        self.event('transfer', bytes=count, total=row['bytes'], percent=count * 100 // row['bytes'])
                        last = time.monotonic()
            if count != row['bytes'] or digest.hexdigest() != row['sha256']:
                raise ValueError('Download SHA-256 verification failed')
            os.replace(pending, target)
        finally:
            pending.unlink(missing_ok=True)
        return target

    def installer(self, program, payload, language_only=False):
        root = Path(self.config['folder'])
        prepared_iso=self.prepare_disc(program) if self.config['iso'] and Path(self.config['iso']).is_file() else self.config['iso']
        entry = Path(program) / 'tools/local_build' / ('install_language.ps1' if language_only else 'install_prebuilt.ps1')
        args = [str(Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'),
                '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', str(entry),
                '-Root', str(root), '-Payload', str(payload), '-Language', self.config['game_language'] if language_only else 'ja-JP']
        if prepared_iso:
            args += ['-Iso', prepared_iso]
        if not language_only:
            args += ['-Program', str(program), '-ReleaseVersion', self.manifest['version']]
        logs = child(root, 'play')
        logs.mkdir(parents=True, exist_ok=True)
        self.last_log = child(logs, 'language-' + self.config['game_language'] + '.log' if language_only else 'game-prepare.log')
        self.message('languageInstall' if language_only else 'originalPrepare', stage=2 if language_only else 1)
        tail = ''
        with self.last_log.open('w', encoding='utf-8') as output:
            process = external_popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                       cwd=str(program), text=True, encoding='utf-8', errors='replace', creationflags=NO_WINDOW)
            for line in process.stdout:
                output.write(line)
                output.flush()
                tail = (tail + line)[-12000:]
                if line.startswith(('BOOT_', 'LOCAL_')) or re.match(r'^\[\d+/\d+\]', line):
                    self.event('install-line', line=line.strip(), language_only=language_only)
            if process.wait():
                raise RuntimeError(tail)

    def play(self):
        if self.busy or self.running:
            return
        self.busy = True
        self.event('state', state=self.snapshot())
        try:
            self.bridge('stopped')
            if not self.manifest:
                self.check()
            self.inspect()
            if not self.installed.get('core_ready'):
                if not self.config['iso'] or not Path(self.config['iso']).is_file():
                    selected=self.choose_iso()
                    if not selected:return
                    self.config['iso']=selected;self.save()
                self.require_disc(self.inspect_iso())
            locale = self.config['game_language']
            plan = update_plan(self.installed, locale) if self.manifest else {'core': False, 'language': False}
            if any(plan.values()):
                self.check_status()
                if self.stale or self.status['closed']:
                    raise RuntimeError('Service status is unavailable or downloads are closed. Use Update Check to retry.')
                if not self.confirm({'kind': 'patch', 'plan': plan, 'manifest': self.manifest, 'locale': locale}):
                    return
                self.check_status()
                if self.stale or self.status['closed']:
                    raise RuntimeError('Downloads are currently unavailable.')
            program = self.installed.get('program')
            core_row = self.manifest['files']['program'] if self.manifest else None
            if plan['core']:
                self.message('coreUpdate', stage=1)
                archive = self.download(core_row)
                program = self.bridge('extract', archive=str(archive), artifact=core_row)['path']
                self.bridge('stage', program=program, version=self.manifest['version'], artifact=core_row)
            self.inspect()
            if not self.installed['core_ready']:
                if not program or not self.manifest:
                    raise RuntimeError('A verified game package is required for first setup.')
                if not self.config['iso'] or not Path(self.config['iso']).is_file():
                    iso = self.choose_iso()
                    if not iso:
                        return
                    self.config['iso'] = iso
                    self.save()
                self.installer(program, Path(program) / 'base-game')
                self.bridge('record', program=program, artifact=core_row)
                self.inspect()
                if not self.installed['core_ready']:
                    raise RuntimeError('Original game verification failed')
            if plan['language']:
                self.message('languageUpdate', stage=2)
                row = self.manifest['language_packs'][locale]
                archive = self.download(row)
                payload = self.bridge('extract-language', archive=str(archive), artifact=row)['path']
                self.installer(program, payload, language_only=True)
                self.bridge('record-language', version=version_from(row))
            self.inspect()
            if not self.installed['language_ready']:
                raise RuntimeError('The selected language is not installed')
            self.bridge('select', ui=self.config['ui_language'])
            self.prepare_graphics()
            if self.launch_aborted:
                self.message('graphicsPrepareCancelled')
                return
            self.message('launch', stage=3)
            self.running, self.busy = True, False
            self.event('state', state=self.snapshot())
            result = self.bridge('launch', ui=self.config['ui_language'])
            self.preserve_session(result['exit_code'])
            self.message('gameExited')
        finally:
            self.busy = self.running = False
            self.inspect()
            self.event('state', state=self.snapshot())

    def prepare_graphics(self):
        """First run / after update: build this PC's GPU pipeline library. Never blocks launch."""
        self.launch_aborted = False
        root = Path(self.config['folder'])
        options = launch_options.parse(launch_options.read(root / 'play/settings.ini') or b'')
        if options['hd_textures'] == '1' and (root / 'bin/assets/aircraft-hd-recipe.json').is_file():
            self.message('hdPrepare', stage=3)
            try:
                self.event('hd-prep', **aircraft_hd.prepare(root))
            except (OSError, ValueError, KeyError) as error:
                raise RuntimeError('HD texture preparation failed: '+str(error)) from error
        prep = graphics_prep.Preparation(self.config['folder'], popen=external_popen)
        if not prep.available():
            return None
        self.graphics_prep = prep
        self.message('graphicsPrepare', stage=3)

        def progress(done, total):
            self.event('transfer', bytes=done, total=total, percent=done * 100 // max(total, 1), unit='items')
        try:
            result = prep.prepare(progress)
        finally:
            self.graphics_prep = None
        self.event('graphics-prep', **result)
        return result

    def cancel_graphics_prep(self):
        """Stop preparation (finished pipelines are kept) and do not start the game."""
        prep = self.graphics_prep
        if prep:
            self.launch_aborted = True
            prep.cancel()
        return prep is not None

    def launcher_first(self, executable, action):
        """Never start game/package work before launcher update and handoff."""
        work = self.self_update(executable, automatic=True)
        if work:
            return work
        action()
        return None

    def self_update(self, executable, automatic=False):
        self.message('launcherChecking')
        row = self.json('PYTHON-LAUNCHER.json')
        if not re.fullmatch(r'\d+\.\d+\.\d+', row.get('version', '')) or row.get('file') != 'Macross30Launcher-'+row['version']+'.exe':
            raise ValueError('Invalid launcher metadata')
        if tuple(map(int, row['version'].split('.'))) <= tuple(map(int, VERSION.split('.'))):
            self.event('self-current')
            return None
        if not automatic and not self.confirm({'kind': 'launcher', 'version': row['version']}):
            return None
        self.message('launcherDownload')
        downloaded = self.download(row, launcher=True)
        executable = Path(executable).absolute()
        work = child(executable.parent, '.launcher-update-' + uuid.uuid4().hex)
        work.mkdir()
        shutil.copyfile(downloaded, work / 'new.exe')
        shutil.copyfile(self.bridge_path, work / 'helper.exe')
        # .NET process start ticks are read in the helper rather than inferred from Unix time.
        process = self.bridge('process', pid=os.getpid())
        plan = {'target': str(executable), 'pid': os.getpid(), 'started': process['started'],
                'current_sha256': sha(executable), 'sha256': row['sha256'],
                'config': str(self.config_path.resolve())}
        (work / 'plan.json').write_text(json.dumps(plan), encoding='utf-8')
        request = {'op': 'self-update', 'root': self.config['folder'], 'plan': str(work / 'plan.json')}
        (work / 'request.json').write_text(json.dumps(request), encoding='utf-8')
        external_popen([str(work / 'helper.exe'), str(work / 'request.json')], creationflags=NO_WINDOW,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        return work
