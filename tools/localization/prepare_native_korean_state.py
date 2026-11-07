"""Prepare a separate HDD installation for the existing Korean text draft.

With --dry-run, validate the reviewed patch files without copying HDD data.
The Japanese VFS/state and the existing destination are never overwritten.
"""
import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import uuid


ROOT = Path(__file__).resolve().parents[2]
INSTALL = Path('hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack')
DISC_PACK = Path('PS3_GAME/USRDIR/DATA/PACK')
PATCH_NAMES = ('DATA.DAT', 'PACK.IDX')


def digest(path):
    sha = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            sha.update(block)
    return sha.hexdigest()


def plain(path):
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
        raise ValueError(f'Links/reparse points are not accepted: {path}')
    return info


def case_path(base, relative):
    """Resolve each component by casefold; reject ambiguous or linked paths."""
    current = base
    plain(current)
    for part in Path(relative).parts:
        matches = [p for p in current.iterdir() if p.name.casefold() == part.casefold()]
        if len(matches) != 1:
            raise ValueError(f'Expected exactly one case-insensitive match: {current / part}')
        current = matches[0]
        plain(current)
    return current


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def inventory(base):
    rows = []
    plain(base)
    for folder, directories, files in os.walk(base, followlinks=False):
        for name in sorted(directories + files):
            path = Path(folder) / name
            info = plain(path)
            if path.is_file():
                rows.append((path.relative_to(base), info.st_size, info.st_mtime_ns))
    return sorted(rows)


