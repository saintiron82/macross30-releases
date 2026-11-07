"""Publish a frozen full English draft only after resource and rendering gates."""
import argparse
import json
import shutil
from pathlib import Path
import sys
from english_pack import ROOT,digest,write,preview_version
sys.path.insert(0,str(ROOT/'tools/play'))
from language_packs import file_digest,resolve_language
from native_binary_launch import select_native_binary

def read(path):return json.loads(path.read_text(encoding='utf8'))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version',type=preview_version,required=True)
    parser.add_argument('--movie-proof',type=Path,required=True)
    parser.add_argument('--voice-proof',type=Path,required=True)
    parser.add_argument('--replace-version',type=preview_version,help='Explicitly supersede this published version, retaining its ready/pointer records')
    args=parser.parse_args();base=ROOT/('build/english-preview-images-'+args.version)
    ready=ROOT/'localization/en-US/pack-ready.json';active=ready.with_name('preview-active.json')
    old_ready=ready.read_bytes() if ready.exists() else None
    old_active=active.read_bytes() if active.exists() else None
    if old_ready is not None:
        previous=json.loads(old_ready)
        if not args.replace_version or args.replace_version==args.version or Path(previous['vfs']).name!='english-preview-images-'+args.replace_version:
            raise ValueError('Select the exact prior published version with --replace-version')
        if old_active is None or Path(json.loads(old_active)['manifest']).parent.name!='english-preview-images-'+args.replace_version:
            raise ValueError('Active preview differs from the version being superseded')
    elif args.replace_version:raise ValueError('No published version to supersede')
    en=ROOT/'localization/en-US';manifest_path=base/'preview.json';manifest=read(manifest_path)
    if manifest['status']!='FULL_ENGLISH_DRAFT_REVIEW_PENDING':raise ValueError('Not a full English draft')
    gop=read(en/'build/gop-manifest.json');installed=read(en/'build/installed-text-coverage.json')
    if gop['total']!=13536 or gop['remaining'] or installed['remaining'] or installed['translated_with_japanese'] or Path(installed['preview'])!=base:raise ValueError('Incomplete English GOP installation')
    catalog=read(en/'build/catalog-audit.json')
    if catalog['untranslated'] or catalog['non_english'] or catalog['token_mismatches'] or catalog['missing_original_font_glyphs']:raise ValueError('English catalog font/token gate failed')
    images=read(en/'images/korean-parity-audit.json')
    if images['missing'] or images['additional_pending'] or Path(images['preview'])!=base or manifest['translated_image_variants']!=images['ui_asset_count']:raise ValueError('Image parity is incomplete')
    epoch=int(args.version[1:]);layout_proof=en/('images/extra-ui-v10/layout-audit.json' if epoch>=10 else 'images/extra-ui-v1/layout-audit.json')
    if read(layout_proof)['status']!='PASS':raise ValueError('Additional image layout gate failed')
    elf=read(en/'build/full-elf-manifest.json');parity=read(en/'build/elf-korean-parity.json')
    command=manifest['command'];elf_path=Path(command[command.index('--elf')+1])
    if parity['missing'] or parity['english_sha256']!=file_digest(elf_path) or elf['output_sha256']!=file_digest(elf_path):raise ValueError('ELF parity changed')
    values={s.split('=',1)[0]:s.split('=',1)[1] for s in command if '=' in s}
    subtitles=Path(values['MACROSS_SUBTITLE_DIR']);battle=Path(values['MACROSS_BATTLE_CATALOG_FILE'])
    movies=read(en/'subtitles/manifest.json');voice=read(en/'subtitles/voice-sidecar-manifest.json')
    movie_proof=read(args.movie_proof)
    if movies['translated']!=1646 or movies['cues']!=1646 or not voice['complete']:raise ValueError('Subtitles incomplete')
    if movie_proof['status']!='PASS' or movie_proof['tracks']!=55 or movie_proof['failed_cases'] or movie_proof['runtime_registry_SHA256']!=file_digest(subtitles/'tracks.tsv'):raise ValueError('Movie renderer proof changed')
    if file_digest(battle)!=voice['sha256']:raise ValueError('Voice captions changed')
    voice_proof=read(args.voice_proof)
    if voice_proof['status']!='PASS' or voice_proof['catalog_sha256']!=voice['sha256'] or voice_proof['stems']!=voice['entries'] or not voice_proof['zero_lookup_allocations']:raise ValueError('English native caption proof changed')
    candidate=command[command.index('--candidate-dir')+1];exe=select_native_binary(ROOT,candidate)
    plan=read(exe.parent/'plan.json')
    loader=next(r for r in plan['compile'] if Path(r['source']).name=='battle_voice_catalog.cpp')
    if loader['source_sha256']!=voice_proof['production_loader_sha256']:raise ValueError('Caption test and candidate loader differ')
    bridge_proof=ROOT/'native/analysis/battle-voice-subtitle-bridge-check/result.json'
    bridge=next(r for r in plan['compile'] if Path(r['source']).name=='battle_voice_subtitles.cpp')
    if read(bridge_proof)['status']!='PASS' or read(bridge_proof)['production_bridge_sha256']!=bridge['source_sha256']:raise ValueError('Caption ownership/pagination proof changed')
    state=Path(values['PS3_HDD0_ROOT']).parent
    files=[elf_path,battle,subtitles/'tracks.tsv',manifest_path,exe,args.movie_proof.resolve(),args.voice_proof.resolve(),en/'build/installed-text-coverage.json',en/'images/korean-parity-audit.json',en/'build/elf-korean-parity.json']
    files+=list((base/'PS3_GAME/USRDIR/DATA/PACK').iterdir())+list(subtitles.glob('*.srt'))
    files += [layout_proof,bridge_proof,en/'build/catalog-audit.json']
    if epoch>=10:
        discovery=en/'images/discovery-v10/layout-audit.json';extra=en/'review-v10/supplemental-elf-review.json'
        if read(discovery)['status']!='PASS' or read(extra)['holds'] or read(extra)['output_sha256']!=file_digest(elf_path):raise ValueError('Supplemental resource review changed')
        files += [discovery,extra,en/'review-v10/short-warning-review.json']
    if epoch>=11:
        residual=en/'images/residual-v11/layout-audit.json'
        if read(residual)['status']!='PASS':raise ValueError('Residual image layout gate failed')
        files += [residual,en/'images/residual-v11/manifest.json']
    if epoch>=12:
        banner=en/'images/combat-banner-v3/manifest.json';record=read(banner)
        if record['status']!='PASS' or file_digest(Path(record['replacement']))!=record['replacement_sha256'] or record['sprite_positions']!=[427,530,633,737]:
            raise ValueError('Combat banner assembly proof changed')
        files += [banner,en/'images/combat-banner-v3/assembled-review.png']
    if epoch>=13:
        music=ROOT/'localization/custom-sound/music-policy.json';record=read(music)
        raw=elf_path.read_bytes()
        for row in record['original_music_literals']:
            off=int(row['offset'],16);original=row['source_ja'].encode()+b'\0'
            if raw[off:off+len(original)]!=original:raise ValueError('Original music title/credit changed')
        files += [music,ROOT/'localization/custom-sound/menus.json']
    # Freeze verification records with this VFS. Subsequent review runs may
    # regenerate working reports without invalidating the published profile.
    frozen=[];proof_dir=base/'verification';proof_dir.mkdir(exist_ok=True)
    for p in files:
        if p.resolve().is_relative_to(base.resolve()) or p.resolve().is_relative_to(subtitles.resolve()) or p==exe:
            frozen.append(p);continue
        name=file_digest(p)[:12]+'-'+p.name;dest=proof_dir/name
        if dest.exists() and file_digest(dest)!=file_digest(p):raise ValueError('Frozen proof changed')
        if not dest.exists():shutil.copyfile(p,dest)
        frozen.append(dest)
    files=frozen
    proofs=[dict(path=p.relative_to(ROOT).as_posix(),sha256=file_digest(p)) for p in files]
    doc=dict(locale='en-US',status='full_draft',untranslated=0,elf=elf_path.relative_to(ROOT).as_posix(),vfs=base.relative_to(ROOT).as_posix(),state=state.relative_to(ROOT).as_posix(),subtitles=subtitles.relative_to(ROOT).as_posix(),battle_catalog=battle.relative_to(ROOT).as_posix(),candidate_dir=candidate,proofs=proofs,human_translation_review_complete=False,full_game_visual_review_complete=False)
    # Validate the same profile used by normal launches before changing pointer.
    if (ready.read_bytes() if ready.exists() else None)!=old_ready or (active.read_bytes() if active.exists() else None)!=old_active:raise ValueError('Published pack changed during verification')
    if old_ready is not None:
        history=en/'published-history'/args.replace_version;history.mkdir(parents=True,exist_ok=True)
        for filename,raw in [('pack-ready.json',old_ready),('preview-active.json',old_active)]:
            path=history/filename
            if path.exists() and path.read_bytes()!=raw:raise ValueError('Published history is immutable')
            if not path.exists():path.write_bytes(raw)
    write(ready,doc)
    try:resolve_language(ROOT,'en-US')
    except Exception:
        if old_ready is not None:ready.write_bytes(old_ready)
        else:ready.unlink()
        raise
    write(en/'preview-active.json',dict(manifest=str(manifest_path)))
    print(json.dumps(dict(status='full_draft',preview=str(manifest_path),normal_saved_language_changed=False,review='pending')))
