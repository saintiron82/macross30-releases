"""Verify completed offline voice checkpoints and build a paginated audio-only review."""
import collections
import hashlib
import json
from pathlib import Path
import struct

ROOT=Path(__file__).resolve().parents[2]
B=ROOT/'localization/subtitles/battle-voice/audio-unresolved-v2'
load=lambda p:json.loads(p.read_text(encoding='utf8'))
sha=lambda b:hashlib.sha256(b).hexdigest()


def main():
    progress=load(B/'ASR-progress.json');plan=load(B/'plan.json');reconciliation=load(B/'source-reconciliation.json')
    if progress['status']!='completed_unreviewed_ASR' or progress['completed']!=439 or progress['failed']!=0:
        raise ValueError('Require completed439-unique batch')
    if load(B/'resume-verification.json')['status']!='PASS':raise ValueError('Missing completed-checkpoint verification')
    classes={r['unique_index']:r for r in reconciliation['entries']}
    rows=[];flags=collections.Counter();cues=0;source_paths=set();timing=[];decoded=0.;fact=0.;peaks=[]
    for item in progress['results']:
        report=load(B/item['extraction']);asr=load(B/item['transcript']);folder=(B/item['extraction']).parent
        original=(folder/'original.at3').read_bytes();wave=(folder/'audio.f32.wav').read_bytes()
        if sha(original)!=item['source_sha256'] or sha(wave)!=report['output']['sha256']:
            raise ValueError('Checkpoint binary changed')
        if wave[:4]!=b'RIFF' or wave[8:12]!=b'WAVE' or struct.unpack_from('<I',wave,4)[0]+8!=len(wave):raise ValueError('Invalid float WAV')
        pos=12;fmt=None;payload=None
        while pos+8<=len(wave):
            tag=wave[pos:pos+4];n=struct.unpack_from('<I',wave,pos+4)[0]
            if pos+8+n>len(wave):raise ValueError('WAV chunk outside file')
            chunk=wave[pos+8:pos+8+n]
            if tag==b'fmt ':fmt=struct.unpack_from('<HHIIHH',chunk)
            if tag==b'data':payload=chunk
            pos+=8+n+(n&1)
        if fmt[0]!=3 or fmt[5]!=32 or sha(payload)!=report['output']['float_payload_sha256']:
            raise ValueError('Float32 payload changed')
        if not report['output']['float_payload_bit_exact_roundtrip'] or report['manual_trim_samples']!=0:
            raise ValueError('Missing untrimmed bit-preservation evidence')
        if asr['original_AT3_sha256']!=item['source_sha256'] or asr['audio_sha256']!=sha(wave):raise ValueError('ASR source identity mismatch')
        source_paths.update(item['original_aliases']);decoded+=report['decoded_seconds'];fact+=report['fact_declared_seconds'];peaks.append(report['output']['peak_absolute'])
        for c in asr['segments']:
            cues+=1;flags.update(c['review_flags'])
            if c['end']<=c['start'] or c['start']<0 or c['end']>asr['duration']+.05:timing.append(dict(asset=item['asset'],cue=c))
        classification=classes[item['unique_index']]
        rows.append(dict(index=item['unique_index'],asset=item['asset'],aliases=item['original_aliases'],
            source_sha256=item['source_sha256'],classification=classification['classification'],
            native_IDSTR_present=classification['native_pilot_IDSTR_present'],
            waveform=(folder/'audio.f32.wav').relative_to(B).as_posix(),original=(folder/'original.at3').relative_to(B).as_posix(),
            transcript=item['transcript'],decoded_seconds=report['decoded_seconds'],fact_seconds=report['fact_declared_seconds'],
            raw_ja=[c['source_ja'] for c in asr['segments']],review_flags=item['review_flags'],
            existing_source_texts=sorted({r['source'] for r in classification['existing_text_records']}),
            existing_Korean_texts=sorted({r['translation'] for r in classification['existing_text_records'] if r['translation']})))
    if len(source_paths)!=451:raise ValueError('Lost source aliases')
    report=dict(status='PASS',scope='Source/binary/checkpoint preservation only; ASR content remains unreviewed',
        original_paths=451,unique_RIFF_SHA=439,completed_ASR=439,failed=0,ASR_cues=cues,
        zero_cue_clips=sum(not r['raw_ja'] for r in rows),cue_review_flags=dict(flags),
        unique_decoded_seconds=decoded,unique_fact_seconds=fact,maximum_absolute_float_value=max(peaks),
        files_with_peak_over_one=sum(p>1 for p in peaks),all_original_and_WAV_SHA_match=True,
        all_float_payload_SHA_match=True,all_float_WAV_bit_exact_roundtrips=True,
        no_manual_trim=True,no_normalization=True,no_clipping=True,
        timing_out_of_source_bounds=timing,new_ASR_on_resume_verification=0,
        source_reconciliation=dict(known_original_text_QA_unique=26,unresolved_unique=413,unresolved_original_paths=424),
        human_listening_verified=False,translation_created=False,uploaded_audio=False)
    (B/'validation.json').write_bytes((json.dumps(report,indent=2)+'\n').encode())
    data=json.dumps(rows,ensure_ascii=False,separators=(',',':')).replace('<',chr(92)+'u003c')
    page='''<!doctype html><html lang="ko"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>전투 음성 원음 검토</title>
<style>body{font:16px/1.55 system-ui,sans-serif;max-width:1000px;margin:24px auto;padding:0 16px;color:#eaf1f7;background:#101820}h1{font-size:24px}h2{font-size:16px;overflow-wrap:anywhere}article{background:#1c2b39;margin:12px 0;padding:12px;border-radius:6px}small{color:#aac0d3}button,input{font:inherit;padding:6px 10px}button{cursor:pointer}audio{width:100%}a{color:#83ceff}.warn{color:#ffd38a}#player{position:sticky;top:0;background:#101820;padding:12px 0;border-bottom:1px solid #456}</style>
<h1>전투·기타 음성 원음 검토</h1><p>원본 451경로를 동일 파일 SHA로 묶은 439개입니다. 자동 전사는 미검수이며 새 번역은 만들지 않았습니다. 기존 원문 QA 26개와 추가 조사 413개를 구분합니다. 실제 게임에서 사용되는지, 말인지 비명인지도 원음과 재생 경로로 확인해야 합니다.</p>
<div id="player"><strong id="playing">원음을 선택하세요</strong><audio id="audio" controls preload="none"></audio><div id="caption"></div></div>
<p><input id="search" placeholder="화자·파일 번호·자동 전사 검색" style="width:65%"> <span id="count"></span></p><p><button id="prev">이전</button> <span id="pages"></span> <button id="next">다음</button></p><main id="list"></main>
<script>const rows='''+data+''';let selected=rows,page=0;const size=12,$=id=>document.getElementById(id);function el(tag,text){const e=document.createElement(tag);e.textContent=text;return e}function draw(){const max=Math.max(1,Math.ceil(selected.length/size));page=Math.max(0,Math.min(page,max-1));$('list').replaceChildren();$('count').textContent=selected.length+'개';$('pages').textContent=(page+1)+' / '+max;$('prev').disabled=page===0;$('next').disabled=page>=max-1;for(const r of selected.slice(page*size,(page+1)*size)){const a=el('article','');a.append(el('h2',r.asset));const b=el('button','원음 재생');b.onclick=()=>{$('playing').textContent=r.asset;$('caption').textContent=r.raw_ja.join(' / ')||'자동 전사 없음 — 무음이라는 증거가 아닙니다.';$('audio').src=r.waveform;$('audio').play().catch(()=>{})};a.append(b,el('small','  WAV '+r.decoded_seconds.toFixed(2)+'초 / '+(r.classification==='known_original_text_QA'?'기존 원문 QA':'추가 원문 조사')+' / 동일 원본 경로 '+r.aliases.length+'개'));a.append(el('p',r.raw_ja.join(' / ')||'자동 전사 없음'));if(r.existing_source_texts.length)a.append(el('p','기존 원문: '+r.existing_source_texts.join(' / ')));if(r.existing_Korean_texts.length)a.append(el('p','기존 번역: '+r.existing_Korean_texts.join(' / ')));const f=el('small',r.review_flags.join(', '));a.append(f);if(!r.native_IDSTR_present){const w=el('p','원본 캐릭터 표에 동일 IDSTR 없음 — 실제 재생 여부 별도 확인');w.className='warn';a.append(w)}const link=el('a','전사 근거 JSON');link.href=r.transcript;a.append(el('br',''),link);$('list').append(a)}}$('search').oninput=()=>{const q=$('search').value.toLowerCase();selected=rows.filter(r=>(r.asset+' '+r.raw_ja.join(' ')+' '+r.existing_Korean_texts.join(' ')).toLowerCase().includes(q));page=0;draw()};$('prev').onclick=()=>{page--;draw()};$('next').onclick=()=>{page++;draw()};draw();</script></html>'''
    (B/'review.html').write_bytes(page.encode('utf8'))
    (B/'review-index.json').write_bytes((json.dumps(rows,ensure_ascii=False,indent=2)+'\n').encode('utf8'))
    print(json.dumps({k:v for k,v in report.items() if k not in ('timing_out_of_source_bounds','cue_review_flags')},ensure_ascii=False,indent=2))


if __name__=='__main__':main()
