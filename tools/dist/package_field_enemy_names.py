"""Repackage a frozen language ZIP with reviewed field-name resources.

Writes a separate unpublished package. Keeps the original package, runtime,
ELF, active manifests and user installation unchanged.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import sys
import zipfile

from make_dist import make_delta, apply_delta_sha, read_delta_header, sha_file

ROOT = Path(__file__).resolve().parents[2]


def package(previous, staged, output, version=None):
    previous, staged, output = (Path(p).resolve() for p in (previous, staged, output))
    if output.exists() or not output.is_relative_to(ROOT / 'build/releases'):
        raise ValueError('Require a new release output directory inside project')
    report = json.loads((staged / 'FIELD-NAMES-RESULT.json').read_text('utf8'))
    catalog_path = ROOT / report.get('catalog', 'localization/ko-KR/field_enemy_names.json')
    catalog = json.loads(catalog_path.read_text('utf8'))
    expected_cells = catalog.get('expected_changed_cells', sum(len(r['object_ids']) for r in catalog.get('entries', [])))
    if report['catalog_sha256'] != sha_file(catalog_path) or report['csv']['changed_cells'] != expected_cells:
        raise ValueError('Field-name staging proof changed')
    if report.get('input_package_sha256', sha_file(previous)) != sha_file(previous):
        raise ValueError('Staging source package changed')
    with zipfile.ZipFile(previous) as z:
        if len(set(z.namelist())) != len(z.namelist()):
            raise ValueError('Duplicate input package members')
        payload = {n: z.read(n) for n in z.namelist() if not n.endswith('/')}
    before = dict(payload)
    recipe = json.loads(payload['payload/recipe.json'])
    locale = report.get('locale', 'ko-KR')
    language = recipe['languages'][locale]
    version = version or recipe['version']
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('Invalid package version')
    recipe['version'] = version
    plan_member = 'payload/' + language['plan']
    source_hashes = dict(report['secondary_source_pack_hashes'], **{'pack.idx': report['source_index_sha256']})
    changed, rows, delta_proofs = {}, [], {}
    for line in payload[plan_member].decode('utf-8-sig').splitlines():
        if not line or line.startswith('#'):
            rows.append(line)
            continue
        fields = line.split('\t')
        fields += [''] * (6 - len(fields))
        kind, target, size, digest, source, delta_member = fields
        name = Path(target).name.lower()
        if kind in ('delta', 'iso') and '/DATA/PACK/' in target.upper() and name in report['output_files']:
            if digest != source_hashes[name]:
                raise ValueError('Language plan is not the reviewed source: ' + name)
            translated = staged / name
            want = report['output_files'][name]
            if translated.stat().st_size != want['bytes'] or sha_file(translated) != want['sha256']:
                raise ValueError('Staged resource changed: ' + name)
            original = ROOT / 'native/vfs' / source
            original_sha = sha_file(original)
            if kind == 'delta' and read_delta_header(payload['payload/' + delta_member])['source_sha'] != original_sha:
                raise ValueError('Original resource differs from previous delta source: ' + name)
            if kind == 'iso' and original_sha != digest:
                raise ValueError('Original ISO resource changed: ' + name)
            data, _ = make_delta(original, translated, original_sha, want['sha256'], 2048)
            if apply_delta_sha(data, original) != (want['bytes'], want['sha256']):
                raise ValueError('Delta readback failed: ' + name)
            # Reuse the existing member when present, leaving no obsolete delta.
            delta_member = delta_member or ('field-display/' + locale + '/' + name + '.mkdelta')
            payload['payload/' + delta_member] = data
            fields = ['delta', target, str(want['bytes']), want['sha256'], source, delta_member]
            changed[target.lower()] = (fields[2], fields[3])
            delta_proofs[name] = dict(original_sha256=original_sha, **want,
                                      delta_sha256=hashlib.sha256(data).hexdigest(),
                                      delta_bytes=len(data), roundtrip_verified=True)
        elif kind == 'copy' and source.lower() in changed:
            fields[2:4] = changed[source.lower()]
        rows.append('\t'.join(fields))
    if len(changed) != len(report['output_files']):
        raise ValueError('Staged resources missing from language plan')
    plan = ('\n'.join(rows) + '\n').encode('utf8')
    payload[plan_member] = plan
    old_data_id = language['data_id']
    language['data_id'] = hashlib.sha256(plan).hexdigest()[:16]
    if old_data_id == language['data_id']:
        raise ValueError('Language update identity did not change')
    payload['payload/recipe.json'] = (json.dumps(recipe, ensure_ascii=False, indent=2) + '\n').encode('utf8')
    output.mkdir(parents=True)
    target = output / ('Macross30-language-' + locale + '-' + version + '.zip')
    with zipfile.ZipFile(target, 'x', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name, content in sorted(payload.items()):
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            z.writestr(info, content, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
    with zipfile.ZipFile(target) as z:
        if {n: z.read(n) for n in z.namelist()} != payload:
            raise ValueError('ZIP readback failed')
    result = dict(version=recipe['version'], locale=locale, file=target.name,
                  bytes=target.stat().st_size, sha256=sha_file(target),
                  input_sha256=sha_file(previous), previous_data_id=old_data_id,
                  data_id=language['data_id'], elf_sha256=language['elf_sha256'],
                  plan_sha256=hashlib.sha256(plan).hexdigest(), deltas=delta_proofs,
                  changed_members=[n for n in payload if payload[n] != before.get(n)],
                  game_started=False, published=False, visual_check='pending',
                  integration='Language replacement package; merge identity and any English base guard into release metadata before publishing')
    (output / 'BUILD-RESULT.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    return result


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--previous', type=Path, required=True)
    ap.add_argument('--staged', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--version')
    a = ap.parse_args()
    print(json.dumps(package(a.previous, a.staged, a.output, a.version), ensure_ascii=False, indent=2))
