"""Independent-component release staging using the existing distribution auditors."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools/dist'))
from audit_prebuilt_release import audit

COMPONENTS = ('game', 'korean', 'english', 'launcher')
META = ('BOOT-INSTALL.json', 'LANGUAGE-PACKS.json', 'LAUNCHER.json', 'PYTHON-LAUNCHER.json', 'STATUS.json')
VERSION = r'\d+\.\d+\.\d+'


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def version(value):
    if not isinstance(value, str) or not re.fullmatch(VERSION, value):
        raise ValueError('Invalid version: '+str(value))
    return tuple(map(int, value.split('.')))


def identity(path):
    path = Path(path)
    return dict(file=path.name, bytes=path.stat().st_size, sha256=sha(path))


def rows(boot, launcher):
    return dict(game=boot['files']['program'], korean=boot['language_packs']['ko-KR'],
                english=boot['language_packs']['en-US'], launcher=launcher)


def row_version(component, row):
    prefix = {'game':'Macross30-program-', 'korean':'Macross30-language-ko-KR-',
              'english':'Macross30-language-en-US-', 'launcher':'Macross30Launcher-'}[component]
    suffix = '.exe' if component == 'launcher' else '.zip'
    match = re.fullmatch(re.escape(prefix)+'('+VERSION+')'+re.escape(suffix), row.get('file', ''))
    if not match or not re.fullmatch(r'[0-9a-f]{64}', row.get('sha256', '')) or type(row.get('bytes')) is not int or row['bytes'] <= 0:
        raise ValueError('Invalid '+component+' artifact identity')
    if component == 'launcher' and row.get('version') != match[1]:
        raise ValueError('Launcher version differs from executable filename')
    return match[1]


def assets_dir(config, key, component):
    value = config[key]
    return Path(value[component] if isinstance(value,dict) else value).resolve(strict=True)


def plan(config):
    changed = config['changed']
    if not changed or len(set(changed)) != len(changed) or set(changed)-set(COMPONENTS):
        raise ValueError('Choose distinct changed components from '+str(COMPONENTS))
    version(config['release'])
    base = Path(config['baseline_metadata']).resolve(strict=True)
    boot = read(base/'BOOT-INSTALL.json')
    launcher = read(base/'PYTHON-LAUNCHER.json')
    if read(base/'LAUNCHER.json') != launcher or read(base/'LANGUAGE-PACKS.json') != boot['language_packs']:
        raise ValueError('Baseline metadata aliases disagree')
    old = rows(boot, launcher)
    old_text = copy.deepcopy(boot.get('locale_packs', {}))
    if 'game' in changed:
        next_boot = read(assets_dir(config,'input_assets','game')/'BOOT-INSTALL.json')
        boot = copy.deepcopy(next_boot)
        boot['language_packs'] = copy.deepcopy(read(base/'BOOT-INSTALL.json')['language_packs'])
    else:
        boot = copy.deepcopy(boot)
    for component, locale in (('korean','ko-KR'), ('english','en-US')):
        if component in changed:
            next_boot = read(assets_dir(config,'input_assets',component)/'BOOT-INSTALL.json')
            boot['language_packs'][locale] = next_boot['language_packs'][locale]
    if 'launcher' in changed:
        launcher = read(assets_dir(config,'input_assets','launcher')/'PYTHON-LAUNCHER.json')
    selected = rows(boot, launcher)
    components = {}
    for component, row in selected.items():
        old_version, new_version = row_version(component, old[component]), row_version(component, row)
        if component in changed and version(new_version) <= version(old_version):
            raise ValueError('Changed component must advance: '+component)
        if component in changed and new_version != config['release']:
            raise ValueError('Changed artifact version must match its upload tag: '+component)
        if version(new_version) > version(config['release']):
            raise ValueError('Component newer than release tag: '+component)
        directory = assets_dir(config, 'input_assets' if component in changed else 'baseline_assets', component)
        path = directory/row['file']
        if not path.is_file() or path.stat().st_size != row['bytes'] or sha(path) != row['sha256']:
            raise ValueError('Artifact missing or changed: '+str(path))
        components[component] = dict(version=new_version, changed=component in changed,
                                     artifact=row, path=str(path),
                                     url='https://github.com/saintiron82/macross30-releases/releases/download/v'+new_version+'/'+row['file'])
    if boot['version'] != components['game']['version']:
        raise ValueError('Game manifest version differs from program artifact')
    text_packs = {}
    for locale, row in boot.get('locale_packs', {}).items():
        if not re.fullmatch(r'[A-Za-z]{2,8}(?:-[A-Za-z0-9]{1,8})*', locale):
            raise ValueError('Invalid extra language locale')
        match = re.fullmatch(r'Macross30-text-' + re.escape(locale) + r'-(\d+\.\d+\.\d+)\.zip', row.get('file', ''))
        if not match or not re.fullmatch('[a-f0-9]{64}', row.get('manifest_sha256', '')):
            raise ValueError('Invalid text-pack identity')
        changed_text = old_text.get(locale) != row
        if changed_text and match[1] != config['release']:
            raise ValueError('Changed text-pack version must match release tag')
        directory = Path(config['locale_assets']).resolve(strict=True) if config.get('locale_assets') else assets_dir(config, 'baseline_assets', 'game')
        path = directory / row['file']
        if not path.is_file() or path.stat().st_size != row['bytes'] or sha(path) != row['sha256']:
            raise ValueError('Text-pack missing or changed: ' + str(path))
        text_packs[locale] = dict(artifact=row, path=str(path), changed=changed_text, version=match[1])
    return dict(schema_version=1, release=config['release'], changed=changed,
                components=components, boot=boot, launcher=launcher,
                text_packs=text_packs,
                baseline_metadata=str(base), baseline_hashes={n:sha(base/n) for n in META})


def verify_text_packs(result):
    """Validate the data-only schema, every proof, base guard and archive paths."""
    if not result.get('text_packs'):
        return
    sys.path.insert(0, str(ROOT / 'tools/play'))
    from pack_registry import import_pack, digest
    with zipfile.ZipFile(result['components']['game']['path']) as program:
        base = program.read('program/bin/assets/locale-base.json')
    with tempfile.TemporaryDirectory(prefix='macross-text-audit-') as temporary:
        root = Path(temporary)
        guard = root / 'bin/assets/locale-base.json'
        guard.parent.mkdir(parents=True)
        guard.write_bytes(base)
        for locale, item in result['text_packs'].items():
            row = import_pack(root, item['path'])
            expected = 'en-US-fix' if locale == 'en-US' else locale
            if row['locale'] != expected or digest(Path(row['manifest'])) != item['artifact']['manifest_sha256']:
                raise ValueError('Text-pack locale or manifest mismatch')


def verify_launcher(executable, archive):
    with Path(executable).open('rb') as stream:
        if stream.read(2) != b'MZ': raise ValueError('Launcher is not a Windows EXE')
    with zipfile.ZipFile(archive) as z:
        if len(z.namelist()) != 2 or set(z.namelist()) != {'Macross30Launcher.exe', 'USAGE.txt'}:
            raise ValueError('Unexpected launcher ZIP members')
        with z.open('Macross30Launcher.exe') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != sha(executable):
                raise ValueError('Launcher ZIP and standalone EXE differ')


def prepare(config, output):
    result = plan(config)
    output = Path(output).resolve()
    if output.exists(): raise ValueError('Output must be a fresh directory')
    # Never create staging output under an input tree, or copy inputs onto themselves.
    for directory in ('baseline_metadata','baseline_assets','input_assets'):
        value = config[directory]
        for path in value.values() if isinstance(value,dict) else [value]:
            if output.is_relative_to(Path(path).resolve()):
                raise ValueError('Output overlaps input tree')
    output.mkdir(parents=True)
    assets = output/'assets'; assets.mkdir()
    # Audit all four components, including the reused packages, without retaining copies.
    with tempfile.TemporaryDirectory(prefix='macross-release-audit-') as td:
        audit_dir = Path(td)
        write(audit_dir/'BOOT-INSTALL.json', result['boot'])
        for component in ('game','korean','english'):
            source = Path(result['components'][component]['path'])
            target = audit_dir/source.name
            try: os.link(source, target)
            except OSError: shutil.copyfile(source, target)
        result['package_audit'] = audit(audit_dir)
    verify_text_packs(result)
    launch = result['components']['launcher']
    launch_dir = Path(launch['path']).parent
    launch_zip = launch_dir/('Macross30-launcher-'+launch['version']+'.zip')
    verify_launcher(launch['path'], launch_zip)
    for component in result['changed']:
        row = result['components'][component]
        shutil.copyfile(row['path'], assets/row['artifact']['file'])
    if launch['changed']: shutil.copyfile(launch_zip, assets/launch_zip.name)
    for row in result.get('text_packs', {}).values():
        if row['changed']:
            shutil.copyfile(row['path'], assets/row['artifact']['file'])
    base = Path(result['baseline_metadata'])
    status = read(base/'STATUS.json')
    status['latest']['game']['version'] = result['components']['game']['version']
    status['latest']['launcher']['version'] = launch['version']
    for component, locale in (('korean','ko-KR'), ('english','en-US')):
        status['latest']['language_packs'][locale]['version'] = result['components'][component]['version']
    if result.get('text_packs'):
        status['latest']['text_packs'] = {locale: dict(version=row['version'], text_revision=row['artifact'].get('text_revision'))
                                          for locale, row in result['text_packs'].items()}
    status['patch_notes'] = config.get('patch_notes', []) + status.get('patch_notes', [])
    status['updated_at'] = __import__('datetime').datetime.now(__import__('datetime').timezone.utc).isoformat()
    for name, value in (('BOOT-INSTALL.json',result['boot']), ('LANGUAGE-PACKS.json',result['boot']['language_packs']),
                        ('LAUNCHER.json',result['launcher']), ('PYTHON-LAUNCHER.json',result['launcher']), ('STATUS.json',status)):
        write(assets/name, value)
    preserve = []
    if not set(result['changed']) & {'game','korean','english'}: preserve.append('BOOT-INSTALL.json')
    if not set(result['changed']) & {'korean','english'}: preserve.append('LANGUAGE-PACKS.json')
    if 'launcher' not in result['changed']: preserve.extend(('LAUNCHER.json','PYTHON-LAUNCHER.json'))
    for name in preserve: shutil.copyfile(base/name,assets/name)
    notes = Path(config['notes']).read_text(encoding='utf-8-sig')
    (assets/'RELEASE-NOTES.txt').write_text(notes, encoding='utf-8')
    result['artifacts'] = [identity(p) for p in sorted(assets.iterdir())]
    result['config'] = config
    write(output/'release-plan.json', result)
    verify(output)
    return result


def verify(output):
    output = Path(output)
    result = read(output/'release-plan.json')
    expected = {r['file']:r for r in result['artifacts']}
    actual = {p.name for p in (output/'assets').iterdir()}
    if actual != set(expected): raise ValueError('Staged upload inventory changed')
    for name, row in expected.items():
        if Path(name).name != name or '\\' in name or ':' in name:
            raise ValueError('Unsafe staged file name')
        if identity(output/'assets'/name) != row: raise ValueError('Staged artifact changed: '+name)
    for component in COMPONENTS:
        row = result['components'][component]
        row_version(component, row['artifact'])
        if row['changed']:
            item = identity(output/'assets'/row['artifact']['file'])
            if any(item[key] != row['artifact'][key] for key in ('file','bytes','sha256')):
                raise ValueError('Selected component identity changed: '+component)
        if not row['changed'] and row['artifact']['file'] in actual:
            raise ValueError('Unchanged component must not be uploaded: '+component)
    if 'launcher' in result['changed']:
        row = result['components']['launcher']
        verify_launcher(output/'assets'/row['artifact']['file'],
                        output/'assets'/('Macross30-launcher-'+row['version']+'.zip'))
    boot = read(output/'assets/BOOT-INSTALL.json')
    launcher = read(output/'assets/PYTHON-LAUNCHER.json')
    if rows(boot, launcher) != {k:v['artifact'] for k,v in result['components'].items()}:
        raise ValueError('Staged metadata disagrees with component plan')
    if read(output/'assets/LAUNCHER.json') != launcher or read(output/'assets/LANGUAGE-PACKS.json') != boot['language_packs']:
        raise ValueError('Metadata aliases disagree')
    if boot.get('locale_packs', {}) != {locale: row['artifact'] for locale, row in result.get('text_packs', {}).items()}:
        raise ValueError('Staged text-pack catalog disagrees')
    for locale, row in result.get('text_packs', {}).items():
        if row['changed']:
            item = identity(output/'assets'/row['artifact']['file'])
            if any(item[key] != row['artifact'][key] for key in ('file','bytes','sha256')):
                raise ValueError('Staged text-pack identity changed: ' + locale)
    status = read(output/'assets/STATUS.json')['latest']
    versions = dict(game=status['game']['version'],launcher=status['launcher']['version'],
                    korean=status['language_packs']['ko-KR']['version'],english=status['language_packs']['en-US']['version'])
    if versions != {k:v['version'] for k,v in result['components'].items()}:
        raise ValueError('Status versions disagree')
    return dict(verified=True, release=result['release'], changed=result['changed'],
                versions={k:v['version'] for k,v in result['components'].items()}, files=len(expected))


def build(config):
    """Run explicit argv steps, using the existing compiler/packager entry points."""
    completed = []
    for component in config['changed']:
        steps = config.get('build_steps', {}).get(component)
        if not steps: raise ValueError('No explicit build steps for '+component+'; use prepare for prebuilt inputs')
        for argv in steps:
            if not isinstance(argv, list) or not argv or not all(isinstance(x,str) and x for x in argv):
                raise ValueError('Build commands must be nonempty argv arrays')
            subprocess.run(argv, cwd=ROOT, check=True, shell=False)
            completed.append(dict(component=component, command=argv[0]))
    return dict(completed=completed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('plan','build','prepare'):
        command = sub.add_parser(name); command.add_argument('--config', type=Path, required=True)
        if name == 'prepare': command.add_argument('--output', type=Path, required=True)
    for name in ('verify','verify-public'):
        command = sub.add_parser(name); command.add_argument('output', type=Path)
    command = sub.add_parser('publish'); command.add_argument('output', type=Path); command.add_argument('--checkout', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'verify': result = verify(args.output)
    elif args.command == 'verify-public':
        from publish import verify_public
        result = verify_public(args.output)
    elif args.command == 'publish':
        from publish import publish
        result = publish(args.output, args.checkout)
    else:
        config = read(args.config)
        result = plan(config) if args.command == 'plan' else build(config) if args.command == 'build' else prepare(config,args.output)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
