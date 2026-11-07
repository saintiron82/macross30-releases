"""Extract exact reviewed AT3 assets as untrimmed float32 audio; optional offline ASR.

Run with the existing .tools/subtitle-asr Python. No network, game launch, source
mutation, normalization, isolation, waveform clipping or manual padding removal.
"""
import argparse
import hashlib
import html
import io
import json
import math
import os
from pathlib import Path
import struct
import time

ROOT=Path(__file__).resolve().parents[2]
PACK=ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'
sha=lambda b:hashlib.sha256(b).hexdigest()


def dump(path,data):
    path.write_bytes((json.dumps(data,ensure_ascii=False,indent=2)+'\n').encode('utf8'))


def riff_metadata(raw):
    if raw[:4]!=b'RIFF' or raw[8:12]!=b'WAVE' or struct.unpack_from('<I',raw,4)[0]+8!=len(raw):
        raise ValueError('Exact RIFF size/type guard failed')
    result={};pos=12
    while pos+8<=len(raw):
        tag=raw[pos:pos+4];n=struct.unpack_from('<I',raw,pos+4)[0]
        if pos+8+n>len(raw):raise ValueError('RIFF chunk outside source asset')
        data=raw[pos+8:pos+8+n]
        if tag==b'fmt ':
            fmt,ch,rate=struct.unpack_from('<HHI',data)
            result.update(format_tag=fmt,channels=ch,sample_rate=rate,fmt_chunk_hex=data.hex())
        elif tag==b'fact':
            result.update(fact_declared_samples=struct.unpack_from('<I',data)[0],fact_chunk_hex=data.hex())
        elif tag==b'data':result['compressed_data_bytes']=n
        pos+=8+n+(n&1)
    if not all(k in result for k in ('channels','sample_rate','fact_declared_samples','compressed_data_bytes')):
        raise ValueError('Missing required source audio metadata')
    return result


def float_wave(pcm,rate,channels,samples):
    if len(pcm)!=samples*channels*4:raise ValueError('Float output length mismatch')
    fmt=struct.pack('<HHIIHH',3,channels,rate,rate*channels*4,channels*4,32)
    body=b'WAVE'+b'fmt '+struct.pack('<I',len(fmt))+fmt
    body+=b'fact'+struct.pack('<II',4,samples)
    body+=b'data'+struct.pack('<I',len(pcm))+pcm
    return b'RIFF'+struct.pack('<I',len(body))+body


