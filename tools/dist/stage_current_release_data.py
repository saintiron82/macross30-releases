"""Rebase frozen KO menu resources on original ISO data; stage current EN separately.

Explicitly consumes a verified previous split release plus a selected KO bundle.
Never installs into the development game or publishes any files.
"""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

from make_dist import make_delta, apply_delta_sha
from prebuilt_package import sha

ROOT = Path(__file__).resolve().parents[2]


def stage(previous, output, bundle_name, version, elf_override=None, elf_sha256=None):
    previous, output = Path(previous), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((previous/'BOOT-INSTALL.json').read_text(encoding='utf-8'))
    # Verify the precise prior archive inputs before any merge.
    inputs = [manifest['files']['program'], *manifest['language_packs'].values()]
    for row in inputs:
        path = previous/row['file']
        if path.stat().st_size != row['bytes'] or sha(path) != row['sha256']:
            raise ValueError('Previous artifact identity mismatch: '+path.name)
    payload, languages = {}, {}
    for row in inputs:
        with zipfile.ZipFile(previous/row['file']) as archive:
            for name in archive.namelist():
                prefix = 'program/base-game/' if row is inputs[0] else 'payload/'
                if not name.startswith(prefix) or name.endswith('/'):
                    continue
                relative = name[len(prefix):]
                if relative == 'recipe.json':
                    recipe = json.loads(archive.read(name))
                    languages.update(recipe['languages'])
                else:
                    content = archive.read(name)
                    if relative in payload and payload[relative] != content:
                        raise ValueError('Conflicting previous common file: '+relative)
                    payload[relative] = content
    recipe.update(languages=languages, version=version, mode='user-original-eboot')
    # Paths in this ready record are explicit immutable candidates, not the active installation.
    if not bundle_name or Path(bundle_name).name != bundle_name:
        raise ValueError('Invalid KO bundle')
    bundle = ROOT/'localization/image_assets'/bundle_name
    ready = json.loads((bundle/'ready.json').read_text(encoding='utf-8'))
    selected = {}
    for row in ready['files']:
        path = (bundle/row['candidate']).resolve()
        if not path.is_relative_to(bundle.resolve()) or sha(path) != row['sha256']:
            raise ValueError('KO ready resource changed: '+str(path))
        selected[row['filename'].lower()] = path
    plan_path = languages['ko-KR']['plan']
    rows = []
    changed = {}
    cache = output/'delta-cache'
    cache.mkdir(exist_ok=True)
    for line in payload[plan_path].decode('utf-8-sig').splitlines():
        if not line or line.startswith('#'):
            rows.append(line)
            continue
        fields = line.split('\t')
        fields += ['']*(6-len(fields))
        kind, target, size, digest, source, delta = fields
        name = Path(target).name.lower()
        if kind in ('iso', 'delta') and '/DATA/PACK/' in target.upper() and name in selected:
            translated = selected[name]
            original = ROOT/'native/vfs'/source
            original_sha = sha(original)
            expected = sha(translated)
            delta_file = cache/(expected[:16]+'-'+name+'.mkdelta')
            if not delta_file.exists():
                print('KO_DELTA '+name, flush=True)
                data, _ = make_delta(original, translated, original_sha, expected, 2048)
                count, actual = apply_delta_sha(data, original)
                if actual != expected or count != translated.stat().st_size:
                    raise ValueError('KO delta readback mismatch')
                delta_file.write_bytes(data)
            relative = 'korean-current/'+name+'.mkdelta'
            payload[relative] = delta_file.read_bytes()
            fields = ['delta', target, str(translated.stat().st_size), expected, source, relative]
            changed[target.lower()] = (fields[2], expected)
        elif kind == 'copy' and source.lower() in changed:
            fields[2:4] = changed[source.lower()]
        rows.append('\t'.join(fields))
    if len(changed) != len(selected):
        raise ValueError('Some selected KO resources are missing from the install plan')
    plan = ('\n'.join(rows)+'\n').encode()
    payload[plan_path] = plan
    languages['ko-KR']['data_id'] = hashlib.sha256(plan).hexdigest()[:16]
    elf = Path(elf_override).resolve() if elf_override else (bundle/ready['elf']['path']).resolve()
    expected_elf = elf_sha256 if elf_override else ready['elf']['sha256']
    if not expected_elf or (not elf_override and not elf.is_relative_to(bundle.resolve())) or sha(elf) != expected_elf:
        raise ValueError('KO ELF ready proof changed')
    original_elf = ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf'
    if sha(original_elf) != recipe['original_elf_sha256']:
        raise ValueError('Original ELF identity mismatch')
    delta, _ = make_delta(original_elf, elf, sha(original_elf), sha(elf), 2048)
    if apply_delta_sha(delta, original_elf)[1] != sha(elf):
        raise ValueError('KO ELF delta readback failed')
    payload[languages['ko-KR']['elf_patch']] = delta
    languages['ko-KR']['elf_sha256'] = sha(elf)
    for name in ('collect-logs.ps1', 'launch-helper.ps1'):
        payload['launch-tools/'+name] = (ROOT/'tools/dist/template/tools'/name).read_bytes()
    payload['CollectLogs.cmd'] = (ROOT/'tools/dist/template/CollectLogs.cmd').read_bytes()
    payload['recipe.json'] = json.dumps(recipe, indent=2).encode()
    target = output/'combined-ko-current.zip'
    with zipfile.ZipFile(target, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(payload.items()):
            archive.writestr('payload/'+name, data)
    (output/'ko-stage-proof.json').write_text(json.dumps(dict(bundle=bundle_name,
        bundle_ready_sha256=sha(bundle/'ready.json'), changed_resources=changed,
        combined_sha256=sha(target), live_verified=ready.get('in_game_verified', False)), indent=2))
    print(str(target), flush=True)
    return target


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--previous', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--ko-bundle', required=True)
    p.add_argument('--version', required=True)
    p.add_argument('--ko-elf', type=Path)
    p.add_argument('--ko-elf-sha256')
    a = p.parse_args()
    stage(a.previous, a.output, a.ko_bundle, a.version, a.ko_elf, a.ko_elf_sha256)
