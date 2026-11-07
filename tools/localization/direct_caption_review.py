"""Freeze Japanese native UI sources and validate directly authored locale checkpoints."""
import argparse,hashlib,json,re,uuid
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
BASE=ROOT/'localization/ja-en-ru-review'
PATTERN=re.compile(r'^MACROSS_UI_STRING\("([^"\\]+)", L"((?:[^"\\]|\\.)*)", L"((?:[^"\\]|\\.)*)", L"((?:[^"\\]|\\.)*)"\)\s*$',re.M)
TOKENS=re.compile(r'%(?:\d+\$)?[-+# 0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn%]|[\n\t]')

def read(p):return json.loads(Path(p).read_text(encoding='utf8'))
def write(p,d):
    p=Path(p);p.parent.mkdir(parents=True,exist_ok=True)
    atomic_bytes(p,(json.dumps(d,ensure_ascii=False,indent=2)+'\n').encode('utf8'))
def atomic_bytes(p,data):
    p=Path(p);temporary=p.with_name(p.name+'.'+uuid.uuid4().hex+'.tmp')
    try:
        temporary.write_bytes(data);temporary.replace(p)
    finally:
        temporary.unlink(missing_ok=True)
def sha(s):return hashlib.sha256(s.encode()).hexdigest()
def prepare_ui():
    entries=[];files={}
    for relative in ['vendor/ps3recomp-master/libs/video/macross_ui_strings.def','vendor/ps3recomp-master/libs/system/savedlg_strings.def']:
        data=(ROOT/relative).read_text(encoding='utf8');files[relative]=sha(data)
        for m in PATTERN.finditer(data):
            source=json.loads('"'+m[4]+'"');english=json.loads('"'+m[3]+'"')
            entries.append(dict(id='native-ui:'+m[1],key=m[1],source_ja=source,installed_en=english,source_sha256=sha(source)))
    if len({e['key'] for e in entries})!=len(entries):raise ValueError('Duplicate UI key')
    output=BASE/'native-ui-scope.json';write(output,dict(schema_version=1,source_files=files,entries=entries))
    return dict(path=str(output),count=len(entries))
def check_ui(file):
    scope={r['key']:r for r in read(BASE/'native-ui-scope.json')['entries']};doc=read(file)
    counts={}
    for field in ('translations','translations_es'):
        values=doc[field]
        if set(values)!=set(scope):raise ValueError('UI coverage mismatch '+field)
        for key,text in values.items():
            if not isinstance(text,str) or not text.strip() or '\0' in text:raise ValueError('Invalid UI '+key)
            if TOKENS.findall(text)!=TOKENS.findall(scope[key]['source_ja']):raise ValueError('UI token mismatch '+key)
            if doc['source_sha256'][key]!=scope[key]['source_sha256']:raise ValueError('UI source mismatch '+key)
        counts[field]=len(values)
    return counts

