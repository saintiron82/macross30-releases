"""Publish the verified UI bundle's install file list; no active game writes."""
import json
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from stage_ui_images import sha_file
from install_korean_ui import BUNDLE, bundle_targets, preflight


def main():
    paths = ['staged-pack-v2/ui-images-manifest.json', 'staged-data-v2/ui-images-manifest.json']
    a, b = [json.loads((BUNDLE/p).read_text(encoding='utf-8')) for p in paths]
    if b['source_index_sha256'] != a['output_index_sha256']:
        raise ValueError('Final index does not build on DATA2 translations')
    proof = json.loads((BUNDLE/'final-six-dds-verification-v2.json').read_text(encoding='utf-8'))
    if len(proof['readbacks']) != 6 or not all(r['master_and_embedded_match'] for r in proof['readbacks']):
        raise ValueError('Six DDS roundtrip evidence required')
    files = [
        dict(filename='data2.dat', candidate='staged-pack-v2/DATA2.DAT',
             source_sha256=a['source_pack_sha256'], sha256=a['output_pack_sha256']),
        dict(filename='data.dat', candidate='staged-data-v2/DATA.DAT',
             source_sha256=b['source_pack_sha256'], sha256=b['output_pack_sha256']),
        dict(filename='pack.idx', candidate='staged-data-v2/pack.idx',
             source_sha256=a['source_index_sha256'], sha256=b['output_index_sha256']),
    ]
    for row in files:
        if sha_file(BUNDLE/row['candidate']) != row['sha256']:
            raise ValueError('Candidate changed after verification')
    result = dict(status='asset-reviewed; prepared for next launch; not installed or game-verified',
                  files=files, manifests=[dict(path=p, sha256=sha_file(BUNDLE/p)) for p in paths],
                  dds_files=6, menu_labels=20, map_labels=1, region_labels=3,
                  control_guide_types=3, elf_string_slots=7,
                  active_game_modified=False, in_game_verified=False)
    (BUNDLE/'ready.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    # Read-only validation of the exact installer mapping against BOTH active roots.
    pending = preflight(bundle_targets())
    (BUNDLE/'installer-preflight.json').write_text(json.dumps(
        dict(pending_files=len(pending), active_files_modified=False,
             tested='candidate SHA + both active source roots + independent backup paths'), indent=2), encoding='utf-8')
    print(f'Korean UI bundle ready; {len(pending)} source files validated; no installation performed.')


if __name__ == '__main__':
    main()
