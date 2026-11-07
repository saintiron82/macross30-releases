"""Validate completed model comparisons and build a lean audio-only review page."""
import collections
import json
import math
import os
from pathlib import Path
import unicodedata

from transcribe_unresolved_battle_voice_batch import file_sha,atomic_dump

ROOT=Path(__file__).resolve().parents[2]
B=ROOT/'localization/subtitles/battle-voice/medium-crosscheck-v1'
load=lambda p:json.loads(p.read_text(encoding='utf8'))


def main():
    plan=load(B/'plan.json');progress=load(B/'progress.json');verified=load(B/'resume-verification.json')
    if progress['status']!='completed_unreviewed_Medium_crosscheck' or progress['completed']!=182 or progress['failed']:
        raise ValueError('Require completed182-clip Medium job')
    if verified['status']!='PASS' or verified['completed']!=182 or verified['new_ASR_inferences']!=0:
        raise ValueError('Missing complete zero-inference checkpoint verification')
    rows=[];flags=collections.Counter();word_count=0;timing=[];empty_small=[];empty_medium=[];new_for_empty=[]
    for entry in plan['entries']:
        path=B/f"{entry['priority_index']:04}-{entry['source_sha256'][:16]}.json"
        r=load(path)
        if r['source_sha256']!=entry['source_sha256'] or r['audio_sha256']!=entry['audio_sha256'] or r['Small_source_ja']!=entry['Small_source_ja']:
            raise ValueError('Frozen source identity mismatch')
        if r['model']['model_bin_sha256']!=plan['model']['model_bin_sha256']:raise ValueError('Model mismatch')
        small=r['Small_source_ja'];medium=' '.join(s['source_ja'] for s in r['segments'])
        if medium!=r['Medium_source_ja']:raise ValueError('Medium segment text join changed')
        exact=small==medium;nfkc=unicodedata.normalize('NFKC',small)==unicodedata.normalize('NFKC',medium)
        expected='exact_match' if exact else 'NFKC_match' if nfkc else 'different'
        if (r['exact_text_match'],r['NFKC_text_match'],r['comparison'])!=(exact,nfkc,expected):raise ValueError('Comparison category mismatch')
        per_flags=set()
        for cue in r['segments']:
            flags.update(cue['review_flags']);per_flags.update(cue['review_flags'])
            if cue['end']<=cue['start'] or cue['start']<0 or cue['end']>r['duration']+.05:
                timing.append(dict(index=entry['priority_index'],kind='cue',start=cue['start'],end=cue['end'],duration=r['duration']))
            for word in cue['words']:
                word_count+=1
                if not math.isfinite(word['probability']) or not 0<=word['probability']<=1:raise ValueError('Invalid raw word confidence')
        if not small:empty_small.append(entry['priority_index'])
        if not medium:empty_medium.append(entry['priority_index'])
        if not small and medium:new_for_empty.append(dict(index=entry['priority_index'],source_paths=entry['source_paths'],Medium=medium,review_flags=sorted(per_flags)))
        rows.append(dict(index=entry['priority_index'],source_sha256=entry['source_sha256'],source_paths=entry['source_paths'],
            audio=os.path.relpath(ROOT/entry['audio'],B).replace('\\','/'),checkpoint=path.name,duration=r['duration'],
            Small=small,Medium=medium,comparison=expected,lexical_normalized_match=r['lexical_normalized_match'],
            initial_KO_status=entry['initial_KO_status'],initial_translation_ko=entry['initial_translation_ko'],
            initial_uncertainty_note=entry['initial_uncertainty_note'],Medium_review_flags=sorted(per_flags)))
    counts=dict(collections.Counter(r['comparison'] for r in rows))
    comparison=load(B/'comparison-lists.json')
    if comparison['completed']!=182 or {k:counts.get(k,0) for k in ('exact_match','NFKC_match','different')}!=comparison['counts']:
        raise ValueError('Final comparison lists mismatch')
    original_inputs=[]
    for source in plan['inputs']:
        if file_sha(ROOT/source['Small_translation_input'])!=source['Small_translation_input_sha256']:raise ValueError('Original Small translation input changed')
        original_inputs.append(dict(batch=source['batch'],Small_translation_input_sha256=source['Small_translation_input_sha256'],
            initial_KO_draft_sha256=source['initial_KO_draft_sha256'],
            KO_draft_current_matches_initial=file_sha(ROOT/source['initial_KO_draft'])==source['initial_KO_draft_sha256']))
    report=dict(status='PASS',scope='Frozen-input/checkpoint/comparison validation; no listening verification',
        completed=182,failed=0,counts={k:counts.get(k,0) for k in ('exact_match','NFKC_match','different')},
        lexical_normalized_matches=sum(r['lexical_normalized_match'] for r in rows),Medium_cues=sum(len(load(B/r['checkpoint'])['segments']) for r in rows),
        word_confidences_recorded=word_count,word_confidences_finite_in_0_1=True,Medium_review_flags=dict(flags),
        empty_Small_indices=empty_small,empty_Medium_indices=empty_medium,Medium_text_for_empty_Small=new_for_empty,
        cue_timing_anomalies=timing,source_inputs=original_inputs,
        no_new_ASR_on_resume_verification=True,no_audio_extraction_or_reencoding=True,
        human_listening_verified=False,translation_created=False,runtime_catalog_modified=False,uploaded_audio=False,
        caveat='Agreement is not proof and changed text is not automatically an improvement. Generic hallucinations and uncertain controls remain for review.')
    atomic_dump(B/'validation.json',report)
    atomic_dump(B/'review-index.json',dict(summary=report,entries=rows))
    data=json.dumps(rows,ensure_ascii=False,separators=(',',':')).replace('<',chr(92)+'u003c')
    html='''<!doctype html><html lang="ko"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Small·Medium 원음 교차 검토</title>
<style>body{font:16px/1.55 system-ui,sans-serif;max-width:1050px;margin:24px auto;padding:0 16px;color:#ecf2f8;background:#101820}h1{font-size:24px}h2{font-size:16px;overflow-wrap:anywhere}article{background:#1d2b39;padding:12px;margin:12px 0;border-radius:7px}small{color:#b1c6d8}.warn{color:#ffd089}button,input,select{font:inherit;padding:6px 9px}button{cursor:pointer}audio{width:100%}a{color:#8ad4ff}#player{position:sticky;top:0;background:#101820;border-bottom:1px solid #456;padding:10px 0}</style>
<h1>불확실한 음성 182개: Small·Medium 교차 검토</h1><p>두 모델 모두 미검수 자동 인식입니다. 일치해도 청취 검증은 아니며, Medium이 다르게 읽었다고 더 정확하다는 뜻도 아닙니다. 원음은 기존 float32 WAV를 그대로 사용하고 새 번역·게임 적용은 하지 않았습니다.</p>
<div id="player"><strong id="playing">원음 선택</strong><audio id="audio" controls preload="none"></audio></div><p><input id="search" placeholder="화자·파일·일본어 검색" style="width:58%"> <select id="filter"><option value="all">전체</option><option value="different">다름</option><option value="exact_match">완전 일치</option><option value="NFKC_match">NFKC만 일치</option></select> <span id="count"></span></p><p><button id="prev">이전</button> <span id="pages"></span> <button id="next">다음</button></p><main id="list"></main>
<script>const rows='''+data+''';const $=id=>document.getElementById(id),size=10;let list=rows,page=0;function e(tag,text){const x=document.createElement(tag);x.textContent=text;return x}function select(){const q=$('search').value.toLowerCase(),f=$('filter').value;list=rows.filter(r=>(f==='all'||r.comparison===f)&&(r.source_paths.join(' ')+' '+r.Small+' '+r.Medium).toLowerCase().includes(q));page=0;draw()}function draw(){const max=Math.max(1,Math.ceil(list.length/size));page=Math.max(0,Math.min(page,max-1));$('list').replaceChildren();$('count').textContent=list.length+'개';$('pages').textContent=(page+1)+' / '+max;$('prev').disabled=page===0;$('next').disabled=page>=max-1;for(const r of list.slice(page*size,(page+1)*size)){const a=e('article','');a.append(e('h2',r.source_paths.join(' / ')));const b=e('button','원음 재생');b.onclick=()=>{$('playing').textContent=r.source_paths[0];$('audio').src=r.audio;$('audio').play().catch(()=>{})};a.append(b,e('small',' '+r.duration.toFixed(2)+'초 · '+r.comparison));a.append(e('p','Small: '+(r.Small||'[자동 인식 없음]')),e('p','Medium: '+(r.Medium||'[자동 인식 없음]')));if(r.initial_translation_ko)a.append(e('p','시작 시 한국어 초안: '+r.initial_translation_ko));a.append(e('small','초기 검토 사유: '+r.initial_uncertainty_note));const f=e('p',r.Medium_review_flags.join(', '));f.className='warn';a.append(f);const link=e('a','단어 확률·원문 SHA');link.href=r.checkpoint;a.append(link);$('list').append(a)}}$('search').oninput=select;$('filter').onchange=select;$('prev').onclick=()=>{page--;draw()};$('next').onclick=()=>{page++;draw()};draw();</script></html>'''
    (B/'review.html').write_bytes(html.encode('utf8'))
    print(json.dumps({k:v for k,v in report.items() if k not in ('Medium_review_flags','source_inputs','cue_timing_anomalies','Medium_text_for_empty_Small')},ensure_ascii=False,indent=2))


if __name__=='__main__':main()