def prepare_captions():
    """Bind installed runtime files to original Japanese, never English text matching."""
    from collections import Counter,defaultdict
    def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
    old=read(ROOT/'localization/ru-RU/scope.json')
    gop=read(BASE/'gop-scope.json');by_gop={(e['catalog'],e['index']):e for e in gop['entries']}
    ja_to_ids=defaultdict(list)
    for e in gop['entries']:ja_to_ids[e['source_ja']].append(e['id'])
    sources={};source_artifacts={};collisions=[]
    def use(stem,source,catalog,index,artifact,basis):
        key=(catalog,index);g=by_gop.get(key)
        if g and g['source_ja']!=source:raise ValueError('Japanese map/source mismatch '+str(key))
        entry=dict(source_ja=source,source_sha256=sha(source),gop_id=g['id'] if g else None,
                   catalog=catalog,index=index,source_artifact=artifact,source_basis=basis)
        if stem in sources:
            if sources[stem]['source_ja']!=source:collisions.append(dict(stem=stem,preferred=sources[stem],variant=entry))
        else:sources[stem]=entry
    path=ROOT/'localization/subtitles/battle-voice/record-voice-text-map.json'
    mapping=read(path);relative=path.relative_to(ROOT).as_posix();source_artifacts[relative]=digest(path)
    if digest(ROOT/mapping['source'])!=mapping['source_sha256']:raise ValueError('Battle map original hash changed')
    for r in mapping['records']:
        if r['source']:use(r['record_id'][len('R_BattleText'):].rsplit('_',1)[0]+'/'+r['voice_key'],r['source'],'gop_battletext',r['text_string_index'],relative,'original_GOP_record')
    audit=ROOT/'analysis/localization/voice-only-audit'
    for name,catalog in [('scenario-voice-map.json','gop_scenariotext'),('gop_subquesttext-voice-map.json','gop_subquesttext'),('gop_systemtext-voice-map.json','gop_systemtext')]:
        path=audit/name;mapping=read(path);relative=path.relative_to(ROOT).as_posix();source_artifacts[relative]=digest(path)
        if digest(ROOT/mapping['source'])!=mapping['source_sha256']:raise ValueError('Original source hash changed '+name)
        for r in mapping['records']:
            if r['voice_key'] and r['source']:use(r['resolved_stem'],r['source'],catalog,r['text_string_index'],relative,'original_GOP_record')
    path=audit/'exact-alias-caption-map-all-tables.json';aliases=read(path);relative=path.relative_to(ROOT).as_posix();source_artifacts[relative]=digest(path)
    for r in aliases['entries']:
        candidates={s.strip() for s in r['source_text_candidates']}
        if len(candidates)!=1:raise ValueError('Ambiguous Japanese audio alias')
        source=next(iter(candidates))
        matching=[sources.get(p[len('data/sound/voice/'):-4]) for p in r['matching_text_asset_paths']]
        gops=list(dict.fromkeys(s['gop_id'] for s in matching if s and s.get('gop_id') and s['source_ja'].strip()==source))
        if not gops:gops=ja_to_ids.get(source,[])
        if r['resolved_stem'] in sources:raise ValueError('Audio alias replaces direct source')
        sources[r['resolved_stem']]=dict(source_ja=source,source_sha256=sha(source),gop_ids=gops,source_artifact=relative,
            source_basis='exact_audio_SHA_alias',audio_sha256=r['sha256'],matching_text_asset_paths=r['matching_text_asset_paths'])
    path=ROOT/'localization/subtitles/battle-voice/asr-caption-drafts.json';data=read(path);relative=path.relative_to(ROOT).as_posix();source_artifacts[relative]=digest(path)
    for r in data['entries']:
        if r['stem'] in sources:raise ValueError('ASR replaces original')
        sources[r['stem']]=dict(source_ja=r['source_ja'],source_sha256=sha(r['source_ja']),source_artifact=relative,
            source_basis='unreviewed_ASR',audio_sha256=r['source_sha256'],source_hold='Japanese ASR transcription is not verified against audio.')
    path=ROOT/'localization/subtitles/battle-voice/medium-crosscheck-v1/candidate-review-v1/translation-reviewed.json';data=read(path);relative=path.relative_to(ROOT).as_posix();source_artifacts[relative]=digest(path)
    for r in data['entries']:
        for stem in r['resolved_stems']:
            if stem in sources:raise ValueError('Dual ASR replaces prior source')
            source=r['Medium_source_ja']
            sources[stem]=dict(source_ja=source,source_sha256=sha(source),source_artifact=relative,
                source_basis='dual_ASR_agreement',audio_sha256=r['source_sha256'],source_hold='Small/Medium ASR agreement; no verified original written line or listening review.')
    for version in range(2,7):
        path=ROOT/f'localization/subtitles/battle-voice/medium-crosscheck-v1/lexical-review-v{version}/translation-reviewed.json'
        data=read(path);relative=path.relative_to(ROOT).as_posix();source_artifacts[relative]=digest(path)
        for r in data['entries']:
            for stem in r['resolved_stems']:
                if stem in sources:raise ValueError('Lexical ASR replaces prior source')
                source=r['reviewer_normalized_ja']
                sources[stem]=dict(source_ja=source,source_sha256=sha(source),source_artifact=relative,
                    source_basis='normalized_ASR_draft',audio_sha256=r['source_sha256'],
                    source_hold='Reviewer-normalized ASR draft; no verified original written line or listening review.')
    voice=[]
    for r in old['voice']:
        source=sources.get(r['identity'])
        if source is None:raise ValueError('Missing Japanese binding '+r['identity'])
        voice.append(dict(id='voice:'+r['identity'],stem=r['identity'],duration=r['duration'],installed_en=r['source'],**source))
    movies=[];file_ledger=[]
    for path in sorted((ROOT/'localization/en-US/subtitles/catalogs').glob('*.json')):
        data=read(path);transcript=ROOT/data['source_transcript'];source=read(transcript)
        if digest(transcript)!=data['source_sha256']:raise ValueError('Movie transcript changed '+str(path))
        raw=source['segments'];lookup={r['id']:r for r in raw};movie=Path(data['movie']).stem
        cues=[]
        for r in data['cues']:
            ja=lookup[r['id']]
            if any(r[k]!=ja[k] for k in ('id','start','end','source_ja')):raise ValueError('Movie cue changed')
            cues.append(dict(id=f'movie:{movie}:{r["id"]}',cue_id=r['id'],start=r['start'],end=r['end'],source_ja=r['source_ja'],
                source_sha256=sha(r['source_ja']),installed_en=r['translation_en'],review_flags=r.get('review_flags',[]),
                exact_gop_ids=ja_to_ids.get(r['source_ja'],[])))
        movies.append(dict(movie=data['movie'],source_catalog=path.relative_to(ROOT).as_posix(),source_catalog_sha256=digest(path),
            source_transcript=data['source_transcript'],source_transcript_sha256=data['source_sha256'],
            movie_sha256=data['movie_sha256'],origin90k=data['origin90k'],cues=cues,
            targets={lang:f'localization/{lang}/subtitles/direct-v1/{movie}.{lang.split("-")[0]}.srt' for lang in ('ru-RU','es-ES')}))
        for variant in [ROOT/f'localization/en-US/subtitles/preview-v13/{movie}.en.srt',ROOT/f'localization/subtitles/runtime/{movie}.ko.srt',ROOT/f'localization/subtitles/runtime/{movie}.clean.ko.srt']:
            if not variant.exists():raise ValueError('Missing movie variant '+str(variant))
            blocks=re.split(r'\r?\n\s*\r?\n',variant.read_text(encoding='utf-8-sig').strip())
            file_ledger.append(dict(path=variant.relative_to(ROOT).as_posix(),sha256=digest(variant),kind='movie_srt',
                movie=data['movie'],runtime_cues=len(blocks),source_cues=len(cues),source_catalog=path.relative_to(ROOT).as_posix(),
                target_paths=movies[-1]['targets'],status='Japanese_bound_translation_pending'))
    for path in [ROOT/'localization/subtitles/runtime/tracks.tsv',ROOT/'localization/en-US/subtitles/preview-v13/tracks.tsv',ROOT/'localization/en-US/subtitles/preview-v13/battle.en.tsv']:
        file_ledger.append(dict(path=path.relative_to(ROOT).as_posix(),sha256=digest(path),kind='voice_catalog' if path.name.startswith('battle') else 'movie_track_registry',
            status='Japanese_bound_translation_pending',target_paths={lang:f'localization/{lang}/subtitles/direct-v1/'+('battle.'+lang.split('-')[0]+'.tsv' if path.name.startswith('battle') else 'tracks.tsv') for lang in ('ru-RU','es-ES')}))
    doc=dict(schema_version=1,source_language='ja-JP',source_artifacts=source_artifacts,voice=voice,movies=movies,voice_collisions=collisions,
        policy='Original GOP bindings preferred. ASR uncertain source remains explicit hold. No English-pivot translation. Song names/credits remain original.',files=file_ledger)
    write(BASE/'caption-scope.json',doc)
    report=dict(movie_files=len(movies),movie_source_cues=sum(len(m['cues']) for m in movies),movie_runtime_cues=sum(f['runtime_cues'] for f in file_ledger if '/en-US/' in f['path'] and f['kind']=='movie_srt'),
        source_file_ledger_entries=len(file_ledger),voice_rows=len(voice),voice_source_basis=dict(Counter(r['source_basis'] for r in voice)),
        voice_unique_JA=len({r['source_ja'] for r in voice}),voice_without_GOP_id=sum(not r.get('gop_id') and not r.get('gop_ids') for r in voice),
        movie_exact_GOP_cues=sum(bool(c['exact_gop_ids']) for m in movies for c in m['cues']),voice_collisions=len(collisions))
    write(BASE/'caption-source-report.json',report);return report

