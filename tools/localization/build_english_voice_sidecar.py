"""Assemble English voice captions against exact original identities, never Korean text."""
import argparse
import json
from pathlib import Path
import re
import unicodedata
from english_pack import ROOT,digest,write
from english_font import original_font
from english_sidecar_results import load_results

def read(path):return json.loads(path.read_text(encoding='utf8'))

def ascii_caption(text):
    text=unicodedata.normalize('NFKC',text.replace(chr(92)+'n',' '))
    for source,target in {'…':'...','–':'-','—':'-','−':'-','’':"'",'‘':"'",'“':'"','”':'"','・':' ', '「':'"','」':'"','『':'"','』':'"','【':'[','】':']','●':'','◆':'','♪':'','○':'Circle','×':'Cross','△':'Triangle','□':'Square','α':'alpha','β':'beta','γ':'gamma'}.items():text=text.replace(source,target)
    text=text.replace('。','.').replace('、',',').replace('※','Note:').replace('★','*').replace('☆','*').replace('·','-').replace('―','-')
    text=unicodedata.normalize('NFKD',text)
    text=''.join(c for c in text if not unicodedata.combining(c))
    text=' '.join(text.split())
    if not text or any(ord(c)<32 or ord(c)>126 for c in text):raise ValueError('Caption is not printable English ASCII')
    return text

