"""Prepare a selected language view; retain other installed languages for instant switching."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

SUPPORTED = ('ja-JP', 'ko-KR', 'en-US')


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def write_changed(path, data):
    path = Path(path)
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name(path.name + '.pending')
    pending.write_bytes(data)
    os.replace(pending, path)


def plan_rows(path):
    rows = []
    for line in Path(path).read_text(encoding='utf-8-sig').splitlines():
        if not line or line.startswith('#'):
            continue
        fields = line.split('\t')
        fields += [''] * (6 - len(fields))
        kind, target, size, digest, source, delta = fields[:6]
        if kind not in ('dir', 'iso', 'copy', 'delta'):
            raise ValueError('Unknown language-plan operation')
        if not target.startswith(('data/disc/', 'data/hdd0/game/', 'data/hdd1')):
            raise ValueError('Language plan must never target user saves: ' + target)
        rows.append(dict(kind=kind, target=target, size=int(size) if size else 0,
                         sha256=digest, source=source, delta=delta))
    return rows


def child(root, relative):
    root = Path(root).resolve()
    path = root / relative
    if not path.resolve().is_relative_to(root):
        raise ValueError('Language-view path escaped root')
    for parent in [path, *path.parents]:
        if parent == root.parent:
            break
        if parent.is_symlink() or (hasattr(parent, 'is_junction') and parent.is_junction()):
            raise ValueError('Links are not accepted as language-view directories')
    return path


def ready(root, row):
    try:
        view = child(root, row['root'])
        marker = view / 'data/installed-version.txt'
        if marker.read_text().strip() != row['data_id']:
            return False
        for file in row['files']:
            stat = child(view, file['path']).stat()
            if stat.st_size != file['size'] or stat.st_mtime_ns != file['mtime_ns']:
                return False
        return True
    except (OSError, ValueError, KeyError):
        return False


def powershell_install(root, payload, iso, plan, data_id):
    script = Path(root) / '.local-build/install-language-view.ps1'
    write_changed(script, b'''param([string]$Target,[string]$Payload,[string]$Iso,[string]$Plan,[string]$DataId)
$ErrorActionPreference='Stop'
$env:PSModulePath=@((Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/Modules'),(Join-Path $env:ProgramFiles 'WindowsPowerShell/Modules')) -join ';'
Add-Type -TypeDefinition ([IO.File]::ReadAllText((Join-Path $Payload 'DistInstaller.cs'))) -Language CSharp
$problems=[MacrossDist.Installer]::Run($Payload,$Target,$Plan,$Iso,'install',$true,'',$DataId)
if($problems -ne 0){throw 'Language-view installation failed'}
''')
    shell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
    subprocess.run([str(shell), '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                    '-File', str(script), '-Target', str(root), '-Payload', str(payload),
                    '-Iso', str(iso), '-Plan', str(plan), '-DataId', data_id], check=True)


def prepare_views(root, payload, iso, runner=powershell_install, locale=None):
    root, payload = Path(root).resolve(), Path(payload).resolve()
    recipe = json.loads((payload / 'recipe.json').read_text(encoding='utf8'))
    state_file = root / '.local-build/language-views.json'
    previous = json.loads(state_file.read_text(encoding='utf8')) if state_file.exists() else {'languages': {}}
    if locale is not None and (locale not in SUPPORTED or locale not in recipe['languages']):
        raise ValueError('Selected language is not available in this payload')
    result = {'schema': 1, 'languages': dict(previous['languages'])}
    hashed = {}
    for locale in ([locale] if locale else SUPPORTED):
        if locale not in recipe['languages']:
            continue
        language = recipe['languages'][locale]
        plan = child(payload, language['plan'])
        plan_sha = sha(plan)
        old = previous['languages'].get(locale)
        if old and old['data_id'] == language['data_id'] and old['plan_sha256'] == plan_sha and ready(root, old):
            result['languages'][locale] = old
            print('LOCAL_LANGUAGE_REUSED: ' + locale, flush=True)
            continue
        print('LOCAL_LANGUAGE_INSTALL: ' + locale, flush=True)
        view_relative = 'data/languages/' + locale
        view = child(root, view_relative)
        view.mkdir(parents=True, exist_ok=True)
        rows = plan_rows(plan)
        # Reuse verified immutable game files. Never link saves or settings.
        candidates = [root] + [root / ('data/languages/' + code) for code in SUPPORTED if code != locale]
        for file in rows:
            if file['kind'] == 'dir':
                continue
            target = child(view, file['target'])
            if target.exists():
                continue
            for candidate in candidates:
                source = child(candidate, file['target'])
                if not source.is_file() or source.stat().st_size != file['size']:
                    continue
                stat = source.stat()
                key = (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)
                if key not in hashed:
                    hashed[key] = sha(source)
                if hashed[key] != file['sha256']:
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                try:
                    os.link(source, target)
                except OSError:
                    # Cross-volume or non-NTFS: the normal installer handles it.
                    pass
                break
        runner(view, payload, iso, plan, language['data_id'])
        files = []
        for file in rows:
            if file['kind'] == 'dir':
                continue
            path = child(view, file['target'])
            stat = path.stat()
            if stat.st_size != file['size']:
                raise ValueError('Language installation did not produce expected file')
            files.append(dict(path=file['target'], size=stat.st_size, mtime_ns=stat.st_mtime_ns,
                              sha256=file['sha256']))
        info = dict(root=view_relative, disc=view_relative + '/data/disc',
                    hdd0_game=view_relative + '/data/hdd0/game', data_id=language['data_id'],
                    plan_sha256=plan_sha, files=files, elf='bin/EBOOT.' + locale + '.elf',
                    elf_sha256=language['elf_sha256'])
        if not ready(root, info):
            raise ValueError('Language-view completion check failed')
        write_changed(view / 'expected-version.txt', (language['data_id'] + '\n').encode())
        result['languages'][locale] = info
        # Persist successful work even if the next language fails; selection is not changed here.
        checkpoint = dict(previous)
        checkpoint['languages'] = {**previous['languages'], **result['languages']}
        write_changed(state_file, (json.dumps(checkpoint, indent=2) + '\n').encode())
    if not result['languages']:
        raise ValueError('No supported languages in this package')
    write_changed(state_file, (json.dumps(result, indent=2) + '\n').encode())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--payload', required=True, type=Path)
    parser.add_argument('--iso', required=True, type=Path)
    parser.add_argument('--locale', choices=SUPPORTED)
    args = parser.parse_args()
    prepare_views(args.root, args.payload, args.iso, locale=args.locale)


if __name__ == '__main__':
    main()
