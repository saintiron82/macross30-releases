"""Split a verified combined payload into independent on-demand language ZIPs."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

SUPPORTED = ('ko-KR', 'en-US')
BASE_FILES = {'CollectLogs.cmd', 'DistInstaller.cs', 'Macross30.cmd',
              'launch-tools/collect-logs.ps1', 'launch-tools/launch-helper.ps1',
              'plans/ja-JP.tsv', 'recipe.json'}
COMMON = {'payload/Macross30.cmd', 'payload/CollectLogs.cmd', 'payload/DistInstaller.cs'}


def sha(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def base_payload(archive_path, version='0.3.1'):
    """Return only original-game installation instructions, never game data."""
    with zipfile.ZipFile(archive_path) as source:
        recipe = json.loads(source.read('payload/recipe.json'))
        language = recipe['languages']['ja-JP']
        if language['plan'] != 'plans/ja-JP.tsv' or 'elf_patch' in language:
            raise ValueError('Japanese base must use unmodified original ELF')
        plan = source.read('payload/plans/ja-JP.tsv')
        for line in plan.decode('utf-8-sig').splitlines():
            if line and not line.startswith('#') and line.split('\t')[0] not in {'dir', 'iso', 'copy'}:
                raise ValueError('Original-game preparation cannot contain translation patches')
        recipe.update(version=version, mode='user-original-eboot', languages={'ja-JP': language})
        files = {name: source.read('payload/' + name) for name in BASE_FILES if name != 'recipe.json'}
        files['recipe.json'] = json.dumps(recipe, indent=2).encode()
        metadata = dict(locale='ja-JP', data_id=language['data_id'], elf_sha256=language['elf_sha256'],
                        plan_sha256=hashlib.sha256(plan).hexdigest())
        return files, metadata


def split(archive_path, output, version='0.3.1'):
    archive_path, output = Path(archive_path), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    result = {}
    with zipfile.ZipFile(archive_path) as source:
        original = json.loads(source.read('payload/recipe.json'))
        names = set(source.namelist())
        if len(names) != len(source.namelist()):
            raise ValueError('Duplicate combined-payload member')
        for locale in SUPPORTED:
            if locale not in original['languages']:
                continue
            language = original['languages'][locale]
            plan_path = 'payload/' + language['plan']
            plan = source.read(plan_path)
            selected = set(COMMON) | {name for name in names if name.startswith('payload/launch-tools/')}
            selected.add(plan_path)
            if 'elf_patch' in language:
                selected.add('payload/' + language['elf_patch'])
            for line in plan.decode('utf-8-sig').splitlines():
                if not line or line.startswith('#'):
                    continue
                row = line.split('\t')
                if row[0] == 'delta':
                    selected.add('payload/' + row[5])
            subtitle_prefix = 'payload/subtitles-en/' if locale == 'en-US' else 'payload/subtitles/'
            if locale != 'ja-JP':
                selected.update(name for name in names if name.startswith(subtitle_prefix))
            if locale == 'en-US':
                selected.update(name for name in names if name.startswith('payload/english-v') and name.endswith('-distribution.json'))
            if not selected.issubset(names):
                raise ValueError('Missing selected-language payload: ' + str(selected - names))
            recipe = dict(original)
            recipe.update(version=version, mode='user-original-eboot', languages={locale: language})
            target = output / ('Macross30-language-' + locale + '-' + version + '.zip')
            with zipfile.ZipFile(target, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
                for name in sorted(selected):
                    if not name.startswith('payload/') or '..' in name.split('/') or '\\' in name or ':' in name:
                        raise ValueError('Unsafe selected-language member')
                    if Path(name).suffix.lower() in {'.elf', '.self', '.bin', '.exe', '.iso', '.obj', '.dll'}:
                        raise ValueError('Complete binary file is forbidden in a language ZIP')
                    info=zipfile.ZipInfo(name, date_time=(2026,1,1,0,0,0)); info.create_system=3; info.external_attr=0o100644<<16
                    archive.writestr(info, source.read(name), compress_type=zipfile.ZIP_DEFLATED)
                info=zipfile.ZipInfo('payload/recipe.json',date_time=(2026,1,1,0,0,0));info.create_system=3;info.external_attr=0o100644<<16
                archive.writestr(info, json.dumps(recipe, indent=2).encode(),compress_type=zipfile.ZIP_DEFLATED)
            result[locale] = dict(file=target.name, bytes=target.stat().st_size, sha256=sha(target),
                                  data_id=language['data_id'], elf_sha256=language['elf_sha256'],
                                  plan_sha256=hashlib.sha256(plan).hexdigest())
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', default='0.3.1')
    args = parser.parse_args()
    print(json.dumps(split(args.archive, args.output, args.version), indent=2))
