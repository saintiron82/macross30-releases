"""Package the isolated path fix while retaining the released 0.3.9 launcher ABI."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[2]
BASELINE = Path('D:/Game/Macross-release-039-integration-20261107/public-release/assets')
CANDIDATE = ROOT/'native/subtitle-candidate/release-039-startup-path-fix-r2'


def encoded(value):
    return (json.dumps(value, ensure_ascii=False, indent=2)+'\n').encode('utf-8')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def archive(path, files):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, (2026, 11, 8, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            z.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)


def read_archive(folder, row):
    data = (folder/row['file']).read_bytes()
    if len(data) != row['bytes'] or digest(data) != row['sha256']:
        raise ValueError('Baseline artifact changed: '+row['file'])
    with zipfile.ZipFile(folder/row['file']) as z:
        return {name: z.read(name) for name in z.namelist() if not name.endswith('/')}


def prepare(output):
    if output.exists():
        raise ValueError('Hotfix package directory must be fresh')
    output.mkdir(parents=True)
    boot = json.loads((BASELINE/'BOOT-INSTALL.json').read_text(encoding='utf-8'))
    assert boot['version'] == '0.3.9'
    original_files = read_archive(BASELINE, boot['files']['program'])
    files = dict(original_files)
    exe = (CANDIDATE/'Macross30Recompiled.exe').read_bytes()
    built = json.loads((CANDIDATE/'build-result.json').read_text(encoding='utf-8'))
    assert digest(exe) == built['products']['exe']['sha256']
    files['program/bin/Macross30Recompiled.exe'] = exe
    base = json.loads(files['program/bin/assets/locale-base.json'])
    # Existing launcher 0.3.9 validates this compatibility epoch. The actual
    # release identity and executable hash still advance and are verified.
    assert base['runtime_version'] == '0.3.9' and base['schema'] == 1
    base.update(runtime_release_version='0.3.10', runtime_exe_sha256=digest(exe))
    base_data = encoded(base)
    files['program/bin/assets/locale-base.json'] = base_data
    manifest = json.loads(files.pop('program/program-manifest.json'))
    manifest['version'] = '0.3.10'
    manifest['files'] = [dict(path=name[len('program/'):], bytes=len(data), sha256=digest(data)) for name,data in sorted(files.items())]
    files['program/program-manifest.json'] = encoded(manifest)
    target = output/'Macross30-program-0.3.10.zip'
    archive(target, files)
    boot['version'] = '0.3.10'
    boot['min_launcher_version'] = '0.3.9'
    boot['files']['program'] = dict(file=target.name, bytes=target.stat().st_size,
        sha256=digest(target.read_bytes()), exe_sha256=digest(exe))
    boot['features']['locale_packs']['runtime_release_version'] = '0.3.10'
    comparisons = {}
    for locale, old in list(boot['locale_packs'].items()):
        before = read_archive(BASELINE, old)
        after = dict(before)
        doc = json.loads(after['pack.json'])
        doc['source_guards'] = [dict(path='bin/assets/locale-base.json', sha256=digest(base_data))]
        after['pack.json'] = encoded(doc)
        assert {n:d for n,d in before.items() if n != 'pack.json'} == {n:d for n,d in after.items() if n != 'pack.json'}
        target = output/f'Macross30-text-{locale}-0.3.10.zip'
        archive(target, after)
        boot['locale_packs'][locale] = {**old, 'file':target.name, 'bytes':target.stat().st_size,
            'sha256':digest(target.read_bytes()), 'manifest_sha256':digest(after['pack.json'])}
        comparisons[locale] = dict(payloads_identical=True, changed_members=['pack.json'])
    changed = sorted(n for n in original_files if original_files[n] != files[n])
    assert changed == ['program/bin/Macross30Recompiled.exe', 'program/bin/assets/locale-base.json', 'program/program-manifest.json']
    (output/'BOOT-INSTALL.json').write_bytes(encoded(boot))
    (output/'LANGUAGE-PACKS.json').write_bytes(encoded(boot['language_packs']))
    report = dict(ok=True, version='0.3.10', launcher_version='0.3.9', min_launcher_version='0.3.9',
        program_changed_members=changed, translations=comparisons,
        korean_base_unchanged=True, english_image_base_unchanged=True,
        shader_hd_renderer_assets_unchanged=True, game_started=False, public_published=False)
    (output.parent/'PACKAGE-COMPARISON.json').write_bytes(encoded(report))
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.output.resolve())))
