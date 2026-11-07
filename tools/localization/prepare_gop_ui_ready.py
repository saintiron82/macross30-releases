"""Publish route-consistent primary and nested Korean GOP payloads."""
import json
import os
from pathlib import Path
import shutil
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from stage_ui_images import sha, sha_file
from install_korean_ui import bundle_targets, preflight

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT/'localization/image_assets/gop-ui-v5'


def load(path):
    return json.loads((BUNDLE/path).read_text(encoding='utf-8'))


def main():
    primary = load('primary-final-verification.json')
    final = load('nested-final-verification.json')
    nested = load('nested-staged/nested-ui-manifest.json')
    epoch = load('source-state.json')
    if (not primary['passed'] or primary['actual_KO_total'] != 13536 or
            not primary['original_data_prefix_non_target_payload_bytes_unchanged']):
        raise ValueError('Primary GOP verification incomplete')
    if (final['status'] != 'PASS' or final['primary_translations'] != 13536 or
            final['nested_battletext_count'] != 40 or final['nested_KO_translations_per_copy'] != 5901 or
            final['prior_primary_DDS_count'] != 62 or final['unmatched'] or
            not final['nonselected_encoded_payloads_preserved']):
        raise ValueError('Nested GOP or prior-image verification incomplete')
    source = {r['filename']: r['sha256'] for r in epoch['files'] if r['role'] == 'disc'}
    source['fileset2.dat'] = nested['source_state']['fileset2.dat']['sha256']
    candidates = {'data.dat':'staged-primary/data.dat',
                  'fileset2.dat':'nested-staged/fileset2.dat', 'pack.idx':'nested-staged/pack.idx'}
    files = [dict(filename=name, candidate=path, source_sha256=source[name],
                  sha256=final['output_hashes'][name]) for name,path in candidates.items()]
    parent = ROOT/'localization/image_assets/caution-ui-v4'
    parent_ready = json.loads((parent/'ready.json').read_text(encoding='utf-8'))
    prior_elf = parent/parent_ready['elf']['path']
    elf_path = BUNDLE/'EBOOT.gop.elf'
    if not elf_path.exists():
        shutil.copyfile(prior_elf, elf_path)
    elf = dict(path=elf_path.name, sha256=parent_ready['elf']['sha256'])
    for row in files + [dict(candidate=elf['path'],sha256=elf['sha256'])]:
        if sha_file(BUNDLE/row['candidate']) != row['sha256']:
            raise ValueError('Reviewed candidate changed')
    version = sha(json.dumps(dict(files=files,elf=elf),sort_keys=True).encode('utf-8'))
    manifests = ['primary-final-verification.json','nested-final-verification.json',
                 'staged-primary/gop-primary-mirrors-manifest.json',
                 'nested-staged/nested-ui-manifest.json']
    ready = dict(status='verified; waiting for installation before next launch',
                 version=version,files=files,elf=elf,
                 manifests=[dict(path=p,sha256=sha_file(BUNDLE/p)) for p in manifests],
                 primary_GOP_families=12,translated_entries=13536,
                 additional_battletext_copies=40,battletext_entries_per_copy=5901,
                 prior_DDS_preserved=62,ELF_unchanged=True,in_game_verified=False)
    (BUNDLE/'ready.json').write_text(json.dumps(ready,indent=2),encoding='utf-8')
    pending=preflight(bundle_targets(BUNDLE))
    (BUNDLE/'installer-preflight.json').write_text(json.dumps(dict(pending_files=len(pending),
        active_modified=False,version=version),indent=2),encoding='utf-8')
    pointer=ROOT/'localization/image_assets/active-ui.json';temp=pointer.with_suffix('.next.json')
    temp.write_text(json.dumps(dict(bundle=BUNDLE.name),indent=2),encoding='utf-8');os.replace(temp,pointer)
    print(json.dumps(dict(ready=True,primary_translations=13536,nested_copies=40,
                         pending_install_files=len(pending),version=version)))


if __name__=='__main__':
    main()
