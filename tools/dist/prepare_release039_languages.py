"""Prepare 0.3.9 program/text packages from pinned 0.3.8 and reviewed inputs."""
import ast
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PUBLIC = Path('D:/Game/Macross-release-038-hd-shader-20261103/public-final/assets')
OUT = Path('D:/Game/Macross-release-039-integration-20261107')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def encode(value):
    return (json.dumps(value, ensure_ascii=False, indent=2) + '\n').encode('utf-8')


def archive(path, files):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, (2026, 11, 7, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            z.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)


def main():
    packages = OUT / 'packages-r3'
    if packages.exists():
        raise ValueError('Package output must be fresh')
    packages.mkdir(parents=True)
    boot = json.loads((PUBLIC / 'BOOT-INSTALL.json').read_text(encoding='utf-8'))
    original = PUBLIC / boot['files']['program']['file']
    assert sha(original.read_bytes()) == boot['files']['program']['sha256']
    exe = ROOT / 'native/subtitle-candidate/release-039-integrated-r1/Macross30Recompiled.exe'
    result = json.loads(exe.with_name('build-result.json').read_text(encoding='utf-8'))
    assert sha(exe.read_bytes()) == result['products']['exe']['sha256']
    ready = json.loads((ROOT / 'localization/en-US/pack-ready.json').read_text(encoding='utf-8'))
    source_pack = ROOT / ready['vfs'] / 'PS3_GAME/USRDIR/DATA/PACK'
    base = dict(schema=1, runtime_version='0.3.9', runtime_exe_sha256=sha(exe.read_bytes()),
                english_elf_sha256=boot['language_packs']['en-US']['elf_sha256'],
                english_data_id=boot['language_packs']['en-US']['data_id'],
                english_plan_sha256=boot['language_packs']['en-US']['plan_sha256'],
                pack_files=[dict(file=name, sha256=sha((source_pack / name).read_bytes())) for name in ('DATA.DAT', 'PACK.IDX')])
    base_data = encode(base)
    with zipfile.ZipFile(original) as z:
        files = {name: z.read(name) for name in z.namelist() if not name.endswith('/')}
    program = json.loads(files.pop('program/program-manifest.json'))
    files['program/bin/Macross30Recompiled.exe'] = exe.read_bytes()
    files['program/bin/assets/locale-base.json'] = base_data
    files['program/tools/play/launch-locale.ps1'] = (ROOT / 'tools/play/launch-locale.ps1').read_bytes()
    name = 'program/tools/local_build/prebuilt_finalize.py'
    text = files[name].decode('utf-8')
    old = "'bin/assets/aircraft-hd-recipe.json'}"
    assert old in text
    files[name] = text.replace(old, "'bin/assets/aircraft-hd-recipe.json', 'bin/assets/locale-base.json', 'tools/play/launch-locale.ps1'}", 1).encode()
    program.update(version='0.3.9', locale_pack_schema=1)
    program['files'] = [dict(path=name[len('program/'):], bytes=len(data), sha256=sha(data)) for name, data in sorted(files.items())]
    files['program/program-manifest.json'] = encode(program)
    target = packages / 'Macross30-program-0.3.9.zip'
    archive(target, files)
    boot.update(version='0.3.9', min_launcher_version='0.3.9')
    boot['files']['program'] = dict(file=target.name, bytes=target.stat().st_size, sha256=sha(target.read_bytes()), exe_sha256=base['runtime_exe_sha256'])
    boot.setdefault('features', {})['locale_packs'] = dict(schema=1, base_locale='en-US', runtime_version='0.3.9')
    boot['features'].setdefault('hd_textures', {})['available'] = True
    catalog = {}
    def text_pack(locale, files):
        doc = json.loads(files['pack.json'])
        doc['source_guards'] = [dict(path='bin/assets/locale-base.json', sha256=sha(base_data))]
        files['pack.json'] = encode(doc)
        target = packages / ('Macross30-text-' + locale + '-0.3.9.zip')
        archive(target, files)
        catalog[locale] = dict(file=target.name, bytes=target.stat().st_size, sha256=sha(target.read_bytes()),
                               manifest_sha256=sha(files['pack.json']), label='English' if locale == 'en-US' else doc['label'],
                               text_revision='corrections-1980' if locale == 'en-US' else 'v5' if locale == 'ru-RU' else 'v3')
    expected = {'ru-RU-full-text-v5': 'dc7ff01a1fb9e8b28eeddbebf3a45fe5a1e816f3226aaf53f3c26373ab97a678',
                'es-ES-full-text-v3': '06075f913ecfa0d2c833b4d99d3b9d3f26a5763263daa6ddf6b16c3a2566eabb'}
    for name, expected_sha in expected.items():
        source = ROOT / 'final/language-packs' / (name + '.zip')
        assert sha(source.read_bytes()) == expected_sha
        with zipfile.ZipFile(source) as z:
            data = {n: z.read(n) for n in z.namelist()}
        text_pack('ru-RU' if name.startswith('ru') else 'es-ES', data)
    # English corrections remain an overlay over the same immutable v14 base.
    # Russian and Spanish source identities are not silently rebased.
    english = {}
    diagnostic = ROOT / 'analysis/language-pack-release-review-20261107'
    for path in (diagnostic / 'english-gop-diagnostic').glob('*.gop'):
        english['gop/' + path.name] = path.read_bytes()
    english['elf-text.pack'] = (diagnostic / 'english-elf-corrections-diagnostic.pack').read_bytes()
    # Keep reviewed wording intact; extend the font for punctuation such as U+2013.
    sys.path[:0] = [str(ROOT/'tools/localization'), str(ROOT/'tools/analysis')]
    from stage_gop_primary_mirrors import pidx, read_payload, fsts_entries
    from pack_font import build as extend_font
    _, _, archives = pidx(source_pack/'PACK.IDX')
    resident = next(row for row in archives if row['name'] == 'resident')
    members = fsts_entries(read_payload(source_pack/Path(resident['pack']).name.upper(), resident)[0])
    font_source = OUT/'english-font-source'
    font_source.mkdir()
    for name in ('font_j24x24.abf', 'font_j24x24_0.dds'):
        write(font_source/name, next(row['payload'] for row in members if row['name'] == 'data/font/'+name))
    corrections = json.loads((diagnostic/'english-corrections-audit.json').read_text(encoding='utf-8'))
    font_output = OUT/'english-font-extension'
    font_report = extend_font(font_source/'font_j24x24.abf', font_source/'font_j24x24_0.dds',
                              Path('C:/Windows/Fonts/arial.ttf'),
                              ''.join(row['corrected_en'] for row in corrections['corrections']), font_output)
    for name in ('font_j24x24.abf', 'font_j24x24_0.dds'):
        english['gop/'+name] = (font_output/name).read_bytes()
    english['font-provenance.json'] = encode(font_report)
    base_english = PUBLIC / boot['language_packs']['en-US']['file']
    assert sha(base_english.read_bytes()) == boot['language_packs']['en-US']['sha256']
    with zipfile.ZipFile(base_english) as z:
        for name in z.namelist():
            if name.startswith('payload/subtitles-en/') and not name.endswith('/'):
                english['subtitles/' + name[len('payload/subtitles-en/'):]] = z.read(name)
    ui = ['MACROSS_UI_STRINGS_V1\ten-US']
    scope = json.loads((ROOT / 'localization/ja-en-ru-review/native-ui-scope.json').read_text(encoding='utf-8'))
    for entry in scope['entries']:
        text = entry['installed_en']
        ui.append(entry['key'] + '\t' + text.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t'))
    assert len(ui) > 75
    english['native-ui.tsv'] = ('\n'.join(ui) + '\n').encode()
    assert 'subtitles/tracks.tsv' in english and 'subtitles/battle.en.tsv' in english
    doc = dict(schema_version=1, locale='en-US-fix', label='English', description='1,980 reviewed English corrections over the immutable v14 English image/text base.',
               status='complete', base_locale='en-US', image_locale='en-US', coverage=dict(total=1980, translated=1980, fallback=0),
               resources=dict(gop_overlay='gop', elf_overlay='elf-text.pack', native_ui='native-ui.tsv', subtitles='subtitles', battle_catalog='subtitles/battle.en.tsv'),
               proofs=[dict(path=name, sha256=sha(data)) for name, data in sorted(english.items())], source_guards=[])
    english['pack.json'] = encode(doc)
    text_pack('en-US', english)
    boot['locale_packs'] = catalog
    write(packages / 'BOOT-INSTALL.json', encode(boot))
    write(packages / 'LANGUAGE-PACKS.json', encode(boot['language_packs']))
    write(OUT / 'locale-base.json', base_data)
    write(OUT / 'TEXT-PACKS.json', encode(catalog))
    print(json.dumps(dict(output=str(OUT), game=boot['files']['program'], text_packs=catalog)))


if __name__ == '__main__':
    main()
