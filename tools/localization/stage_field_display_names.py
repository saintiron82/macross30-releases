"""Stage reviewed display-name cells in independent multi-CSV resource copies."""
import argparse
from collections import Counter
from contextlib import ExitStack
import json
from pathlib import Path
import shutil
import struct
import zipfile

from stage_field_enemy_names import translate_csv
from stage_gop_primary_mirrors import pidx, read_payload
from stage_hud_guide import sha, sha_file, compare_prefix
from stage_nested_ui import fsts_entries, decoded, rebuild, region

ROOT = Path(__file__).resolve().parents[2]


def stage(source, output, catalog_path, package):
    source, output, catalog_path, package = (Path(p).resolve() for p in (source, output, catalog_path, package))
    if (output.exists() or not output.is_relative_to(ROOT / 'build/tests') or
            source.is_relative_to(output) or output.is_relative_to(source)):
        raise ValueError('Require a new independent build/tests output directory')
    catalog = json.loads(catalog_path.read_text('utf8'))
    locale = catalog['locale']
    with zipfile.ZipFile(package) as z:
        recipe = json.loads(z.read('payload/recipe.json'))
        plan = z.read('payload/' + recipe['languages'][locale]['plan']).decode('utf-8-sig')
    expected = {}
    for line in plan.splitlines():
        if not line or line.startswith('#'):
            continue
        fields = line.split('\t')
        if fields[0] in ('iso', 'delta') and '/DATA/PACK/' in fields[1].upper():
            expected[Path(fields[1]).name.lower()] = fields[3]
    verified = set()

    def verify(name):
        name = name.lower()
        if name not in verified:
            if name not in expected or sha_file(source / name) != expected[name]:
                raise ValueError('Source differs from frozen language plan: ' + name)
            verified.add(name)
        return source / name

    verify('pack.idx')
    _, master, archives = pidx(source / 'pack.idx')
    embedded_cache = {}

    def embedded(name):
        name = name.lower()
        if name not in embedded_cache:
            embedded_cache[name] = pidx(verify(name))[1:]
        return embedded_cache[name]

    csvs, secondary_paths, primary_jobs = {}, {}, []
    for resource in catalog['resources']:
        name = resource['name']
        if name in csvs or Path(name).name != name:
            raise ValueError('Duplicate or unsafe CSV basename')
        rows = [r for r in master if r['name'] == name]
        if not rows:
            raise ValueError('Missing primary CSV: ' + name)
        first_pack = Path(rows[0]['pack']).name.lower()
        raw = read_payload(verify(first_pack), rows[0])[0]
        payload, proof = translate_csv(raw, resource)
        csvs[name] = dict(raw=raw, payload=payload, proof=proof, resource=resource)
        for path in resource['secondary_paths']:
            if path in secondary_paths:
                raise ValueError('Ambiguous exact secondary resource path')
            secondary_paths[path] = name
        for row in rows:
            pack_name = Path(row['pack']).name.lower()
            if read_payload(verify(pack_name), row)[0] != raw:
                raise ValueError('Primary aliases contain different CSVs')
            own_rows = [r for r in embedded(pack_name)[0] if r['name'] == name]
            shape = lambda r: tuple(r[k] for k in ('pack', 'offset', 'size', 'compressed_size'))
            if Counter(map(shape, own_rows)) != Counter(map(shape, [r for r in rows if Path(r['pack']).name.lower() == pack_name])):
                raise ValueError('Primary master and own-pack aliases differ')
            primary_jobs.append((pack_name, row, 'master', name))
        for pack_name in {Path(r['pack']).name.lower() for r in rows}:
            for row in [r for r in embedded(pack_name)[0] if r['name'] == name]:
                primary_jobs.append((pack_name, row, 'embedded', name))
    if sum(c['proof']['changed_cells'] for c in csvs.values()) != catalog['expected_changed_cells']:
        raise ValueError('Reviewed name-cell count changed')
    secondary_jobs, found_paths = [], set()
    for row in archives:
        pack_name = Path(row['pack']).name.lower()
        path = source / pack_name
        if region(path, row['offset'], 4) != b'FSTS':
            continue
        count, table, names, names_size = struct.unpack_from('<4I', region(path, row['offset'], 20), 4)
        if names + names_size > row['size']:
            raise ValueError('FSTS names outside archive')
        exact_paths = set(region(path, row['offset'] + names, names_size).split(b'\0'))
        if not any(path.encode() in exact_paths for path in secondary_paths):
            continue
        verify(pack_name)
        blob = region(path, row['offset'], row['size'])
        patches, paths = {}, []
        for entry in fsts_entries(blob):
            if entry['path'] not in secondary_paths:
                continue
            name = secondary_paths[entry['path']]
            if decoded(blob, entry)[0] != csvs[name]['raw']:
                raise ValueError('Secondary resource differs from exact reviewed source')
            patches[entry['index']] = (sha(csvs[name]['raw']), csvs[name]['payload'])
            paths.append(entry['path'])
            found_paths.add(entry['path'])
        if not patches:
            continue
        own = [r for r in embedded(pack_name)[1] if r['name'] == row['name']]
        if len(own) != 1 or any(row[k] != own[0][k] for k in ('pack', 'offset', 'size')):
            raise ValueError('Secondary master and own-pack aliases differ')
        rebuilt, proof = rebuild(blob, patches)
        secondary_jobs.append((pack_name, row, own[0], rebuilt, paths, proof))
    if found_paths != set(secondary_paths):
        raise ValueError('Missing exact secondary resource routes')
    modified_packs = {j[0] for j in primary_jobs} | {j[0] for j in secondary_jobs}
    output.mkdir(parents=True)
    for name in sorted(modified_packs | {'pack.idx'}):
        shutil.copyfile(verify(name), output / name)
        if (output / name).samefile(source / name):
            raise ValueError('Output aliases source')
    allowed = {name: [] for name in modified_packs | {'pack.idx'}}
    with ExitStack() as stack:
        streams = {name: stack.enter_context((output / name).open('r+b')) for name in allowed}

        def append(pack_name, payload):
            stream = streams[pack_name]
            stream.seek(0, 2)
            stream.write(bytes(-stream.tell() % 2048))
            offset = stream.tell()
            if offset + len(payload) > 0xffffffff:
                raise ValueError('PIDX32 overflow')
            stream.write(payload)
            return offset

        locations = {}
        for pack_name, row, role, name in primary_jobs:
            key = (pack_name, name)
            if key not in locations:
                locations[key] = append(pack_name, csvs[name]['payload'])
            dest = 'pack.idx' if role == 'master' else pack_name
            field = row['record'] + 12
            streams[dest].seek(field)
            streams[dest].write(struct.pack('<3I', locations[key], len(csvs[name]['payload']), 0))
            allowed[dest].append((field, 12))
        for pack_name, master_row, own_row, blob, _, _ in secondary_jobs:
            at = append(pack_name, blob)
            for dest, row in (('pack.idx', master_row), (pack_name, own_row)):
                field = row['record'] + 8
                streams[dest].seek(field)
                streams[dest].write(struct.pack('<2I', at, len(blob)))
                allowed[dest].append((field, 8))
    for name in allowed:
        compare_prefix(source / name, output / name, (source / name).stat().st_size, allowed[name])
    _, new_primary, new_archives = pidx(output / 'pack.idx')
    primary_reads, secondary_reads = 0, 0
    for rows in [new_primary] + [pidx(output / name)[1] for name in modified_packs]:
        for row in rows:
            if row['name'] in csvs:
                if read_payload(output / Path(row['pack']).name, row)[0] != csvs[row['name']]['payload']:
                    raise ValueError('Primary readback failed')
                primary_reads += 1
    target_archives = {(name, row['name']) for name, row, *_ in secondary_jobs}
    for rows in [new_archives] + [pidx(output / name)[2] for name in modified_packs]:
        for row in rows:
            pack_name = Path(row['pack']).name.lower()
            if (pack_name, row['name']) not in target_archives:
                continue
            for entry in fsts_entries(read_payload(output / pack_name, row)[0]):
                if entry['path'] in secondary_paths:
                    name = secondary_paths[entry['path']]
                    if decoded(read_payload(output / pack_name, row)[0], entry)[0] != csvs[name]['payload']:
                        raise ValueError('Secondary readback failed')
                    secondary_reads += 1
    if primary_reads != len(primary_jobs) or secondary_reads != 2 * sum(len(j[4]) for j in secondary_jobs):
        raise ValueError('Alias readback count mismatch')
    fixtures = output / 'csv'
    fixtures.mkdir()
    for name, item in csvs.items():
        (fixtures / name).write_bytes(item['payload'])
        (fixtures / (name + '.cp932')).write_bytes(item['raw'])
        (fixtures / (name + '.original.utf8')).write_bytes(item['raw'].decode(item['resource']['source_encoding']).encode('utf8'))
    result = dict(locale=locale, schema_version=2, catalog=catalog_path.relative_to(ROOT).as_posix(),
                  catalog_sha256=sha_file(catalog_path), input_package_sha256=sha_file(package),
                  csv=dict(files=len(csvs), labels=len(catalog['labels']), changed_cells=catalog['expected_changed_cells'],
                           non_name_cells_unchanged=True, line_endings_preserved=True),
                  primary_readbacks=primary_reads, secondary_readbacks=secondary_reads,
                  source_index_sha256=expected['pack.idx'],
                  secondary_source_pack_hashes={n: expected[n] for n in modified_packs},
                  output_files={n: dict(bytes=(output / n).stat().st_size, sha256=sha_file(output / n)) for n in allowed},
                  resources={n: dict(source_sha256=sha(c['raw']), output_sha256=sha(c['payload']), **c['proof']) for n, c in csvs.items()},
                  outside_allowed_original_bytes_changed=0, game_started=False, visual_check='pending')
    (output / 'FIELD-NAMES-RESULT.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    return result


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'output', 'catalog', 'package'):
        ap.add_argument('--' + name, type=Path, required=True)
    a = ap.parse_args()
    result = stage(a.source, a.output, a.catalog, a.package)
    print(json.dumps({k: v for k, v in result.items() if k != 'resources'}, ensure_ascii=False, indent=2))
