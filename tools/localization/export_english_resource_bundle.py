"""Archive the published English resources and review catalogs, with exact hashes."""
import json
from pathlib import Path
import sys
import zipfile
import argparse
import re
from english_pack import ROOT,write
sys.path.insert(0,str(ROOT/'tools/play'))
from language_packs import file_digest

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision',default='',help='Optional immutable local export suffix')
    args=parser.parse_args()
    if args.revision and not re.fullmatch('[a-z0-9-]+',args.revision):raise ValueError('Invalid revision')
    en=ROOT/'localization/en-US';ready=json.loads((en/'pack-ready.json').read_text(encoding='utf8'))
    if ready['status']!='full_draft' or ready['untranslated']!=0:raise ValueError('Publish the full draft first')
    folder=ROOT/'build/exports';folder.mkdir(parents=True,exist_ok=True)
    version=Path(ready['vfs']).name.removeprefix('english-preview-images-')
    suffix='-'+args.revision if args.revision else ''
    target=folder/f'Macross30-en-US-{version}{suffix}-draft.zip'
    if target.exists():raise FileExistsError(target)
    registry=json.loads((ROOT/'tools/localization/catalogs/image_registry.json').read_text(encoding='utf8'))
    files={};image_rows=[]
    def add(name,path):
        path=Path(path)
        if name in files:raise ValueError('Duplicate archive member')
        files[name]=dict(path=path,sha256=file_digest(path),bytes=path.stat().st_size)
    add('elf/EBOOT.english.elf',ROOT/ready['elf'])
    for path in (en/'build/gop').glob('*.gop'):add('gop/'+path.name,path)
    for asset in registry['assets']:
        if asset['category']!='ui_image':continue
        name='images/'+asset['id']+'.dds';path=ROOT/asset['source_files']['en-US']
        if file_digest(path)!=asset['english_sha256']:raise ValueError('Image snapshot changed')
        add(name,path);image_rows.append(dict(name=asset['name'],original_sha256=asset['original_sha256'],english_sha256=asset['english_sha256'],bundle_path=name,routes=asset['routes'],review=asset['visual_review'],reported_text=asset.get('reported_text')))
    subs=ROOT/ready['subtitles']
    viewer=json.loads((ROOT/'localization/viewer-guide/manifest.json').read_text(encoding='utf8'))['en-US']
    if not any(r['name']==viewer['name'] for r in image_rows):
        member='images/viewer-guide.dds';add(member,viewer['replacement'])
        if files[member]['sha256']!=viewer['replacement_sha256']:raise ValueError('Viewer guide changed')
        image_rows.append(dict(name=viewer['name'],original_sha256=viewer['source_sha256'],english_sha256=viewer['replacement_sha256'],bundle_path=member,routes=[dict(kind='nested',path=viewer['nested_path'])],review='offline source and nested readback passed; live display pending'))
        add('provenance/viewer-guide-manifest.json',ROOT/'localization/viewer-guide/manifest.json')
    for path in subs.iterdir():
        if path.suffix in ('.srt','.tsv'):add('subtitles/'+path.name,path)
    for path in (en/'structured').glob('*.json'):add('catalogs/gop/'+path.name,path)
    for path in (en/'subtitles/catalogs').glob('*.json'):add('catalogs/movies/'+path.name,path)
    for name in ('sidecar-scope.json','sidecar-results.json'):add('provenance/'+name,en/'model-drafts'/name)
    for name in ('sidecar-reviewed-corrections.json','extra-image-labels.json','english-domain-terms.json'):add('provenance/'+name,ROOT/'tools/localization/catalogs'/name)
    for name in ('catalog-audit.json','installed-text-coverage.json','elf-korean-parity.json','semantic-review-flags.json'):add('verification/'+name,en/'build'/name)
    add('verification/image-parity.json',en/'images/korean-parity-audit.json')
    candidate=ROOT/'native/subtitle-candidate'/ready['candidate_dir']
    for name in ('Macross30Recompiled.exe','SDL2.dll','build-result.json','plan.json'):add('native/'+name,candidate/name)
    if (candidate/'baseline-audit.json').exists():add('native/baseline-audit.json',candidate/'baseline-audit.json')
    for name in ('aircraft-hd.pack','aircraft-hd-manifest.json'):add('native/assets/'+name,ROOT/'native/assets'/name)
    if int(version[1:])>=10:
        for path in (en/'review-v10').glob('*.json'):add('review-v10/'+path.name,path)
        for path in (ROOT/ready['vfs']/'verification').glob('*.json'):add('verification/frozen/'+path.name,path)
    manifest=dict(locale='en-US',version=version,status='full_draft_review_pending',GOP_entries=13536,image_variants=len(image_rows),movie_tracks=55,movie_source_cues=1646,on_disc_voice_captions=14177,original_voice_ids_without_audio_payload=520,held_uncertain_voice_only_ASR_candidates=91,images=image_rows,files=[dict(member=name,sha256=r['sha256'],bytes=r['bytes']) for name,r in sorted(files.items())],requirements='Original game data and the existing Macross Windows project; normal saved language is preserved',human_translation_and_full_game_review_complete=False)
    with zipfile.ZipFile(target,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
        for name,row in sorted(files.items()):archive.write(row['path'],name)
        archive.writestr('pack-manifest.json',json.dumps(manifest,ensure_ascii=False,indent=2))
        archive.writestr('README.txt',f'Macross 30 English resource bundle, {version} draft.\nThe current workspace already has this pack installed.\nLaunch: python tools/play/start_english_preview.py\nThis bundle preserves the translated resources and review catalogs. It uses the original game data and existing Windows port environment.\nMachine translation, ASR, subtitle page timing, full scene layout and provisional credit-name readings require review.\n')
    with zipfile.ZipFile(target) as archive:
        if archive.testzip() is not None:raise ValueError('ZIP readback failed')
    result=dict(path=str(target),sha256=file_digest(target),bytes=target.stat().st_size,members=len(files)+2,status='full_draft_review_pending')
    write(en/'resource-bundle.json',result);print(json.dumps(result))
