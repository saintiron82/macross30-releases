"""Publish a fully read-back v6 composition; never install or control the game."""
import json
import os
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from install_korean_ui import bundle_targets, preflight
from stage_ui_images import sha, sha_file

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT/'localization/image_assets/data-ui-v6'
PRIOR = ROOT/'localization/image_assets/gop-ui-v5'


def read(p):
    return json.loads(p.read_text(encoding='utf-8'))


def main():
    proof = read(BUNDLE/'final-pack-readback.json')
    admission = read(BUNDLE/'pool-static-admission.json')
    font = read(BUNDLE/'font-approved439-unicode-format-proof.json')
    if (proof['status'] != 'PASS' or proof['prior_primary_DDS_count'] != 62 or
            proof['primary_GOP_KO_strings'] != 13536 or
            proof['prior_nested_battletext_count'] != 40 or
            proof['new_primary_DDS_count'] != 7 or proof['font_glyphs'] != 8175):
        raise ValueError('Final pack composition evidence incomplete')
    if (admission['status'] != 'PASS' or admission['translated_literals'] != 438 or
            not admission['static_only'] or admission['in_game_verified']):
        raise ValueError('Reviewed static pool admission required')
    if font['status'] != 'PASS' or font['font_missing'] or font['newline_mismatches']:
        raise ValueError('Font/format evidence incomplete')
    elf = BUNDLE/'EBOOT.catalog-pool.elf'
    if sha_file(elf) != admission['ELFsha256']:
        raise ValueError('Reviewed ELF changed')
    prior = read(PRIOR/'ready.json')
    epoch = read(PRIOR/'source-state.json')
    allowed = {}
    for r in epoch['files']:
        allowed.setdefault(r['filename'].lower(), set()).add(r['sha256'])
    for r in prior['files']:
        allowed.setdefault(r['filename'].lower(), set()).update((r['sha256'], r['source_sha256']))
    folders = [BUNDLE/'nested-staged', BUNDLE/'staged-primary', PRIOR/'nested-staged', PRIOR/'staged-primary']
    names = {r['filename'].lower() for r in prior['files']}
    for folder in folders[:2]:
        names.update(p.name.lower() for p in folder.glob('*.dat'))
    names.add('pack.idx')
    disc = ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'
    hdd = ROOT/'native/state-ko-full/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'
    files = []
    for name in sorted(names):
        candidate = next((p/name for p in folders if (p/name).is_file()), None)
        if candidate is None:
            raise ValueError('Missing candidate '+name)
        expected = proof['output_hashes'][name]
        if sha_file(candidate) != expected:
            raise ValueError('Read-back backing file changed '+name)
        current = sha_file(disc/name)
        if current not in allowed.get(name, set()) or sha_file(hdd/name) != current:
            raise ValueError('Unknown or mismatched active source epoch '+name)
        files.append(dict(filename=name, candidate=candidate.relative_to(BUNDLE).as_posix()
                          if BUNDLE in candidate.parents else os.path.relpath(candidate, BUNDLE),
                          source_sha256=current, sha256=expected))
    # Installer requires every candidate to live inside this bundle. Retain v5's
    # immutable FILESET2 as a local independent copy, not a cross-bundle escape.
    import shutil
    for r in files:
        candidate=(BUNDLE/r['candidate']).resolve()
        if BUNDLE.resolve() not in candidate.parents:
            dest=BUNDLE/'preserved-v5'/r['filename']
            dest.parent.mkdir(exist_ok=True)
            if not dest.exists():
                shutil.copyfile(candidate,dest)
            if sha_file(dest)!=r['sha256']:
                raise ValueError('Preserved v5 candidate mismatch')
            r['candidate']=dest.relative_to(BUNDLE).as_posix()
    elfrow=dict(path=elf.name,sha256=sha_file(elf))
    version=sha(json.dumps(dict(files=files,elf=elfrow),sort_keys=True).encode())
    paths=['final-pack-readback.json','pool-static-admission.json',
           'EBOOT.catalog-pool.validation.json','font-approved439-unicode-format-proof.json',
           'staged-primary/ui-images-manifest.json','nested-staged/nested-ui-manifest.json',
           'image-composite-validation.json','font-primary-and-nested-live-audit.json']
    manifests=[dict(path=p,sha256=sha_file(BUNDLE/p)) for p in paths]
    ready=dict(status='verified offline; awaiting install and real game verification',
               version=version,files=files,elf=elfrow,manifests=manifests,
               translated_catalog_literals=438,new_primary_DDS=7,font_glyphs=8175,
               in_game_verified=False,active_resources_modified=False)
    (BUNDLE/'ready.json').write_text(json.dumps(ready,indent=2)+'\n',encoding='utf-8')
    pending=preflight(bundle_targets(BUNDLE))
    (BUNDLE/'installer-preflight.json').write_text(json.dumps(dict(status='PASS',
        pending_files=len(pending),version=version,active_modified=False),indent=2)+'\n',encoding='utf-8')
    pointer=ROOT/'localization/image_assets/active-ui.json'
    temp=pointer.with_suffix('.next.json')
    temp.write_text(json.dumps(dict(bundle=BUNDLE.name),indent=2)+'\n',encoding='utf-8')
    os.replace(temp,pointer)
    print(json.dumps(dict(ready=True,pending_install_files=len(pending),version=version)))


if __name__=='__main__':
    main()
