"""Japanese-source English review and direct Russian translation checkpoints.

Translation text is authored in review files by the current assistant. This
module does no model calls, generation, automatic approval or fuzzy matching.
"""
import argparse,json,hashlib,re
from pathlib import Path
from gop_strings import TOKEN
ROOT=Path(__file__).resolve().parents[2]
BASE=ROOT/'localization/ja-en-ru-review'
def read(p):return json.loads(Path(p).read_text(encoding='utf8'))
def write(p,value):
    p=Path(p);p.parent.mkdir(parents=True,exist_ok=True)
    temp=p.with_suffix(p.suffix+'.tmp');temp.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf8');temp.replace(p)
def prepare():
    previous=read(ROOT/'localization/ru-RU/scope.json')
    installed={g['name']:{r['index']:r['source'] for r in g['entries']} for g in previous['gops']}
    rows=[]
    for p in sorted((ROOT/'localization/en-US/structured').glob('*.json')):
        doc=read(p);name=Path(doc['source_file']).name
        for r in doc['entries']:
            if r['index'] not in installed.get(name,{}):continue
            rows.append(dict(id=r['id'],kind='gop',catalog=p.stem,index=r['index'],
                source_ja=r['source'],source_sha256=hashlib.sha256(r['source'].encode()).hexdigest(),
                installed_en=installed[name][r['index']],status='pending_direct_review'))
    target=BASE/'gop-scope.json'
    assert not target.exists(),'Preserve the frozen source scope'
    write(target,dict(source_language='ja-JP',english_action='semantic_recheck',russian_action='direct_from_Japanese',
        local_translation_models=False,old_russian_machine_draft='excluded',english_source_elf_sha256=previous['english_elf_sha256'],entries=rows))
    print('Frozen Japanese-source GOP entries:',len(rows))
def show(catalog,start,count):
    entries=[r for r in read(BASE/'gop-scope.json')['entries'] if r['catalog']==catalog]
    for r in entries[start:start+count]:print(json.dumps([r['index'],r['source_ja'],r['installed_en']],ensure_ascii=False))

def foreign_script_remainder(identity,text,source):
    path=BASE/'preserved-text.json'
    allowed=read(path).get('entries',{}).get(identity,[]) if path.exists() else []
    for fragment in allowed:
        assert fragment and fragment in source,(identity,'preservation source mismatch')
        assert fragment in text,(identity,'original song title must be preserved')
        text=text.replace(fragment,'')
    return text
def admit(path,validate_only=False):
    batch=read(path);scope={r['id']:r for r in read(BASE/'gop-scope.json')['entries']}
    if 'translations' in batch:
        edits=batch.get('english_corrections',{});entries=[]
        for index,ru in batch['translations'].items():
            original=scope[batch['catalog']+':string:'+index]
            en=edits.get(index,original['installed_en'])
            entries.append(dict(index=int(index),source_ja=original['source_ja'],installed_en=original['installed_en'],
                en=en,ru=ru,en_verdict='retain' if en==original['installed_en'] else 'correct'))
        batch=dict(catalog=batch['catalog'],notes=batch.get('notes',''),source_holds=batch.get('source_holds',{}),entries=entries)
    dest=BASE/'reviewed'/Path(path).name
    if not validate_only:assert not dest.exists(),'Review checkpoint already exists'
    seen=set()
    for row in batch['entries']:
        identity=batch['catalog']+':string:'+str(row['index']);original=scope[identity]
        assert identity not in seen;seen.add(identity)
        assert row['source_ja']==original['source_ja'],identity
        assert row['installed_en']==original['installed_en'],identity
        assert row['en_verdict'] in ('retain','correct')
        assert (row['en']==original['installed_en'])==(row['en_verdict']=='retain'),identity
        for lang in ('en','ru'):
            text=row[lang];assert text.strip() and '\0' not in text,identity
            assert TOKEN.findall(text)==TOKEN.findall(original['source_ja']),(identity,lang,'tokens')
            if lang=='ru':assert not re.search('[\u3040-\u30ff\u3400-\u9fff\uac00-\ud7af]',foreign_script_remainder(identity,text,original['source_ja'])),(identity,'foreign script')
        row.update(id=identity,source_sha256=original['source_sha256'],reviewer='current_assistant',
            review_method='Japanese source semantic comparison; direct Russian translation',
            human_native_review=False,status='assistant_reviewed_draft')
        if str(row['index']) in batch.get('source_holds',{}):
            row['source_review_hold']=batch['source_holds'][str(row['index'])]
    if validate_only:
        print(json.dumps(dict(status='validated',catalog=batch['catalog'],entries=len(batch['entries']))));return
    write(dest,batch);report()
