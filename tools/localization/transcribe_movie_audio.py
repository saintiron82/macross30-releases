"""Local Japanese ASR from extracted audio; writes explicitly unreviewed cues."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import time


def stamp(seconds):
    n = round(seconds * 1000)
    h, n = divmod(n, 3600000)
    m, n = divmod(n, 60000)
    s, ms = divmod(n, 1000)
    return f'{h:02}:{m:02}:{s:02},{ms:03}'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--audio', type=Path)
    ap.add_argument('--output', type=Path)
    ap.add_argument('--model', default='small')
    ap.add_argument('--model-cache', type=Path, default=Path('.tools/subtitle-models'))
    ap.add_argument('--threads', type=int, default=2)
    ap.add_argument('--initial-prompt-file', type=Path, help='Optional terminology hints; recorded as a potential recognition bias')
    ap.add_argument('--download-only', action='store_true')
    n = ap.parse_args()
    if not n.download_only and (n.audio is None or n.output is None):
        ap.error('--audio and --output required')
    if n.output and n.output.exists():
        ap.error('Choose a new output directory; never overwrite transcript work')
    if os.name == 'nt':
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)
    from faster_whisper import WhisperModel
    model = WhisperModel(n.model, device='cpu', compute_type='int8',
                         cpu_threads=n.threads, num_workers=1,
                         download_root=str(n.model_cache.resolve()))
    if n.download_only:
        print('Local CPU ASR model ready', flush=True)
        return
    n.output.mkdir(parents=True)
    audio_sha = hashlib.sha256(n.audio.read_bytes()).hexdigest()
    start = time.monotonic()
    segments, info = model.transcribe(str(n.audio), language='ja', task='transcribe',
        beam_size=5, vad_filter=True, word_timestamps=True,
        condition_on_previous_text=False,
        initial_prompt=n.initial_prompt_file.read_text(encoding='utf-8') if n.initial_prompt_file else None)
    rows = []
    with (n.output/'segments.partial.jsonl').open('w', encoding='utf-8') as log:
        for segment in segments:
            if not math.isfinite(segment.start + segment.end) or segment.end <= segment.start:
                raise ValueError('Invalid cue interval')
            flags = []
            if segment.avg_logprob < -0.75: flags.append('low_average_log_probability')
            if segment.no_speech_prob > 0.4: flags.append('possible_non_speech')
            if segment.compression_ratio > 2.4: flags.append('possible_repetition')
            row = dict(id=len(rows)+1, start=segment.start, end=segment.end,
                source_ja=segment.text.strip(), avg_logprob=segment.avg_logprob,
                no_speech_prob=segment.no_speech_prob, review_flags=flags,
                review_status='unreviewed_machine_transcript',
                words=[dict(start=w.start,end=w.end,word=w.word,probability=w.probability)
                       for w in segment.words or []])
            rows.append(row)
            log.write(json.dumps(row,ensure_ascii=False)+'\n'); log.flush()
            print(json.dumps({k:row[k] for k in ('id','start','end','source_ja','review_flags')},ensure_ascii=False),flush=True)
    report=dict(status='completed_unreviewed_ASR', audio=str(n.audio.resolve()),
        audio_sha256=audio_sha, model=n.model, device='cpu', compute_type='int8',
        cpu_threads=n.threads, language='ja', duration=info.duration,
        language_probability=info.language_probability, elapsed_seconds=time.monotonic()-start,
        time_origin='extracted audio sample zero; map to PAM audio PTS via extraction manifest',
        uploaded_audio=False, human_listening_verified=False, segments=rows)
    if n.initial_prompt_file:
        report['terminology_prompt'] = dict(path=str(n.initial_prompt_file),
            sha256=hashlib.sha256(n.initial_prompt_file.read_bytes()).hexdigest(),
            recognition_bias_possible=True)
    (n.output/'transcript.ja.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    (n.output/'transcript.ja.srt').write_text('\n\n'.join(
        f"{r['id']}\n{stamp(r['start'])} --> {stamp(r['end'])}\n{r['source_ja']}" for r in rows)+'\n',encoding='utf-8')
    print(json.dumps(dict(completed=True,cues=len(rows),seconds=report['elapsed_seconds'])),flush=True)


if __name__ == '__main__':
    main()
