"""Register the verified combat-only special-label fix; no live pack writes."""
import json
import os
from pathlib import Path
from stage_ui_images import sha, sha_file
from install_korean_ui import bundle_targets, preflight

ROOT=Path(__file__).resolve().parents[2]
BUNDLE=ROOT/'localization/image_assets/battle-hud-v8'


def read(name):return json.loads((BUNDLE/name).read_text(encoding='utf-8'))


def main():
    proof=read('final-pack-readback.json');paths=read('ready-paths.json')
    if proof['status']!='PASS' or len(proof['HUD_primary'])!=1 or len(proof['HUD_nested'])!=4:
        raise ValueError('Complete combat HUD readback required')
    files=paths['files']
    for row in files:
        candidate=(BUNDLE/row['candidate']).resolve()
        if BUNDLE.resolve() not in candidate.parents or sha_file(candidate)!=row['sha256']:
            raise ValueError('Reviewed backing candidate changed')
    elf=dict(path=paths['ELF'],sha256=paths['ELFsha256'])
    if sha_file(BUNDLE/elf['path'])!=elf['sha256']:raise ValueError('Prior Korean ELF changed')
    manifests=['final-pack-readback.json','source-plan.json','ready-paths.json',
        'mask-negative-fixture.json','candidate-v2/authority.json',
        'nested-staged/nested-ui-manifest.json']
    version=sha(json.dumps(dict(files=files,elf=elf),sort_keys=True).encode())
    ready=dict(status='verified combat HUD candidate; awaiting next normal launch',
        version=version,files=files,elf=elf,
        manifests=[dict(path=p,sha256=sha_file(BUNDLE/p)) for p in manifests],
        HUD_label='특수',label_occurrences=3,primary_records=1,nested_copies=4,
        previous_menu_region_bundle=paths['source_version'],
        active_resources_modified=False,in_game_verified=False)
    (BUNDLE/'ready.json').write_text(json.dumps(ready,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    pending=preflight(bundle_targets(BUNDLE))
    (BUNDLE/'installer-preflight.json').write_text(json.dumps(dict(status='PASS',
        pending_files=len(pending),version=version,active_modified=False),indent=2)+'\n',encoding='utf-8')
    pointer=ROOT/'localization/image_assets/active-ui.json';temp=pointer.with_suffix('.next.json')
    temp.write_text(json.dumps(dict(bundle=BUNDLE.name),indent=2)+'\n',encoding='utf-8')
    os.replace(temp,pointer)
    print(json.dumps(dict(ready=True,pending_install_files=len(pending),version=version)))


if __name__=='__main__':main()
