"""Make a dependency-free local audio/cue review page; never edits game files."""
import argparse
import html
import json
import os
from pathlib import Path


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--transcript',type=Path,required=True)
    p.add_argument('--audio',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    n=p.parse_args()
    if n.output.exists(): raise SystemExit('Choose a new review filename')
    doc=json.loads(n.transcript.read_text(encoding='utf-8'))
    rows=doc['segments']
    data=json.dumps(rows,ensure_ascii=False).replace('<','\\u003c')
    src=html.escape(os.path.relpath(n.audio,n.output.parent).replace('\\','/'),quote=True)
    page='''<!doctype html><html lang="ko"><meta charset="utf-8">
<title>첫 미션 음성·자막 검토</title><style>
body{background:#151a23;color:#e8eef6;font:17px/1.65 system-ui;max-width:1000px;margin:30px auto;padding:16px}
audio{width:100%}#ko{font-size:29px;min-height:100px;white-space:pre-line}#ja{color:#b7c7db;min-height:60px}
.player{position:sticky;top:0;background:#151a23;padding:16px;border-bottom:1px solid #456}
button{background:#26364a;color:inherit;border:1px solid #52657d;padding:8px;cursor:pointer}
table{width:100%;border-collapse:collapse}td{border-bottom:1px solid #345;padding:12px;vertical-align:top}
.selected{background:#263c51}.note{color:#edc36c}small{color:#b7c7db}</style>
<h1>첫 미션 음성·자막 검토</h1><p class="note">자동 전사와 번역 초안입니다. 원음 청취 검수 완료본이 아니며 게임에는 적용하지 않았습니다.</p>
<div class="player"><label><input type="checkbox" id="subtitles" checked> 자막 표시 (검토용)</label><audio id="audio" controls preload="metadata" src="AUDIO_SOURCE"></audio>
<div id="ko">재생하거나 아래 대사를 선택하세요.</div><div id="ja"></div><small id="state"></small></div>
<table id="list"></table><script>
const cues=CUE_DATA,a=document.querySelector('#audio'),k=document.querySelector('#ko'),j=document.querySelector('#ja'),s=document.querySelector('#state'),list=document.querySelector('#list'),toggle=document.querySelector('#subtitles');
try{toggle.checked=localStorage.getItem('macross-audio-review-subtitles')!=='off'}catch(e){}
toggle.onchange=()=>{try{localStorage.setItem('macross-audio-review-subtitles',toggle.checked?'on':'off')}catch(e){}update()};
for(const c of cues){const tr=document.createElement('tr');tr.id='cue'+c.id;
 const t=document.createElement('td'),b=document.createElement('button');b.textContent=c.start.toFixed(2)+'초';b.onclick=()=>{a.currentTime=c.start;a.play()};t.append(b);tr.append(t);
 const text=document.createElement('td');const ko=document.createElement('div');ko.textContent=c.translation_ko||'한국어 번역 대기';const ja=document.createElement('small');ja.textContent=c.source_ja;text.append(ko,ja);tr.append(text);list.append(tr)}
let previous=null;function update(){const c=toggle.checked?cues.find(x=>a.currentTime>=x.start&&a.currentTime<x.end):null;k.textContent=c?(c.translation_ko||'한국어 번역 대기'):'';j.textContent=c?c.source_ja:'';s.textContent=c?(c.translation_status||c.review_status||'검수 대기'):(toggle.checked?'':'자막 꺼짐');
if(previous!==c){if(previous)document.querySelector('#cue'+previous.id).classList.remove('selected');if(c)document.querySelector('#cue'+c.id).classList.add('selected');previous=c}}
a.addEventListener('timeupdate',update);a.addEventListener('seeked',update);a.addEventListener('ended',update);
</script></html>'''.replace('AUDIO_SOURCE',src).replace('CUE_DATA',data)
    n.output.parent.mkdir(parents=True,exist_ok=True)
    n.output.write_text(page,encoding='utf-8')
    print(n.output)


if __name__=='__main__':main()
