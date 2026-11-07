"""Register reviewed menu/location candidate; never replace live game packs."""
import json
import os
from pathlib import Path
import shutil
from stage_ui_images import sha, sha_file
from install_korean_ui import bundle_targets, preflight

ROOT=Path(__file__).resolve().parents[2]
BUNDLE=ROOT/'localization/image_assets/menu-ui-v7'


def load(name):return json.loads((BUNDLE/name).read_text(encoding='utf-8'))


def main():
    proof=load('final-pack-readback-v2.json')
    paths=load('ready-paths.json')
    epoch=load('stage-preparation-all/source-plan.json')['source_epoch']
    if (proof['status']!='PASS' or proof['authority_variants']!=86 or
            proof['missing'] or proof['unknown_variants'] or
            len(proof['primary_readbacks'])!=122 or len(proof['nested_readbacks'])!=14 or
            not proof['source_epoch_unchanged']):
        raise ValueError('Complete independent readback required')
    byname={r['filename']:r for r in epoch}
    files=[]
    for row in paths['files']:
        name=row['filename'];source=byname[name]
        if any(sha_file(Path(source[role]))!=source['sha256'] for role in ('disc','hdd')):
            raise ValueError('Active source epoch changed '+name)
        candidate=Path(row['candidate']).resolve()
        if sha_file(candidate)!=row['sha256']:raise ValueError('Reviewed candidate changed '+name)
        if BUNDLE.resolve() not in candidate.parents:
            # Pin unchanged backing packs too, so later source changes fail closed.
            dest=BUNDLE/'preserved-baseline'/name
            dest.parent.mkdir(exist_ok=True)
            if not dest.exists():shutil.copyfile(candidate,dest)
            if sha_file(dest)!=row['sha256']:raise ValueError('Preserved backing mismatch')
            candidate=dest
        files.append(dict(filename=name,candidate=candidate.relative_to(BUNDLE).as_posix(),
                          source_sha256=source['sha256'],sha256=row['sha256']))
    elf=BUNDLE/'EBOOT.catalog-pool.elf'
    if not elf.exists():shutil.copyfile(paths['ELF'],elf)
    if sha_file(elf)!=paths['ELFsha256']:raise ValueError('Prior Korean ELF changed')
    elfrow=dict(path=elf.name,sha256=paths['ELFsha256'])
    manifests=['final-pack-readback-v2.json','ready-paths.json','stage-final-result.json',
        'stage-preparation-all/source-plan.json','stage-preparation-all/normalized-authorities.json',
        'stage-preparation-all/nested-extra-map.json','staged-primary/primary-bundle-result.json',
        'nested-staged/nested-ui-manifest.json']
    version=sha(json.dumps(dict(files=files,elf=elfrow),sort_keys=True).encode())
    ready=dict(status='verified static candidate; install at next normal launch',
        version=version,files=files,elf=elfrow,
        manifests=[dict(path=p,sha256=sha_file(BUNDLE/p)) for p in manifests],
        authority_variants=86,menu_text_changes=84,location_names=44,
        primary_records=122,nested_copies=14,illustrations_modified=False,
        in_game_verified=False,active_resources_modified=False)
    (BUNDLE/'ready.json').write_text(json.dumps(ready,indent=2)+'\n',encoding='utf-8')
    pending=preflight(bundle_targets(BUNDLE))
    (BUNDLE/'installer-preflight.json').write_text(json.dumps(dict(status='PASS',
        pending_files=len(pending),version=version,active_modified=False),indent=2)+'\n',encoding='utf-8')
    pointer=ROOT/'localization/image_assets/active-ui.json';temp=pointer.with_suffix('.next.json')
    temp.write_text(json.dumps(dict(bundle=BUNDLE.name),indent=2)+'\n',encoding='utf-8')
    os.replace(temp,pointer)
    print(json.dumps(dict(ready=True,pending_install_files=len(pending),version=version)))


if __name__=='__main__':main()
