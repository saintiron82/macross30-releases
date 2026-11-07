"""Publish clean sidecars and atomically switch the active registry to them."""
import hashlib
import json
import os
from pathlib import Path
import re

from caption_text import clean_caption_text,has_work_annotation

ROOT=Path(__file__).resolve().parents[2]
B=ROOT/'localization/subtitles'
runtime=B/'runtime'
registry=runtime/'tracks.tsv'
old=registry.read_bytes()
rows=old.decode('utf8').splitlines();assert rows[0]=='MACROSS_SUBTITLES_V1'
version=hashlib.sha256(old).hexdigest()[:12]
backup=B/'runtime-history'/('before-note-removal-'+version);backup.mkdir(parents=True,exist_ok=True)
if not (backup/'tracks.tsv').exists():(backup/'tracks.tsv').write_bytes(old)
report=[];newrows=[rows[0]]
for line in rows[1:]:
    if not line:continue
    fields=line.split('\t');assert len(fields)==7
    source=runtime/fields[1];data=source.read_bytes()
    assert hashlib.sha256(data).hexdigest()==fields[5]
    if not (backup/source.name).exists():(backup/source.name).write_bytes(data)
    blocks=re.split(r'\n\s*\n',data.decode('utf8').replace('\r\n','\n').strip())
    output=[];changed=0;dropped=[];mapping=[]
    for block in blocks:
        lines=block.splitlines();assert len(lines)>=3 and lines[0].isdigit() and ' --> ' in lines[1]
        original='\n'.join(lines[2:]);text=clean_caption_text(original)
        changed+=text!=original
        assert not has_work_annotation(text)
        if not text:dropped.append(int(lines[0]));continue
        mapping.append(dict(original_id=int(lines[0]),new_id=len(output)+1,timing=lines[1]))
        output.append(f'{len(output)+1}\n{lines[1]}\n{text}')
    assert output,'Cannot publish an empty movie track'
    content=('\n\n'.join(output)+'\n').encode('utf8')
    movie=fields[0].rsplit('.',1)[0]
    name=movie+'.clean.ko.srt';dest=runtime/name
    temp=dest.with_suffix('.tmp');temp.write_bytes(content);os.replace(temp,dest)
    fields[1]=name;fields[5]=hashlib.sha256(content).hexdigest();newrows.append('\t'.join(fields))
    report.append(dict(movie=fields[0],source=source.name,output=name,changed_cues=changed,
                       original_cues=len(blocks),clean_cues=len(output),dropped_annotation_only_cues=dropped,
                       surviving_cue_times_unchanged=True,cue_id_map=mapping))
new=('\n'.join(newrows)+'\n').encode('utf8')
temp=runtime/'tracks.clean.tmp';temp.write_bytes(new);os.replace(temp,registry)
doc=dict(status='PASS',tracks=len(report),changed_cues=sum(r['changed_cues'] for r in report),
    original_cues=sum(r['original_cues'] for r in report),clean_cues=sum(r['clean_cues'] for r in report),
    registry_sha256=hashlib.sha256(new).hexdigest(),tracks_detail=report,
    backup=str(backup.relative_to(ROOT)),active_tracks_have_work_annotations=False,
    existing_loaded_movie_memory_modified=False)
(runtime/'note-removal.json').write_text(json.dumps(doc,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
print(json.dumps({k:v for k,v in doc.items() if k!='tracks_detail'},ensure_ascii=False))