def report():
    scope=read(BASE/'gop-scope.json');reviewed={};corrected=0
    for p in sorted((BASE/'reviewed').glob('*.json')):
        for r in read(p)['entries']:
            assert r['id'] not in reviewed,'Duplicate review identity'
            reviewed[r['id']]=r;corrected+=r['en_verdict']=='correct'
    out=dict(total=len(scope['entries']),assistant_reviewed=len(reviewed),remaining=len(scope['entries'])-len(reviewed),
        english_corrections=corrected,russian_direct_translations=len(reviewed),human_native_review=False,
        game_installed=False,old_ollama_draft_admitted=False)
    old=read(ROOT/'localization/ru-RU/scope.json')
    out['all_resource_counts']=dict(gop=len(scope['entries']),elf=len(old['elf_slots']),
        movie_cues=sum(len(m['cues']) for m in old['movies']),voice_stems=len(old['voice']),native_ui=len(old['native_ui']))
    out['all_resource_total']=sum(out['all_resource_counts'].values())
    out['all_resource_reviewed']=len(reviewed)
    out['all_resource_remaining']=out['all_resource_total']-len(reviewed)
    out['source_review_holds']=[dict(id=r['id'],reason=r['source_review_hold']) for r in reviewed.values() if r.get('source_review_hold')]
    es={}
    for p in sorted((BASE/'reviewed-es').glob('*.json')):
        for r in read(p)['entries']:
            assert r['id'] not in es,'Duplicate Spanish identity'
            es[r['id']]=r
    out['spanish_direct_translations']=len(es)
    out['spanish_gop_remaining']=len(scope['entries'])-len(es)
    write(BASE/'progress.json',out)
    print(json.dumps({k:out[k] for k in ('total','assistant_reviewed','remaining','english_corrections','all_resource_total')}|{'source_holds':len(out['source_review_holds'])}))
def spanish(path,validate_only=False):
    batch=read(path);scope={r['id']:r for r in read(BASE/'gop-scope.json')['entries']};entries=[]
    dest=BASE/'reviewed-es'/Path(path).name
    if not validate_only:assert not dest.exists(),'Spanish checkpoint already exists'
    assert batch.get('translations_es'),'Explicitly authored Spanish translations required'
    for index,text in batch['translations_es'].items():
        key=batch['catalog']+':string:'+index;source=scope[key]
        assert text.strip() and '\0' not in text,key
        assert TOKEN.findall(text)==TOKEN.findall(source['source_ja']),(key,'control tokens')
        assert not re.search('[\u3040-\u30ff\u3400-\u9fff\uac00-\ud7af\u0400-\u052f]',foreign_script_remainder(key,text,source['source_ja'])),(key,'foreign script')
        r=dict(id=key,index=int(index),source_ja=source['source_ja'],source_sha256=source['source_sha256'],
            es=text,status='assistant_translated_draft',review_method='Direct Japanese to Spanish in current assistant',human_native_review=False)
        if index in batch.get('source_holds',{}):r['source_review_hold']=batch['source_holds'][index]
        entries.append(r)
    if validate_only:print(json.dumps(dict(status='validated',locale='es-ES',entries=len(entries))));return
    write(dest,dict(catalog=batch['catalog'],locale='es-ES',image_locale='en-US',entries=entries));report()
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('action',choices=['prepare','show','admit','check','admit-es','check-es','report']);p.add_argument('--catalog');p.add_argument('--start',type=int,default=0);p.add_argument('--count',type=int,default=50);p.add_argument('--file',type=Path);a=p.parse_args()
    if a.action=='prepare':prepare()
    elif a.action=='show':show(a.catalog,a.start,a.count)
    elif a.action=='admit':admit(a.file)
    elif a.action=='check':admit(a.file,validate_only=True)
    elif a.action=='admit-es':spanish(a.file)
    elif a.action=='check-es':spanish(a.file,validate_only=True)
    else:report()
