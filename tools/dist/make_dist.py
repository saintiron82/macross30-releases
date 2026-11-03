"""Build the Macross 30 QA distribution without any original disc data.

Output (default C:/GameDev/Macross-dist):
  Macross30/            runnable QA folder
    (program pack)      Macross30.cmd, Install.cmd, VerifyData.cmd, CollectLogs.cmd,
                        bin/ (exe, VC runtime, localized EBOOT, ffmpeg/), tools/, docs,
                        program-manifest.json
    korean/             Korean resource pack: MKDELTA1 deltas against the original
                        disc files + install-plan.tsv (SHA-256 of every installed file)
    data/ play/ firmware/  created on the QA machine by Install.cmd / Macross30.cmd
                        (never written or deleted by this script)
  packages/             Macross30-program-<ver>.zip, Macross30-korean-<ver>.zip (--zip)
  work/                 hash cache, delta cache, build report

The plan is generated from the current good trees (build/game-ko-full and
native/state-ko-full/hdd0); unchanged files must be byte-identical to the
original disc (native/vfs/source_manifest.json) and are extracted from the
user's ISO by the installer, changed files ship only as deltas.

  python tools/dist/make_dist.py [--target DIR] [--zip] [--skip-check]
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import io
import json
import mmap
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from inspect_iso import inventory as iso_inventory  # noqa: E402
import check_dist_env  # noqa: E402
from release_profile import STABLE_ENV, NORMALIZED_ENV
from release_inputs import program_inputs

TEMPLATE = ROOT / 'tools/dist/template'
FFMPEG = ('avutil-61.dll', 'avcodec-63.dll', 'avformat-63.dll', 'swresample-7.dll')  # pam_decoder.cpp loads util/codec/format; codec imports swresample
VC_RUNTIME = ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')  # exe imports (PE import table)
# Present in build/game-ko-full but not needed by the game: build metadata with workspace paths.
EXCLUDED_VFS_FILES = {'read_only_shared_assets.json'}
OWNED_DIRS = ('bin', 'tools', 'korean')           # rebuilt on every run
USER_DIRS = ('data', 'play', 'firmware', 'bug-reports')  # never touched
TEXT_SUFFIXES = {'.cmd', '.ps1', '.cs', '.txt'}
MAGIC = b'MKDELTA1'
CHUNK = 8 << 20


def sha_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(CHUNK), b''):
            digest.update(block)
    return digest.hexdigest()


class HashCache:
    """sha256 per (path, size, mtime_ns) so rebuilds do not re-read 20 GB."""

    def __init__(self, path):
        self.path = path
        self.rows = json.loads(path.read_text(encoding='utf-8')) if path.exists() else {}

    def get(self, path):
        info = path.stat()
        key = str(path)
        row = self.rows.get(key)
        if row and row[0] == info.st_size and row[1] == info.st_mtime_ns:
            return row[2]
        digest = sha_file(path)
        after = path.stat()
        if (after.st_size, after.st_mtime_ns) != (info.st_size, info.st_mtime_ns):
            raise RuntimeError(f'file changed while hashing: {path}')
        self.rows[key] = [info.st_size, info.st_mtime_ns, digest]
        return digest

    def save(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(json.dumps(self.rows, indent=0), encoding='utf-8')


def hash_all(cache, paths, jobs):
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        return dict(zip(paths, pool.map(cache.get, paths)))


def resolve_elf(selected_bundle=None):
    """Read-only twin of localization_launch.prepare_localization (never installs)."""
    pointer = ROOT / 'localization/image_assets/active-ui.json'
    if not pointer.exists():
        raise SystemExit('No active localization bundle (localization/image_assets/active-ui.json)')
    active = json.loads(pointer.read_text(encoding='utf-8'))
    name = selected_bundle or active['bundle']
    if not isinstance(name, str) or not re.fullmatch(r'[a-z0-9-]+', name):
        raise SystemExit('Invalid localization bundle name')
    bundle = ROOT / 'localization/image_assets' / name
    ready = json.loads((bundle / 'ready.json').read_text(encoding='utf-8'))
    elf = (bundle / ready['elf']['path']).resolve()
    if bundle.resolve() not in elf.parents or sha_file(elf) != ready['elf']['sha256']:
        raise SystemExit('Localized ELF missing or checksum changed')
    installed_path = bundle / 'installation.json'
    installed = json.loads(installed_path.read_text(encoding='utf-8')) if installed_path.exists() else {}
    if installed.get('bundle_version') != ready['version']:
        raise SystemExit(f'Bundle {name} is not installed into the game trees yet; launch once with play_native.py first')
    override = active.get('elf_override') if name == active['bundle'] else None
    if override:
        selected = (bundle / override['path']).resolve()
        if (override.get('base_elf_sha256') != ready['elf']['sha256'] or
                bundle.resolve() not in selected.parents or sha_file(selected) != override['sha256']):
            raise SystemExit('Localized ELF override checksum/base/path changed')
        elf = selected
    return elf, name, ready, installed


def tree_files(base):
    files, empty_dirs = [], []
    for folder, directories, names in os.walk(base):
        folder = Path(folder)
        if not directories and not names:
            empty_dirs.append(folder.relative_to(base).as_posix())
        files.extend(folder / n for n in names)
    return sorted(files), sorted(empty_dirs)


def make_delta(source, target, source_sha, target_sha, block):
    """MKDELTA1: copy-from-source / literal ops over the target; literals raw-deflated."""
    with open(source, 'rb') as fs, open(target, 'rb') as ft:
        src = mmap.mmap(fs.fileno(), 0, access=mmap.ACCESS_READ)
        tgt = mmap.mmap(ft.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            index = {}
            for offset in range(0, len(src) - block + 1, block):
                index.setdefault(hashlib.md5(src[offset:offset + block]).digest(), offset)
            ops = []
            literal = zlib.compressobj(9, zlib.DEFLATED, -15)
            packed = io.BytesIO()
            literal_bytes = 0
            for offset in range(0, len(tgt), block):
                chunk = tgt[offset:offset + block]
                n = len(chunk)
                match = None
                if ops and ops[-1][0] == 0:
                    follow = ops[-1][1] + ops[-1][2]
                    if src[follow:follow + n] == chunk:
                        match = follow
                if match is None and src[offset:offset + n] == chunk:
                    match = offset
                if match is None and n == block:
                    candidate = index.get(hashlib.md5(chunk).digest())
                    if candidate is not None and src[candidate:candidate + n] == chunk:
                        match = candidate
                if match is not None:
                    if ops and ops[-1][0] == 0 and ops[-1][1] + ops[-1][2] == match:
                        ops[-1][2] += n
                    else:
                        ops.append([0, match, n])
                else:
                    packed.write(literal.compress(chunk))
                    literal_bytes += n
                    if ops and ops[-1][0] == 1:
                        ops[-1][2] += n
                    else:
                        ops.append([1, 0, n])
            packed.write(literal.flush())
            header = io.BytesIO()
            header.write(MAGIC + struct.pack('<II', 1, block))
            header.write(struct.pack('<Q', len(src)) + bytes.fromhex(source_sha))
            header.write(struct.pack('<Q', len(tgt)) + bytes.fromhex(target_sha))
            header.write(struct.pack('<I', len(ops)))
            for kind, offset, length in ops:
                header.write(struct.pack('<BQQ', kind, offset, length))
            header.write(struct.pack('<Q', packed.tell()))
            stats = dict(ops=len(ops), literal_bytes=literal_bytes, copied_bytes=len(tgt) - literal_bytes)
            return header.getvalue() + packed.getvalue(), stats
        finally:
            src.close()
            tgt.close()


def read_delta_header(data):
    if data[:8] != MAGIC or struct.unpack_from('<I', data, 8)[0] != 1:
        raise ValueError('not an MKDELTA1 file')
    block, = struct.unpack_from('<I', data, 12)
    source_size, = struct.unpack_from('<Q', data, 16)
    source_sha = data[24:56].hex()
    target_size, = struct.unpack_from('<Q', data, 56)
    target_sha = data[64:96].hex()
    count, = struct.unpack_from('<I', data, 96)
    ops = [struct.unpack_from('<BQQ', data, 100 + 17 * i) for i in range(count)]
    literal_size, = struct.unpack_from('<Q', data, 100 + 17 * count)
    start = 108 + 17 * count
    return dict(block=block, source_size=source_size, source_sha=source_sha, target_size=target_size,
                target_sha=target_sha, ops=ops, literal=(start, literal_size))


def apply_delta_sha(data, source):
    """Independent re-application (hash only) to prove the delta reproduces the target."""
    header = read_delta_header(data)
    start, size = header['literal']
    if start + size != len(data):
        raise ValueError('delta literal size mismatch')
    inflater = zlib.decompressobj(-15)
    pending = io.BytesIO(inflater.decompress(data[start:]) + inflater.flush())
    digest = hashlib.sha256()
    total = 0
    with open(source, 'rb') as stream:
        for kind, offset, length in header['ops']:
            if kind == 0:
                stream.seek(offset)
                left = length
                while left:
                    part = stream.read(min(CHUNK, left))
                    if not part:
                        raise ValueError('source too short')
                    digest.update(part)
                    left -= len(part)
            else:
                part = pending.read(length)
                if len(part) != length:
                    raise ValueError('literal stream too short')
                digest.update(part)
            total += length
    if pending.read(1):
        raise ValueError('unused literal bytes')
    return total, digest.hexdigest()


def latest_vc_runtime():
    base = Path(r'C:\Program Files\Microsoft Visual Studio')
    candidates = []
    for crt in base.glob('*/*/VC/Redist/MSVC/*/x64/Microsoft.VC14*.CRT'):
        version = crt.parents[1].name
        if re.fullmatch(r'\d+(\.\d+)+', version) and all((crt / n).exists() for n in VC_RUNTIME):
            candidates.append((tuple(int(x) for x in version.split('.')), crt))
    if not candidates:
        raise SystemExit('Visual C++ x64 redistributable DLLs not found under ' + str(base))
    return max(candidates)[1]


def env_blocks():
    """Generate the launcher's static environment from the real play_native.py."""
    base = {k: os.environ[k] for k in ('SYSTEMROOT', 'WINDIR', 'TEMP', 'TMP') if k in os.environ}
    # Static block = the capped launch; the unlock block adds the rest. The
    # template's fps_unlock default (0) matches play_native.py's.
    default_env, _, _, fake_default = check_dist_env.play_native_launch('fps_unlock=0\n', base)
    unlock_env, _, _, fake_unlock = check_dist_env.play_native_launch('fps_unlock=1\n', base)
    try:
        def lines(env, reference, root):
            out = []
            for key in sorted(env):
                if key in reference and reference[key] == env[key]:
                    continue
                value = env[key]
                if value.lower().startswith(str(root).lower() + '\\'):
                    rel = value[len(str(root)) + 1:].replace('\\', '/')
                    if rel not in check_dist_env.PATH_MAP:
                        raise SystemExit(f'play_native.py sets {key} to an unmapped workspace path {rel}; extend PATH_MAP')
                    value = '%_M30_ROOT%\\' + check_dist_env.PATH_MAP[rel].replace('/', '\\')
                elif not re.fullmatch(r'[A-Za-z0-9_.:\-]*', value):
                    raise SystemExit(f'{key}={value!r} is not safe to emit into a .cmd file')
                if not re.fullmatch(r'[A-Z0-9_]+', key):
                    raise SystemExit(f'unexpected variable name {key!r}')
                out.append(f'set "{key}={value}"')
            return out
        removed = set(default_env) - set(unlock_env)
        if removed:
            raise SystemExit(f'fps_unlock removes variables {removed}; template cannot express that')
        static = lines(default_env, base, fake_default)
        unlock = lines(unlock_env, default_env, fake_unlock)
    finally:
        shutil.rmtree(fake_default, ignore_errors=True)
        shutil.rmtree(fake_unlock, ignore_errors=True)
    return static, unlock