def decode_asset(entry,destination):
    import av
    import numpy as np
    if Path(entry['pack']).name.lower()!=entry['pack'].lower() or entry['pack'].lower()!='sound.dat':
        raise ValueError('Unexpected source pack')
    path=PACK/entry['pack']
    before=path.stat()
    if entry['offset']<0 or entry['size']<=0 or entry['offset']+entry['size']>before.st_size:
        raise ValueError('Asset range outside source pack')
    with path.open('rb') as f:
        f.seek(entry['offset']);raw=f.read(entry['size'])
    if len(raw)!=entry['size'] or sha(raw)!=entry['sha256']:raise ValueError('Exact original AT3 SHA guard failed')
    metadata=riff_metadata(raw)
    if metadata['sample_rate']!=entry['sample_rate'] or metadata['fact_declared_samples']!=entry['fact_declared_samples']:
        raise ValueError('Source fact/rate differs from reviewed inventory')
    source=destination/'original.at3';source.write_bytes(raw)
    frames=[];packets=[];chunks=[];peak=0.0;sample_count=0;nonzero=0
    with av.open(io.BytesIO(raw),mode='r') as container:
        streams=list(container.streams.audio)
        if len(streams)!=1:raise ValueError('Expected one source audio stream')
        stream=streams[0]
        codec=stream.codec_context.name
        if codec!='atrac3plus':raise ValueError('Expected ATRAC3plus, got '+codec)
        rate=stream.codec_context.sample_rate;channels=stream.codec_context.channels
        if channels not in (1,2) or rate!=metadata['sample_rate']:raise ValueError('Unexpected channel/rate configuration')
        stream_info=dict(codec=codec,time_base=str(stream.time_base),duration=stream.duration,start_time=stream.start_time)
        for packet in container.demux(stream):
            if packet.size:
                packets.append(dict(pts=packet.pts,dts=packet.dts,duration=packet.duration,bytes=packet.size))
            for frame in packet.decode():
                if frame.sample_rate!=rate or len(frame.layout.channels)!=channels:raise ValueError('Decoder format changed')
                array=frame.to_ndarray()
                if array.dtype!=np.float32:raise ValueError('Decoder did not supply native float32')
                if not np.isfinite(array).all():raise ValueError('Nonfinite decoder output')
                if frame.format.is_planar:
                    if array.shape!=(channels,frame.samples):raise ValueError('Unexpected planar shape')
                    interleaved=array.T.copy()
                else:
                    if array.size!=channels*frame.samples:raise ValueError('Unexpected packed shape')
                    interleaved=array.reshape(frame.samples,channels)
                pcm=interleaved.astype('<f4',copy=False).tobytes()
                chunks.append(pcm);sample_count+=frame.samples
                peak=max(peak,float(np.abs(array).max(initial=0)));nonzero+=int(np.count_nonzero(array))
                frames.append(dict(pts=frame.pts,time_base=str(frame.time_base),samples=frame.samples,format=frame.format.name))
    if not sample_count:raise ValueError('Decoder emitted zero samples')
    pcm=b''.join(chunks);wave=float_wave(pcm,rate,channels,sample_count)
    wav=destination/'audio.f32.wav';wav.write_bytes(wave)
    # Decode the IEEE-float WAV and compare exact float bit patterns. This is not
    # a second ATRAC decode, trim, resample or alternate audio extraction route.
    roundtrip=[]
    with av.open(io.BytesIO(wave),mode='r') as check:
        if check.streams.audio[0].codec_context.name!='pcm_f32le':raise ValueError('Output is not float32 WAV')
        for frame in check.decode(audio=0):
            array=frame.to_ndarray()
            roundtrip.append((array.T.copy() if frame.format.is_planar else array.reshape(-1,channels)).astype('<f4',copy=False).tobytes())
    if b''.join(roundtrip)!=pcm:raise ValueError('Float WAV roundtrip changed decoded values')
    after=path.stat()
    if (before.st_size,before.st_mtime_ns)!=(after.st_size,after.st_mtime_ns):raise ValueError('Source pack changed while extracting')
    report=dict(status='decoded_audio_only_untrimmed',original_asset=entry,original_AT3_sha256=sha(raw),
        source_pack=dict(path=str(path),size=before.st_size,mtime_ns=before.st_mtime_ns),
        source_metadata=metadata,stream=stream_info,PyAV_version=av.__version__,
        decoded_samples_per_channel=sample_count,decoded_seconds=sample_count/rate,
        fact_declared_seconds=metadata['fact_declared_samples']/rate,
        decoded_minus_fact_samples=sample_count-metadata['fact_declared_samples'],
        output=dict(path=str(wav),codec='pcm_f32le',sample_rate=rate,channels=channels,sha256=sha(wave),
                    float_payload_sha256=sha(pcm),float_payload_bit_exact_roundtrip=True,peak_absolute=peak,nonzero_values=nonzero),
        frames=frames,packets=packets,manual_trim_samples=0,normalization=False,sample_clipping=False,
        audio_isolation=False,uploaded_audio=False,human_listening_verified=False,
        time_origin='Concatenated decoder-emitted samples in decode order, including decoder padding; no PAM timing assumptions.',
        padding_limit='RIFF fact count can differ from decoded frame count. All emitted samples retained. Frame PTS may be missing/irregular and are not used to discard or retime samples.')
    dump(destination/'extraction.json',report)
    return report


