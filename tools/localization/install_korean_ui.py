"""Install the reviewed Korean UI bundle after the game has closed.

Keeps independently hashed backups. Never changes a running game's resources.
Changes only reviewed DATA/FILESET0/FILESET2 packs and PACK.IDX on disc/HDD.
"""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import tempfile
from datetime import datetime, timezone

import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from stage_ui_images import sha_file

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT/'localization/image_assets/ui-ko-v1'


def game_running():
    import ctypes
    from ctypes import wintypes

    class ProcessEntry(ctypes.Structure):
        _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD),
                    ('pid', wintypes.DWORD), ('heap', ctypes.c_size_t),
                    ('module', wintypes.DWORD), ('threads', wintypes.DWORD),
                    ('parent', wintypes.DWORD), ('priority', wintypes.LONG),
                    ('flags', wintypes.DWORD), ('name', wintypes.WCHAR * 260)]

    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(entry)
        present = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while present:
            if entry.name.lower() == 'macross30recompiled.exe':
                return True
            present = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES
            raise ctypes.WinError(ctypes.get_last_error())
        return False
    finally:
        kernel.CloseHandle(snapshot)


def preflight(targets):
    """Validate ALL inputs before the first target write; allow repeat installs."""
    pending = []
    for target, candidate, old_sha, new_sha, backup in targets:
        if sha_file(candidate) != new_sha:
            raise ValueError('Candidate checksum changed: ' + str(candidate))
        current = sha_file(target)
        if current == new_sha:
            continue
        if current != old_sha:
            raise ValueError('Resource changed since bundle preparation: ' + str(target))
        if backup.exists() and sha_file(backup) != old_sha:
            raise ValueError('Backup checksum mismatch: ' + str(backup))
        pending.append((target, candidate, old_sha, new_sha, backup))
    return pending


def checked_copy(source, target, expected):
    shutil.copyfile(source, target)
    if sha_file(target) != expected:
        raise ValueError('Copy checksum mismatch: ' + str(target))


def install(targets):
    pending = preflight(targets)
    prepared = []
    swapped = []
    try:
        # All backups and replacement copies exist before changing any target.
        for target, candidate, old_sha, new_sha, backup in pending:
            backup.parent.mkdir(parents=True, exist_ok=True)
            if not backup.exists():
                checked_copy(target, backup, old_sha)
            fd, name = tempfile.mkstemp(prefix='.korean-ui-', dir=target.parent)
            os.close(fd)
            temp = Path(name)
            prepared.append(temp)
            checked_copy(candidate, temp, new_sha)
        for row, temp in zip(pending, prepared):
            target, _, old_sha, _, backup = row
            # Recheck against concurrent modifications before replacing the name.
            if sha_file(target) != old_sha:
                raise ValueError('Target changed during installation: ' + str(target))
            os.replace(temp, target)
            swapped.append((target, backup, old_sha))
    except Exception:
        for target, backup, old_sha in reversed(swapped):
            fd, name = tempfile.mkstemp(prefix='.korean-ui-restore-', dir=target.parent)
            os.close(fd)
            restore = Path(name)
            checked_copy(backup, restore, old_sha)
            os.replace(restore, target)
        raise
    finally:
        for temp in prepared:
            if temp.exists():
                temp.unlink()
    return len(pending)


def resolve_bundle(name='ui-ko-v1'):
    if not re.fullmatch(r'[a-z0-9-]+', name):
        raise ValueError('Bundle name must contain only lowercase letters, digits and hyphens')
    return ROOT/'localization/image_assets'/name


def bundle_elf(bundle, ready):
    entry = ready.get('elf')
    if entry is not None:
        if not isinstance(entry, dict):
            raise ValueError('ELF metadata must be an object')
        path = (bundle/entry['path']).resolve()
        if bundle.resolve() not in path.parents or sha_file(path) != entry['sha256']:
            raise ValueError('Localized ELF checksum changed or path is outside bundle')
        return path
    if bundle.name == 'ui-ko-v1':
        manifest = json.loads((bundle/'EBOOT.ui-ko.manifest.json').read_text(encoding='utf-8'))
        path = bundle/'EBOOT.ui-ko.elf'
        if sha_file(path) != manifest['output_sha256']:
            raise ValueError('Localized ELF checksum changed')
        return path
    return None


