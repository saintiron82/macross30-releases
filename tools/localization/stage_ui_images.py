"""Plan or stage manifest-selected UI DDS replacements in NEW independent DATA2/PACK.IDX copies.

Default is read-only dry-run. --apply publishes a new directory; never edits inputs.
Full source hashes must be supplied from a separately reviewed source-state manifest.
Accepts quest-ui-v1 textures and hud-guide sources manifests.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import uuid

import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from analyze_archives import unpack

# Shared transaction helpers remain in the original HUD tool.
from stage_hud_guide import sha_file, sha, bounded_range, compare_prefix

ALIGN = 2048


def read_pidx(path, targets, selection=None):
    """Select unique names, or exact provenance tuples when basenames repeat.

    With selection, every target requires pack basename + offset/size/encoding.
    Exact duplicate tuples remain ambiguous and are rejected, never guessed.
    """
    if selection is not None and set(selection) != set(targets):
        raise ValueError('Provenance selection must cover exact target set')
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
        if name not in targets:
            continue
        if selection is not None:
            wanted = selection[name]
            if (Path(string(pn)).name.lower() != Path(wanted['pack']).name.lower() or
                    any(value != wanted[key] for key, value in
                        (('offset', off), ('size', size), ('compressed_size', zs)))):
                continue
        if name in entries:
            raise ValueError('Duplicate exact provenance tuple' if selection is not None
                             else 'Duplicate target PIDX entry')
        entries[name] = dict(record=at, pack=string(pn), offset=off,
                             size=size, compressed_size=zs)
    if set(entries) != set(targets):
        raise ValueError('PIDX missing manifest target entries')
    return blob, entries


def validate_dds(data):
    """Original BGRA8 or bounded BC3 mip chains; fail closed otherwise."""
    if len(data) < 128 or data[:4] != b'DDS ' or struct.unpack_from('<I', data, 4)[0] != 124:
        raise ValueError('Invalid DDS header')
    height, width, pitch, depth, mips = struct.unpack_from('<5I', data, 12)
    if not width or not height or depth not in (0, 1):
        raise ValueError('Unsupported DDS dimensions/depth/mips')
    pixel_format = struct.unpack_from('<8I', data, 76)
    if pixel_format == (32, 4, int.from_bytes(b'DXT5', 'little'), 0, 0, 0, 0, 0):
        levels=max(1,mips)
        if levels>max(width,height).bit_length():raise ValueError('BC3 mip count exceeds dimensions')
        top_length = ((width + 3) // 4) * ((height + 3) // 4) * 16
        length=sum(((max(1,width>>i)+3)//4)*((max(1,height>>i)+3)//4)*16 for i in range(levels))
        if levels>1 and struct.unpack_from('<I',data,108)[0]&0x400008!=0x400008:raise ValueError('BC3 mipmap caps missing')
        if (len(data) != 128 + length or pitch != top_length or
                struct.unpack_from('<I', data, 8)[0] & (0x8 | 0x80000) != 0x80000 or
                struct.unpack_from('<I', data, 112)[0] != 0):
            raise ValueError('Invalid BC3 single-mip length/linear-size/caps2')
        return
    if mips not in (0,1):raise ValueError('BGRA mip chains are not supported')
    if len(data) != 128 + width * height * 4:
        raise ValueError('DDS payload length differs from BGRA8 dimensions')
    layout = struct.unpack_from('<I', data, 8)[0] & (0x8 | 0x80000)
    if pitch != {0x8: width * 4, 0x80000: width * height * 4}.get(layout):
        raise ValueError('DDS pitch/linear-size mismatch')
    if struct.unpack_from('<8I', data, 76) != (32, 65, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000):
        raise ValueError('Unsupported DDS pixel format; expected original BGRA8')


def manifest_sources(doc):
    rows = doc.get('textures', doc.get('sources'))
    if not isinstance(rows, list) or not rows:
        raise ValueError('Manifest requires nonempty textures or sources')
    result = []
    for original in rows:
        row = dict(original)
        name = row.get('path', '')
        if not name.lower().endswith('.dds') or '/' in name or '\\' in name or name in ('.', '..'):
            raise ValueError('Manifest target must be a flat DDS filename')
        row['sha256'] = row.get('dds_sha256', row.get('sha256'))
        row['encoded_sha256'] = row.get('compressed_sha256', row.get('encoded_sha256'))
        if any(not isinstance(row.get(k), str) or len(row[k]) != 64 for k in ('sha256', 'encoded_sha256')):
            raise ValueError('Manifest requires original DDS and encoded SHA256')
        result.append(row)
    if len({r['path'].lower() for r in result}) != len(result):
        raise ValueError('Duplicate manifest target')
    return result


def plan(data2, index, replacements, provenance, data2_hash, index_hash):
    data2, index = Path(data2).resolve(), Path(index).resolve()
    for path, expected in ((data2, data2_hash), (index, index_hash)):
        if len(expected) != 64 or sha_file(path) != expected.lower():
            raise ValueError('Full source hash mismatch: ' + str(path))
    sources = manifest_sources(provenance)
    names = tuple(r['path'] for r in sources)
    proof = {r['path']: r for r in sources}
    selection = {name: dict(row, pack=row.get('pack', data2.name)) for name, row in proof.items()}
    master_blob, master = read_pidx(index, names, selection)
    embedded_blob, embedded = read_pidx(data2, names, selection)
    if set(proof) != set(names):
        raise ValueError('Unexpected provenance target set')
    cursor = data2.stat().st_size
    changes = []
    with data2.open('rb') as f:
        for name in names:
            m, e, known = master[name], embedded[name], proof[name]
            for row in (m, e):
                if Path(row['pack']).name.lower() != data2.name.lower():
                    raise ValueError('Target refers to another pack')
                if any(row[k] != known[k] for k in ('offset', 'size', 'compressed_size')):
                    raise ValueError('Source PIDX entry differs from provenance')
            size, zs = m['size'], m['compressed_size']
            if not 0 <= zs < size:
                raise ValueError('Unexpected original entry encoding')
            bounded_range(m['offset'], zs or size, data2.stat().st_size)
            f.seek(m['offset']); encoded = f.read(zs or size)
            if sha(encoded) != known['encoded_sha256']:
                raise ValueError('Original compressed payload hash mismatch')
            original = unpack(encoded, size) if zs else encoded
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
            changes.append(dict(name=name, pack=m['pack'], replacement=str(replacement), sha256=sha(payload),
                                offset=cursor, size=len(payload), compressed_size=0,
                                master_field=m['record'] + 12, embedded_field=e['record'] + 12))
            cursor += len(payload)
    return dict(kind='new isolated manifest-selected UI pack copies', data2=str(data2), index=str(index), pack_filename=data2.name,
                source_pack_sha256=data2_hash.lower(),
                source_data2_sha256=data2_hash.lower(), source_index_sha256=index_hash.lower(),
                original_length=data2.stat().st_size, final_length=cursor,
                changes=changes, source_modified=False), master_blob, embedded_blob


def apply(plan_doc, output):
    output = Path(output).resolve()
    root = Path(__file__).resolve().parents[2]
    protected = [root / 'native/vfs', root / 'native/play']
    protected += list((root / 'build').glob('game*'))
    if any(output == x.resolve() or x.resolve() in output.parents for x in protected):
        raise ValueError('Output cannot be inside an active game/VFS tree')
    if output.exists():
        raise FileExistsError('Output directory already exists')
    if not output.parent.is_dir():
        raise ValueError('Output parent must already exist')
    inputs = [Path(plan_doc[k]) for k in ('data2', 'index')]
    pack_filename = plan_doc.get('pack_filename', inputs[0].name)
    if Path(pack_filename).name != pack_filename or pack_filename.lower() == 'pack.idx':
        raise ValueError('Invalid output pack filename')
    if any(output == p or output in p.parents for p in inputs):
        raise ValueError('Output contains source')
    # tempfile.mkdtemp uses mode 0700, which creates a restrictive Windows ACL.
    # A normal mkdir inherits the workspace parent ACL for later installation.
    staging = output.parent / ('.ui-images-' + uuid.uuid4().hex)
    staging.mkdir(mode=0o777)
    try:
        for src, name, expected in zip(inputs, (pack_filename, 'pack.idx'),
                                      (plan_doc['source_data2_sha256'], plan_doc['source_index_sha256'])):
            dst = staging / name
            with src.open('rb') as inp, dst.open('xb') as out:
                shutil.copyfileobj(inp, out, 8 * 1024 * 1024)
                out.flush(); os.fsync(out.fileno())
            if dst.stat().st_nlink != 1 or sha_file(dst) != expected:
                raise ValueError('Independent copy/source consistency check failed')
        data, idx = staging / pack_filename, staging / 'pack.idx'
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
        with data.open('rb') as f:
            for c in plan_doc['changes']:
                # Custom reviewed plans may contain multiple rows with one basename.
                # Each append has a unique tuple; validate every row independently.
                selection = {c['name']: dict(c, pack=c.get('pack', pack_filename))}
                _, new_master = read_pidx(idx, [c['name']], selection)
                _, new_embedded = read_pidx(data, [c['name']], selection)
                for entries in (new_master, new_embedded):
                    if any(entries[c['name']][k] != c[k] for k in ('offset', 'size', 'compressed_size')):
                        raise ValueError('Mirror roundtrip failed')
                f.seek(c['offset'])
                if sha(f.read(c['size'])) != c['sha256']:
                    raise ValueError('Replacement roundtrip failed')
        output_pack_hash = sha_file(data)
        result = dict(plan_doc, output=str(output), independent_copies=True,
                      output_data2_sha256=output_pack_hash, output_pack_sha256=output_pack_hash, output_index_sha256=sha_file(idx))
        (staging / 'ui-images-manifest.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        staging.rename(output)
        return result
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def resolve_manifest(value):
    aliases = {'quest-ui-v1': 'native/analysis/quest-ui-localization/manifest.json',
               'hud-guide': 'native/analysis/hud-guide-localization/source-provenance.json'}
    return Path(aliases.get(value, value))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data2', type=Path, required=True, help='Source pack path (DATA2.DAT or DATA.DAT; legacy option name)')
    p.add_argument('--index', type=Path, required=True)
    p.add_argument('--data2-sha256', required=True)
    p.add_argument('--index-sha256', required=True)
    p.add_argument('--replacements', type=Path, required=True)
    p.add_argument('--manifest', required=True, help='JSON path, quest-ui-v1, or hud-guide')
    p.add_argument('--output', type=Path)
    p.add_argument('--apply', action='store_true')
    a = p.parse_args()
    doc, _, _ = plan(a.data2, a.index, a.replacements,
                     json.loads(resolve_manifest(a.manifest).read_text(encoding='utf-8')), a.data2_sha256, a.index_sha256)
    if a.apply:
        if not a.output:
            p.error('--apply requires --output NEW_DIRECTORY')
        doc = apply(doc, a.output)
    print(json.dumps(dict(doc, mode='apply' if a.apply else 'dry-run'), indent=2))

if __name__ == '__main__':
    main()