def transcribe(reports,output,model_path,threads):
    for name in ('HF_HUB_OFFLINE','TRANSFORMERS_OFFLINE','HF_HUB_DISABLE_TELEMETRY'):
        os.environ[name]='1'
    from faster_whisper import WhisperModel
    if not (model_path/'model.bin').is_file():raise ValueError('Cached local model.bin missing; no download permitted')
    model=WhisperModel(str(model_path),device='cpu',compute_type='int8',cpu_threads=threads,num_workers=1,local_files_only=True)
    model_meta=dict(path=str(model_path),model_bin_sha256=sha((model_path/'model.bin').read_bytes()),
                    device='cpu',compute_type='int8',cpu_threads=threads,network_disabled=True)
    rows=[]
    for report in reports:
        wav=Path(report['output']['path']);started=time.monotonic()
        segments,info=model.transcribe(str(wav),language='ja',task='transcribe',beam_size=5,
            vad_filter=False,word_timestamps=True,condition_on_previous_text=False)
        cues=[]
        for s in segments:
            if not math.isfinite(s.start+s.end) or s.start<0 or s.end<s.start:raise ValueError('Invalid ASR cue interval')
            flags=['unreviewed_machine_transcript']
            if s.avg_logprob < -0.75:flags.append('low_average_log_probability')
            if s.no_speech_prob > 0.4:flags.append('possible_non_speech')
            if s.compression_ratio > 2.4:flags.append('possible_repetition')
            if report['decoded_seconds']<1.0:flags.append('very_short_reaction_requires_listening')
            if any(p in s.text for p in ('ご視聴','チャンネル登録','ありがとうございました')):
                flags.append('possible_generic_ASR_hallucination')
            cues.append(dict(id=len(cues)+1,start=s.start,end=s.end,source_ja=s.text.strip(),
                avg_logprob=s.avg_logprob,no_speech_prob=s.no_speech_prob,compression_ratio=s.compression_ratio,
                review_flags=flags,words=[dict(start=w.start,end=w.end,word=w.word,probability=w.probability) for w in s.words or []]))
        result=dict(status='completed_unreviewed_ASR',original_asset_path=report['original_asset']['full_path'],
            original_AT3_sha256=report['original_AT3_sha256'],audio_sha256=report['output']['sha256'],
            model=model_meta,language='ja',vad_filter=False,initial_prompt=None,condition_on_previous_text=False,
            duration=info.duration,elapsed_seconds=time.monotonic()-started,
            human_listening_verified=False,uploaded_audio=False,translation=None,
            zero_cues_meaning='No ASR cue is not evidence of silence/nonverbal content.',segments=cues)
        dump(wav.parent/'transcript.ja.json',result)
        rows.append(result)
        dump(output/'ASR-progress.json',dict(status='running',completed=len(rows),total=len(reports),results=rows))
        print(json.dumps(dict(asset=result['original_asset_path'],cues=len(cues),raw_ja=[c['source_ja'] for c in cues]),ensure_ascii=False),flush=True)
    dump(output/'ASR-progress.json',dict(status='completed_unreviewed_ASR',completed=len(rows),total=len(reports),results=rows))


