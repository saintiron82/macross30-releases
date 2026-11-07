"""Run only the six pinned, authorized offline Medium acoustic rechecks."""
import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import time
import unicodedata

from transcribe_unresolved_battle_voice_batch import atomic_dump, below_normal, file_sha, owner_is_live

ROOT=Path(__file__).resolve().parents[2]
BASE=ROOT/'localization/subtitles/battle-voice/medium-crosscheck-v1'
PLAN=BASE/'acoustic-recheck-plan-v1/plan.json'
PIN='24e333f59ae5f6d5a518ec3fa9138699cce81c4a1ccb3561fb3f354be77c323a'
OUT=BASE/'acoustic-recheck-results-v1'
load=lambda p:json.loads(p.read_text(encoding='utf8'))
PARAMETERS=dict(language='ja',task='transcribe',beam_size=10,temperature=0.0,
    vad_filter=False,word_timestamps=True,condition_on_previous_text=False,
    initial_prompt=None,prefix=None,hotwords=None)


def compare(before,after):
    return 'exact_match' if before==after else 'NFKC_match' if unicodedata.normalize('NFKC',before)==unicodedata.normalize('NFKC',after) else 'different'


def checkpoint_valid(result,row):
    return (result['source_sha256']==row['source_sha256'] and result['audio_sha256']==row['audio_sha256']
        and result['plan_sha256']==PIN and result['parameters']==PARAMETERS
        and result['Small_source_ja']==row['Small_source_ja']
        and result['baseline_Medium_source_ja']==row['Medium_source_ja'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resume',action='store_true')
    parser.add_argument('--verify-only',action='store_true')
    args=parser.parse_args()
    if file_sha(PLAN)!=PIN:raise ValueError('Authorized plan SHA changed')
    plan=load(PLAN)
    if len(plan['entries'])!=6 or {r['index'] for r in plan['entries']}!={3,13,14,47,50,73}:raise ValueError('Six-clip scope changed')
    OUT.mkdir(exist_ok=True)
    progress_path=OUT/'progress.json'
    if progress_path.exists():
        previous=load(progress_path)
        if owner_is_live(previous['pid']):raise ValueError('Existing owner still live; duplicate execution refused')
        if not args.resume:raise ValueError('Explicit resume required')
    elif args.verify_only:raise ValueError('No previous checkpoints')
    priority=below_normal()
    for name in ('HF_HUB_OFFLINE','TRANSFORMERS_OFFLINE','HF_HUB_DISABLE_TELEMETRY'):os.environ[name]='1'
    model_path=ROOT/plan['model']['path']
    if file_sha(model_path/'model.bin')!=plan['model']['model_bin_sha256']:raise ValueError('Cached Medium changed')
    frozen=load(BASE/'plan.json')
    if file_sha(BASE/'plan.json')!=plan['source_plan_sha256']:raise ValueError('Frozen baseline plan changed')
    origins={r['priority_index']:r for r in frozen['entries']}
    assets={a['full_path']:a for a in load(ROOT/'analysis/localization/voice-only-audit/actual-voice-assets.json')['assets']}
    pack=ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'
    for row in plan['entries']:
        origin=origins[row['index']]
        if file_sha(ROOT/row['audio'])!=row['audio_sha256']:raise ValueError('Untrimmed WAV changed')
        if file_sha(ROOT/row['Small_transcript'])!=row['Small_transcript_sha256']:raise ValueError('Small checkpoint changed')
        if file_sha(BASE/row['baseline_checkpoint'])!=row['baseline_checkpoint_sha256']:raise ValueError('Medium baseline changed')
        if file_sha(ROOT/origin['batch_input']['Small_translation_input'])!=row['frozen_input_sha256']:raise ValueError('Small input changed')
        if row['trim_applied'] or row['trim_start_seconds']!=0:raise ValueError('No trims authorized')
        for path in row['source_paths']:
            asset=assets[path]
            with (pack/asset['pack']).open('rb') as f:
                f.seek(asset['offset']);raw=f.read(asset['size'])
            import hashlib
            if len(raw)!=asset['size'] or raw[:4]!=b'RIFF' or hashlib.sha256(raw).hexdigest()!=row['source_sha256']:raise ValueError('Original RIFF changed')
    if args.verify_only:
        for row in plan['entries']:
            result=load(OUT/f"{row['index']:04}-{row['source_sha256'][:16]}.json")
            if not checkpoint_valid(result,row):raise ValueError('Result provenance changed')
        atomic_dump(OUT/'resume-verification.json',dict(status='PASS',completed=6,new_inferences=0,plan_sha256=PIN))
        print('PASS six checkpoint identities; zero new inference',flush=True)
        return
    started=time.monotonic();results=[]
    progress=dict(status='running',pid=os.getpid(),priority_class=priority,plan_sha256=PIN,
        total=6,completed=0,failed=0,started_utc=datetime.now(timezone.utc).isoformat(),phase='loading_cached_model')
    atomic_dump(progress_path,progress)
    print(json.dumps(dict(event='started',**progress)),flush=True)
    from faster_whisper import WhisperModel
    model=WhisperModel(str(model_path),device='cpu',compute_type='int8',cpu_threads=2,num_workers=1,local_files_only=True)
    try:
        for row in plan['entries']:
            index=row['index'];path=OUT/f"{index:04}-{row['source_sha256'][:16]}.json"
            progress.update(phase='transcribing',current_index=index)
            atomic_dump(progress_path,progress)
            if path.exists():
                result=load(path)
                if not checkpoint_valid(result,row):raise ValueError('Existing checkpoint identity mismatch')
            else:
                tick=time.monotonic();segments,info=model.transcribe(str(ROOT/row['audio']),**PARAMETERS)
                cues=[]
                for s in segments:
                    if not math.isfinite(s.start+s.end) or s.start<0 or s.end<s.start:raise ValueError('Invalid segment time')
                    words=[dict(start=w.start,end=w.end,word=w.word,probability=w.probability) for w in s.words or []]
                    if any(not math.isfinite(w['probability']) or not 0<=w['probability']<=1 for w in words):raise ValueError('Invalid word probability')
                    cues.append(dict(id=len(cues)+1,start=s.start,end=s.end,source_ja=s.text.strip(),avg_logprob=s.avg_logprob,no_speech_prob=s.no_speech_prob,compression_ratio=s.compression_ratio,words=words))
                text=' '.join(c['source_ja'] for c in cues)
                result=dict(status='UNREVIEWED_ACOUSTIC_RECHECK',index=index,source_sha256=row['source_sha256'],
                    source_paths=row['source_paths'],audio=row['audio'],audio_sha256=row['audio_sha256'],plan_sha256=PIN,
                    baseline_checkpoint_sha256=row['baseline_checkpoint_sha256'],frozen_input_sha256=row['frozen_input_sha256'],
                    model=plan['model'],parameters=PARAMETERS,priority_class=priority,trim_applied=False,
                    Small_source_ja=row['Small_source_ja'],baseline_Medium_source_ja=row['Medium_source_ja'],
                    recheck_source_ja=text,comparison_to_Small=compare(row['Small_source_ja'],text),
                    comparison_to_Medium=compare(row['Medium_source_ja'],text),segments=cues,duration=info.duration,
                    elapsed_seconds=time.monotonic()-tick,human_listening_verified=False,translation_created=False,
                    caveat='Different raw text is not automatically an improvement; no cues is not proof of silence.')
                atomic_dump(path,result)
            results.append(dict(index=index,checkpoint=path.name,source_sha256=row['source_sha256'],
                Small=row['Small_source_ja'],Medium=row['Medium_source_ja'],recheck=result['recheck_source_ja'],
                comparison_to_Medium=result['comparison_to_Medium'],comparison_to_Small=result['comparison_to_Small']))
            progress.update(completed=len(results),elapsed_seconds=time.monotonic()-started)
            atomic_dump(progress_path,progress)
            atomic_dump(OUT/'comparison.json',dict(status='UNREVIEWED_RAW_COMPARISON',entries=results))
            print(json.dumps(dict(event='clip_complete',**results[-1]),ensure_ascii=False),flush=True)
        progress.update(status='completed',phase='terminal',finished_utc=datetime.now(timezone.utc).isoformat())
        atomic_dump(progress_path,progress)
        print(json.dumps(dict(event='terminal',**progress)),flush=True)
    except Exception as error:
        progress.update(status='failed',phase='terminal',failed=1,error_type=type(error).__name__,error=str(error))
        atomic_dump(progress_path,progress)
        raise


if __name__=='__main__':main()