def check_captions(file):
    from gop_strings import TOKEN
    doc=read(file);scope=read(BASE/'caption-scope.json')
    sources={r['id']:r for r in scope['voice']}
    sources.update({r['id']:r for m in scope['movies'] for r in m['cues']})
    counts={}
    for field in ('translations','translations_es'):
        values=doc.get(field,{})
        for key,text in values.items():
            if key not in sources:raise ValueError('Unknown caption key '+key)
            if not isinstance(text,str) or not text.strip() or '\0' in text:raise ValueError('Invalid caption '+key)
            if doc['source_sha256'][key]!=sources[key]['source_sha256']:raise ValueError('Caption source changed '+key)
            if TOKEN.findall(text)!=TOKEN.findall(sources[key]['source_ja']):raise ValueError('Caption token changed '+key)
        counts[field]=len(values)
    return dict(**counts,source_holds=len(doc.get('source_holds',{})))

def separate_source_notes(file):
    """Move clearly described spelling/term normalization out of ambiguity holds."""
    doc=read(file);notes=doc.setdefault('source_notes',{});holds=doc.get('source_holds',{})
    ambiguous=('corrupt','damaged','probable','unresolved','uncertain','provisional','needs','require','may ',
               'not verified','gap','missing','clipped','truncat','guess','inferred','ambiguous','not represented','not audio')
    moved=[]
    for key,reason in list(holds.items()):
        if 'normaliz' in reason.lower() and not any(word in reason.lower() for word in ambiguous):
            notes[key]=holds.pop(key);moved.append(key)
    write(file,doc);return dict(source_notes_added=len(moved),source_holds=len(holds))