def build_review_page(output):
    extraction=json.loads((output/'extraction-manifest.json').read_text(encoding='utf8'))
    asr=json.loads((output/'ASR-progress.json').read_text(encoding='utf8'))
    transcripts={r['original_asset_path']:r for r in asr['results']}
    cards=[]
    escape=lambda s:html.escape(str(s),quote=True)
    for report in extraction['reports']:
        asset=report['original_asset']['full_path']
        wav=Path(report['output']['path'])
        relative=wav.relative_to(output).as_posix()
        transcript=transcripts[asset]
        rows=''.join('<li><small>'+escape(f"{s['start']:.2f}–{s['end']:.2f} s")+'</small> '+
                     escape(s['source_ja'])+'<br><small>'+escape(', '.join(s['review_flags']))+'</small></li>'
                     for s in transcript['segments'])
        cards.append('<article><h2>'+escape(asset)+'</h2><p>원본 fact '+
                     escape(f"{report['fact_declared_seconds']:.3f}")+'초 / 패딩 포함 WAV '+
                     escape(f"{report['decoded_seconds']:.3f}")+'초</p><audio controls preload="none" src="'+escape(relative)+
                     '"></audio><ul>'+rows+'</ul><p><a href="'+escape(wav.parent.relative_to(output).as_posix()+'/original.at3')+
                     '">원본 AT3</a> · <a href="'+escape(wav.parent.relative_to(output).as_posix()+'/extraction.json')+
                     '">추출 근거</a></p></article>')
    page='''<!doctype html><html lang="ko"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>전투 음성 원음·전사 검토</title><style>body{font:16px/1.6 system-ui,sans-serif;max-width:900px;margin:24px auto;padding:0 16px;color:#eef3f7;background:#111820}h1{font-size:24px}h2{font-size:17px;overflow-wrap:anywhere}article{padding:16px;margin:16px 0;background:#1b2734;border-radius:8px}audio{width:100%}small{color:#a8bacb}a{color:#7dccff}</style>
<h1>전투 음성 12개 — 미검수 일본어 자동 전사</h1><p>영상 없는 원음 검토용 페이지입니다. 파형은 float32이며 크기 조정·클리핑·임의 패딩 제거를 하지 않았습니다. ASR 문장을 정답이나 번역 완료로 보지 마세요. 짧은 음성의 빈 결과나 이상한 단어도 무음·비명이라는 증거가 아닙니다.</p>'''+''.join(cards)+'''<script>document.querySelectorAll('audio').forEach(a=>a.addEventListener('play',()=>document.querySelectorAll('audio').forEach(b=>{if(b!==a)b.pause()})))</script></html>'''
    (output/'review.html').write_bytes(page.encode('utf8'))


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--manifest',type=Path,default=ROOT/'analysis/localization/voice-only-audit/next-review-set.json')
    ap.add_argument('--output',type=Path,default=ROOT/'localization/subtitles/battle-voice/audio-review-v1')
    ap.add_argument('--transcribe',action='store_true')
    ap.add_argument('--model-path',type=Path)
    ap.add_argument('--threads',type=int,default=2)
    args=ap.parse_args()
    if args.output.exists():ap.error('Fresh output directory required; original review work is never overwritten')
    if not 1<=args.threads<=2:ap.error('Use one or two CPU threads for this review batch')
    if os.name=='nt':
        import ctypes
        from ctypes import wintypes
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.GetCurrentProcess.argtypes=[]
        kernel.GetCurrentProcess.restype=wintypes.HANDLE
        kernel.SetPriorityClass.argtypes=[wintypes.HANDLE,wintypes.DWORD]
        kernel.SetPriorityClass.restype=wintypes.BOOL
        if not kernel.SetPriorityClass(kernel.GetCurrentProcess(),0x4000):
            raise ctypes.WinError(ctypes.get_last_error())
    manifest=json.loads(args.manifest.read_text(encoding='utf8'))
    entries=manifest['entries'];args.output.mkdir(parents=True)
    reports=[];failures=[]
    for entry in entries:
        stem=entry['full_path'].removeprefix('data/sound/voice/').removesuffix('.at3')
        if len(stem.split('/'))!=2 or '..' in stem:raise ValueError('Invalid exact asset stem')
        destination=args.output/stem.replace('/','__');destination.mkdir()
        try:reports.append(decode_asset(entry,destination))
        except Exception as error:
            failure=dict(asset=entry['full_path'],error_type=type(error).__name__,error=str(error))
            dump(destination/'extraction-error.json',failure);failures.append(failure)
        dump(args.output/'extraction-manifest.json',dict(status='running',source_manifest_sha256=sha(args.manifest.read_bytes()),
             completed=len(reports),failed=len(failures),total=len(entries),reports=reports,failures=failures))
    summary=dict(status='PASS' if not failures else 'FAIL',source_manifest_sha256=sha(args.manifest.read_bytes()),
        completed=len(reports),failed=len(failures),total=len(entries),
        total_decoded_seconds=sum(r['decoded_seconds'] for r in reports),
        total_fact_declared_seconds=sum(r['fact_declared_seconds'] for r in reports),
        no_source_mutation=True,manual_trim_samples=0,reports=reports,failures=failures)
    dump(args.output/'extraction-manifest.json',summary)
    print(json.dumps({k:v for k,v in summary.items() if k not in ('reports','failures')},ensure_ascii=False),flush=True)
    if args.transcribe:
        model=args.model_path
        if model is None:
            snapshots=ROOT/'.tools/subtitle-models/models--Systran--faster-whisper-small/snapshots'
            found=[p for p in snapshots.iterdir() if (p/'model.bin').is_file()]
            if len(found)!=1:raise ValueError('Specify one existing local model snapshot; no download permitted')
            model=found[0]
        transcribe(reports,args.output,model.resolve(),args.threads)
        build_review_page(args.output)
    if failures:raise SystemExit(1)


if __name__=='__main__':main()
