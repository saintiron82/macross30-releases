"""Publish the full quest-title/menu/combat-family localization bundle."""
import json
import os
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from stage_ui_images import sha, sha_file
from install_korean_ui import bundle_targets, preflight

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT/'localization/image_assets/caution-ui-v4'


def load(path):
    return json.loads((BUNDLE/path).read_text(encoding='utf-8'))


def main():
    proof = load('final-verification.json')
    elf_doc = load('EBOOT.caution.manifest.json')
    elf_proof = load('elf-validation.json')
    titles = load('quest-title-family-final-validation.json')
    semantic = load('quest-title-independent-semantic-review.json')
    epoch = load('source-state.json')
    if (proof['status'] != 'PASS' or proof['primary_rows_verified'] != 62 or
            proof['nested_entries_verified'] != 43 or
            not all(r['match'] for r in proof['primary_readbacks'])):
        raise ValueError('Complete primary/nested roundtrip evidence required')
    if not proof['all_source_prefix_bytes_identical_except_approved_PIDx_fields']:
        raise ValueError('Original pack prefix protection failed')
    if (not titles['passed'] or not titles['new37_ready'] or semantic['status'] != 'PASS' or
            semantic['source_plan_mismatches'] or semantic['candidate_text_mismatches']):
        raise ValueError('Quest-title artwork semantic review failed')
    if (elf_proof['status'] != 'PASS' or elf_proof['total_slots'] != 461 or elf_proof['font_missing'] or
            not elf_proof['menu_item_construction_and_visibility_code_byte_identical'] or
            not elf_proof['menu_label_TOC_pointer_table_byte_identical']):
        raise ValueError('Menu/ELF preservation or coverage failed')
    if any(elf_doc[k] for k in ('outside_slot_changed_bytes', 'executable_sections_changed_bytes',
                               'elf_headers_changed_bytes', 'ui_font_missing', 'fixed_name_font_missing')):
        raise ValueError('Localized ELF changed outside approved text')
    source = {r['filename']: r['sha256'] for r in epoch['files'] if r['role'] == 'disc'}
    # Nested stage starts from primary-stage copies. Install must use the ACTUAL
    # active basic-ui-v3 epoch, not the intermediate candidate file hashes.
    files = [dict(filename=name, candidate='nested-staged/'+name,
                  source_sha256=source[name], sha256=proof['final_output_hashes'][name])
             for name in ('data.dat', 'pack.idx')]
    elf = dict(path='EBOOT.caution.elf', sha256=elf_doc['output_sha256'])
    for row in files + [dict(candidate=elf['path'], sha256=elf['sha256'])]:
        if sha_file(BUNDLE/row['candidate']) != row['sha256']:
            raise ValueError('Reviewed candidate changed')
    version = sha(json.dumps(dict(files=files, elf=elf), sort_keys=True).encode('utf-8'))
    manifests = ['final-verification.json', 'EBOOT.caution.manifest.json', 'elf-validation.json',
                 'quest-title-family-final-validation.json', 'quest-title-independent-semantic-review.json',
                 'nested-staged/nested-ui-manifest.json']
    ready = dict(status='verified; waiting for installation before next launch',
                 version=version, files=files, elf=elf,
                 manifests=[dict(path=p, sha256=sha_file(BUNDLE/p)) for p in manifests],
                 additional_menu_strings=245, all_ELF_text_slots=461,
                 quest_title_family=41, new_Japanese_titles=37,
                 prior_Korean_titles_preserved=2, original_English_titles_preserved=2,
                 combat_glyphs=16, other_combat_alerts=22,
                 menu_construction_modified=False, in_game_verified=False)
    (BUNDLE/'ready.json').write_text(json.dumps(ready, indent=2), encoding='utf-8')
    pending = preflight(bundle_targets(BUNDLE))
    (BUNDLE/'installer-preflight.json').write_text(json.dumps(dict(pending_files=len(pending),
        active_modified=False, version=version), indent=2), encoding='utf-8')
    pointer = ROOT/'localization/image_assets/active-ui.json'
    temp = pointer.with_suffix('.next.json')
    temp.write_text(json.dumps(dict(bundle=BUNDLE.name), indent=2), encoding='utf-8')
    os.replace(temp, pointer)
    print(json.dumps(dict(ready=True, menu_strings=245, quest_titles=37,
                         pending_install_files=len(pending), version=version)))


if __name__ == '__main__':
    main()