def reuse_voice():
    """Copy only existing directly authored text through explicit Japanese-source bindings."""
    scope=read(BASE/'caption-scope.json');translations={};translations_es={};bindings={};hashes={};pending={}
    projects={lang:read(ROOT/f'localization/{lang}/authoring-project-v1/project.json') for lang in ('ru-RU','es-ES')}
    entries={r['id']:r for r in read(BASE/'gop-scope.json')['entries']}
    for r in scope['voice']:
        ids=[r['gop_id']] if r.get('gop_id') else r.get('gop_ids',[])
        if not ids:continue
        hashes[r['id']]=r['source_sha256'];bindings[r['id']]=ids
        for lang,target in [('ru-RU',translations),('es-ES',translations_es)]:
            candidates=[]
            for key in ids:
                source=entries[key];value=projects[lang]['entries'].get(key,{})
                if source['source_ja'].strip()!=r['source_ja'].strip():raise ValueError('Voice Japanese identity mismatch')
                if value.get('source')!=source['source_ja'] or value.get('source_sha256')!=source['source_sha256']:raise ValueError('Authored project source mismatch')
                if value.get('text'):candidates.append((key,value['text']))
            if candidates:target[r['id']]=candidates[0][1]
            else:pending.setdefault(lang,[]).append(r['id'])
    doc=dict(catalog='voice-gop-reuse',translations=translations,translations_es=translations_es,source_sha256=hashes,
        gop_source_bindings=bindings,source_holds={},notes='Exact original Japanese identity and record/full-stem or exact-audio binding. Existing directly authored GOP translations copied without generation or English pivot. Preferred first source ID follows original matched audio source. No additional human/audio review claim.',pending=pending)
    output=BASE/'authored/voice-gop-reuse-001.json';write(output,doc)
    report=check_captions(output);report['pending']={k:len(v) for k,v in pending.items()};return report

