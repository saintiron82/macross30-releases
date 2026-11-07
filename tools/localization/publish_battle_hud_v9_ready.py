"""Register the verified battle-hud-v9 (wingman-order labels) bundle and select it.

Requires PASS readback + negative fixture. Copies the v8 Korean ELF into the
bundle (unchanged), writes ready.json / installer-preflight.json and points
active-ui.json at battle-hud-v9. Does not write active packs: installation is
install_korean_ui.py --bundle battle-hud-v9 --apply (refuses a running game)
or the next normal launch.
"""
import json
import os
import shutil
from pathlib import Path

from stage_ui_images import sha, sha_file
from install_korean_ui import bundle_targets, preflight

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT / 'localization/image_assets/battle-hud-v9'
V8 = ROOT / 'localization/image_assets/battle-hud-v8'
FINAL = {'data2.dat': 'staged-primary/data2.dat', 'data.dat': 'nested-staged/data.dat',
         'fileset0.dat': 'nested-staged/fileset0.dat', 'pack.idx': 'nested-staged/pack.idx'}


def read(p):
    return json.loads(Path(p).read_text(encoding='utf-8'))


def main():
    proof = read(BUNDLE / 'final-pack-readback.json')
    neg = read(BUNDLE / 'mask-negative-fixture.json')
    if proof['status'] != 'PASS' or len(proof['HUD_primary']) != 1 or len(proof['HUD_nested']) != 4 or neg['status'] != 'PASS':
        raise ValueError('Complete readback and negative fixture required')
    v8 = read(V8 / 'ready.json')
    v8src = {f['filename']: f['sha256'] for f in v8['files']}
    files = []
    for name, rel in FINAL.items():
        h = sha_file(BUNDLE / rel)
        if h != proof['staged_hashes'][name]:
            raise ValueError('Staged pack changed after readback: ' + name)
        files.append(dict(filename=name, candidate=rel, sha256=h, source_sha256=v8src[name]))
    elf_src = V8 / v8['elf']['path']
    elf_dst = BUNDLE / 'EBOOT.catalog-pool.elf'
    if not elf_dst.exists():
        shutil.copyfile(elf_src, elf_dst)
    if sha_file(elf_dst) != v8['elf']['sha256']:
        raise ValueError('ELF copy mismatch')
    elf = dict(path=elf_dst.name, sha256=v8['elf']['sha256'])
    manifests = ['final-pack-readback.json', 'source-plan.json', 'mask-negative-fixture.json',
                 'candidate/authority.json', 'nested-staged/nested-ui-manifest.json']
    version = sha(json.dumps(dict(files=files, elf=elf), sort_keys=True).encode())
    ready = dict(status='verified wingman-order HUD candidate; awaiting install', version=version, files=files, elf=elf,
                 manifests=[dict(path=p, sha256=sha_file(BUNDLE / p)) for p in manifests],
                 HUD_labels={'通常': '일반', '集合': '집합', 'スキル': '스킬', '僚機': '편대기'},
                 preserved=['SPA/SPECIAL', 'NORMAL/ASSEMBLY/SKILL sub-labels', 'digits 1/2', 'v8 특수'],
                 primary_records=1, nested_copies=4, previous_bundle=dict(name='battle-hud-v8', version=v8['version']),
                 active_resources_modified=False, in_game_verified=False)
    (BUNDLE / 'ready.json').write_text(json.dumps(ready, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    pending = preflight(bundle_targets(BUNDLE))
    (BUNDLE / 'installer-preflight.json').write_text(json.dumps(dict(status='PASS', pending_files=len(pending),
                                                                     version=version, active_modified=False), indent=2) + '\n', encoding='utf-8')
    pointer = ROOT / 'localization/image_assets/active-ui.json'
    temp = pointer.with_suffix('.next.json')
    temp.write_text(json.dumps(dict(bundle=BUNDLE.name), indent=2) + '\n', encoding='utf-8')
    os.replace(temp, pointer)
    print(json.dumps(dict(ready=True, pending_install_files=len(pending), version=version)))


if __name__ == '__main__':
    main()
