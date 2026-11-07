"""Portable data-pack install and resolution, independent of a developer checkout."""
from contextlib import contextmanager
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import uuid

if not getattr(sys, 'frozen', False):
    for directory in ('play', 'localization', 'analysis'):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / directory))

from pack_registry import (create_inherited_stage, digest, discover_packs, import_pack,
                           registry_root, resolve_pack, safe_path, valid_locale, validate_pack)

BASE_FILE = 'bin/assets/locale-base.json'
BUILTINS = ('ko-KR', 'en-US', 'ja-JP')
ENGLISH_OVERLAY = 'en-US-fix'
_verified = {}


def internal(locale):
    return ENGLISH_OVERLAY if locale == 'en-US' else locale


def metadata(manifest):
    rows = (manifest or {}).get('locale_packs', {})
    if not isinstance(rows, dict):
        raise ValueError('Invalid text-pack catalog')
    for locale, row in rows.items():
        if not valid_locale(locale) or locale in ('ja-JP', 'ko-KR') or not isinstance(row, dict):
            raise ValueError('Invalid text-pack locale')
        if not re.fullmatch(r'Macross30-text-' + re.escape(locale) + r'-\d+\.\d+\.\d+\.zip', row.get('file', '')):
            raise ValueError('Invalid text-pack artifact name')
        if not re.fullmatch('[a-f0-9]{64}', row.get('sha256', '')) or not re.fullmatch('[a-f0-9]{64}', row.get('manifest_sha256', '')):
            raise ValueError('Invalid text-pack identity')
        if type(row.get('bytes')) is not int or not 0 < row['bytes'] <= 1024**3:
            raise ValueError('Invalid text-pack archive bounds')
    return rows


def available(root, manifest):
    result = {locale: dict(locale=locale, label=locale, status='available', ready=False)
              for locale in metadata(manifest)}
    for row in discover_packs(root, verify=False):
        if row['locale'] == ENGLISH_OVERLAY:
            row = dict(row, locale='en-US')
        result[row['locale']] = {**result.get(row['locale'], {}), **row}
    for locale, row in metadata(manifest).items():
        result[locale]['label'] = row.get('label') or result[locale]['label']
    return result


def selected(root, locale, manifest):
    return locale in metadata(manifest) or (locale not in ('ko-KR', 'ja-JP') and
           safe_path(registry_root(root), internal(locale) + '/pack.json').is_file())


def installed(root, locale, manifest):
    folder = safe_path(registry_root(root), internal(locale))
    try:
        doc, resources = validate_pack(root, folder, verify=True)
        identity = digest(folder / 'pack.json')
        row = metadata(manifest).get(locale)
        current = not row or identity == row['manifest_sha256']
        return dict(ready=True, current=current, manifest_sha256=identity, doc=doc, folder=folder,
                    version=re.search(r'-(\d+\.\d+\.\d+)\.zip$', row['file'])[1] if row else 'local')
    except (OSError, ValueError, KeyError, TypeError) as error:
        return dict(ready=False, current=False, error=str(error))


def verify_cached(path, expected):
    info = path.stat()
    key = (str(path), info.st_size, info.st_mtime_ns, expected)
    if key not in _verified:
        if digest(path) != expected:
            raise ValueError('English image base changed: ' + path.name)
        _verified[key] = True


def base_profile(root):
    root = Path(root)
    spec = json.loads(safe_path(root, BASE_FILE).read_text(encoding='utf-8'))
    if spec.get('schema') != 1 or spec.get('runtime_version') != '0.3.9':
        raise ValueError('Update the common game runtime before installing text packs')
    verify_cached(safe_path(root, 'bin/Macross30Recompiled.exe'), spec['runtime_exe_sha256'])
    views = json.loads(safe_path(root, '.local-build/language-views.json').read_text(encoding='utf-8'))['languages']
    view = views['en-US']
    if view['data_id'] != spec['english_data_id'] or view['plan_sha256'] != spec['english_plan_sha256']:
        raise ValueError('Install the matching English image base first')
    elf = safe_path(root, 'bin/EBOOT.en-US.elf')
    verify_cached(elf, spec['english_elf_sha256'])
    vfs = safe_path(root, view['disc'])
    for row in spec['pack_files']:
        verify_cached(safe_path(vfs, 'PS3_GAME/USRDIR/DATA/PACK/' + row['file']), row['sha256'])
    state = safe_path(root, '.local-build/locale-base-state/' + spec['english_data_id'])
    hdd = safe_path(root, view['hdd0_game'])
    if not state.exists():
        stage = create_inherited_stage(state.parent if state.parent.exists() else _mkdir(state.parent), '.base-')
        try:
            shutil.copytree(hdd, stage / 'hdd0/game', copy_function=os.link)
            stage.rename(state)
        finally:
            if stage.exists():
                shutil.rmtree(stage)
    return dict(locale='en-US', elf=elf, vfs=vfs, state=state, status='complete')


