"""Offline Medium crosscheck of frozen uncertain Small-ASR translation inputs.

Reads verified existing float WAVs only. Never extracts, re-encodes, uploads,
translates, edits Small/Korean drafts, or changes a runtime subtitle catalog.
"""
import argparse
import collections
from datetime import datetime, timezone
import importlib.metadata
import json
import math
import os
from pathlib import Path
import time
import unicodedata

from transcribe_unresolved_battle_voice_batch import atomic_dump,below_normal,file_sha,owner_is_live

ROOT=Path(__file__).resolve().parents[2]
INPUT=ROOT/'localization/subtitles/battle-voice/translation-batches'
DEFAULT=ROOT/'localization/subtitles/battle-voice/medium-crosscheck-v1'
load=lambda p:json.loads(p.read_text(encoding='utf8'))


def relative(path):
    path=path.resolve()
    if not path.is_relative_to(ROOT):raise ValueError('Source outside workspace')
    return path.relative_to(ROOT).as_posix()


def fresh_plan(model,model_sha,priority):
    rows=[];inputs=[]
    for number in (1,2,3):
        source=INPUT/f'batch-{number}.json';draft=INPUT/f'batch-{number}.ko.json'
        original=load(source);ko=load(draft)
        if ko['input_sha256']!=file_sha(source):raise ValueError('Korean draft source identity changed')
        input_doc=dict(batch=number,Small_translation_input=relative(source),Small_translation_input_sha256=file_sha(source),
                       initial_KO_draft=relative(draft),initial_KO_draft_sha256=file_sha(draft))
        inputs.append(input_doc)
        by_sha={r['source_sha256']:r for r in original['entries']}
        for r in ko['entries']:
            if r['status'] not in ('needs_audio_review','empty_asr'):continue
            origin=by_sha[r['source_sha256']]
            if origin['source_ja']!=r['source_ja']:raise ValueError('Original Small source text changed')
            small=(ROOT/origin['transcript']).resolve()
            if not small.is_relative_to(ROOT) or file_sha(small)!=origin['transcript_sha256']:
                raise ValueError('Small transcript provenance changed')
            extraction=load(small.parent/'extraction.json');small_result=load(small)
            wav=Path(extraction['output']['path']).resolve()
            if not wav.is_relative_to(ROOT) or file_sha(wav)!=extraction['output']['sha256']:
                raise ValueError('Verified source WAV changed')
            if small_result['audio_sha256']!=extraction['output']['sha256'] or small_result['original_AT3_sha256']!=r['source_sha256']:
                raise ValueError('Small/AT3/WAV identity mismatch')
            if not extraction['output']['float_payload_bit_exact_roundtrip']:raise ValueError('Missing bit-exact float preservation proof')
            flags=origin['review_flags']
            rows.append(dict(source_sha256=origin['source_sha256'],Small_source_ja=origin['source_ja'],
                source_paths=origin['source_paths'],batch_input=input_doc,
                Small_transcript=relative(small),Small_transcript_sha256=origin['transcript_sha256'],
                audio=relative(wav),audio_sha256=extraction['output']['sha256'],
                duration=extraction['decoded_seconds'],fact_duration=extraction['fact_declared_seconds'],
                initial_KO_status=r['status'],initial_translation_ko=r['translation_ko'],
                initial_uncertainty_note=r['uncertainty_note'],Small_review_flags=flags))
    if len(rows)!=182 or len({r['source_sha256'] for r in rows})!=182:raise ValueError('Expected182 unique uncertain inputs')
    counts=collections.Counter(r['initial_KO_status'] for r in rows)
    if dict(counts)!={'needs_audio_review':181,'empty_asr':1}:raise ValueError('Unexpected uncertain input scope')
    nonverbal={'possible_non_speech','possible_repetition','very_short_reaction_requires_listening'}
    rows.sort(key=lambda r:(bool(nonverbal.intersection(r['Small_review_flags'])),-r['duration'],r['source_sha256']))
    for index,row in enumerate(rows,1):row['priority_index']=index
    return dict(status='FROZEN_UNCERTAIN_ASR_INPUTS',inputs=inputs,selected_count=len(rows),
        status_counts=dict(counts),priority='Longest clips without nonverbal/very-short flags first, then all remaining inputs',
        model=dict(path=relative(model),model_bin_sha256=model_sha,device='cpu',compute_type='int8',cpu_threads=2,
                   num_workers=1,process_priority_class=priority,network_disabled=True),
        parameters=dict(language='ja',task='transcribe',beam_size=5,vad_filter=False,word_timestamps=True,
                        condition_on_previous_text=False,initial_prompt=None),entries=rows)


def validate_plan(plan,model_sha):
    if plan['model']['model_bin_sha256']!=model_sha:raise ValueError('Cached Medium model changed')
    for source in plan['inputs']:
        if file_sha(ROOT/source['Small_translation_input'])!=source['Small_translation_input_sha256']:
            raise ValueError('Original frozen translation input changed')
    for row in plan['entries']:
        if file_sha(ROOT/row['Small_transcript'])!=row['Small_transcript_sha256']:raise ValueError('Frozen Small transcript changed')
        if file_sha(ROOT/row['audio'])!=row['audio_sha256']:raise ValueError('Verified existing audio changed')


