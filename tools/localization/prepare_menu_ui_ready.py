"""Publish the verified missing-menu/title patch for the normal launcher."""
import json
import os
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from stage_ui_images import sha, sha_file
from install_korean_ui import bundle_targets, preflight

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT/'localization/image_assets/menu-ui-v2'


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def main():
    staged = load(BUNDLE/'staged-data/ui-images-manifest.json')
    proof = load(BUNDLE/'final-23-row-verification.json')
    elf_doc = load(BUNDLE/'EBOOT.menu.manifest.json')
    if (proof['primary_rows_verified'] != 23 or len(proof['readbacks']) != 23 or
            not all(row['readback_match'] for row in proof['readbacks'])):
        raise ValueError('Complete 23-row readback evidence required')
    if proof['previous_battle_and_UI1_rows_preserved'] != 21:
        raise ValueError('Prior translations not fully preserved')
    if any(elf_doc[k] for k in ('outside_slot_changed_bytes', 'executable_sections_changed_bytes',
                               'elf_headers_changed_bytes', 'fixed_name_font_missing')):
        raise ValueError('ELF protection or font validation failed')
    if len(elf_doc['strings']) != 165 or elf_doc['fixed_names'] != 139:
        raise ValueError('Prior names and new menu strings required')
    files = [dict(filename=staged['pack_filename'].lower(),
                  candidate='staged-data/'+staged['pack_filename'],
                  source_sha256=staged['source_pack_sha256'], sha256=staged['output_pack_sha256']),
             dict(filename='pack.idx', candidate='staged-data/pack.idx',
                  source_sha256=staged['source_index_sha256'], sha256=staged['output_index_sha256'])]
    elf = dict(path='EBOOT.menu.elf', sha256=elf_doc['output_sha256'])
    for row in files + [dict(candidate=elf['path'], sha256=elf['sha256'])]:
        if sha_file(BUNDLE/row['candidate']) != row['sha256']:
            raise ValueError('Reviewed output changed')
    version = sha(json.dumps(dict(files=files, elf=elf), sort_keys=True).encode('utf-8'))
    manifests = ['staged-data/ui-images-manifest.json', 'final-23-row-verification.json',
                 'EBOOT.menu.manifest.json', 'elf-validation.json']
    ready = dict(status='verified; waiting for installation before next launch',
                 version=version, files=files, elf=elf,
                 manifests=[dict(path=p, sha256=sha_file(BUNDLE/p)) for p in manifests],
                 new_dds=2, new_menu_slots=19, previous_name_slots=139,
                 prior_primary_rows_preserved=21, in_game_verified=False)
    (BUNDLE/'ready.json').write_text(json.dumps(ready, indent=2), encoding='utf-8')
    pending = preflight(bundle_targets(BUNDLE))
    (BUNDLE/'installer-preflight.json').write_text(json.dumps(dict(pending_files=len(pending),
        active_modified=False, version=version), indent=2), encoding='utf-8')
    pointer = ROOT/'localization/image_assets/active-ui.json'
    temporary = pointer.with_suffix('.next.json')
    temporary.write_text(json.dumps(dict(bundle=BUNDLE.name), indent=2), encoding='utf-8')
    os.replace(temporary, pointer)
    print(json.dumps(dict(ready=True, verified_primary_rows=23, pending_install_files=len(pending), version=version)))


if __name__ == '__main__':
    main()