def _mkdir(path):
    path.mkdir(parents=True, exist_ok=True)
    return path


@contextmanager
def stopped(root):
    lock = safe_path(root, 'play/runner.lock')
    lock.parent.mkdir(parents=True, exist_ok=True)
    if os.name == 'nt':
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = (ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                      ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p)
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
        handle = kernel.CreateFileW(str(lock), 0xC0000000, 0, None, 4, 0x80, None)
        if handle == ctypes.c_void_p(-1).value:
            raise RuntimeError('Close the running game before changing language packs')
        try:
            yield
        finally:
            kernel.CloseHandle(handle)
    else:
        import fcntl
        with lock.open('a+b') as stream:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            yield


def install(root, source, expected=None, prepare_cache=True):
    root = Path(root)
    if expected and (Path(source).stat().st_size != expected['bytes'] or digest(source) != expected['sha256']):
        raise ValueError('Downloaded text-pack identity mismatch')
    with stopped(root):
        base = base_profile(root)
        work = create_inherited_stage(_mkdir(safe_path(root, '.local-build')), '.text-import-')
        destination = backup = None
        try:
            guard = safe_path(work, BASE_FILE)
            guard.parent.mkdir(parents=True)
            shutil.copyfile(safe_path(root, BASE_FILE), guard)
            row = import_pack(work, source)
            staged = safe_path(registry_root(work), row['locale'])
            doc, resources = validate_pack(root, staged)
            if expected and digest(staged / 'pack.json') != expected['manifest_sha256']:
                raise ValueError('Downloaded text-pack manifest mismatch')
            if prepare_cache:
                from pack_vfs import materialize_pack_vfs
                materialize_pack_vfs(root, doc, staged, base, resources['gop_overlay'])
            destination = safe_path(registry_root(root), doc['locale'])
            destination.parent.mkdir(parents=True, exist_ok=True)
            if destination.exists():
                backups = _mkdir(safe_path(root, '.local-build/text-pack-backups'))
                backup = backups / (doc['locale'] + '-' + uuid.uuid4().hex)
                destination.rename(backup)
            try:
                staged.rename(destination)
                validate_pack(root, destination)
            except BaseException:
                if destination.exists():
                    shutil.rmtree(destination)
                if backup:
                    backup.rename(destination)
                raise
            return dict(locale='en-US' if doc['locale'] == ENGLISH_OVERLAY else doc['locale'],
                        manifest_sha256=digest(destination / 'pack.json'), backup=str(backup) if backup else None)
        finally:
            if work.exists():
                shutil.rmtree(work)


def activate(root, locale, allow_partial=False):
    with stopped(root):
        return _activate(root, locale, allow_partial)


def _activate(root, locale, allow_partial=False):
    root = Path(root)
    base = base_profile(root)
    def resolve_base(code, seen):
        if code != 'en-US':
            raise ValueError('Unsupported text-pack base')
        return base
    profile = resolve_pack(root, internal(locale), allow_partial=allow_partial, resolve_base=resolve_base)
    values = dict(schema=1, locale=locale,
                  runtime_exe_sha256=json.loads(safe_path(root, BASE_FILE).read_text())['runtime_exe_sha256'])
    for key in ('elf', 'vfs', 'subtitles', 'battle_catalog', 'elf_overlay', 'native_ui'):
        if key in profile:
            values[key] = Path(profile[key]).relative_to(root).as_posix()
    values['hdd0_game'] = (Path(profile['state']) / 'hdd0/game').relative_to(root).as_posix()
    command = safe_path(root, 'Macross30.cmd')
    text = command.read_text(encoding='utf-8')
    old = '"%~1" "%~2" "%~3" 1>"%~4\\stdout.log" 2>"%~4\\stderr.log"'
    new = '"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~4\\..\\tools\\play\\launch-locale.ps1" -Root "%~4\\.." -BaseElf "%~2" -BaseVfs "%~3"'
    if old in text:
        if text.count(old) != 1:
            raise ValueError('Unexpected installed launch script')
        safe_path(root, '.local-build/Macross30.cmd.before-text-packs').write_bytes(command.read_bytes())
        pending = command.with_suffix('.text-new')
        pending.write_bytes(text.replace(old, new).replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8'))
        os.replace(pending, command)
    elif 'tools\\play\\launch-locale.ps1' not in text:
        raise ValueError('Update the game installation before selecting a text pack')
    target = safe_path(root, '.local-build/active-locale.json')
    pending = target.with_suffix('.new')
    pending.write_text(json.dumps(values, ensure_ascii=True, indent=2), encoding='utf-8')
    os.replace(pending, target)
    return values


def deactivate(root):
    with stopped(root):
        safe_path(root, '.local-build/active-locale.json').unlink(missing_ok=True)