def content_norm(text):
    return ''.join(c for c in unicodedata.normalize('NFKC',text) if unicodedata.category(c)[0] not in 'PZS')


def medium_result(model,row,model_meta):
    started=time.monotonic()
    segments,info=model.transcribe(str(ROOT/row['audio']),language='ja',task='transcribe',beam_size=5,
        vad_filter=False,word_timestamps=True,condition_on_previous_text=False,initial_prompt=None)
    cues=[]
    for s in segments:
        if not math.isfinite(s.start+s.end) or s.start<0 or s.end<s.start:raise ValueError('Invalid Medium cue interval')
        flags=['unreviewed_medium_crosscheck']
        if s.avg_logprob < -0.75:flags.append('low_average_log_probability')
        if s.no_speech_prob > .4:flags.append('possible_non_speech')
        if s.compression_ratio > 2.4:flags.append('possible_repetition')
        if row['duration']<1:flags.append('very_short_reaction_requires_listening')
        if any(p in s.text for p in ('ご視聴','チャンネル登録','ありがとうございました')):flags.append('possible_generic_ASR_hallucination')
        cues.append(dict(id=len(cues)+1,start=s.start,end=s.end,source_ja=s.text.strip(),avg_logprob=s.avg_logprob,
            no_speech_prob=s.no_speech_prob,compression_ratio=s.compression_ratio,review_flags=flags,
            words=[dict(start=w.start,end=w.end,word=w.word,probability=w.probability) for w in s.words or []]))
    joined=' '.join(c['source_ja'] for c in cues)
    small=row['Small_source_ja'];exact=small==joined
    nfkc=unicodedata.normalize('NFKC',small)==unicodedata.normalize('NFKC',joined)
    return dict(status='completed_unreviewed_Medium_crosscheck',priority_index=row['priority_index'],
        source_sha256=row['source_sha256'],source_paths=row['source_paths'],audio=row['audio'],audio_sha256=row['audio_sha256'],
        initial_translation_input=row['batch_input'],Small_source_ja=small,Small_transcript=row['Small_transcript'],
        Small_transcript_sha256=row['Small_transcript_sha256'],initial_KO_status=row['initial_KO_status'],
        initial_uncertainty_note=row['initial_uncertainty_note'],model=model_meta,
        parameters=dict(language='ja',vad_filter=False,word_timestamps=True,condition_on_previous_text=False,initial_prompt=None,beam_size=5),
        duration=info.duration,elapsed_seconds=time.monotonic()-started,segments=cues,Medium_source_ja=joined,
        exact_text_match=exact,NFKC_text_match=nfkc,
        lexical_normalized_match=content_norm(small)==content_norm(joined),
        comparison='exact_match' if exact else 'NFKC_match' if nfkc else 'different',
        human_listening_verified=False,translation_created=False,uploaded_audio=False,
        caveat='Two ASR models agreeing does not verify spoken content. No cues does not prove silence or nonverbal audio.')


def checkpoint_valid(result,row,model_sha):
    return (result.get('status')=='completed_unreviewed_Medium_crosscheck' and
            result.get('source_sha256')==row['source_sha256'] and result.get('audio_sha256')==row['audio_sha256'] and
            result.get('Small_source_ja')==row['Small_source_ja'] and result.get('model',{}).get('model_bin_sha256')==model_sha and
            result.get('parameters',{}).get('initial_prompt') is None and
            result.get('parameters',{}).get('condition_on_previous_text') is False)


