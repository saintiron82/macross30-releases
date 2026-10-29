"""Create source + local-generation data packages, never a game EXE/ELF."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tools/dist'), str(ROOT / 'tools/play')]
from make_dist import make_delta
from elf_patch import apply
from audit import inspect


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', type=Path, default=Path('C:/GameDev/Macross-local-build-0.2.0'))
    args = parser.parse_args()
    out = args.target.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Never leave an old installable manifest beside a partially rebuilt candidate.
    (out / 'LOCAL-BUILD.json').unlink(missing_ok=True)
    (out / 'PUBLIC-PACKAGE-AUDIT.json').unlink(missing_ok=True)
    version = '0.2.0'
    source_zip = out / f'Macross30-source-{version}.zip'
    subprocess.run([sys.executable, str(ROOT / 'tools/local_build/source_bundle.py'), '--output', str(source_zip)], check=True)
    dist = Path('C:/GameDev/Macross-dist-0.1.0/Macross30')
    original = ROOT / 'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf'
    localized = dist / 'bin/EBOOT.ko.elf'
    delta, _ = make_delta(original, localized, sha(original), sha(localized), 2048)
    assert hashlib.sha256(apply(original.read_bytes(), delta)).hexdigest() == sha(localized)
    original_plan = Path('C:/GameDev/Macross-language-0.1.0/languages/ja-JP/0.1.0/install-plan.tsv')
    if not original_plan.exists():
        subprocess.run([sys.executable, str(ROOT / 'tools/launcher/build_language_catalog.py')], check=True)
    recipe = dict(version=version, mode='user-local-generate-and-compile',
                  original_elf_sha256=sha(original), original_elf_bytes=original.stat().st_size,
                  languages={'ko-KR': dict(elf_sha256=sha(localized), plan='plans/ko-KR.tsv', elf_patch='elf/ko-KR.mkdelta'),
                             'ja-JP': dict(elf_sha256=sha(original), plan='plans/ja-JP.tsv')},
                  encrypted_self_supported=True, downloaded_game_executable=False)
    for locale, row in recipe['languages'].items():
        plan = ((dist / 'korean/install-plan.tsv') if locale == 'ko-KR' else original_plan).read_text(encoding='utf8').replace('\r\n', '\n')
        row['data_id'] = hashlib.sha256(plan.encode()).hexdigest()[:16]
    data_zip = out / f'Macross30-local-data-{version}.zip'
    with zipfile.ZipFile(data_zip, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        for file in sorted((dist / 'korean').rglob('*')):
            if file.is_file():
                z.write(file, 'payload/' + file.relative_to(dist).as_posix())
        for file in sorted((dist / 'bin/subtitles').rglob('*')):
            if file.is_file():
                z.write(file, 'payload/subtitles/' + file.relative_to(dist / 'bin/subtitles').as_posix())
        z.writestr('payload/elf/ko-KR.mkdelta', delta)
        for locale, row in recipe['languages'].items():
            plan = ((dist / 'korean/install-plan.tsv') if locale == 'ko-KR' else original_plan).read_text(encoding='utf8').replace('\r\n', '\n')
            z.writestr('payload/' + row['plan'], plan)
        cmd = (dist / 'Macross30.cmd').read_text(encoding='utf8').replace('bin\\EBOOT.ko.elf', 'bin\\EBOOT.local.elf').replace('korean\\data-version.txt', '.local-build\\data-version.txt')
        z.writestr('payload/Macross30.cmd', cmd)
        for name in ('launch-helper.ps1', 'collect-logs.ps1'):
            z.write(ROOT / 'tools/dist/template/tools' / name, 'payload/launch-tools/' + name)
        z.write(ROOT / 'tools/dist/template/CollectLogs.cmd', 'payload/CollectLogs.cmd')
        z.write(ROOT / 'tools/dist/template/tools/DistInstaller.cs', 'payload/DistInstaller.cs')
        z.writestr('payload/recipe.json', json.dumps(recipe, indent=2))
        captions = []
        for line in (ROOT / 'native/macross30/battle_voice_catalog.inc').read_text(encoding='utf8').splitlines():
            match = re.fullmatch(r'\s*\{\s*("(?:[^"\\]|\\.)*")\s*,\s*("(?:[^"\\]|\\.)*")\s*\},?\s*', line)
            if match:
                captions.append([json.loads(match[1]), json.loads(match[2])])
        if not captions:
            raise ValueError('No locally generatable caption data')
        z.writestr('payload/battle-captions.json', json.dumps(captions, ensure_ascii=False))
    runtime_zip = out / f'Macross30-third-party-{version}.zip'
    with zipfile.ZipFile(runtime_zip, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        for file in sorted((dist / 'bin/ffmpeg').glob('*.dll')):
            z.write(file, 'runtime/bin/ffmpeg/' + file.name)
        z.write(dist / 'THIRD-PARTY-NOTICES.txt', 'runtime/THIRD-PARTY-NOTICES.txt')
    manifest = dict(version=version, mode=recipe['mode'], original_elf_sha256=sha(original),
                    languages=['ko-KR', 'ja-JP'], source_only=True, encrypted_self_supported=True,
                    files={kind: dict(file=path.name, bytes=path.stat().st_size, sha256=sha(path)) for kind, path in
                           [('source', source_zip), ('data', data_zip), ('third_party', runtime_zip)]})
    report = inspect(out, manifest, original.read_bytes())
    (out / 'PUBLIC-PACKAGE-AUDIT.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
    (out / 'LOCAL-BUILD.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
