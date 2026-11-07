"""Prepare v12 installer metadata; --select changes launch pointer after game exit.

Never installs packs or controls processes. Default leaves active-ui unchanged.
"""
import argparse
import json
import os
from pathlib import Path

from install_korean_ui import bundle_targets, preflight, game_running
from stage_ui_images import sha, sha_file
from stage_title_menu_v12 import ROOT, B, PREV, load, dump, check_epoch, PACKS


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--select',action='store_true')
    args=parser.parse_args()
    proof=load(B/'final-pack-readback.json');paths=load(B/'ready-paths.json')
    if proof['status']!='PASS' or len(proof['nested_title_readbacks'])!=1 or proof['unknown_variants']:
        raise ValueError('Require exact single-title verification')
    if not proof['all_old_pack_bytes_preserved_except_secondary_mirror_fields'] or not proof['archive_prefix_preserved_except_selected_entry_fields']:
        raise ValueError('Preservation verification missing')
    if load(B/'mask-negative-fixture.json')['status']!='PASS':
        raise ValueError('Negative mask fixtures missing')
    check_epoch(paths['source_epoch'])
    if set(paths['files'])!=set(PACKS):
        raise ValueError('Incomplete bundle-local backing files')
    files=[]
    for name,value in paths['files'].items():
        p=Path(value).resolve()
        if B.resolve() not in p.parents or sha_file(p)!=proof['staged_hashes'][name]:
            raise ValueError('Staged backing file changed')
        files.append(dict(filename=name,candidate=p.relative_to(B.resolve()).as_posix(),
                          sha256=proof['staged_hashes'][name],source_sha256=paths['source_epoch'][name]))
    elf=Path(paths['elf']).resolve()
    if B.resolve() not in elf.parents or sha_file(elf)!=proof['ELFsha256']:
        raise ValueError('ELF changed')
    elfdoc=dict(path=elf.relative_to(B.resolve()).as_posix(),sha256=proof['ELFsha256'])
    manifests=['final-pack-readback.json','source-plan.json','candidate/authority.json','mask-negative-fixture.json','ink-comparison.json']
    doc=dict(status='verified title-menu refit candidate; awaiting guarded installation',
             version=sha(json.dumps(dict(files=files,elf=elfdoc),sort_keys=True).encode()),files=files,elf=elfdoc,
             manifests=[dict(path=n,sha256=sha_file(B/n)) for n in manifests],
             previous_bundle=dict(name='title-menu-v11',version=load(PREV/'ready.json')['version']),
             active_resources_modified=False,in_game_verified=False,
             scope='Only existing Korean title_text glyphs refitted to alpha>96 core height <=22px; subtitle/brand/ARK preserved')
    dump(B/'ready.json',doc)
    pending=preflight(bundle_targets(B))
    dump(B/'installer-preflight.json',dict(status='PASS',pending_files=len(pending),version=doc['version']))
    if args.select:
        if game_running():
            raise ValueError('Game must exit before selecting this reviewed refit')
        pointer=ROOT/'localization/image_assets/active-ui.json'
        if load(pointer)['bundle'] not in ('title-menu-v11','title-menu-v12'):
            raise ValueError('Launch pointer changed')
        before=B/'active-ui.before-v12.json'
        if not before.exists():
            before.write_bytes(pointer.read_bytes())
        tmp=pointer.with_suffix('.title-menu-v12.tmp')
        dump(tmp,dict(bundle=B.name));os.replace(tmp,pointer)
    print(json.dumps(dict(ready=True,selected=args.select,pending_files=len(pending),version=doc['version'])))


if __name__=='__main__':
    main()
