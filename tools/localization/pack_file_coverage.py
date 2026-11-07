"""Report authored and installed text coverage separately for every source file."""
import collections
import csv
import hashlib
import json
from pathlib import Path
from gop_strings import parse
ROOT=Path(__file__).resolve().parents[2]
BASE=ROOT/'localization/ja-en-ru-review'
def read(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def tsv_keys(path):
    if path is None or not path.is_file():return set()
    return {line.split('\t')[0] for line in path.read_text(encoding='utf8').splitlines()[1:] if line}
def state(total,applied):return 'applied' if total==applied else 'missing' if applied==0 else 'partial'

def generate(locale):
    project=read(ROOT/f'localization/{locale}/authoring-project-v1/project.json')
    folder=ROOT/'localization/installed-packs'/locale
    manifest=read(folder/'pack.json') if (folder/'pack.json').exists() else {'resources':{}}
    resources={k:folder/v for k,v in manifest['resources'].items()}
    scope=read(BASE/'gop-scope.json');captions=read(BASE/'caption-scope.json')
    by_catalog=collections.defaultdict(list)
    for row in scope['entries']:by_catalog[row['catalog']].append(row)
    files=[]
    for catalog,rows in sorted(by_catalog.items()):
        target=resources.get('gop_overlay',folder/'absent')/(catalog+'.gop')
        installed={r['index']:r['text'] for r in parse(target.read_bytes())} if target.is_file() else {}
        authored=sum(bool(project['entries'][r['id']]['text']) for r in rows)
        applied=sum(bool(project['entries'][r['id']]['text']) and installed.get(r['index'])==project['entries'][r['id']]['text'] for r in rows)
        files.append(dict(kind='gop',source=catalog+'.gop',target=str(target.relative_to(ROOT)),total=len(rows),authored=authored,applied=applied,status=state(len(rows),applied)))
    elf=read(BASE/'elf-scope.json');overlay=resources.get('elf_overlay');report=read(overlay.with_suffix(overlay.suffix+'.json')) if overlay and overlay.with_suffix(overlay.suffix+'.json').exists() else {}
    files.append(dict(kind='elf',source='EBOOT.elf',target=str(overlay.relative_to(ROOT)) if overlay else None,total=len(elf['entries']),authored=sum(bool(project['entries'][r['id']]['text']) for r in elf['entries']),applied=report.get('records',0),status=state(len(elf['entries']),report.get('records',0))))
    ui=resources.get('native_ui');ui_count=len(tsv_keys(ui))
    files.append(dict(kind='native_ui',source='macross_ui_strings.def + savedlg_strings.def',target=str(ui.relative_to(ROOT)) if ui else None,total=82,authored=len(project.get('native_ui',{})),applied=ui_count,status=state(82,ui_count)))
    direct=ROOT/f'localization/{locale}/subtitles/direct-v1';authored_report=read(direct/'file-coverage.json') if (direct/'file-coverage.json').exists() else {'movies':[]}
    authored_movies={r['movie']:r for r in authored_report['movies']}
    short=locale.split('-')[0];subs=resources.get('subtitles')
    for movie in captions['movies']:
        stem=Path(movie['movie']).stem;name=stem+'.'+short+'.srt';target=subs/name if subs else None
        original=authored_movies.get(movie['movie'],{});authored=original.get('translated',0)
        exact=bool(target and target.is_file() and (direct/name).is_file() and hashlib.sha256(target.read_bytes()).digest()==hashlib.sha256((direct/name).read_bytes()).digest())
        applied=len(movie['cues']) if exact else 0
        variants=[r['path'] for r in captions['files'] if r.get('movie')==movie['movie']]
        files.append(dict(kind='movie',source=movie['movie'],source_variants=variants,target=str(target.relative_to(ROOT)) if target else None,total=len(movie['cues']),authored=authored,applied=applied,status=state(len(movie['cues']),applied)))
    authored_voice=tsv_keys(direct/('battle.'+short+'.tsv'));installed_voice=tsv_keys(resources.get('battle_catalog'))
    for row in captions['voice']:
        applied=int(row['stem'] in installed_voice);authored=int(row['stem'] in authored_voice)
        files.append(dict(kind='voice',source='data/sound/voice/'+row['stem']+'.at3',target=(str(resources['battle_catalog'].relative_to(ROOT))+'#'+row['stem']) if 'battle_catalog' in resources else None,total=1,authored=authored,applied=applied,status=state(1,applied)))
    summary={}
    for kind in ('gop','elf','native_ui','movie','voice'):
        rows=[r for r in files if r['kind']==kind]
        summary[kind]=dict(files=len(rows),files_applied=sum(r['status']=='applied' for r in rows),total=sum(r['total'] for r in rows),authored=sum(r['authored'] for r in rows),applied=sum(r['applied'] for r in rows))
    out=ROOT/f'localization/{locale}/file-coverage.json'
    result=dict(locale=locale,goal='All file-based text resources; only explicit original song/credit and nonexistent-audio exclusions.',images='en-US inherited intentionally',source_movie_cues=sum(len(m['cues']) for m in captions['movies']),excluded_no_audio_keys=read(BASE/'voice-no-audio-exclusions.json')['excluded_nonexistent_audio_files'],summary=summary,files=files)
    out.write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf8')
    with out.with_suffix('.csv').open('w',encoding='utf-8-sig',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=['kind','source','target','total','authored','applied','status'],extrasaction='ignore');writer.writeheader();writer.writerows(files)
    return summary
if __name__=='__main__':
    for locale in ('ru-RU','es-ES'):print(locale,json.dumps(generate(locale)))
