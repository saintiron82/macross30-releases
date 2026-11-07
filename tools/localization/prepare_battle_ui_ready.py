"""Verify all new/prior DDS through the final index and publish next-launch UI."""
import hashlib
import json
import os
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from stage_ui_images import read_pidx, sha_file, sha
from install_korean_ui import bundle_targets, preflight

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT/'localization/image_assets/battle-ui-v1'


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def main():
    paths = ['staged-data2/ui-images-manifest.json', 'staged-data/ui-images-manifest.json']
    first, second = [load(BUNDLE/p) for p in paths]
    if second['source_index_sha256'] != first['output_index_sha256']:
        raise ValueError('Final index does not incorporate DATA2 stage')
    final_index = BUNDLE/'staged-data/pack.idx'
    packs = {first['pack_filename'].lower(): BUNDLE/'staged-data2'/first['pack_filename'],
             second['pack_filename'].lower(): BUNDLE/'staged-data'/second['pack_filename']}
    entries = []
    identities = set()
    prior = ROOT/'localization/image_assets/ui-ko-v1'
    documents = [(first, False), (second, False),
                 (load(prior/'staged-pack-v2/ui-images-manifest.json'), True),
                 (load(prior/'staged-data-v2/ui-images-manifest.json'), True)]
    for doc, previous in documents:
        pack_name = doc.get('pack_filename', Path(doc['data2']).name).lower()
        for row in doc['changes']:
            name = row['name']
            spec = dict(pack=pack_name, offset=row['offset'], size=row['size'],
                        compressed_size=row['compressed_size'])
            identity = (name, pack_name, row['offset'], row['size'])
            if identity in identities:
                raise ValueError('Duplicate staged identity; each alias must have its own offset')
            identities.add(identity)
            entries.append(dict(row, previous=previous, spec=spec))
    if len(entries) != 21 or sum(not r['previous'] for r in entries) != 15:
        raise ValueError('Expected fifteen new primary rows including copies, plus six prior DDS')
    if len({r['name'] for r in entries if not r['previous']}) != 9:
        raise ValueError('Expected nine newly translated asset names')
    readbacks = []
    for pack_name, pack in packs.items():
        with pack.open('rb') as stream:
            for row in (r for r in entries if r['spec']['pack'] == pack_name):
                name, spec = row['name'], row['spec']
                _, master = read_pidx(final_index, [name], selection={name: spec})
                _, embedded = read_pidx(pack, [name], selection={name: spec})
                for table in (master, embedded):
                    if any(table[name][k] != spec[k] for k in ('offset', 'size', 'compressed_size')):
                        raise ValueError('Final mirrored index mismatch')
                if spec['compressed_size'] != 0:
                    raise ValueError('Expected staged uncompressed DDS')
                stream.seek(spec['offset'])
                actual = sha(stream.read(spec['size']))
                if actual != row['sha256']:
                    raise ValueError('Final DDS payload mismatch: ' + name)
                readbacks.append(dict(name=name, pack=pack_name, sha256=actual,
                                      offset=spec['offset'], previous_preserved=row['previous'],
                                      master_and_embedded_match=True))
    proof_path = BUNDLE/'final-twenty-one-records-verification.json'
    proof_path.write_text(json.dumps(dict(readbacks=readbacks, verified=21,
                                         active_files_modified=False), indent=2), encoding='utf-8')
    elf_manifest = load(BUNDLE/'EBOOT.battle.manifest.json')
    if any(elf_manifest[k] for k in ('outside_slot_changed_bytes', 'executable_sections_changed_bytes', 'elf_headers_changed_bytes')):
        raise ValueError('Localized ELF changed outside reviewed text slots')
    files = [dict(filename=first['pack_filename'].lower(),
                  candidate='staged-data2/'+first['pack_filename'],
                  source_sha256=first['source_pack_sha256'], sha256=first['output_pack_sha256']),
             dict(filename=second['pack_filename'].lower(),
                  candidate='staged-data/'+second['pack_filename'],
                  source_sha256=second['source_pack_sha256'], sha256=second['output_pack_sha256']),
             dict(filename='pack.idx', candidate='staged-data/pack.idx',
                  source_sha256=first['source_index_sha256'], sha256=second['output_index_sha256'])]
    elf = dict(path='EBOOT.battle.elf', sha256=elf_manifest['output_sha256'])
    for row in files + [dict(candidate=elf['path'], sha256=elf['sha256'])]:
        if sha_file(BUNDLE/row['candidate']) != row['sha256']:
            raise ValueError('Reviewed candidate checksum changed')
    version = sha(json.dumps(dict(files=files, elf=elf), sort_keys=True).encode('utf-8'))
    manifests = paths + ['EBOOT.battle.manifest.json', proof_path.name]
    ready = dict(status='verified; waiting for installation before next launch', version=version,
                 files=files, elf=elf, manifests=[dict(path=p, sha256=sha_file(BUNDLE/p)) for p in manifests],
                 new_dds_assets=9, new_primary_rows_including_copies=15,
                 prior_dds_preserved=6, combat_labels=26, minimap_labels=42,
                 fixed_name_locations=139, fixed_name_terms=72, in_game_verified=False)
    (BUNDLE/'ready.json').write_text(json.dumps(ready, indent=2), encoding='utf-8')
    pending = preflight(bundle_targets(BUNDLE))
    (BUNDLE/'installer-preflight.json').write_text(json.dumps(dict(pending_files=len(pending),
         active_files_modified=False, version=version), indent=2), encoding='utf-8')
    pointer = ROOT/'localization/image_assets/active-ui.json'
    temporary = pointer.with_suffix('.next.json')
    temporary.write_text(json.dumps(dict(bundle=BUNDLE.name), indent=2), encoding='utf-8')
    os.replace(temporary, pointer)
    print(json.dumps(dict(ready=True, verified_primary_rows=21, pending_install_files=len(pending), version=version)))


if __name__ == '__main__':
    main()
