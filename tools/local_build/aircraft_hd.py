"""Rebuild the pinned preview-texture pack from the user's installed game.

The shipped recipe contains names and hashes only. No DDS payload is shipped.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import zlib


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def unpack(data, expected):
    if data[:4] != b'segs':
        result = zlib.decompress(data)
    else:
        _, count, full, _ = struct.unpack_from('>HHII', data, 4)
        if full != expected or count > 65536 or 16 + count * 8 > len(data):
            raise ValueError('Invalid texture compression table')
        relative = struct.unpack_from('>I', data, 20)[0] == 1
        result = bytearray()
        for i in range(count):
            size, length, offset = struct.unpack_from('>HHI', data, 16 + i * 8)
            length = length or 65536
            offset = offset - 1 + (16 + count * 8 if relative else 0)
            if offset < 0 or offset + size > len(data):
                raise ValueError('Texture block outside archive')
            chunk = data[offset:offset + size]
            decoded = chunk if size == length else zlib.decompress(chunk, -15)
            if len(decoded) != length or len(result) + length > expected:
                raise ValueError('Invalid decompressed texture length')
            result.extend(decoded)
    if len(result) != expected:
        raise ValueError('Texture length mismatch')
    return bytes(result)


def index(pack):
    files = {p.name.lower(): p for p in Path(pack).iterdir() if p.is_file()}
    data = files['pack.idx'].read_bytes()
    if len(data) < 40 or len(data) > 32 * 1024 * 1024 or data[:4] != b'PIDX':
        raise ValueError('Invalid game texture index')
    _, _, table, count, _, _, _, names, names_size = struct.unpack_from('<9I', data, 4)
    if table + count * 24 > len(data) or names + names_size > len(data):
        raise ValueError('Texture index outside file')

    def text(offset):
        if offset >= names_size:
            raise ValueError('Texture name outside index')
        stop = data.find(b'\0', names + offset, names + names_size)
        if stop < 0:
            raise ValueError('Unterminated texture name')
        return data[names + offset:stop].decode('utf-8')
    rows = {}
    for i in range(count):
        folder, name, filename, offset, size, compressed = struct.unpack_from('<6I', data, table + 24 * i)
        if not folder:
            rows[text(name).lower()] = (text(filename).replace('\\', '/').split('/')[-1].lower(),
                                        offset, size, compressed)
    return files, rows


def build(pack, recipe, target):
    recipe, target = Path(recipe), Path(target)
    spec = json.loads(recipe.read_text(encoding='utf-8'))
    if spec['schema'] != 1 or spec['count'] != len(spec['entries']) or spec['count'] > 10000:
        raise ValueError('Invalid HD recipe')
    if target.is_file() and target.stat().st_size == spec['bytes'] and sha(target) == spec['sha256']:
        return dict(status='current', maps=spec['count'], sha256=spec['sha256'])
    files, rows = index(pack)

    def read(name, expected):
        filename, offset, size, compressed = rows[name.lower()]
        if size <= 0 or size > 64 * 1024 * 1024 or compressed > size:
            raise ValueError('Invalid texture size: ' + name)
        stored = compressed if 0 < compressed < size else size
        path = files[filename]
        if offset + stored > path.stat().st_size:
            raise ValueError('Texture outside game archive: ' + name)
        with path.open('rb') as stream:
            stream.seek(offset)
            raw = stream.read(stored)
        data = unpack(raw, size) if stored != size else raw
        if data[:4] != b'DDS ' or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('Original texture identity mismatch: ' + name)
        return data
    target.parent.mkdir(parents=True, exist_ok=True)
    handle, pending = tempfile.mkstemp(prefix='.aircraft-hd-', dir=target.parent)
    try:
        with os.fdopen(handle, 'wb') as stream:
            stream.write(b'M30HD001' + struct.pack('<I', spec['count']))
            for row in spec['entries']:
                a = read(row['source'], row['source_sha256'])
                b = read(row['target'], row['target_sha256'])
                stream.write(struct.pack('<II', len(a), len(b)))
                stream.write(a)
                stream.write(b)
            stream.flush()
            os.fsync(stream.fileno())
        if Path(pending).stat().st_size != spec['bytes'] or sha(pending) != spec['sha256']:
            raise ValueError('Rebuilt HD pack differs from the compiled pin')
        os.replace(pending, target)
    finally:
        if os.path.exists(pending):
            os.unlink(pending)
    return dict(status='built', maps=spec['count'], sha256=spec['sha256'])


def prepare(root):
    root = Path(root)
    recipe = root / 'bin/assets/aircraft-hd-recipe.json'
    if not recipe.is_file():
        raise FileNotFoundError('HD texture recipe is missing')
    profile = json.loads((root / 'release-profile.json').read_text(encoding='utf-8-sig'))
    views = profile['language_views']
    for locale in ('ja-JP', 'ko-KR', 'en-US'):
        if locale in views:
            disc = (root / views[locale]['disc']).resolve()
            if not disc.is_relative_to(root.resolve()):
                raise ValueError('Game data outside install folder')
            pack = disc / 'PS3_GAME/USRDIR/DATA/PACK'
            if pack.is_dir():
                return build(pack, recipe, root / 'bin/assets/aircraft-hd.pack')
    raise FileNotFoundError('Prepare original game data before enabling HD textures')