def require_game_closed():
    if os.name != 'nt':
        raise RuntimeError('Actual preparation currently requires the Windows process check')
    # tasklist may depend on services unavailable in the restricted host.
    # Toolhelp enumerates the actual process table without that dependency.
    import ctypes
    from ctypes import wintypes as w
    class Entry(ctypes.Structure):
        _fields_=[('size',w.DWORD),('usage',w.DWORD),('pid',w.DWORD),
                  ('heap',ctypes.c_size_t),('module',w.DWORD),('threads',w.DWORD),
                  ('parent',w.DWORD),('priority',w.LONG),('flags',w.DWORD),('name',w.WCHAR*260)]
    k=ctypes.WinDLL('kernel32',use_last_error=True)
    k.CreateToolhelp32Snapshot.restype=w.HANDLE
    k.Process32FirstW.argtypes=[w.HANDLE,ctypes.POINTER(Entry)]
    k.Process32NextW.argtypes=[w.HANDLE,ctypes.POINTER(Entry)]
    k.CloseHandle.argtypes=[w.HANDLE]
    handle=k.CreateToolhelp32Snapshot(2,0)
    if handle==ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry=Entry(); entry.size=ctypes.sizeof(entry)
        ok=k.Process32FirstW(handle,ctypes.byref(entry))
        if not ok:
            raise ctypes.WinError(ctypes.get_last_error())
        while ok:
            if entry.name.casefold()=='macross30recompiled.exe':
                raise RuntimeError('Close native game tests before copying their HDD state')
            ok=k.Process32NextW(handle,ctypes.byref(entry))
        if ctypes.get_last_error()!=18:
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        k.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-state', type=Path, default=ROOT / 'native/state')
    parser.add_argument('--destination', type=Path, default=ROOT / 'native/state-ko')
    parser.add_argument('--vfs', type=Path, default=ROOT / 'build/game-next')
    parser.add_argument('--dry-run', action='store_true', help='Validate and print metadata only')
    args = parser.parse_args()
    source, destination, vfs = (p.absolute() for p in (args.source_state, args.destination, args.vfs))
    native = (ROOT / 'native').resolve()
    # Validate lexical ancestors as well as the resolved target: no junction escape.
    for path in (source, destination, vfs):
        if not path.resolve().is_relative_to(ROOT.resolve()):
            raise ValueError(f'Path must stay inside the workspace: {path}')
        for ancestor in (path, *path.parents):
            if ancestor.exists():
                plain(ancestor)
            if ancestor == ROOT:
                break
    source, destination, vfs = source.resolve(), destination.resolve(), vfs.resolve()
    if destination.exists():
        raise FileExistsError(f'Preserving existing destination without changes: {destination}')
    if destination.parent != native or destination == source or source.is_relative_to(destination):
        raise ValueError('Destination must be a new direct child of native/, separate from source state')

    build = read_json(ROOT / 'analysis/game_build_validation.json')
    full = read_json(ROOT / 'analysis/full_game_validation.json')
    archive = read_json(ROOT / 'analysis/archive_validation.json')
    for report in (build, full):
        if Path(report['destination']).resolve() != vfs:
            raise ValueError('VFS does not match the existing reviewed build/verification destination')
    if build.get('original_iso_modified') is not False or full['files_verified'] != build['files']:
        raise ValueError('Build and full verification reports disagree')
    expected_paths = {(DISC_PACK / name).as_posix().casefold() for name in PATCH_NAMES}
    hashes = {name.replace('\\', '/').casefold(): value
              for name, value in full['allowed_modified_files'].items()}
    if set(hashes) != expected_paths or archive['sha256'] != build['archive_sha256']:
        raise ValueError('Unexpected patch scope or archive hash mismatch')
    payload = ROOT / 'build/patch/gop_archive.fsts'
    if digest(payload) != archive['sha256'] or payload.stat().st_size != archive['rebuilt_size']:
        raise ValueError('Rebuilt archive differs from the reviewed manifest')

    patches = []
    for name in PATCH_NAMES:
        patched = case_path(vfs, DISC_PACK / name)
        installed = case_path(source, INSTALL / name)
        expected = hashes[(DISC_PACK / name).as_posix().casefold()]
        if digest(patched) != expected:
            raise ValueError(f'Patched VFS file hash mismatch: {patched}')
        if patched.stat().st_size != installed.stat().st_size:
            raise ValueError(f'Installed file size differs from bounded patch source: {installed}')
        patches.append(dict(source=str(patched), target=installed.relative_to(source).as_posix(),
                            sha256=expected, size=patched.stat().st_size))

    groups = []
    for path in sorted((ROOT / 'localization/ko-KR/structured').glob('*.json')):
        rows = read_json(path)['entries']
        groups.append(dict(group=path.stem, candidates=len(rows),
                           translated=sum(bool(row.get('translation')) for row in rows)))
    source_files = inventory(source)
    manifest = dict(kind='isolated Korean draft HDD state', source_state=str(source),
                    destination=str(destination), patched_vfs=str(vfs), dry_run=args.dry_run,
                    source_files=len(source_files), source_bytes=sum(r[1] for r in source_files),
                    patch_files=patches, archive_sha256=archive['sha256'],
                    translation_groups=groups,
                    translated_entries=sum(g['translated'] for g in groups),
                    candidate_entries=sum(g['candidates'] for g in groups),
                    source_state_modified=False, original_vfs_modified=False, hardlinks_used=False,
                    font_rendering_verified=False, runtime_verified=False,
                    scope='Three GOP text drafts only; no font, layout, gameplay or full-translation claim')
    if args.dry_run:
        print(json.dumps(manifest, ensure_ascii=False, indent=2))
        return

    require_game_closed()
    staging = native / ('.' + destination.name + '.preparing-' + uuid.uuid4().hex)
    staging.mkdir()  # A failure leaves this uniquely named partial copy for inspection.
    for relative, size, mtime in source_files:
        original, copied = source / relative, staging / relative
        copied.parent.mkdir(parents=True, exist_ok=True)
        before = plain(original)
        if (before.st_size, before.st_mtime_ns) != (size, mtime):
            raise RuntimeError(f'Source state changed before copying: {original}')
        shutil.copy2(original, copied)  # Independent file contents, never hard links.
        if digest(original) != digest(copied):
            raise RuntimeError(f'Independent copy failed byte verification: {relative}')
        after = plain(original)
        if (after.st_size, after.st_mtime_ns) != (size, mtime):
            raise RuntimeError(f'Source state changed while copying: {original}')
    if inventory(source) != source_files:
        raise RuntimeError('Source state inventory changed during preparation')
    for patch in patches:
        copied = staging / patch['target']
        shutil.copy2(Path(patch['source']), copied)
        if digest(copied) != patch['sha256']:
            raise RuntimeError(f'Isolated patch copy failed hash verification: {copied}')
    # Preserve empty state directories as well.
    for folder, directories, _ in os.walk(source):
        for name in directories:
            (staging / (Path(folder) / name).relative_to(source)).mkdir(parents=True, exist_ok=True)
    manifest['dry_run'] = False
    manifest['copied_files_verified'] = len(source_files)
    (staging / 'korean_state_manifest.json').write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    staging.rename(destination)  # Fails if a destination appeared; never overwrite it.
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
