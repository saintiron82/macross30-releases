"""Resumable offline ASR of real unmatched voice assets, deduplicated by RIFF SHA.

Never labels these files as verified speech or changes a runtime subtitle catalog.
"""
import argparse
import collections
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import time

import extract_battle_voice_review as extract

ROOT=Path(__file__).resolve().parents[2]
sha=lambda b:hashlib.sha256(b).hexdigest()
load=lambda p:json.loads(p.read_text(encoding='utf8'))
ATOMIC_REPLACE_RETRIES=0


def atomic_dump(path,data):
    global ATOMIC_REPLACE_RETRIES
    pending=path.with_suffix(path.suffix+'.partial')
    pending.write_bytes((json.dumps(data,ensure_ascii=False,indent=2)+'\n').encode('utf8'))
    # Windows readers/antivirus may briefly open the destination without
    # FILE_SHARE_DELETE. Keep the complete pending checkpoint and retry only
    # these sharing/access errors; never discard data or retry indefinitely.
    deadline=time.monotonic()+5.0
    while True:
        try:
            os.replace(pending,path)
            return
        except PermissionError as error:
            if os.name!='nt' or getattr(error,'winerror',None) not in (5,32,33) or time.monotonic()>=deadline:
                raise
            ATOMIC_REPLACE_RETRIES+=1
            time.sleep(.05)


def file_sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(4*1024*1024),b''):h.update(block)
    return h.hexdigest()


def below_normal():
    if os.name!='nt':return None
    import ctypes
    from ctypes import wintypes
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.GetCurrentProcess.argtypes=[];kernel.GetCurrentProcess.restype=wintypes.HANDLE
    kernel.SetPriorityClass.argtypes=[wintypes.HANDLE,wintypes.DWORD];kernel.SetPriorityClass.restype=wintypes.BOOL
    kernel.GetPriorityClass.argtypes=[wintypes.HANDLE];kernel.GetPriorityClass.restype=wintypes.DWORD
    process=kernel.GetCurrentProcess()
    if not kernel.SetPriorityClass(process,0x4000):raise ctypes.WinError(ctypes.get_last_error())
    value=kernel.GetPriorityClass(process)
    if value!=0x4000:raise ValueError('BelowNormal priority readback failed')
    return value


def owner_is_live(pid):
    """Observe an old owner; an observation failure never authorizes a restart."""
    if os.name!='nt':
        try:os.kill(pid,0)
        except ProcessLookupError:return False
        return True
    import ctypes
    from ctypes import wintypes
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
    kernel.OpenProcess.restype=wintypes.HANDLE
    kernel.GetExitCodeProcess.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.DWORD)]
    kernel.GetExitCodeProcess.restype=wintypes.BOOL
    kernel.CloseHandle.argtypes=[wintypes.HANDLE]
    handle=kernel.OpenProcess(0x1000,False,pid)
    if not handle:
        error=ctypes.get_last_error()
        if error==87:return False
        raise ctypes.WinError(error)
    try:
        code=wintypes.DWORD()
        if not kernel.GetExitCodeProcess(handle,ctypes.byref(code)):raise ctypes.WinError(ctypes.get_last_error())
        return code.value==259
    finally:kernel.CloseHandle(handle)


def source_bytes(entry):
    if entry['pack'].lower()!='sound.dat' or Path(entry['pack']).name!=entry['pack']:
        raise ValueError('Unexpected original audio pack')
    path=extract.PACK/entry['pack']
    with path.open('rb') as f:
        if entry['offset']<0 or entry['offset']+entry['size']>path.stat().st_size:raise ValueError('Source range outside pack')
        f.seek(entry['offset']);raw=f.read(entry['size'])
    if len(raw)!=entry['size'] or sha(raw)!=entry['sha256']:raise ValueError('Original audio source changed '+entry['full_path'])
    extract.riff_metadata(raw)
    return raw