def bundle_targets(bundle=BUNDLE, diagnostic_state=None):
    if not re.fullmatch(r'[a-z0-9-]+', bundle.name):
        raise ValueError('Invalid bundle name')
    ready = json.loads((bundle/'ready.json').read_text(encoding='utf-8'))
    for item in ready['manifests']:
        manifest = (bundle/item['path']).resolve()
        if bundle.resolve() not in manifest.parents or sha_file(manifest) != item['sha256']:
            raise ValueError('Reviewed manifest changed or is outside bundle')
        if (manifest.parent/'REJECTED.json').exists():
            raise ValueError('This staged bundle was rejected')
    bundle_elf(bundle, ready)
    targets = []
    roots = (('disc', ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'),
             ('hdd', ROOT/'native/state-ko-full/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'))
    if diagnostic_state is not None:
        state = Path(diagnostic_state).resolve()
        if not state.is_relative_to(ROOT/'native/analysis') or state.name != 'state' or not state.parent.name.startswith('vf0-hd-'):
            raise ValueError('Expected an isolated VF-0 test state beneath native/analysis')
        roots += (('diagnostic-'+state.parent.name, state/'hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'),)
    filenames = [r['filename'].lower() for r in ready['files']]
    if (not set(filenames) <= {'data.dat', 'data2.dat', 'fileset0.dat', 'fileset2.dat', 'pack.idx'} or
            'pack.idx' not in filenames or len(filenames) != len(set(filenames))):
        raise ValueError('Unexpected UI bundle target set')
    for label, folder in roots:
        for row in ready['files']:
            filename = row['filename']
            if Path(filename).name != filename:
                raise ValueError('Expected flat pack filename')
            target = folder/filename
            if not target.exists():
                raise FileNotFoundError(target)
            candidate = (bundle/row['candidate']).resolve()
            if bundle.resolve() not in candidate.parents:
                raise ValueError('Candidate outside bundle')
            backup = ROOT/'native/localization-backups'/bundle.name/label/filename
            targets.append((target, candidate, row['source_sha256'], row['sha256'], backup))
    return targets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='install; otherwise validate only')
    parser.add_argument('--bundle', default='ui-ko-v1', help='Reviewed bundle name under localization/image_assets')
    parser.add_argument('--diagnostic-state', type=Path, help='Also synchronize an isolated VF-0 test HDD state')
    args = parser.parse_args()
    bundle = resolve_bundle(args.bundle)
    if game_running():
        raise SystemExit('Game is running. Close it before installing Korean UI.')
    import msvcrt
    lease = (ROOT/'native/runner.lock').open('a+b')
    lease.seek(0)
    try:
        msvcrt.locking(lease.fileno(), msvcrt.LK_NBLCK, 1)
    except OSError:
        raise SystemExit('Game/test is running. Close the game before installing Korean UI.')
    targets = bundle_targets(bundle, args.diagnostic_state)
    count = install(targets) if args.apply else len(preflight(targets))
    if args.apply:
        ready = json.loads((bundle/'ready.json').read_text(encoding='utf-8'))
        elf = bundle_elf(bundle, ready)
        record = dict(installed_at=datetime.now(timezone.utc).isoformat(),
                      bundle_version=ready.get('version'),
                      replaced_files_this_invocation=count, installed_files=len(targets),
                      resource_files=[dict(path=str(r[0]), sha256=r[3], source_sha256=r[2],
                                                               backup=str(r[4])) for r in targets],
                      elf=str(elf) if elf else None, in_game_verified=False)
        (bundle/'installation.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
        if args.diagnostic_state:
            (args.diagnostic_state.resolve()/'ui-bundle-installed.json').write_text(
                json.dumps(dict(bundle=bundle.name, version=ready.get('version')), indent=2), encoding='utf8')
        ready_path = bundle/'ready.json'
        ready = json.loads(ready_path.read_text(encoding='utf-8'))
        ready.update(status='installed; asset verification passed; awaiting in-game display verification',
                     active_resources_modified=True, active_game_modified=False)
        ready_path.write_text(json.dumps(ready, indent=2), encoding='utf-8')
    print(json.dumps(dict(mode='installed' if args.apply else 'validated',
                          files=count, backups=str(ROOT/'native/localization-backups'/bundle.name))))


if __name__ == '__main__':
    main()
