"""Stage exact-source Korean field names in independent pack/index copies.

Only the reviewed CSV display-name cells change after decoding. The original
mode-2 CSV reader accepts UTF8 without the CP932 conversion used for originals.
Both primary PIDX routes and every exact secondary resource path are updated.
No game, installation, active manifest, or source pack is modified.
"""
import argparse
import csv
from contextlib import ExitStack
import json
from pathlib import Path
import shutil
import struct

from stage_gop_primary_mirrors import pidx, read_payload
from stage_hud_guide import sha, sha_file, compare_prefix
from stage_nested_ui import fsts_entries, decoded, rebuild, region

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / 'localization/ko-KR/field_enemy_names.json'


def translate_csv(raw, catalog):
    if sha(raw) != catalog['source_sha256']:
        raise ValueError('Unreviewed field CSV source')
    original = raw.decode(catalog['source_encoding'])
    targets = {}
    for entry in catalog['entries']:
        text = entry['translation']
        if not text or any(c in text for c in ',"\r\n\0'):
            raise ValueError('Unsafe CSV display name')
        for object_id in entry['object_ids']:
            if object_id in targets:
                raise ValueError('Duplicate object ID')
            targets[object_id] = entry
    output = []
    changed = []
    for line in original.splitlines(keepends=True):
        fields = line.rstrip('\r\n').split(',')
        if len(fields) > 2 and not fields[0].startswith('#') and fields[1] in targets:
            entry = targets[fields[1]]
            if fields[2] != entry['source'] or fields[1] in changed:
                raise ValueError('Source name mismatch or repeated object ID')
            # Preserve all delimiters, trailing empty cells and CR/CR/LF bytes.
            prefix = fields[0] + ',' + fields[1] + ','
            line = prefix + entry['translation'] + line[len(prefix) + len(fields[2]):]
            changed.append(fields[1])
        output.append(line)
    if set(changed) != set(targets):
        raise ValueError('Missing reviewed enemy objects')
    result = ''.join(output).encode(catalog['output_encoding'])
    before = [next(csv.reader([l]), []) for l in original.splitlines()]
    after = [next(csv.reader([l]), []) for l in result.decode('utf8').splitlines()]
    if len(before) != len(after):
        raise ValueError('CSV line count changed')
    differences = 0
    for a, b in zip(before, after):
        expected = a.copy()
        if len(a) > 2 and not a[0].startswith('#') and a[1] in targets:
            expected[2] = targets[a[1]]['translation']
            differences += 1
        if b != expected:
            raise ValueError('Non-name CSV cells changed')
    return result, dict(changed_objects=changed, changed_cells=differences,
                        csv_lines=len(before), non_name_cells_unchanged=True,
                        line_endings_preserved=original.count('\r\r\n') == result.count(b'\r\r\n'))


