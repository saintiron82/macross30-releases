"""Publish verified basic UI, nested resource and punctuation-font fixes."""
import json
import os
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from stage_ui_images import sha, sha_file
from install_korean_ui import bundle_targets, preflight

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT/'localization/image_assets/basic-ui-v3'


def load(path):
    return json.loads((BUNDLE/path).read_text(encoding='utf-8'))


def main():
    nested = load('nested-staged/nested-ui-manifest.json')
    proof = load('nested-final-verification.json')
    independent = load('independent-nested-verification.json')
    elf_doc = load('EBOOT.basic.manifest.json')
    coverage = load('font-coverage-validation.json')
    all_text = load('font-alias/structured-unicode-coverage.json')
    if (proof['status'] != 'PASS' or proof['primary_rows'] != 23 or proof['nested_entries'] != 41 or
            proof['unmatched_exact_SHA'] or not all(r['match'] for r in proof['nested_readbacks'])):
        raise ValueError('Complete nested resource evidence required')
    if (independent['independent_status'] != 'PASS' or independent['nested_readback_count'] != 41 or
            independent['primary_rows_preserved'] != 23 or
            independent['unselected_FSTS_encoded_and_decoded_payloads_identical'] != 1637 or
            not independent['all_original_pack_prefix_bytes_identical_except_reviewed_secondary_PIDX_fields']):
        raise ValueError('Independent archive preservation verification failed')
    if any(elf_doc[k] for k in ('outside_slot_changed_bytes', 'executable_sections_changed_bytes',
                               'elf_headers_changed_bytes', 'fixed_name_font_missing', 'ui_font_missing')):
        raise ValueError('ELF protection or font coverage failed')
    if len(elf_doc['strings']) != 216 or elf_doc['basic_ui_strings'] != 51 or not coverage['passed']:
        raise ValueError('Expected complete basic-message update')
    if coverage['unicode_literal_coverage_missing'] or all_text['missing_unique_characters']:
        raise ValueError('Unsupported Unicode remains')
    names = {'data.dat', 'fileset0.dat', 'pack.idx'}
    if set(nested['output_hashes']) != names or nested['output_hashes'] != independent['output_hashes']:
        raise ValueError('Unexpected or unverified output packs')
    files = [dict(filename=name, candidate='nested-staged/'+name,
                  source_sha256=nested['source_state'][name]['sha256'],
                  sha256=nested['output_hashes'][name]) for name in sorted(names)]
    elf = dict(path='EBOOT.basic.elf', sha256=elf_doc['output_sha256'])
    for row in files + [dict(candidate=elf['path'], sha256=elf['sha256'])]:
        if sha_file(BUNDLE/row['candidate']) != row['sha256']:
            raise ValueError('Reviewed candidate changed')
    version = sha(json.dumps(dict(files=files, elf=elf), sort_keys=True).encode('utf-8'))
    manifests = ['nested-staged/nested-ui-manifest.json', 'nested-final-verification.json',
                 'independent-nested-verification.json', 'EBOOT.basic.manifest.json',
                 'font-coverage-validation.json', 'font-alias/manifest.json',
                 'font-alias/structured-unicode-coverage.json']
    ready = dict(status='verified; waiting for installation before next launch',
                 version=version, files=files, elf=elf,
                 manifests=[dict(path=p, sha256=sha_file(BUNDLE/p)) for p in manifests],
                 new_basic_messages=51, nested_DDS_copies_fixed=40, nested_archives=8,
                 original_primary_rows_preserved=23, font_glyphs=8162,
                 all_translated_text_missing_codepoints=0, in_game_verified=False)
    (BUNDLE/'ready.json').write_text(json.dumps(ready, indent=2), encoding='utf-8')
    pending = preflight(bundle_targets(BUNDLE))
    (BUNDLE/'installer-preflight.json').write_text(json.dumps(dict(pending_files=len(pending),
        active_modified=False, version=version), indent=2), encoding='utf-8')
    pointer = ROOT/'localization/image_assets/active-ui.json'
    temporary = pointer.with_suffix('.next.json')
    temporary.write_text(json.dumps(dict(bundle=BUNDLE.name), indent=2), encoding='utf-8')
    os.replace(temporary, pointer)
    print(json.dumps(dict(ready=True, basic_messages=51, nested_DDS=40,
                         pending_install_files=len(pending), version=version)))


if __name__ == '__main__':
    main()
