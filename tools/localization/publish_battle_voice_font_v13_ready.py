"""Prepare v13 installer metadata and read-only preflight ONLY.

This tool has no selection/install/process action. Root must separately review
and select a future bundle; an existing game is always left untouched.
"""
import json
from pathlib import Path

from install_korean_ui import bundle_targets, preflight
from stage_battle_voice_font_v13 import B, PREV, PACKS, load, dump, check_epoch
from stage_ui_images import sha, sha_file


def main():
    proof=load(B/'final-pack-readback.json');paths=load(B/'ready-paths.json')
    if proof['status']!='PASS' or len(proof['resident_font_readbacks'])!=2 or proof['unknown_variants']:
        raise ValueError('Require exact resident-font readback proof')
    if not proof['all_old_pack_bytes_preserved_except_secondary_mirror_fields'] or not proof['resident_prefix_preserved_except_two_font_entry_fields']:
        raise ValueError('Source preservation proof missing')
    if proof['font_coverage']['status']!='PASS' or proof['font_coverage']['proposed_entries']!=413 or proof['font_coverage']['maximum_UTF8_bytes']>254:
        raise ValueError('KO font coverage/byte-limit proof missing')
    check_epoch(paths['source_epoch'])
    if set(paths['files'])!=set(PACKS):raise ValueError('Incomplete bundle-local backing files')
    files=[]
    for name,value in paths['files'].items():
        path=Path(value).resolve()
        if B.resolve() not in path.parents or sha_file(path)!=proof['staged_hashes'][name]:raise ValueError('Staged file changed')
        files.append(dict(filename=name,candidate=path.relative_to(B.resolve()).as_posix(),sha256=proof['staged_hashes'][name],source_sha256=paths['source_epoch'][name]))
    elf=Path(paths['elf']).resolve()
    if B.resolve() not in elf.parents or sha_file(elf)!=proof['ELFsha256']:raise ValueError('ELF changed')
    elfdoc=dict(path=elf.relative_to(B.resolve()).as_posix(),sha256=proof['ELFsha256'])
    manifests=['final-pack-readback.json','source-plan.json','candidate/authority.json','candidate-verification.json','font-placement-audit.json','source-provenance.json','candidate/manifest.json']
    version=sha(json.dumps(dict(files=files,elf=elfdoc),sort_keys=True).encode())
    doc=dict(status='verified 8177-glyph resident font candidate; NOT selected or installed',version=version,files=files,elf=elfdoc,
             manifests=[dict(path=p,sha256=sha_file(B/p)) for p in manifests],
             previous_bundle=dict(name='title-menu-v12',version=load(PREV/'ready.json')['version']),
             active_resources_modified=False,in_game_verified=False,font_coverage=proof['font_coverage'],
             font_added=['뚱 U+B6B1','팍 U+D30D'],selected=False,installed=False)
    dump(B/'ready.json',doc)
    pending=preflight(bundle_targets(B))
    dump(B/'installer-preflight.json',dict(status='PASS',pending_files=len(pending),version=version,selected=False,installed=False,active_modified=False))
    print(json.dumps(dict(ready=True,pending_files=len(pending),version=version,selected=False,installed=False)))


if __name__=='__main__':main()
