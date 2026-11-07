"""Export review-only Korean sidecars with guarded source timing and audio PTS."""
import argparse
import hashlib
import json
import math
from pathlib import Path
from transcribe_movie_audio import stamp
from caption_text import clean_caption_text


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--draft',type=Path,required=True)
    p.add_argument('--source-transcript',type=Path,required=True)
    p.add_argument('--audio-validation',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    n=p.parse_args()
    load=lambda path:json.loads(path.read_text(encoding='utf-8'))
    draft,source,audio=map(load,(n.draft,n.source_transcript,n.audio_validation))
    if n.output.exists():raise SystemExit('Choose a new export directory')
    cues=draft['cues'];raw=source['segments']
    if len(cues)!=len(raw) or source['audio_sha256']!=audio['WAV_SHA256']:
        raise ValueError('Source cue count/audio identity mismatch')
    rows=[];previous=0
    for cue,original in zip(cues,raw):
        if any(cue[k]!=original[k] for k in ('id','start','end','source_ja')):
            raise ValueError('Source text/timing altered')
        begin,end=cue['start'],cue['end']
        if not math.isfinite(begin+end) or not previous<=begin<end<=audio['seconds']:
            raise ValueError('Invalid/overlapping cue timing')
        text=cue['translation_ko_draft']
        if not text.strip() or '\0' in text:raise ValueError('Empty/NUL target')
        text=clean_caption_text(text)
        previous=end
        if not text:continue
        rows.append(dict(id=cue['id'],start=begin,end=end,source_ja=cue['source_ja'],
            translation_ko=text,translation_status='검수 필요'+(' · 우선 확인' if cue['audio_review_priority']=='high' else ''),
            needs_audio_review=cue['needs_audio_review'],audio_review_priority=cue['audio_review_priority'],
            start_pts90k=audio['first_pts90k']+round(begin*90000),end_pts90k=audio['first_pts90k']+round(end*90000)))
        previous=end
    hashes={str(path):hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (n.draft,n.source_transcript,n.audio_validation)}
    report=dict(status='review_only_not_game_ready',movie=draft['movie'],
        audio=audio['WAV'],audio_sha256=audio['WAV_SHA256'],
        time_origin_pts90k=audio['first_pts90k'],timebase=90000,
        source_timing_unchanged=True,human_listening_verified=False,game_ready=False,
        high_priority_cues=[r['id'] for r in rows if r['audio_review_priority']=='high'],
        source_hashes=hashes,segments=rows)
    n.output.mkdir(parents=True)
    (n.output/'020.review.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    (n.output/'020.draft.ko.srt').write_text('\n\n'.join(
        f"{r['id']}\n{stamp(r['start'])} --> {stamp(r['end'])}\n{r['translation_ko']}" for r in rows)+'\n',encoding='utf-8')
    (n.output/'020.draft.ko.vtt').write_text('WEBVTT\n\n'+'\n\n'.join(
        f"{r['id']}\n{stamp(r['start']).replace(',','.')} --> {stamp(r['end']).replace(',','.')}\n{r['translation_ko']}" for r in rows)+'\n',encoding='utf-8')
    print(json.dumps(dict(cues=len(rows),high_priority=report['high_priority_cues'],game_ready=False)))


if __name__=='__main__':main()
