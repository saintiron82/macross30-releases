"""Independently compare emitted sidecar words/intervals with authored source-bound text."""
import hashlib,json,re
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
def read(p):return json.loads(Path(p).read_text(encoding='utf8'))
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def ms(t):
    h,m,s,f=map(int,re.split('[:,]',t));return ((h*60+m)*60+s)*1000+f
def audit(locale):
    scope=read(ROOT/'localization/ja-en-ru-review/caption-scope.json')
    out=ROOT/f'localization/{locale}/subtitles/direct-v1';report=read(out/'file-coverage.json')
    field='translations' if locale=='ru-RU' else 'translations_es';docs={};texts={}
    for key,path in report['translation_provenance'].items():
        if path not in docs:docs[path]=read(ROOT/path)
        texts[key]=docs[path][field][key]
    movie_rows=[]
    for movie in scope['movies']:
        path=out/(Path(movie['movie']).stem+'.'+locale[:2]+'.srt')
        blocks=re.split(r'\r?\n\s*\r?\n',path.read_text(encoding='utf-8-sig').strip())
        rendered=[];intervals=[];previous=0
        for n,block in enumerate(blocks,1):
            lines=block.splitlines();assert lines[0]==str(n),(path,n)
            a,b=map(ms,lines[1].split(' --> '));assert previous<=a<b,(path,n,a,b);previous=b
            intervals.append((a,b));rendered.extend(lines[2:])
        expected=' '.join(' '.join(texts[c['id']] for c in movie['cues']).split())
        assert ' '.join(' '.join(rendered).split())==expected,('Movie words changed',path)
        starts={a for a,b in intervals};ends={b for a,b in intervals}
        for cue in movie['cues']:
            assert round(cue['start']*1000) in starts and round(cue['end']*1000) in ends,('Source interval lost',path,cue['id'])
        movie_rows.append(dict(path=path.relative_to(ROOT).as_posix(),sha256=digest(path),source_cues=len(movie['cues']),runtime_cues=len(blocks)))
    voice_path=out/f'battle.{locale[:2]}.tsv';rows=voice_path.read_text(encoding='utf8').splitlines()[1:]
    voice={}
    for line in rows:
        stem,duration,encoded=line.split('\t');decoded=[];i=0
        while i<len(encoded):
            c=encoded[i]
            if c=='\\':
                i+=1;c={'n':'\n','f':'\f','\\':'\\'}[encoded[i]]
            decoded.append(c);i+=1
        text=''.join(decoded)
        expected=' '.join(texts['voice:'+stem].replace('\\n',' ').replace('\\f',' ').split())
        assert ' '.join(text.split())==expected,('Voice words changed',stem)
        assert stem not in voice;voice[stem]=duration
    assert set(voice)=={r['stem'] for r in scope['voice']}
    assert all(voice[r['stem']]==r['duration'] for r in scope['voice'])
    return dict(locale=locale,status='PASS',movie_files=len(movie_rows),movie_source_cues=sum(r['source_cues'] for r in movie_rows),
        movie_runtime_cues=sum(r['runtime_cues'] for r in movie_rows),voice_rows=len(voice),voice_sha256=digest(voice_path),
        authored_words_preserved=True,all_source_cue_outer_intervals_preserved=True,voice_ids_and_durations_preserved=True,
        movie_files_sha256=movie_rows,human_audio_review=False)
if __name__=='__main__':
    results=[audit(locale) for locale in ('ru-RU','es-ES')]
    path=ROOT/'build/direct-caption-final-audit.json';path.write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    print(json.dumps([{k:v for k,v in r.items() if k!='movie_files_sha256'} for r in results]))