def reuse_asr():
    import unicodedata
    from collections import defaultdict
    def normalized(s):
        return ''.join(c for c in unicodedata.normalize('NFKC',s.replace('\\n',' ')) if not c.isspace() and unicodedata.category(c)[0]!='P')
    scope=read(BASE/'caption-scope.json');gop=read(BASE/'gop-scope.json');lookup=defaultdict(list)
    for r in gop['entries']:lookup[normalized(r['source_ja'])].append(r)
    projects={lang:read(ROOT/f'localization/{lang}/authoring-project-v1/project.json') for lang in ('ru-RU','es-ES')}
    doc=dict(catalog='voice-asr-reuse',translations={},translations_es={},source_sha256={},source_holds={},gop_source_bindings={},
        notes='Existing directly authored Japanese GOP text reused only for identical lexical Japanese after NFKC, whitespace and punctuation normalization. No word substitution or translation generation. ASR provenance holds retained for all rows.')
    for r in scope['voice']:
        if 'ASR' not in r['source_basis']:continue
        candidates=lookup[normalized(r['source_ja'])]
        if not candidates:continue
        doc['source_sha256'][r['id']]=r['source_sha256'];doc['source_holds'][r['id']]=r['source_hold']
        doc['gop_source_bindings'][r['id']]=[c['id'] for c in candidates]
        for lang,field in [('ru-RU','translations'),('es-ES','translations_es')]:
            for c in candidates:
                item=projects[lang]['entries'][c['id']]
                if item.get('source')!=c['source_ja'] or item.get('source_sha256')!=c['source_sha256']:raise ValueError('GOP reuse source changed')
                if item.get('text'):
                    doc[field][r['id']]=item['text'].replace('\\n',' ').replace('\n',' ');break
    output=BASE/'authored/voice-asr-gop-reuse-001.json';write(output,doc);return check_captions(output)

