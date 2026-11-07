"""Prepare three bounded draft-translation batches from completed local ASR."""
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
base=ROOT/'localization/subtitles/battle-voice/audio-unresolved-v2'
load=lambda p:json.loads(p.read_text(encoding='utf8'))
assert load(base/'validation.json')['status']=='PASS'
reconciliation=load(base/'source-reconciliation.json')
records={r['source_sha256']:r for r in reconciliation['entries'] if r['classification']=='unresolved_actual_audio'}
assert len(records)==413
entries=[]
for path in sorted(base.glob('*/transcript.ja.json')):
    data=load(path);key=data['original_AT3_sha256']
    if key not in records:continue
    assert data['status']=='completed_unreviewed_ASR'
    source=' '.join(s['source_ja'].strip() for s in data['segments'])
    entries.append(dict(source_sha256=key,source_ja=source,
        source_paths=records[key]['original_asset_paths'],native_speaker_IDSTR_present=records[key]['native_pilot_IDSTR_present'],
        transcript=str(path.relative_to(ROOT)),transcript_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        duration=data['duration'],review_flags=sorted({f for s in data['segments'] for f in s['review_flags']}),
        segments=[{k:s[k] for k in ('id','start','end','source_ja','avg_logprob','no_speech_prob','review_flags')} for s in data['segments']]))
assert len(entries)==413
bins=[[] for _ in range(3)];weights=[0]*3
for row in sorted(entries,key=lambda r:len(r['source_ja']),reverse=True):
    index=min(range(3),key=lambda i:weights[i]);bins[index].append(row);weights[index]+=max(1,len(row['source_ja']))
out=ROOT/'localization/subtitles/battle-voice/translation-batches';out.mkdir(exist_ok=True)
for i,rows in enumerate(bins,1):
    doc=dict(status='ASR_TEXT_TRANSLATION_DRAFT_INPUT',batch=i,clips=len(rows),Japanese_characters=sum(len(r['source_ja']) for r in rows),
        caveat='Machine ASR is unreviewed; translate clear text only, flag uncertainty/non-speech, never claim listening or invent missing dialogue.',entries=sorted(rows,key=lambda r:r['source_paths'][0]))
    (out/f'batch-{i}.json').write_bytes((json.dumps(doc,ensure_ascii=False,indent=2)+'\n').encode())
print([(i+1,len(b),weights[i]) for i,b in enumerate(bins)])
