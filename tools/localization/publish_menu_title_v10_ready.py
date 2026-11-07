"""Register the verified menu-title-v10 (refitted screen titles) bundle and select it.

Requires PASS readback + negative fixture. Copies the v9 Korean ELF into the bundle (unchanged),
writes ready.json / installer-preflight.json and points active-ui.json at menu-title-v10 (the
previous pointer is saved as active-ui.before-v10.json). Does not write active packs:
installation is install_korean_ui.py --bundle menu-title-v10 --apply (refuses a running game)
or the next normal launch.
"""
import json
import os
import shutil
from pathlib import Path

from stage_ui_images import sha, sha_file
from install_korean_ui import bundle_targets, preflight

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT / 'localization/image_assets/menu-title-v10'
PREV = ROOT / 'localization/image_assets/battle-hud-v9'
FINAL = {'data.dat': 'staged-primary/data.dat', 'pack.idx': 'staged-primary/pack.idx'}


def read(p):
    return json.loads(Path(p).read_text(encoding='utf-8'))


def main():
    proof = read(BUNDLE / 'final-pack-readback.json')
    neg = read(BUNDLE / 'mask-negative-fixture.json')
    if proof['status'] != 'PASS' or len(proof['title_primary']) != 1 or neg['status'] != 'PASS':
        raise ValueError('Complete readback and negative fixture required')
    prev = read(PREV / 'ready.json')
    prevsrc = {f['filename']: f['sha256'] for f in prev['files']}
    disc = ROOT / 'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'
    files = []
    for name, rel in FINAL.items():
        h = sha_file(BUNDLE / rel)
        if h != proof['staged_hashes'][name]:
            raise ValueError('Staged pack changed after readback: ' + name)
        source = prevsrc.get(name) or sha_file(disc / name)
        files.append(dict(filename=name, candidate=rel, sha256=h, source_sha256=source))
    elf_src = PREV / prev['elf']['path']
    elf_dst = BUNDLE / 'EBOOT.catalog-pool.elf'
    if not elf_dst.exists():
        shutil.copyfile(elf_src, elf_dst)
    if sha_file(elf_dst) != prev['elf']['sha256']:
        raise ValueError('ELF copy mismatch')
    elf = dict(path=elf_dst.name, sha256=prev['elf']['sha256'])
    manifests = ['final-pack-readback.json', 'source-plan.json', 'mask-negative-fixture.json',
                 'candidate/authority.json']
    version = sha(json.dumps(dict(files=files, elf=elf), sort_keys=True).encode())
    ready = dict(status='verified screen-title candidate; awaiting install', version=version, files=files, elf=elf,
                 manifests=[dict(path=p, sha256=sha_file(BUNDLE / p)) for p in manifests],
                 titles={'操作タイプ': '조작 방식', '選択': '선택', '難易度選択': '난이도 선택'},
                 preserved=['DIFFICULTY / CONTROLLER TYPE sub-titles', 'rule', 'every pixel outside TXOS 30/31/35'],
                 primary_records=1, nested_copies=0, previous_bundle=dict(name=PREV.name, version=prev['version']),
                 active_resources_modified=False, in_game_verified=False)
    (BUNDLE / 'ready.json').write_text(json.dumps(ready, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    pending = preflight(bundle_targets(BUNDLE))
    (BUNDLE / 'installer-preflight.json').write_text(json.dumps(dict(status='PASS', pending_files=len(pending),
                                                                     version=version, active_modified=False), indent=2) + '\n', encoding='utf-8')
    pointer = ROOT / 'localization/image_assets/active-ui.json'
    before = BUNDLE / 'active-ui.before-v10.json'
    if not before.exists():
        before.write_text(pointer.read_text(encoding='utf-8'), encoding='utf-8')
    temp = pointer.with_suffix('.next.json')
    temp.write_text(json.dumps(dict(bundle=BUNDLE.name), indent=2) + '\n', encoding='utf-8')
    os.replace(temp, pointer)
    print(json.dumps(dict(ready=True, pending_install_files=len(pending), version=version)))


if __name__ == '__main__':
    main()
