"""Resumable local audio-only ASR for scene clips; no runtime or video writes."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import time
from transcribe_movie_audio import stamp


def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
    return h.hexdigest()


def save(path,doc):
    tmp=path.with_suffix('.next.json')
    tmp.write_text(json.dumps(doc,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    os.replace(tmp,path)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inventory',type=Path,default=Path('localization/subtitles/audio/batch-extraction-manifest.json'))
    p.add_argument('--output',type=Path,default=Path('localization/subtitles/asr/batch-small'))
    p.add_argument('--model',default='small')
    p.add_argument('--threads',type=int,default=2)
    n=p.parse_args()
    inventory=json.loads(n.inventory.read_text(encoding='utf-8'))
    if inventory['status']!='PASS' or inventory['completed']!=64:raise ValueError('Verified audio inventory required')
    if os.name=='nt':
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(),0x4000)
    from faster_whisper import WhisperModel
    model=WhisperModel(n.model,device='cpu',compute_type='int8',cpu_threads=n.threads,
                      num_workers=1,download_root=str(Path('.tools/subtitle-models').resolve()),local_files_only=True)
    selected=[r for r in inventory['results'] if r['audio_classification']=='scene_audio_dialogue_unverified']
    excluded=[dict(audio=r['WAV'],reason='song/opening/ending candidate; review separately')
              for r in inventory['results'] if r not in selected]
    n.output.mkdir(parents=True,exist_ok=True)
    manifest=dict(status='running',model=n.model,cpu_threads=n.threads,uploaded_audio=False,
        selected=len(selected),completed=0,failed=0,excluded=excluded,results=[],game_modified=False)
    marker=n.output/'batch-progress.json'
    save(marker,manifest)
    for original in selected:
        audio=Path(original['WAV']);movie=Path(original['source']).stem
        folder=n.output/movie;folder.mkdir(exist_ok=True)
        target=folder/'transcript.ja.json'
        started=time.monotonic()
        try:
            if digest(audio)!=original['WAV_SHA256']:raise ValueError('Audio SHA mismatch')
            if target.exists():
                report=json.loads(target.read_text(encoding='utf-8'))
                if report.get('status')!='completed_unreviewed_ASR' or report['audio_sha256']!=original['WAV_SHA256'] or report['model']!=n.model:
                    raise ValueError('Existing transcript differs; preserve for review')
            else:
                segments,info=model.transcribe(str(audio),language='ja',task='transcribe',beam_size=5,
                    vad_filter=True,condition_on_previous_text=False,word_timestamps=True)
                rows=[]
                with (folder/'segments.partial.jsonl').open('w',encoding='utf-8') as log:
                    for s in segments:
                        if not math.isfinite(s.start+s.end) or s.start<0 or s.end<=s.start or s.end>info.duration+0.1:
                            raise ValueError('Invalid cue interval')
                        flags=[]
                        if s.avg_logprob<-.75:flags.append('low_average_log_probability')
                        if s.no_speech_prob>.4:flags.append('possible_non_speech')
                        if s.compression_ratio>2.4:flags.append('possible_repetition')
                        row=dict(id=len(rows)+1,start=s.start,end=s.end,source_ja=s.text.strip(),
                            avg_logprob=s.avg_logprob,no_speech_prob=s.no_speech_prob,review_flags=flags,
                            review_status='unreviewed_machine_transcript',
                            words=[dict(start=w.start,end=w.end,word=w.word,probability=w.probability) for w in s.words or []])
                        rows.append(row);log.write(json.dumps(row,ensure_ascii=False)+'\n');log.flush()
                report=dict(status='completed_unreviewed_ASR',audio=str(audio),audio_sha256=original['WAV_SHA256'],
                    model=n.model,device='cpu',compute_type='int8',duration=info.duration,
                    first_audio_pts90k=original['first_pts90k'],human_listening_verified=False,
                    game_ready=False,segments=rows,elapsed_seconds=time.monotonic()-started)
                save(target,report)
                (folder/'transcript.ja.srt').write_text('\n\n'.join(
                    f"{r['id']}\n{stamp(r['start'])} --> {stamp(r['end'])}\n{r['source_ja']}" for r in rows)+'\n',encoding='utf-8')
            manifest['completed']+=1
            item=dict(movie=movie,status='completed_unreviewed_ASR',cues=len(report['segments']),transcript=str(target))
        except Exception as error:
            manifest['failed']+=1
            item=dict(movie=movie,status='failed',error=str(error))
        manifest['results'].append(item);save(marker,manifest)
        print(json.dumps(item,ensure_ascii=False),flush=True)
    manifest['status']='completed_unreviewed_ASR' if not manifest['failed'] else 'partial_failure'
    save(marker,manifest)


if __name__=='__main__':main()