def emit_captions():
    """Emit authored movie/voice sidecars incrementally, with per-file source coverage."""
    import textwrap
    from english_subtitle_layout import pages,project
    from english_subtitles import stamp
    scope=read(BASE/'caption-scope.json');all_rows={r['id']:r for r in scope['voice']}
    all_rows.update({r['id']:r for m in scope['movies'] for r in m['cues']})
    fields={'ru-RU':'translations','es-ES':'translations_es'};texts={lang:{} for lang in fields};holds={};source_notes={};provenance={lang:{} for lang in fields}
    paths=[BASE/'authored/voice-gop-reuse-001.json',BASE/'authored/voice-extra-001.json']
    paths+=sorted((BASE/'authored').glob('voice-asr-*.json'))
    paths+=sorted((BASE/'authored').glob('movie-*.json'))
    for path in paths:
        if not path.exists():continue
        check_captions(path);doc=read(path)
        for lang,field in fields.items():
            for key,text in doc.get(field,{}).items():
                texts[lang][key]=text;provenance[lang][key]=path.relative_to(ROOT).as_posix()
        holds.update(doc.get('source_holds',{}))
        source_notes.update(doc.get('source_notes',{}))
    for r in scope['voice']:
        if r.get('source_hold'):holds.setdefault(r['id'],r['source_hold'])
    reports={}
    for lang,values in texts.items():
        short=lang.split('-')[0];out=ROOT/f'localization/{lang}/subtitles/direct-v1';out.mkdir(parents=True,exist_ok=True)
        registry=['MACROSS_SUBTITLES_V1'];files=[]
        for m in scope['movies']:
            missing=[r['id'] for r in m['cues'] if r['id'] not in values]
            row=dict(movie=m['movie'],source_catalog=m['source_catalog'],source_transcript=m['source_transcript'],
                source_cues=len(m['cues']),translated=sum(r['id'] in values for r in m['cues']),missing=missing,
                source_holds={r['id']:holds[r['id']] for r in m['cues'] if r['id'] in holds},target=m['targets'][lang],status='translation_pending')
            if not missing:
                blocks=[];runtime_id=0
                for r in m['cues']:
                    for start,end,text in project(r['start'],r['end'],pages(values[r['id']])):
                        runtime_id+=1;blocks.append(f'{runtime_id}\n{stamp(start)} --> {stamp(end)}\n{text}')
                content=('\n\n'.join(blocks)+'\n').encode('utf8');target=out/(Path(m['movie']).stem+'.'+short+'.srt');atomic_bytes(target,content)
                row.update(status='translated_draft',runtime_cues=runtime_id,sha256=hashlib.sha256(content).hexdigest())
                registry.append('\t'.join([m['movie'],target.name,str(m['origin90k']),'1',m['movie_sha256'],row['sha256'],'draft']))
            files.append(row)
        atomic_bytes(out/'tracks.tsv', ('\n'.join(registry)+'\n').encode('utf8'))
        voice_rows=[];missing_voice=[]
        for r in scope['voice']:
            if r['id'] not in values:missing_voice.append(r['id']);continue
            flat=' '.join(values[r['id']].replace('\\n',' ').replace('\\f',' ').split())
            lines=textwrap.wrap(flat,26,break_long_words=False,break_on_hyphens=False)
            parts=['\n'.join(lines[i:i+2]) for i in range(0,len(lines),2)]
            if not parts or len(parts)>16 or any(len(p.encode('utf8'))>254 for p in parts):raise ValueError('Voice page capacity exceeded '+r['id'])
            if ' '.join(' '.join(parts).split())!=flat:raise ValueError('Voice words lost '+r['id'])
            text='\f'.join(parts).replace('\\','\\\\').replace('\n','\\n').replace('\f','\\f')
            voice_rows.append(r['stem']+'\t'+str(r['duration'])+'\t'+text)
        atomic_bytes(out/('battle.'+short+'.tsv'),('MACROSS_BATTLE_CAPTIONS_V3\t'+lang+'\n'+'\n'.join(voice_rows)+'\n').encode('utf8'))
        report=dict(locale=lang,status='draft_in_progress',source_scope_sha256=hashlib.sha256((BASE/'caption-scope.json').read_bytes()).hexdigest(),
            movies=files,movie_files_complete=sum(f['status']=='translated_draft' for f in files),movie_source_cues=sum(f['source_cues'] for f in files),
            movie_translated=sum(f['translated'] for f in files),voice_stems=len(scope['voice']),voice_translated=len(voice_rows),voice_missing=missing_voice,
            source_holds=holds,source_notes=source_notes,translated_source_holds=sum(key in holds for key in values),
            translation_provenance=provenance[lang],english_fallback_authored=False,human_audio_review=False)
        write(out/'file-coverage.json',report);reports[lang]={k:report[k] for k in ('movie_files_complete','movie_translated','voice_translated')}
    return reports
def main():
    p=argparse.ArgumentParser();s=p.add_subparsers(dest='action',required=True)
    s.add_parser('prepare-ui');s.add_parser('prepare-captions');s.add_parser('reuse-voice');s.add_parser('reuse-asr');s.add_parser('emit')
    for name in ('check-ui','check-captions','separate-source-notes'):
        q=s.add_parser(name);q.add_argument('--file',required=True)
    actions={'prepare-ui':prepare_ui,'prepare-captions':prepare_captions,'reuse-voice':reuse_voice,'reuse-asr':reuse_asr,'emit':emit_captions}
    a=p.parse_args();print(json.dumps(actions[a.action]() if a.action in actions else check_ui(a.file) if a.action=='check-ui' else separate_source_notes(a.file) if a.action=='separate-source-notes' else check_captions(a.file)))
if __name__=='__main__':main()