def comparison_lists(results):
    groups={key:[r for r in results if r['comparison']==key] for key in ('exact_match','NFKC_match','different')}
    return dict(status='UNREVIEWED_MODEL_COMPARISON',completed=len(results),
        counts={k:len(v) for k,v in groups.items()},
        exact_match=groups['exact_match'],NFKC_only_match=groups['NFKC_match'],different=groups['different'],
        no_Medium_cues=[r for r in results if not r['Medium_source_ja']],
        caveat='Agreement, differences and model confidence are not human listening verification.')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,default=DEFAULT)
    ap.add_argument('--resume',action='store_true')
    ap.add_argument('--verify-only',action='store_true')
    ap.add_argument('--model-path',type=Path)
    args=ap.parse_args();output=args.output.resolve()
    if not output.is_relative_to((ROOT/'localization/subtitles/battle-voice').resolve()):raise ValueError('Output outside review tree')
    if output.exists() and not args.resume:ap.error('Fresh output required; use --resume only for the same frozen job')
    if (output/'progress.json').exists():
        old=load(output/'progress.json')
        if old.get('status')=='running' and owner_is_live(old['pid']):
            raise SystemExit('Existing Medium owner is still live; poll its original handle, do not restart')
    priority=below_normal()
    os.environ.update(HF_HUB_OFFLINE='1',TRANSFORMERS_OFFLINE='1',HF_HUB_DISABLE_TELEMETRY='1')
    if args.model_path:model_path=args.model_path.resolve()
    else:
        snapshots=ROOT/'.tools/subtitle-models/models--Systran--faster-whisper-medium/snapshots'
        candidates=[p for p in snapshots.iterdir() if (p/'model.bin').is_file()]
        if len(candidates)!=1:raise ValueError('Specify one existing local Medium snapshot; no downloads')
        model_path=candidates[0]
    model_sha=file_sha(model_path/'model.bin');output.mkdir(parents=True,exist_ok=True)
    if (output/'plan.json').exists():plan=load(output/'plan.json')
    else:
        plan=fresh_plan(model_path,model_sha,priority);atomic_dump(output/'plan.json',plan)
    validate_plan(plan,model_sha)
    if args.verify_only:
        checked=[]
        for row in plan['entries']:
            path=output/f"{row['priority_index']:04}-{row['source_sha256'][:16]}.json"
            result=load(path)
            if not checkpoint_valid(result,row,model_sha):raise ValueError('Completed Medium checkpoint changed')
            checked.append(dict(source_sha256=row['source_sha256'],checkpoint=path.name,comparison=result['comparison']))
        atomic_dump(output/'resume-verification.json',dict(status='PASS',completed=len(checked),model_loaded=False,new_ASR_inferences=0,checkpoints=checked))
        print(json.dumps(dict(event='completed_Medium_work_verified',completed=len(checked),new_ASR_inferences=0)),flush=True)
        return
    started=time.monotonic();results=[];failures=[]
    progress=dict(status='running',phase='loading_cached_Medium',pid=os.getpid(),started_utc=datetime.now(timezone.utc).isoformat(),
        completed=0,failed=0,total=len(plan['entries']),model=plan['model'],human_listening_verified=False,
        translation_created=False,uploaded_audio=False)
    atomic_dump(output/'progress.json',dict(progress,results=results,failures=failures))
    print(json.dumps(dict(event='Medium_job_started',pid=os.getpid(),total=len(plan['entries']),model=plan['model'])),flush=True)
    from faster_whisper import WhisperModel
    model=WhisperModel(str(model_path),device='cpu',compute_type='int8',cpu_threads=2,num_workers=1,local_files_only=True)
    atomic_dump(output/'environment.json',dict(Python=os.sys.version,packages={n:importlib.metadata.version(n) for n in ('faster-whisper','ctranslate2','numpy')},
        model=plan['model'],parameters=plan['parameters']))
    for row in plan['entries']:
        index=row['priority_index'];path=output/f"{index:04}-{row['source_sha256'][:16]}.json"
        progress.update(phase='crosschecking',current_index=index,current_audio=row['audio'],elapsed_seconds=time.monotonic()-started)
        atomic_dump(output/'progress.json',dict(progress,results=results,failures=failures))
        try:
            if path.exists():
                result=load(path)
                if not checkpoint_valid(result,row,model_sha):raise ValueError('Existing Medium checkpoint identity changed')
            else:
                result=medium_result(model,row,plan['model']);atomic_dump(path,result)
            summary=dict(priority_index=index,source_sha256=row['source_sha256'],source_paths=row['source_paths'],checkpoint=path.name,
                Small_source_ja=result['Small_source_ja'],Medium_source_ja=result['Medium_source_ja'],comparison=result['comparison'],
                exact_text_match=result['exact_text_match'],NFKC_text_match=result['NFKC_text_match'],
                lexical_normalized_match=result['lexical_normalized_match'],Medium_cues=len(result['segments']),
                lowest_avg_logprob=min((c['avg_logprob'] for c in result['segments']),default=None),
                highest_no_speech_prob=max((c['no_speech_prob'] for c in result['segments']),default=None),
                Medium_review_flags=sorted({f for c in result['segments'] for f in c['review_flags']}))
            results.append(summary)
            print(json.dumps(dict(event='Medium_clip_completed',index=index,total=len(plan['entries']),comparison=summary['comparison'],
                source_paths=row['source_paths'],Small=summary['Small_source_ja'],Medium=summary['Medium_source_ja']),ensure_ascii=False),flush=True)
        except Exception as error:
            failure=dict(priority_index=index,source_sha256=row['source_sha256'],error_type=type(error).__name__,error=str(error))
            failures.append(failure);atomic_dump(output/f'{index:04}-error.json',failure)
            print(json.dumps(dict(event='Medium_clip_failed',**failure)),flush=True)
        progress.update(completed=len(results),failed=len(failures),elapsed_seconds=time.monotonic()-started)
        atomic_dump(output/'progress.json',dict(progress,results=results,failures=failures))
        atomic_dump(output/'comparison-lists.json',comparison_lists(results))
    progress.update(status='completed_unreviewed_Medium_crosscheck' if not failures else 'completed_with_errors',phase='terminal',
        finished_utc=datetime.now(timezone.utc).isoformat(),elapsed_seconds=time.monotonic()-started)
    atomic_dump(output/'progress.json',dict(progress,results=results,failures=failures))
    print(json.dumps(dict(event='Medium_job_finished',status=progress['status'],completed=len(results),failed=len(failures),elapsed_seconds=progress['elapsed_seconds'])),flush=True)
    if failures:raise SystemExit(1)


if __name__=='__main__':main()
