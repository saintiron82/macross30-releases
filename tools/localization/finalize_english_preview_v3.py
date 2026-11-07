"""Attach the guarded startup ELF and wrapped English movie sidecars to v3."""
import shutil
import argparse
from english_pack import ROOT,digest,write,preview_version
from pathlib import Path
from build_english_ui_images import read


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version',type=preview_version,default='v3')
    parser.add_argument('--full',action='store_true')
    parser.add_argument('--candidate-dir',help='Separately verified native candidate for this preview')
    args=parser.parse_args();version=args.version
    base=ROOT/('build/english-preview-images-'+version)
    manifest=read(base/'preview.json')
    elf_version='v4' if int(version[1:])>=5 else version
    elf=read(ROOT/'localization/en-US/build/full-elf-manifest.json' if args.full else ROOT/f'localization/en-US/build/startup-{elf_version}-manifest.json')
    elf_path=elf['elf'] if args.full else elf['output']
    if digest(Path(elf_path).read_bytes())!=elf['output_sha256']:
        raise ValueError('English startup ELF changed')
    if args.full:
        frozen=base/'EBOOT.english.elf'
        if frozen.exists():raise FileExistsError(frozen)
        shutil.copyfile(elf_path,frozen);elf_path=str(frozen)
    manifest['command'][manifest['command'].index('--elf')+1]=elf_path
    review=ROOT/'localization/en-US/subtitles/review'
    target=ROOT/('localization/en-US/subtitles/preview-'+version)
    if target.exists():raise FileExistsError(target)
    target.mkdir()
    lines=(review/'tracks.tsv').read_text(encoding='utf8').splitlines()
    registry=[lines[0]]
    for line in lines[1:]:
        columns=line.split('\t');raw=(review/columns[1]).read_bytes()
        if digest(raw)!=columns[5]:raise ValueError('Subtitle digest mismatch')
        shutil.copyfile(review/columns[1],target/columns[1]);columns[3]='1'
        registry.append('\t'.join(columns))
    (target/'tracks.tsv').write_bytes(('\n'.join(registry)+'\n').encode('utf8'))
    manifest['command']=[('MACROSS_SUBTITLE_DIR='+str(target)) if s.startswith('MACROSS_SUBTITLE_DIR=') else s for s in manifest['command']]
    if args.full:
        voice=read(ROOT/'localization/en-US/subtitles/voice-sidecar-manifest.json')
        if not voice['complete']:raise ValueError('Full English voice sidecar is incomplete')
        source=Path(voice['path'])
        if digest(source.read_bytes())!=voice['sha256']:raise ValueError('Voice caption digest mismatch')
        shutil.copyfile(source,target/'battle.en.tsv')
        candidate=args.candidate_dir or ('english-menu-v13' if int(version[1:])>=13 else 'english-catalog-v4')
        manifest['command'] += ['--candidate-dir',candidate,'--env','MACROSS_BATTLE_CATALOG_FILE='+str(target/'battle.en.tsv'),'--env','MACROSS_BATTLE_SUBTITLES=1']
        manifest.update(status='FULL_ENGLISH_DRAFT_REVIEW_PENDING',battle_caption_entries=voice['entries'],human_translation_review_complete=False)
    manifest.update(english_elf_slots=elf['manual_startup_slots']+elf['translated_literals'] if args.full else elf['translated_slots'],movie_line_character_limit=38,
                    movie_max_lines=3,enabled_movies=len(registry)-1,title_live_verified=False,
                    difficulty_live_verified=False,scenario_live_verified=False)
    write(base/'preview.json',manifest)
    print(version+': guarded English startup ELF and wrapped movie captions attached.')
