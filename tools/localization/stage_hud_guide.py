"""Plan or stage three HUD DDS replacements in NEW independent DATA2/PACK.IDX copies.

Default is read-only dry-run. --apply publishes a new directory; never edits inputs.
Full source hashes must be supplied from a separately reviewed source-state manifest.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile

import sys as _sys, pathlib as _pl
if not getattr(_sys, "frozen", False):
    _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]
from analyze_archives import unpack

NAMES = tuple('hud_cguide_' + c + '.dds' for c in 'abc')
ALIGN = 2048


def sha_file(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda: f.read(8 * 1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def bounded_range(offset, length, limit):
    if offset < 0 or length < 0 or offset + length > limit:
        raise ValueError('PIDX range outside file')


def read_pidx(path):
    with Path(path).open('rb') as f:
        header = f.read(40)
        if len(header) != 40 or header[:4] != b'PIDX':
            raise ValueError('Missing PIDX header')
        fields = struct.unpack_from('<9I', header, 4)
        info, count, names, names_size = fields[2], fields[3], fields[7], fields[8]
        total = max(40, info + count * 24, names + names_size)
        if total > 32 * 1024 * 1024:
            raise ValueError('Unbounded PIDX metadata')
        bounded_range(0, total, Path(path).stat().st_size)
        f.seek(0)
        blob = f.read(total)
    strings = blob[names:names + names_size]
    def string(offset):
        bounded_range(offset, 1, len(strings))
        end = strings.find(b'\0', offset)
        if end < 0:
            raise ValueError('Unterminated PIDX name')
        return strings[offset:end].decode('utf-8')
    entries = {}
    for i in range(count):
        at = info + i * 24
        folder, no, pn, off, size, zs = struct.unpack_from('<6I', blob, at)
        if folder:
            continue
        name = string(no)
        if name not in NAMES:
            continue
        if name in entries:
            raise ValueError('Duplicate target PIDX entry')
        entries[name] = dict(record=at, pack=string(pn), offset=off,
                             size=size, compressed_size=zs)
    if set(entries) != set(NAMES):
        raise ValueError('PIDX missing exact three target entries')
    return blob, entries


def validate_dds(data):
    if len(data) != 131200 or data[:4] != b'DDS ':
        raise ValueError('Expected 131200-byte DDS')
    if struct.unpack_from('<I', data, 4)[0] != 124:
        raise ValueError('Invalid DDS header size')
    height, width, pitch_or_linear_size, depth, mips = struct.unpack_from('<5I', data, 12)
    if (width, height) != (256, 128) or depth not in (0, 1) or mips not in (0, 1):
        raise ValueError('DDS dimensions/depth/mips changed')
    # Original HUD files advertise DDSD_LINEARSIZE, including for BGRA8.
    # Keep the regular DDSD_PITCH form, but reject ambiguous or wrong sizes.
    layout = struct.unpack_from('<I', data, 8)[0] & (0x8 | 0x80000)
    expected = {0x8: width * 4, 0x80000: width * height * 4}.get(layout)
    if expected is None or pitch_or_linear_size != expected:
        raise ValueError('DDS pitch/linear-size flags or value changed')
    pfsize, flags, fourcc, bits, r, g, b, a = struct.unpack_from('<8I', data, 76)
    if (pfsize, flags, fourcc, bits, r, g, b, a) != (32, 65, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000):
        raise ValueError('Expected uncompressed BGRA8 DDS with alpha')


def plan(data2, index, replacements, provenance, data2_hash, index_hash):
    data2, index = Path(data2).resolve(), Path(index).resolve()
    for path, expected in ((data2, data2_hash), (index, index_hash)):
        if len(expected) != 64 or sha_file(path) != expected.lower():
            raise ValueError('Full source hash mismatch: ' + str(path))
    master_blob, master = read_pidx(index)
    embedded_blob, embedded = read_pidx(data2)
    proof = {r['path']: r for r in provenance['sources']}
    if set(proof) != set(NAMES):
        raise ValueError('Unexpected provenance target set')
    cursor = data2.stat().st_size
    changes = []
    with data2.open('rb') as f:
        for name in NAMES:
            m, e, known = master[name], embedded[name], proof[name]
            for row in (m, e):
                if Path(row['pack']).name.lower() != 'data2.dat':
                    raise ValueError('Target refers to another pack')
                if any(row[k] != known[k] for k in ('offset', 'size', 'compressed_size')):
                    raise ValueError('Source PIDX entry differs from provenance')
            size, zs = m['size'], m['compressed_size']
            if not 0 < zs < size or size != 131200:
                raise ValueError('Unexpected original entry encoding')
            bounded_range(m['offset'], zs, data2.stat().st_size)
            f.seek(m['offset']); encoded = f.read(zs)
            if sha(encoded) != known['encoded_sha256']:
                raise ValueError('Original compressed payload hash mismatch')
            original = unpack(encoded, size)
            if sha(original) != known['sha256']:
                raise ValueError('Original DDS hash mismatch')
            validate_dds(original)
            replacement = (Path(replacements) / name).resolve()
            if replacement in (data2, index):
                raise ValueError('Replacement aliases pack/index')
            payload = replacement.read_bytes()
            validate_dds(payload)
            if payload[:128] != original[:128]:
                raise ValueError('Replacement DDS header changed')
            cursor = (cursor + ALIGN - 1) // ALIGN * ALIGN
            if cursor + len(payload) > 0xFFFFFFFF:
                raise ValueError('PIDX32 offset overflow')
            changes.append(dict(name=name, replacement=str(replacement), sha256=sha(payload),
                                offset=cursor, size=len(payload), compressed_size=0,
                                master_field=m['record'] + 12, embedded_field=e['record'] + 12))
            cursor += len(payload)
    return dict(kind='new isolated HUD guide pack copies', data2=str(data2), index=str(index),
                source_data2_sha256=data2_hash.lower(), source_index_sha256=index_hash.lower(),
                original_length=data2.stat().st_size, final_length=cursor,
                changes=changes, source_modified=False), master_blob, embedded_blob


def compare_prefix(original, copied, length, allowed):
    with Path(original).open('rb') as a, Path(copied).open('rb') as b:
        offset = 0
        while offset < length:
            count = min(8 * 1024 * 1024, length - offset)
            x, y = a.read(count), bytearray(b.read(count))
            if len(x) != count or len(y) != count:
                raise ValueError('Short verification read')
            for start, size in allowed:
                lo, hi = max(start, offset), min(start + size, offset + count)
                if lo < hi:
                    y[lo-offset:hi-offset] = x[lo-offset:hi-offset]
            if x != y:
                raise ValueError('Unexpected modification outside approved fields')
            offset += count


def apply(plan_doc, output):
    output = Path(output).absolute()
    if output.exists():
        raise FileExistsError('Output directory already exists')
    if not output.parent.is_dir():
        raise ValueError('Output parent must already exist')
    inputs = [Path(plan_doc[k]) for k in ('data2', 'index')]
    if any(output == p or output in p.parents for p in inputs):
        raise ValueError('Output contains source')
    staging = Path(tempfile.mkdtemp(prefix='.hud-guide-', dir=output.parent))
    try:
        for src, name, expected in zip(inputs, ('data2.dat', 'pack.idx'),
                                      (plan_doc['source_data2_sha256'], plan_doc['source_index_sha256'])):
            dst = staging / name
            with src.open('rb') as inp, dst.open('xb') as out:
                shutil.copyfileobj(inp, out, 8 * 1024 * 1024)
                out.flush(); os.fsync(out.fileno())
            if dst.stat().st_nlink != 1 or sha_file(dst) != expected:
                raise ValueError('Independent copy/source consistency check failed')
        data, idx = staging / 'data2.dat', staging / 'pack.idx'
        with data.open('r+b') as f, idx.open('r+b') as m:
            for c in plan_doc['changes']:
                payload = Path(c['replacement']).read_bytes()
                if sha(payload) != c['sha256']:
                    raise ValueError('Replacement changed after validation')
                f.seek(0, 2); padding = c['offset'] - f.tell()
                if not 0 <= padding < ALIGN:
                    raise ValueError('Unexpected append position')
                f.write(bytes(padding)); f.write(payload)
                record = struct.pack('<3I', c['offset'], c['size'], 0)
                f.seek(c['embedded_field']); f.write(record)
                m.seek(c['master_field']); m.write(record)
            f.flush(); m.flush(); os.fsync(f.fileno()); os.fsync(m.fileno())
        if data.stat().st_size != plan_doc['final_length']:
            raise ValueError('Unexpected final data length')
        compare_prefix(inputs[0], data, plan_doc['original_length'],
                       [(c['embedded_field'], 12) for c in plan_doc['changes']])
        compare_prefix(inputs[1], idx, inputs[1].stat().st_size,
                       [(c['master_field'], 12) for c in plan_doc['changes']])
        _, new_master = read_pidx(idx); _, new_embedded = read_pidx(data)
        with data.open('rb') as f:
            for c in plan_doc['changes']:
                for entries in (new_master, new_embedded):
                    if any(entries[c['name']][k] != c[k] for k in ('offset', 'size', 'compressed_size')):
                        raise ValueError('Mirror roundtrip failed')
                f.seek(c['offset'])
                if sha(f.read(c['size'])) != c['sha256']:
                    raise ValueError('Replacement roundtrip failed')
        result = dict(plan_doc, output=str(output), independent_copies=True,
                      output_data2_sha256=sha_file(data), output_index_sha256=sha_file(idx))
        (staging / 'hud-guide-manifest.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        staging.rename(output)
        return result
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data2', type=Path, required=True)
    p.add_argument('--index', type=Path, required=True)
    p.add_argument('--data2-sha256', required=True)
    p.add_argument('--index-sha256', required=True)
    p.add_argument('--replacements', type=Path, required=True)
    p.add_argument('--provenance', type=Path, default=Path('native/analysis/hud-guide-localization/source-provenance.json'))
    p.add_argument('--output', type=Path)
    p.add_argument('--apply', action='store_true')
    a = p.parse_args()
    doc, _, _ = plan(a.data2, a.index, a.replacements,
                     json.loads(a.provenance.read_text(encoding='utf-8')), a.data2_sha256, a.index_sha256)
    if a.apply:
        if not a.output:
            p.error('--apply requires --output NEW_DIRECTORY')
        doc = apply(doc, a.output)
    print(json.dumps(dict(doc, mode='apply' if a.apply else 'dry-run'), indent=2))

if __name__ == '__main__':
    main()