def write_text(path, text, bom=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    data = text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8')
    path.write_bytes((b'\xef\xbb\xbf' if bom else b'') + data)


def copy_file(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if source.suffix.lower() in TEXT_SUFFIXES:
        text = source.read_text(encoding='utf-8')
        write_text(target, text, bom=source.name.startswith('README'))
    else:
        shutil.copy2(source, target)


def prune(directory, keep):
    """Delete files under one of OUR directories that are no longer part of the build."""
    if not directory.exists():
        return
    for path in sorted(directory.rglob('*'), reverse=True):
        if path.is_file() and path not in keep:
            path.unlink()
        elif path.is_dir() and not any(path.iterdir()):
            path.rmdir()


def main():
    raise SystemExit('Precompiled game distribution is disabled. Use tools/local_build/source_bundle.py and the user-local build pipeline. No EBOOT or game EXE export is permitted.')
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--target', type=Path, default=Path(r'C:\GameDev\Macross-dist'))
    parser.add_argument('--vfs', type=Path, default=ROOT / 'build/game-ko-full')
    parser.add_argument('--state', type=Path, default=ROOT / 'native/state-ko-full')
    parser.add_argument('--original', type=Path, default=ROOT / 'native/vfs', help='unmodified disc extraction + source_manifest.json')
    parser.add_argument('--iso', type=Path, help='original ISO (default: */*.iso in the workspace); only its directory is read')
    parser.add_argument('--block', type=int, default=2048)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--zip', action='store_true', help='also write packages/*.zip')
    parser.add_argument('--skip-check', action='store_true', help='skip tools/dist/check_dist_env.py')
    parser.add_argument('--profile', choices=('development', 'stable', 'normalized'), default='development')
    parser.add_argument('--bundle', help='explicit installed localization bundle; does not change active-ui.json')
    parser.add_argument('--version', default=(Path(__file__).parent / 'version.txt').read_text(encoding='ascii').strip(), help='public patch version (major.minor.patch)')
    args = parser.parse_args()
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', args.version):
        parser.error('Version must be major.minor.patch')

    target = args.target.resolve()
    if target == ROOT or ROOT in target.parents:
        raise SystemExit('Target must be outside the workspace')
    dist, work, packages = target / 'Macross30', target / 'work', target / 'packages'
    work.mkdir(parents=True, exist_ok=True)
    cache = HashCache(work / 'hash-cache.json')
    started = datetime.now(timezone.utc)

    # Program inputs.
    exe, binary_provenance = program_inputs(ROOT)
    elf, bundle, ready, installed = resolve_elf(args.bundle)
    elf_sha = sha_file(elf)
    vc = latest_vc_runtime()
    ffmpeg = ROOT / 'runtime/rpcs3'
    print(f'EBOOT: {elf} (bundle {bundle})\nVC runtime: {vc}', flush=True)

    # Original disc manifest, checked against the ISO directory (paths and sizes).
    original_manifest = json.loads((args.original / 'source_manifest.json').read_text(encoding='utf-8'))
    originals = {row['path'].casefold(): row for row in original_manifest['files']}
    iso = args.iso or next(ROOT.glob('*/*.iso'), None)
    iso_check = None
    if iso and iso.exists():
        with iso.open('rb') as stream:
            disc = {row['path']: row['size'] for row in iso_inventory(stream) if not row['path'].startswith('PS3_UPDATE/')}
        bad = [r['path'] for r in original_manifest['files'] if disc.get(r['path']) != r['size']]
        if bad or len(disc) != len(original_manifest['files']):
            raise SystemExit(f'ISO directory disagrees with source_manifest.json: {bad[:5]}')
        iso_check = dict(iso=str(iso), iso_size=iso.stat().st_size, files=len(disc), paths_and_sizes_match=True)

    # Inventory and hash the good trees.
    vfs = args.vfs.resolve()
    hdd0 = (args.state / 'hdd0').resolve()
    vfs_files, vfs_empty = tree_files(vfs)
    hdd_files, hdd_empty = tree_files(hdd0)
    before = {p: (p.stat().st_size, p.stat().st_mtime_ns) for p in vfs_files + hdd_files}
    print(f'Hashing {len(vfs_files)} VFS files and {len(hdd_files)} HDD files ...', flush=True)
    hashes = hash_all(cache, vfs_files + hdd_files, args.jobs)
    cache.save()

    plan, korean, excluded = [], [], []
    seen = set()
    disc_rows = {}
    for path in vfs_files:
        rel = path.relative_to(vfs).as_posix()
        if path.name.casefold() in EXCLUDED_VFS_FILES:
            excluded.append(dict(path=rel, size=path.stat().st_size, reason='build metadata (workspace paths), not read by the game'))
            continue
        original = originals.get(rel.casefold())
        if original is None:
            raise SystemExit(f'{rel} is not on the original disc; extend make_dist.py before shipping new files')
        seen.add(rel.casefold())
        size, sha = path.stat().st_size, hashes[path]
        row = dict(kind='iso', target='data/disc/' + rel, size=size, sha256=sha, source=original['path'], delta='')
        if sha != original['source_sha256']:
            korean.append(dict(rel=rel, path=path, original=original, size=size, sha256=sha))
            row.update(kind='delta', delta='korean/deltas/' + Path(rel).name + '.mkdelta')
            if any(k['rel'] != rel and Path(k['rel']).name == path.name for k in korean):
                raise SystemExit(f'two Korean files share the delta name {path.name}')
        disc_rows[rel.casefold()] = row
        plan.append(row)
    missing = set(originals) - seen
    if missing:
        raise SystemExit(f'original disc files missing from {vfs}: {sorted(missing)[:5]}')

    copy_rows = []
    for path in hdd_files:
        rel = path.relative_to(hdd0).as_posix()
        parts = rel.split('/')
        if parts[:2] != ['game', 'BLJS10184_INSTALL']:
            raise SystemExit(f'unexpected HDD file {rel}')
        disc_rel = 'PS3_GAME/' + '/'.join(parts[2:])
        source = disc_rows.get(disc_rel.casefold())
        if source is None or source['sha256'] != hashes[path]:
            raise SystemExit(f'HDD file {rel} is not a byte copy of the Korean disc file {disc_rel}')
        copy_rows.append(dict(kind='copy', target='data/hdd0/' + rel, size=path.stat().st_size,
                              sha256=hashes[path], source=source['target'], delta=''))
    dir_rows = [dict(kind='dir', target=t, size='', sha256='', source='', delta='')
                for t in ['data/disc/' + d for d in vfs_empty if d != '.'] + ['data/hdd0/' + d for d in hdd_empty if d != '.'] +
                ['data/hdd1']]

    # Delta sources must be the untouched originals recorded from the ISO.
    print(f'{len(korean)} Korean files differ from the disc; verifying originals and building deltas ...', flush=True)
    delta_dir = work / 'deltas'
    delta_dir.mkdir(exist_ok=True)
    delta_files = {}
    for item in korean:
        source = args.original / item['original']['path']
        if cache.get(source) != item['original']['source_sha256']:
            raise SystemExit(f'{source} differs from the ISO-recorded hash')
        cached = delta_dir / f"{Path(item['rel']).name}.{item['original']['source_sha256'][:12]}-{item['sha256'][:12]}.mkdelta"
        if cached.exists():
            data = cached.read_bytes()
            stats = json.loads(cached.with_suffix('.json').read_text(encoding='utf-8'))
        else:
            data, stats = make_delta(source, item['path'], item['original']['source_sha256'], item['sha256'], args.block)
            cached.write_bytes(data)
            cached.with_suffix('.json').write_text(json.dumps(stats), encoding='utf-8')
        # Cached deltas are also re-applied before shipping; a cache hit alone
        # is not evidence that the bytes still reconstruct the selected tree.
        size, digest = apply_delta_sha(data, source)
        if (size, digest) != (item['size'], item['sha256']):
            raise SystemExit(f"delta for {item['rel']} does not reproduce the target")
        stats['verified_by_reapply'] = True
        item.update(delta_bytes=len(data), delta_sha256=hashlib.sha256(data).hexdigest(), **stats)
        delta_files['korean/deltas/' + Path(item['rel']).name + '.mkdelta'] = cached
        print(f"  {item['rel']}: {item['size']:,} -> delta {len(data):,} bytes ({stats['literal_bytes']:,} literal)", flush=True)
    cache.save()

    after = {p: (p.stat().st_size, p.stat().st_mtime_ns) for p in vfs_files + hdd_files}
    if after != before:
        raise SystemExit('Source trees changed during the build (localization work running?); run again')

    # install-plan.tsv: directories, disc files, Korean deltas, HDD copies.
    ordered = dir_rows + [r for r in plan if r['kind'] == 'iso'] + [r for r in plan if r['kind'] == 'delta'] + copy_rows
    tsv = ['# Macross 30 QA install plan (tools/dist/make_dist.py). kind\ttarget\tsize\tsha256\tsource\tdelta']
    tsv += ['\t'.join(str(r[k]) for k in ('kind', 'target', 'size', 'sha256', 'source', 'delta')) for r in ordered]
    plan_text = '\n'.join(tsv) + '\n'
    data_version = hashlib.sha256(plan_text.encode('utf-8')).hexdigest()[:16]

    # Assemble the QA folder: our directories are rebuilt, user directories never touched.
    dist.mkdir(parents=True, exist_ok=True)
    produced = set()

    def put(source, relative):
        destination = dist / relative
        copy_file(source, destination)
        produced.add(destination)

    put(exe, 'bin/Macross30Recompiled.exe')
    if sha_file(dist / 'bin/Macross30Recompiled.exe') != binary_provenance['exe_sha256']:
        raise SystemExit('Selected executable changed while packaging')
    # cellPad's controller backend (DualSense/DS4/Xbox ...; delay-loaded, XInput without it).
    put(exe.parent / 'SDL2.dll', 'bin/SDL2.dll')
    put(elf, 'bin/EBOOT.ko.elf')
    if sha_file(dist / 'bin/EBOOT.ko.elf') != elf_sha:
        raise SystemExit('Selected ELF changed while packaging')
    # Only registered, hash-matched subtitle sidecars ship, never audio/video,
    # transcription intermediates or local review/deployment metadata.
    subtitles = ROOT / 'localization/subtitles/runtime'
    registry = (subtitles / 'tracks.tsv').read_bytes()
    lines = registry.decode('utf-8').splitlines()
    if not lines or lines[0] != 'MACROSS_SUBTITLES_V1':
        raise SystemExit('Invalid subtitle registry')
    subtitle_names = set()
    for line in lines[1:]:
        movie, srt, origin, enabled, movie_sha, srt_sha, status = line.split('\t')
        if not re.fullmatch(r'[A-Za-z0-9_]+(?:\.clean)?\.ko\.srt', srt) or srt in subtitle_names:
            raise SystemExit('Invalid or duplicate subtitle filename')
        if not re.fullmatch(r'[A-Za-z0-9_]+\.pam', movie):
            raise SystemExit('Invalid subtitle movie filename')
        source_movie = vfs / 'PS3_GAME/USRDIR/DATA/MOVIE' / movie
        if sha_file(subtitles / srt) != srt_sha or cache.get(source_movie) != movie_sha:
            raise SystemExit(f'Subtitle/source hash mismatch: {movie}')
        subtitle_names.add(srt)
        put(subtitles / srt, 'bin/subtitles/' + srt)
        if sha_file(dist / 'bin/subtitles' / srt) != srt_sha:
            raise SystemExit(f'Subtitle changed while copying: {srt}')
    if (subtitles / 'tracks.tsv').read_bytes() != registry:
        raise SystemExit('Subtitle registry changed while packaging')
    (dist / 'bin/subtitles/tracks.tsv').write_bytes(registry)
    produced.add(dist / 'bin/subtitles/tracks.tsv')
    for name in VC_RUNTIME:
        put(vc / name, 'bin/' + name)
    for name in FFMPEG:
        put(ffmpeg / name, 'bin/ffmpeg/' + name)
    for source in sorted(p for p in TEMPLATE.rglob('*') if p.is_file() and p.suffix not in ('.template', '.pyc') and '__pycache__' not in p.parts):
        put(source, source.relative_to(TEMPLATE).as_posix())
    static, unlock = env_blocks()
    policy = {'stable': STABLE_ENV, 'normalized': NORMALIZED_ENV}.get(args.profile, {})
    static.extend(f'set "{key}={value}"' for key, value in policy.items())
    static.append(f'set "MACROSS_BATTLE_SUBTITLES={int(binary_provenance["battle_subtitles"])}"')
    launcher = (TEMPLATE / 'Macross30.cmd.template').read_text(encoding='utf-8')
    launcher = launcher.replace('@@ENV_BLOCK@@', '\n'.join(static)).replace('@@UNLOCK_BLOCK@@', '\n'.join(unlock))
    write_text(dist / 'Macross30.cmd', launcher)
    produced.add(dist / 'Macross30.cmd')
    profile = dict(name=args.profile, release_version=args.version, environment=policy, binary_provenance=binary_provenance,
                   localization_bundle=bundle, subtitle_tracks=len(subtitle_names),
                   subtitles='normal feature; default on; automatic translation draft',
                   subtitle_registry_sha256=hashlib.sha256(registry).hexdigest(),
                   git_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip())
    write_text(dist / 'release-profile.json', json.dumps(profile, indent=2))
    produced.add(dist / 'release-profile.json')

    for relative, cached in delta_files.items():
        (dist / relative).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(cached, dist / relative)
        produced.add(dist / relative)
    write_text(dist / 'korean/install-plan.tsv', plan_text)
    write_text(dist / 'korean/data-version.txt', data_version + '\n')
    pack = dict(
        release_version=args.version, data_version=data_version, created=started.isoformat(), localization_bundle=bundle,
        bundle_version=ready['version'], elf_sha256=elf_sha,
        disc=dict(title='Macross 30 - Ginga o Tsunagu Utagoe (BLJS10184)', files=len(originals),
                  bytes=sum(r['size'] for r in original_manifest['files']), check=iso_check),
        korean_files=[dict(target='data/disc/' + k['rel'], size=k['size'], sha256=k['sha256'],
                           original=k['original']['path'], original_size=k['original']['size'],
                           original_sha256=k['original']['source_sha256'], delta=delta,
                           delta_bytes=k['delta_bytes'], delta_sha256=k['delta_sha256'],
                           literal_bytes=k['literal_bytes'], ops=k['ops'])
                      for k, delta in zip(korean, delta_files)],
        hdd_install_copies=len(copy_rows), excluded_vfs_files=excluded,
        install_total_bytes=sum(r['size'] for r in ordered if r['kind'] != 'dir'))
    write_text(dist / 'korean/korean-pack.json', json.dumps(pack, indent=2, ensure_ascii=False))
    produced.update(dist / p for p in ('korean/install-plan.tsv', 'korean/data-version.txt', 'korean/korean-pack.json'))

    for directory in OWNED_DIRS:
        prune(dist / directory, produced)
    # Root: files from earlier builds (and stray __pycache__); the QA-owned
    # data/, play/, firmware/ directories are never touched.
    for path in dist.iterdir():
        if path.is_file() and path not in produced and path.name != 'program-manifest.json':
            path.unlink()
        elif path.is_dir() and path.name == '__pycache__':
            shutil.rmtree(path)

    program = sorted(p for p in produced if 'korean' not in p.relative_to(dist).parts[:1])
    files = [dict(path=p.relative_to(dist).as_posix(), size=p.stat().st_size, sha256=sha_file(p)) for p in program]
    program_version = hashlib.sha256(json.dumps(files).encode()).hexdigest()[:16]
    manifest = dict(release_version=args.version, version=program_version, data_version=data_version, created=started.isoformat(),
                    sources=dict(exe=str(exe), exe_mtime=datetime.fromtimestamp(exe.stat().st_mtime).isoformat(),
                                 elf=str(elf), vc_runtime=str(vc), ffmpeg=str(ffmpeg)),
                    files=files)
    manifest['sources']['git_commit'] = profile['git_commit']
    manifest['sources']['working_tree_status'] = subprocess.check_output(
        ['git', 'status', '--porcelain', '--untracked-files=normal'], cwd=ROOT, text=True)
    # These locally developed subtitle sources are intentionally included in
    # the binary; record exact input hashes without staging another owner's work.
    manifest['sources']['subtitle_source_sha256'] = {
        str(p.relative_to(ROOT)).replace('\\', '/'): sha_file(p)
        for p in [ROOT / 'native/macross30' / name for name in (
            'sail_playback.cpp', 'pam_audio_decoder.cpp', 'movie_subtitle_renderer.cpp',
            'movie_subtitle_renderer.h', 'movie_subtitle_timeline.cpp', 'movie_subtitle_timeline.h',
            'subtitle_visibility.h')] + [ROOT / 'vendor/ps3recomp-master/libs/video/rsx_null_backend.c',
                                      ROOT / 'vendor/ps3recomp-master/libs/video/rsx_null_backend.h']}
    write_text(dist / 'program-manifest.json', json.dumps(manifest, indent=2))

    if any(p.suffix.lower() in ('.pdb', '.map', '.dmp', '.log', '.ilk') for p in dist.rglob('*')
           if not any(part in USER_DIRS for part in p.relative_to(dist).parts[:1])):
        raise SystemExit('debug artifact found in the program pack')

    check = None
    if not args.skip_check:
        report_path = work / 'env-check.json'
        result = subprocess.run([sys.executable, str(ROOT / 'tools/dist/check_dist_env.py'), '--dist', str(dist),
                                 '--report', str(report_path)], cwd=ROOT)
        check = json.loads(report_path.read_text(encoding='utf-8')) if report_path.exists() else dict(ok=False)
        if result.returncode:
            raise SystemExit('Environment check FAILED; see ' + str(report_path))

    zips = {}
    if args.zip:
        packages.mkdir(exist_ok=True)
        groups = {f'Macross30-program-{args.version}.zip': program + [dist / 'program-manifest.json'],
                  f'Macross30-korean-{args.version}.zip': sorted(p for p in produced if p.relative_to(dist).parts[0] == 'korean')}
        for name, paths in groups.items():
            with zipfile.ZipFile(packages / name, 'w') as archive:
                for path in paths:
                    stored = path.suffix in ('.mkdelta',)
                    archive.write(path, 'Macross30/' + path.relative_to(dist).as_posix(),
                                  compress_type=zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED, compresslevel=6)
            zips[name] = (packages / name).stat().st_size
            print(f'package {name}: {zips[name]:,} bytes', flush=True)
        # Older packages go after the new ones are written. One still open
        # elsewhere (an upload in progress) is left with a warning.
        for old in packages.glob('Macross30-*.zip'):
            if old.name in groups:
                continue
            try:
                old.unlink()
            except PermissionError:
                print(f'WARNING: {old.name} is in use; not removed', flush=True)

    def total(paths):
        return sum(p.stat().st_size for p in paths)
    report = dict(created=started.isoformat(), finished=datetime.now(timezone.utc).isoformat(), dist=str(dist),
                  release_version=args.version, program_version=program_version, data_version=data_version,
                  program_pack_bytes=total(program) + (dist / 'program-manifest.json').stat().st_size,
                  korean_pack_bytes=total(p for p in produced if p.relative_to(dist).parts[0] == 'korean'),
                  korean_full_files_bytes=sum(k['size'] for k in korean),
                  korean_delta_bytes=sum(k['delta_bytes'] for k in korean),
                  installed_data_bytes=pack['install_total_bytes'], packages=zips,
                  program_files=[(f['path'], f['size']) for f in files],
                  korean_files=pack['korean_files'], excluded=excluded, iso_check=iso_check,
                  env_check_ok=None if check is None else check['ok'])
    (work / 'make-dist-report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k not in ('program_files', 'korean_files')}, indent=2))


if __name__ == '__main__':
    main()