def validate_extraction(report,folder,entry):
    if report['original_AT3_sha256']!=entry['sha256']:raise ValueError('Extraction identity changed')
    if file_sha(folder/'original.at3')!=entry['sha256']:raise ValueError('Copied AT3 changed')
    if file_sha(folder/'audio.f32.wav')!=report['output']['sha256']:raise ValueError('Review WAV changed')
    if not report['output']['float_payload_bit_exact_roundtrip'] or report['manual_trim_samples']!=0:
        raise ValueError('Extraction preservation checks missing')


def validate_transcript(result,report,model_sha):
    return (result.get('status')=='completed_unreviewed_ASR' and
            result.get('original_AT3_sha256')==report['original_AT3_sha256'] and
            result.get('audio_sha256')==report['output']['sha256'] and
            result.get('model',{}).get('model_bin_sha256')==model_sha and
            result.get('vad_filter') is False and result.get('initial_prompt') is None and
            result.get('condition_on_previous_text') is False)


def raw_transcript(model,metadata,report):
    started=time.monotonic()
    segments,info=model.transcribe(report['output']['path'],language='ja',task='transcribe',beam_size=5,
        vad_filter=False,word_timestamps=True,condition_on_previous_text=False)
    cues=[]
    for s in segments:
        if not math.isfinite(s.start+s.end) or s.start<0 or s.end<s.start:raise ValueError('Invalid ASR interval')
        flags=['unreviewed_machine_transcript']
        if s.avg_logprob < -0.75:flags.append('low_average_log_probability')
        if s.no_speech_prob > 0.4:flags.append('possible_non_speech')
        if s.compression_ratio > 2.4:flags.append('possible_repetition')
        if report['decoded_seconds']<1:flags.append('very_short_reaction_requires_listening')
        if any(p in s.text for p in ('ご視聴','チャンネル登録','ありがとうございました')):flags.append('possible_generic_ASR_hallucination')
        cues.append(dict(id=len(cues)+1,start=s.start,end=s.end,source_ja=s.text.strip(),
            avg_logprob=s.avg_logprob,no_speech_prob=s.no_speech_prob,compression_ratio=s.compression_ratio,
            review_flags=flags,words=[dict(start=w.start,end=w.end,word=w.word,probability=w.probability) for w in s.words or []]))
    return dict(status='completed_unreviewed_ASR',original_asset_path=report['original_asset']['full_path'],
        original_AT3_sha256=report['original_AT3_sha256'],audio_sha256=report['output']['sha256'],
        model=metadata,language='ja',vad_filter=False,initial_prompt=None,condition_on_previous_text=False,
        duration=info.duration,elapsed_seconds=time.monotonic()-started,
        human_listening_verified=False,uploaded_audio=False,translation=None,
        zero_cues_meaning='No ASR cue is not evidence of silence/nonverbal content.',segments=cues)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,default=ROOT/'localization/subtitles/battle-voice/audio-unresolved-v2')
    ap.add_argument('--coverage',type=Path,default=ROOT/'analysis/localization/voice-only-audit/coverage-union.json')
    ap.add_argument('--model-path',type=Path)
    ap.add_argument('--resume',action='store_true')
    ap.add_argument('--verify-only',action='store_true',help='Validate completed checkpoints without loading/inferencing the model')
    args=ap.parse_args();output=args.output.resolve()
    if not output.is_relative_to((ROOT/'localization/subtitles/battle-voice').resolve()):raise ValueError('Output outside battle-voice review tree')
    if output.exists() and not args.resume:ap.error('Output exists; use explicit --resume for the same immutable plan')
    if (output/'ASR-progress.json').exists():
        previous=load(output/'ASR-progress.json')
        if previous.get('status')=='running' and owner_is_live(previous['pid']):
            raise SystemExit('Existing batch owner is still live; poll its original handle instead of restarting')
    priority=below_normal()
    os.environ.update(HF_HUB_OFFLINE='1',TRANSFORMERS_OFFLINE='1',HF_HUB_DISABLE_TELEMETRY='1')
    if args.model_path:model_path=args.model_path.resolve()
    else:
        snapshots=ROOT/'.tools/subtitle-models/models--Systran--faster-whisper-small/snapshots'
        candidates=[p for p in snapshots.iterdir() if (p/'model.bin').is_file()]
        if len(candidates)!=1:raise ValueError('Specify one existing cached local snapshot; downloads forbidden')
        model_path=candidates[0]
    model_sha=file_sha(model_path/'model.bin')
    model_meta=dict(path=str(model_path),model_bin_sha256=model_sha,device='cpu',compute_type='int8',
                    cpu_threads=2,num_workers=1,network_disabled=True,process_priority_class=priority)
    coverage=load(args.coverage);entries=coverage['unresolved_actual_assets']
    if coverage['status']!='PASS' or len(entries)!=451:raise ValueError('Expected reviewed actual unmatched asset list')
    groups=collections.defaultdict(list)
    for entry in entries:
        source_bytes(entry)
        groups[entry['sha256']].append(entry)
    if len(groups)!=439:raise ValueError('Expected exact RIFF deduplication to 439 unique assets')
    plan=dict(source_coverage_sha256=file_sha(args.coverage),total_original_paths=len(entries),unique_RIFF_SHA=len(groups),
        model=model_meta,language='ja',vad_filter=False,initial_prompt=None,condition_on_previous_text=False,
        groups=[dict(sha256=h,representative=min(rows,key=lambda e:e['full_path']),
                     aliases=sorted(e['full_path'] for e in rows)) for h,rows in groups.items()])
    plan['groups'].sort(key=lambda g:g['representative']['full_path'])
    output.mkdir(parents=True,exist_ok=True)
    if (output/'plan.json').exists():
        if load(output/'plan.json')!=plan:raise ValueError('Resume plan/source/model changed; fresh output required')
    else:atomic_dump(output/'plan.json',plan)
    existing=load(output/'ASR-progress.json') if (output/'ASR-progress.json').exists() else None
    if args.verify_only or (existing and existing.get('status')=='completed_unreviewed_ASR'):
        checked=[]
        for number,group in enumerate(plan['groups'],1):
            entry=group['representative'];stem=entry['full_path'].removeprefix('data/sound/voice/').removesuffix('.at3')
            folder=output/f'{number:04}-{stem.replace("/","__")}-{group["sha256"][:12]}'
            report=load(folder/'extraction.json');validate_extraction(report,folder,entry)
            result=load(folder/'transcript.ja.json')
            if not validate_transcript(result,report,model_sha):raise ValueError('Completed transcript provenance changed')
            if result['original_asset_aliases']!=group['aliases']:raise ValueError('Completed aliases changed')
            checked.append(dict(unique_index=number,original_asset_path=entry['full_path'],source_sha256=group['sha256'],
                audio_sha256=report['output']['sha256'],ASR_cues=len(result['segments'])))
        atomic_dump(output/'resume-verification.json',dict(status='PASS',completed_unique=len(checked),
            original_paths=len(entries),model_loaded=False,new_ASR_inferences=0,checkpoints=checked))
        print(json.dumps(dict(event='completed_work_verified',unique=len(checked),paths=len(entries),new_ASR_inferences=0)),flush=True)
        return
    prior_root=ROOT/'localization/subtitles/battle-voice/audio-review-v1'
    prior=load(prior_root/'extraction-manifest.json')
    reusable={r['original_AT3_sha256']:r for r in prior['reports']}
    extract.dump=atomic_dump
    from faster_whisper import WhisperModel
    model=WhisperModel(str(model_path),device='cpu',compute_type='int8',cpu_threads=2,num_workers=1,local_files_only=True)
    started=time.monotonic();results=[];failures=[]
    progress=dict(status='running',pid=os.getpid(),started_utc=datetime.now(timezone.utc).isoformat(),
        total_unique=len(plan['groups']),total_original_paths=len(entries),completed=0,failed=0,
        model=model_meta,source_coverage_sha256=plan['source_coverage_sha256'],human_listening_verified=False,
        translation_created=False,uploaded_audio=False)
    atomic_dump(output/'ASR-progress.json',dict(progress,results=results,failures=failures))
    print(json.dumps(dict(event='job_started',pid=os.getpid(),unique=len(groups),paths=len(entries),model=model_meta)),flush=True)
    for number,group in enumerate(plan['groups'],1):
        entry=group['representative'];stem=entry['full_path'].removeprefix('data/sound/voice/').removesuffix('.at3')
        folder=output/f'{number:04}-{stem.replace("/","__")}-{group["sha256"][:12]}'
        folder.mkdir(exist_ok=True)
        progress.update(current_unique=number,current_asset=entry['full_path'],elapsed_seconds=time.monotonic()-started)
        atomic_dump(output/'ASR-progress.json',dict(progress,results=results,failures=failures))
        try:
            reused=False
            if (folder/'extraction.json').exists():
                report=load(folder/'extraction.json');validate_extraction(report,folder,entry)
            elif group['sha256'] in reusable:
                old=reusable[group['sha256']];old_wav=Path(old['output']['path']);validate_extraction(old,old_wav.parent,entry)
                shutil.copyfile(old_wav.parent/'original.at3',folder/'original.at3')
                shutil.copyfile(old_wav,folder/'audio.f32.wav')
                report=dict(old,original_asset=entry,reused_from=str(old_wav.parent),original_asset_aliases=group['aliases'])
                report['output']=dict(old['output'],path=str(folder/'audio.f32.wav'))
                atomic_dump(folder/'extraction.json',report);reused=True
                old_transcript=load(old_wav.parent/'transcript.ja.json')
                if validate_transcript(old_transcript,report,model_sha):
                    reused_asr=dict(old_transcript,original_asset_path=entry['full_path'],original_asset_aliases=group['aliases'],
                                    reused_from=str(old_wav.parent/'transcript.ja.json'))
                    atomic_dump(folder/'transcript.ja.json',reused_asr)
            else:
                report=extract.decode_asset(entry,folder)
                report['original_asset_aliases']=group['aliases'];atomic_dump(folder/'extraction.json',report)
            if (folder/'transcript.ja.json').exists():
                result=load(folder/'transcript.ja.json')
                if not validate_transcript(result,report,model_sha):raise ValueError('Existing transcript provenance changed')
                reused=True
            else:
                result=raw_transcript(model,model_meta,report)
                result['original_asset_aliases']=group['aliases'];atomic_dump(folder/'transcript.ja.json',result)
            summary=dict(unique_index=number,asset=entry['full_path'],original_aliases=group['aliases'],source_sha256=group['sha256'],
                extraction=(folder/'extraction.json').relative_to(output).as_posix(),
                transcript=(folder/'transcript.ja.json').relative_to(output).as_posix(),
                decoded_seconds=report['decoded_seconds'],fact_seconds=report['fact_declared_seconds'],
                reused_verified_work=reused,cues=len(result['segments']),
                raw_ja=[s['source_ja'] for s in result['segments']],
                review_flags=sorted({f for s in result['segments'] for f in s['review_flags']}))
            results.append(summary)
            print(json.dumps(dict(event='clip_completed',unique=number,total=len(groups),asset=entry['full_path'],cues=summary['cues'],raw_ja=summary['raw_ja']),ensure_ascii=False),flush=True)
        except Exception as error:
            failure=dict(unique_index=number,asset=entry['full_path'],error_type=type(error).__name__,error=str(error))
            failures.append(failure);atomic_dump(folder/'error.json',failure)
            print(json.dumps(dict(event='clip_failed',**failure)),flush=True)
        progress.update(completed=len(results),failed=len(failures),elapsed_seconds=time.monotonic()-started)
        atomic_dump(output/'ASR-progress.json',dict(progress,results=results,failures=failures))
    progress.update(status='completed_unreviewed_ASR' if not failures else 'completed_with_errors',
                    finished_utc=datetime.now(timezone.utc).isoformat(),elapsed_seconds=time.monotonic()-started)
    atomic_dump(output/'ASR-progress.json',dict(progress,results=results,failures=failures))
    print(json.dumps(dict(event='job_finished',status=progress['status'],completed=len(results),failed=len(failures),elapsed_seconds=progress['elapsed_seconds'])),flush=True)
    if failures:raise SystemExit(1)


if __name__=='__main__':main()