def wrap_caption(text,glyphs):
    lines=[];line=''
    def width(s):
        pen=0;left=0;right=0
        for c in s:
            g=glyphs[ord(c)];left=min(left,pen+g['bearing_x']);right=max(right,pen+g['bearing_x']+g['width']);pen+=g['advance']
        return (max(pen,right)-left)*28/24
    for word in text.split():
        if width(word)>600:raise ValueError('Caption contains an over-wide word')
        candidate=(line+' '+word).strip()
        if line and width(candidate)>600:lines.append(line);line=word
        else:line=candidate
    if line:lines.append(line)
    parts=['\n'.join(lines[i:i+2]) for i in range(0,len(lines),2)]
    if len(parts)>16 or any(len(p.encode())>254 for p in parts):raise ValueError('Caption exceeds native page capacity')
    out='\f'.join(parts)
    return out

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--allow-partial',action='store_true');args=parser.parse_args()
    base=ROOT/'localization/en-US';audit=ROOT/'analysis/localization/voice-only-audit'
    lookup={};catalogs={};entries={};pending=[];provenance={};collisions=[]
    for path in (base/'structured').glob('*.json'):
        doc=read(path);assert digest((ROOT/doc['source_file']).read_bytes())==doc['source_sha256']
        catalogs[path.stem]={r['index']:r for r in doc['entries']}
        for row in doc['entries']:
            if row['translation']:lookup.setdefault(row['source'],set()).add(row['translation'])
    def add(stem,source,text,proof):
        assert re.fullmatch(r'[a-z0-9_]+/[0-9]{6}',stem)
        if stem in entries:
            if entries[stem]!=text:collisions.append(stem)
            return
        if not text:pending.append(dict(stem=stem,source=source,reason='untranslated'));return
        entries[stem]=text;provenance[stem]=proof
    battle=read(ROOT/'localization/subtitles/battle-voice/record-voice-text-map.json')
    for row in battle['records']:
        record=row['record_id'];speaker,voice=record[len('R_BattleText'):].rsplit('_',1);assert voice==row['voice_key']
        entry=catalogs['gop_battletext'].get(row['text_string_index']);text=entry['translation'] if entry else 'OK' if row['source']=='OK' else ''
        if not row['source']:continue
        if entry:assert entry['source']==row['source']
        add(speaker+'/'+voice,row['source'],text,dict(catalog='gop_battletext',index=row['text_string_index']))
    for group,mapname in [('gop_scenariotext','scenario-voice-map.json'),('gop_subquesttext','gop_subquesttext-voice-map.json'),('gop_systemtext','gop_systemtext-voice-map.json')]:
        mapping=read(audit/mapname)
        for row in mapping['records']:
            if not row['voice_key']:continue
            entry=catalogs[group].get(row['text_string_index'])
            text=entry['translation'] if entry else unicodedata.normalize('NFKC',row['source'])
            if entry:assert entry['source']==row['source']
            add(row['resolved_stem'],row['source'],text,dict(catalog=group,index=row['text_string_index']))
    assets={r['full_path']:r for r in read(audit/'actual-voice-assets.json')['assets']}
    pack=ROOT/'native/vfs/PS3_GAME/USRDIR/DATA/PACK';checked={}
    def check_audio(path,expected):
        if path not in checked:
            row=assets[path]
            with (pack/Path(row['pack']).name.upper()).open('rb') as f:f.seek(row['offset']);raw=f.read(row['size'])
            assert len(raw)==row['size'] and raw[:4]==b'RIFF';checked[path]=digest(raw)
        assert checked[path]==expected==assets[path]['sha256']
    for row in read(audit/'exact-alias-caption-map-all-tables.json')['entries']:
        check_audio(row['target_path'],row['sha256'])
        for path in row['matching_text_asset_paths']:check_audio(path,row['sha256'])
        targets=[entries[path.removeprefix('data/sound/voice/').removesuffix('.at3')] for path in row['matching_text_asset_paths'] if path.removeprefix('data/sound/voice/').removesuffix('.at3') in entries]
        if not targets:targets=sorted({t for source in row['source_text_candidates'] for t in lookup.get(source,())})
        add(row['resolved_stem'],' / '.join(row['source_text_candidates']),targets[0] if targets else '',dict(kind='exact_audio_alias',sha256=row['sha256']))
    scope,results=load_results()
    assert results['scope_sha256']==digest((base/'model-drafts/sidecar-scope.json').read_bytes())
    for row in scope['jobs']:
        if row['kind']!='voice':continue
        check_audio(row['source_path'],row['source_audio_sha256'])
        translated=results['entries'].get(row['id'],{});assert not translated or translated['source']==row['source']
        add(row['stem'],row['source'],translated.get('translation',''),dict(kind='ASR_original_identity',source_audio_sha256=row['source_audio_sha256'],listening_review=False))
    _,_,glyphs,_=original_font();ready={};durations={};long=[];unreachable=[]
    for stem,text in entries.items():
        try:
            asset_path='data/sound/voice/'+stem+'.at3';asset=assets.get(asset_path)
            # Unused GDAT voice IDs have no audio payload on the original disc.
            # Their full text remains translated in GOP; do not invent timing.
            if asset is None:unreachable.append(stem);continue
            check_audio(asset_path,asset['sha256'])
            duration=round(asset['fact_declared_samples']*1000/asset['sample_rate'])
            if not 0<duration<=120000:raise ValueError('Invalid original audio duration')
            ready[stem]=wrap_caption(ascii_caption(text),glyphs);durations[stem]=duration
        except ValueError as error:long.append(dict(stem=stem,text=text,reason=str(error),provenance=provenance[stem]))
    pending=[r for r in pending if r['stem'] not in entries]
    report=dict(known_english=len(entries),ready=len(ready),pending=pending,layout_holds=long,unreachable_GDAT_voice_ids_without_original_audio=unreachable,exact_audio_payloads_checked=len(checked),battle_preferred_collisions=collisions,paged_captions=sum('\f' in t for t in ready.values()),page_timing='Original RIFF fact duration, proportional English page bytes; listening review pending',human_listening_verified=False)
    write(base/'subtitles/voice-sidecar-report.json',report)
    if (pending or long) and not args.allow_partial:raise ValueError(f'English voice captions: {len(pending)} untranslated, {len(long)} layout holds')
    target=base/'subtitles/battle.en.tsv';target.parent.mkdir(parents=True,exist_ok=True)
    lines=['MACROSS_BATTLE_CAPTIONS_V2\ten-US']
    lines += [stem+'\t'+str(durations[stem])+'\t'+text.replace('\\','\\\\').replace('\n',r'\n').replace('\f',r'\f') for stem,text in sorted(ready.items())]
    target.write_bytes(('\n'.join(lines)+'\n').encode('ascii'))
    write(base/'subtitles/voice-sidecar-manifest.json',dict(path=str(target),sha256=digest(target.read_bytes()),entries=len(ready),untranslated=len(pending),layout_holds=len(long),complete=not pending and not long))
    print(dict(ready=len(ready),untranslated=len(pending),layout_holds=len(long)))