def stage(source, output, catalog, expected_pack, expected_index, archive_hashes=None):
    source, output = Path(source).resolve(), Path(output).resolve()
    if (not output.is_relative_to(ROOT) or output.exists() or
            output.is_relative_to(source) or source.is_relative_to(output)):
        raise ValueError('Require a new independent output directory inside project')
    pack, index = source / 'data.dat', source / 'pack.idx'
    if sha_file(pack) != expected_pack or sha_file(index) != expected_index:
        raise ValueError('Source pack/index differs from frozen release recipe')
    master_blob, master, archives = pidx(index)
    embedded_blob, embedded, embedded_archives = pidx(pack)
    archive_hashes = dict(archive_hashes or {})
    archive_hashes[pack.name] = expected_pack
    verified_packs = {pack.name}
    basename = Path(catalog['resource']).name
    primary = [r for r in master if r['name'] == basename]
    embedded_primary = [r for r in embedded if r['name'] == basename]
    shape = lambda r: tuple(r[k] for k in ('name', 'pack', 'offset', 'size', 'compressed_size'))
    if not primary or list(map(shape, primary)) != list(map(shape, embedded_primary)):
        raise ValueError('Primary master/embedded aliases disagree')
    original = read_payload(pack, primary[0])[0]
    replacement, csv_proof = translate_csv(original, catalog)
    for r in primary + embedded_primary:
        if Path(r['pack']).name.lower() != pack.name or read_payload(pack, r)[0] != original:
            raise ValueError('Unexpected primary variant')
    secondary = []
    for r in archives:
        arc_pack = source / Path(r['pack']).name
        if region(arc_pack, r['offset'], 4) != b'FSTS':
            continue
        header = region(arc_pack, r['offset'], 20)
        count, table, names, names_size = struct.unpack_from('<4I', header, 4)
        if names + names_size > r['size']:
            raise ValueError('FSTS name table outside archive')
        if catalog['resource'].encode() + b'\0' not in region(arc_pack, r['offset'] + names, names_size):
            continue
        if arc_pack.name.lower() not in verified_packs:
            if sha_file(arc_pack) != archive_hashes.get(arc_pack.name.lower()):
                raise ValueError('Unverified secondary source pack: ' + arc_pack.name)
            verified_packs.add(arc_pack.name.lower())
        blob = region(arc_pack, r['offset'], r['size'])
        entries = fsts_entries(blob)
        matching = [e for e in entries if e['path'] == catalog['resource']]
        patches = {}
        for entry in matching:
            if decoded(blob, entry)[0] != original:
                raise ValueError('Secondary CSV differs from reviewed primary')
            patches[entry['index']] = (sha(original), replacement)
        if not patches:
            continue
        rebuilt, proof = rebuild(blob, patches)
        _, _, own_archives = pidx(arc_pack)
        aliases = [e for e in own_archives if e['name'] == r['name']]
        if len(aliases) != 1 or any(aliases[0][k] != r[k] for k in ('pack', 'offset', 'size')):
            raise ValueError('Secondary master/embedded aliases disagree')
        secondary.append((r, aliases[0], rebuilt, proof, arc_pack))
    if not secondary:
        raise ValueError('Exact secondary CSV route not found')
    output.mkdir(parents=True)
    target_pack, target_index = output / pack.name, output / index.name
    pack_sources = {pack.name: pack, **{p.name.lower(): p for _, _, _, _, p in secondary}}
    for name, path in pack_sources.items():
        shutil.copyfile(path, output / name)
        if (output / name).samefile(path):
            raise ValueError('Output pack aliases source')
    shutil.copyfile(index, target_index)
    if target_pack.samefile(pack) or target_index.samefile(index):
        raise ValueError('Outputs alias source')
    allowed_packs = {name: [] for name in pack_sources}
    allowed_pack, allowed_index = allowed_packs[pack.name], []
    with ExitStack() as stack:
        streams = {name: stack.enter_context((output / name).open('r+b')) for name in pack_sources}
        df = streams[pack.name]
        mf = stack.enter_context(target_index.open('r+b'))
        def append(stream, payload):
            stream.seek(0, 2)
            stream.write(bytes(-stream.tell() % 2048))
            at = stream.tell()
            if at + len(payload) > 0xffffffff:
                raise ValueError('PIDX32 overflow')
            stream.write(payload)
            return at
        at = append(df, replacement)
        for rows, stream, allowed in ((primary, mf, allowed_index), (embedded_primary, df, allowed_pack)):
            for row in rows:
                field = row['record'] + 12
                stream.seek(field)
                stream.write(struct.pack('<3I', at, len(replacement), 0))
                allowed.append((field, 12))
        for master_arc, embedded_arc, blob, _, source_path in secondary:
            key = source_path.name.lower()
            arc_stream = streams[key]
            at = append(arc_stream, blob)
            for row, stream, allowed in ((master_arc, mf, allowed_index),
                                         (embedded_arc, arc_stream, allowed_packs[key])):
                field = row['record'] + 8
                stream.seek(field)
                stream.write(struct.pack('<2I', at, len(blob)))
                allowed.append((field, 8))
    for name, source_path in pack_sources.items():
        compare_prefix(source_path, output / name, source_path.stat().st_size, allowed_packs[name])
    compare_prefix(index, target_index, index.stat().st_size, allowed_index)
    _, ms, arcs = pidx(target_index)
    _, es, earcs = pidx(target_pack)
    for rows in (ms, es):
        for row in [r for r in rows if r['name'] == basename]:
            if read_payload(target_pack, row)[0] != replacement:
                raise ValueError('Primary readback failed')
    secondary_reads = 0
    own_arcs = [pidx(output / name)[2] for name in pack_sources]
    for rows in [arcs] + own_arcs:
        for row in rows:
            if row['name'] not in {r[0]['name'] for r in secondary}:
                continue
            blob = region(output / Path(row['pack']).name, row['offset'], row['size'])
            for entry in fsts_entries(blob):
                if entry['path'] == catalog['resource']:
                    if decoded(blob, entry)[0] != replacement:
                        raise ValueError('Secondary readback failed')
                    secondary_reads += 1
    (output / 'mm_island.original.cp932').write_bytes(original)
    (output / 'mm_island.csv').write_bytes(replacement)
    report = dict(csv=csv_proof, catalog_sha256=sha_file(CATALOG),
                  original_csv_sha256=sha(original), output_csv_sha256=sha(replacement),
                  source_pack_sha256=expected_pack, source_index_sha256=expected_index,
                  secondary_source_pack_hashes={name: archive_hashes[name] for name in sorted(verified_packs)},
                  output_pack_sha256=sha_file(target_pack), output_index_sha256=sha_file(target_index),
                  primary_aliases=len(primary) + len(embedded_primary), secondary_readbacks=secondary_reads,
                  secondary_archives=[dict(name=r['name'], entries=p) for r, _, _, p, _ in secondary],
                  output_files={name: dict(bytes=(output / name).stat().st_size, sha256=sha_file(output / name))
                                for name in list(pack_sources) + [index.name]},
                  outside_allowed_original_bytes_changed=0, game_started=False,
                  installed_or_active_files_modified=False, visual_check='pending')
    (output / 'FIELD-NAMES-RESULT.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    return report


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--pack-sha256', required=True)
    ap.add_argument('--index-sha256', required=True)
    ap.add_argument('--archive-source-sha256', action='append', default=[], metavar='NAME=SHA256')
    args = ap.parse_args()
    print(json.dumps(stage(args.source, args.output, json.loads(CATALOG.read_text('utf8')),
                           args.pack_sha256, args.index_sha256,
                           dict(value.split('=', 1) for value in args.archive_source_sha256)), ensure_ascii=False, indent=2))
