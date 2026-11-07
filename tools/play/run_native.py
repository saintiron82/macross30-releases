"""Run the actual recompiled game with bounded time and persisted diagnostics."""
import json
import hashlib
import argparse
import os
from pathlib import Path
import subprocess
import time
import sys
from datetime import datetime


root = Path(__file__).resolve().parents[2]
# Keep this handle alive for the entire run; OS releases the lease on failures.
lease = (root/'native/runner.lock').open('a+b')
if os.name == 'nt':
    import msvcrt
    lease.seek(0)
    if not lease.read(1):
        lease.write(b'0'); lease.flush()
    lease.seek(0)
    try:
        msvcrt.locking(lease.fileno(), msvcrt.LK_NBLCK, 1)
    except OSError:
        raise SystemExit('Another native runner is active; do not overlap tests.')
    # Also detect a child left by older runners that did not hold a lease.
    previous = root/'native/run/process.json'
    if previous.exists():
        import ctypes
        k32=ctypes.WinDLL('kernel32',use_last_error=True)
        k32.OpenProcess.restype=ctypes.c_void_p
        k32.GetExitCodeProcess.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)]
        k32.CloseHandle.argtypes=[ctypes.c_void_p]
        pid=json.loads(previous.read_text(encoding='utf-8'))['pid']
        handle=k32.OpenProcess(0x1000,False,pid)
        if handle:
            code=ctypes.c_ulong()
            active=k32.GetExitCodeProcess(handle,ctypes.byref(code)) and code.value==259
            k32.CloseHandle(handle)
            if active:
                raise SystemExit(f'Previous native test PID {pid} is still active; wait for cleanup.')
parser = argparse.ArgumentParser()
parser.add_argument('--fresh-state', action='store_true', help='Keep an isolated first-boot HDD under this run')
parser.add_argument('--vfs', type=Path, default=root/'native/vfs', help='Disc VFS root')
parser.add_argument('--state', type=Path, help='Separate persistent HDD state inside this workspace')
parser.add_argument('--stop-on-job-miss', action='store_true', help='Stop after capturing a new raw SPU job for lifting')
parser.add_argument('--seconds',type=int,default=60,choices=range(1,601),metavar='1..600')
parser.add_argument('--frame-limit',type=int,default=32,choices=range(1,257),metavar='1..256')
parser.add_argument('--frame-every',type=int,default=120,choices=range(1,601),metavar='1..600')
args = parser.parse_args()
if args.fresh_state and args.state:
    parser.error('--fresh-state and --state are mutually exclusive')
vfs=args.vfs.resolve()
if not vfs.is_dir():
    parser.error(f'VFS root does not exist: {vfs}')
if args.state and not args.state.resolve().is_relative_to(root.resolve()):
    parser.error('--state must stay inside this workspace')
run = root / 'native/run'
if run.exists() and (run / 'result.json').exists():
    archived = root / 'native/runs' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    archived.parent.mkdir(parents=True, exist_ok=True)
    run.rename(archived)
run.mkdir(parents=True, exist_ok=True)
frames = run / 'frames'
frames.mkdir(exist_ok=True)
misses=run/'spu_misses'
misses.mkdir(exist_ok=True)
state = run/'state' if args.fresh_state else (args.state.resolve() if args.state else root/'native/state')
for name in ('hdd0','hdd1'):
    (state / name).mkdir(parents=True, exist_ok=True)
env = dict(os.environ)
for key,value in dict(MACROSS_LFQUEUE_BRIDGE='1',MACROSS_LFQUEUE_WINDOW='1',
                      MACROSS_JOB_QUEUE_BACKOFF='1',MACROSS_JOB_QUEUE_BACKOFF_EVERY='1',
                      MACROSS_POSTTASK_NATIVE='1',
                      SPU_JOB_FP_CACHE='1',
                      SPURS_JOB_DIRECT_STAGE='1',
                      AUDIO_BLOCK_PACING='1',
                      PS3_JC_LATCH='1',PS3_JC_SYNTHETIC_EVENTS='0',PS3_VERBOSE='0',
                      GCM_FLIPCB_ONTICK='1', GCM_FIFO_STRICT='1', SCREEN_AS_TEX='0', RSX_PRESENT_ON_FLIP='1', VP_NOFIXPROJ='1').items():
    env.setdefault(key,value)
env.update(PS3_TITLE='Macross 30 - static recompilation bring-up',
           MACROSS_RUN_SECONDS=str(args.seconds), RSX_LIVE_DRAW='1',
           PS3_HDD0_ROOT=str(state/'hdd0'), PS3_HDD1_ROOT=str(state/'hdd1'),
           PS3_DEV_FLASH=str(root/'runtime/rpcs3/dev_flash'),
           SPU_DUMP_MISS=str(misses),
           MACROSS_VFS_ROOT=str(vfs), MACROSS_FFMPEG_DIR=str(root/'runtime/rpcs3'),
           CELLMARK_DUMP=str(args.frame_limit), CELLMARK_DUMP_MINDRAWS='1', CELLMARK_DUMP_EVERY=str(args.frame_every),
           CELLMARK_DUMP_DIR=str(frames))
if env.get('MACROSS_RENDERER') == 'live':
    env.setdefault('LD_FRAME_DUMP', str(frames))
    env.setdefault('LD_FRAME_DUMP_EVERY', str(args.frame_every))
command = [str(root/'native/build/Macross30Recompiled.exe'),
           str(root/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf'), str(vfs)]
start = time.time()
with (run/'stdout.log').open('wb') as stdout, (run/'stderr.log').open('wb') as stderr:
    child = subprocess.Popen(command, cwd=run, env=env, stdin=subprocess.DEVNULL,
                             stdout=stdout, stderr=stderr)
    try:
        settings = {k: env[k] for k in ('MACROSS_LFQUEUE_BRIDGE','MACROSS_LFQUEUE_WINDOW',
                    'MACROSS_RUN_SECONDS','MACROSS_RENDERER','MACROSS_FRAME_TIMING','MACROSS_FIFO_IDLE_MS','MACROSS_FIFO_IDLE_MS_FILE','RSX_LIVE_DRAW','PS3_JC_LATCH','PS3_VERBOSE',
                    'PS3_JC_SYNTHETIC_EVENTS','GCM_FLIPCB_ONTICK','GCM_FIFO_STRICT','SPU_MBOXTRACE',
                    'PAD_FILE','PAD_FILE_DIAG','PAD_SCRIPT','PAD_CLOCK','PAD_STICK','MACROSS_FONT_PROBE','MACROSS_LUA_PROBE','MACROSS_VBLANK_PROBE',
                    'AUDIO_PEAK','AUDIO_RATE','AUDIO_WAV','PS3_MUTE',
                    'AUDIO_BLOCK_PACING','AUDIO_RING_TIMING','AUDIO_RING_TIMING_LIMIT','AUDIO_RING_TIMING_GATE',
                    'SPURS_JOB_PROBE_IMAGE','SPURS_JOB_DUMP','SPU_DMA_REPEAT_SECS',
                    'PS3_HDD0_ROOT','PS3_HDD1_ROOT','CELLMARK_DUMP','CELLMARK_DUMP_EVERY',
                    'MACROSS_SAIL_PLAYBACK','MACROSS_VFS_ROOT','MACROSS_FFMPEG_DIR') if k in env}
        settings.update({k: env[k] for k in ('RSX_TARGET_TRACE','LD_PRESENT_DBG','RSX_DRAW_CSV','RSX_DRAW_CSV_GATE','MACROSS_RSX_FRAME_TRACE','MACROSS_RSX_FRAME_TRACE_START_MS','MACROSS_RSX_FRAME_TRACE_LIMIT','SCREEN_AS_TEX','RSX_PRESENT_ON_FLIP','VP_NOFIXPROJ','MACROSS_NV3089_PROBE','MACROSS_NV3089_PROBE_START_MS','D3D12_DBG','D3D12_IQ','RTT_SAVERT','RTT_SAVERT_AFTER','PERF','FP_CONSTBUF') if k in env})
        settings.update({k: env[k] for k in ('LD_FRAME_DUMP', 'LD_FRAME_DUMP_EVERY', 'LD_FRAME_DUMP_REQUEST_FILE',
            'LD_FRAME_DUMP_FROM', 'LD_SURF_DUMP', 'LD_SURF_DUMP_FRAME', 'LD_SURF_DUMP_GATE', 'LD_RAW_SURFACE_DUMP',
            'FIFO_PUT_WAKE', 'FIFO_PARK_PATCH_WAKE', 'GCM_FIFO_PUT_KICK', 'GCM_FIFO_WAIT_DIAG',
            'GCM_PARK_RELEASE_DIAG', 'GCM_PARK_RELEASE_DIAG_FROM_FLIP',
            'LD_SRGB_DIAG', 'LD_SRGB_DIAG_FROM',
            'LD_TRACE_PSO', 'LD_TRACE_PSO_FROM',
            'LD_CANONICAL_CAPTURE_DIR', 'LD_CANONICAL_CAPTURE_VP',
            'LD_CANONICAL_CAPTURE_PSO', 'LD_CANONICAL_CAPTURE_PS_HASH', 'LD_CANONICAL_CAPTURE_FROM',
            'LD_CANONICAL_CAPTURE_MIN_VERTICES', 'LD_CANONICAL_CAPTURE_TARGET', 'SPURS_EF_STATS',
            'LD_CANONICAL_CAPTURE_SPREAD', 'SPU_TASK_PERF', 'SPU_PCSAMPLE',
            'LD_CANONICAL_CAPTURE_FP',
            'LD_CANONICAL_CAPTURE_SURFACES', 'LD_RAW_SURFACE_DUMP',
            'LD_DRAW_PAIR_DIR', 'LD_DRAW_PAIR_VP', 'LD_DRAW_PAIR_TARGET',
            'LD_DRAW_PAIR_MIN_VERTICES', 'LD_DRAW_PAIR_MAX_VERTICES', 'LD_DRAW_PAIR_FROM', 'LD_DRAW_PAIR_REQUEST_FILE',
            'LD_DRAW_PAIR_FIRST_TARGET', 'LD_CANONICAL_CAPTURE_FIRST_TARGET', 'LD_CANONICAL_CAPTURE_MAX_VERTICES',
            'LD_DRAW_PAIR_COLOR_ONLY', 'LD_CANONICAL_CAPTURE_COLOR_ONLY',
            'LD_DRAW_PAIR_SCAN_LIMIT', 'LD_DRAW_PAIR_SCAN_NONBLACK_PERCENT',
            'SPU_SMC_CAPTURE_DIR', 'SPU_SMC_CAPTURE_IMAGE', 'SPU_SMC_CAPTURE_FULL_LS',
            'SPU_SMC_CAPTURE_PC',
            'MACROSS_POSTTASK_NATIVE',
            'MACROSS_JOB_QUEUE_BACKOFF', 'MACROSS_JOB_QUEUE_BACKOFF_EVERY',
            'SPU_DMA_REPEAT_CONTENT', 'SPU_DMA_REPEAT_DIAG',
            'MACROSS_SPURS_STAGE_PERF', 'MACROSS_SPURS_STAGE_PERF_FROM_MS',
            'FIFO_PARK_WAKE_COALESCE',
            'MACROSS_POSTTASK_PERF',
            'MACROSS_POSTTASK_DMA_TRACE',
            'MACROSS_POSTTASK_DMA_TRACE_GATE',
            'MACROSS_SPU_FUNCTION_PERF', 'MACROSS_SPU_FUNCTION_PERF_INTERVAL',
            'MACROSS_SPU_FUNCTION_PERF_REPORTS',
            'SPU_JOBPROF',
            'SPURS_JOB_STAGE_PERF',
            'SPURS_JOB_DIRECT_STAGE',
            'SPURS_JOBLIST_PARALLEL',
            'SPURS_POOL_DIAG',
            'SPURS_CHAIN_OWNER_POOL',
            'MACROSS_MAIN_CPU_PROBE',
            'MACROSS_MAIN_CPU_PROBE_OUTPUT',
            'MACROSS_MAIN_CPU_PROBE_FROM_MS',
            'MACROSS_MAIN_CPU_PROBE_DURATION_MS',
            'MACROSS_MAIN_CPU_PROBE_INTERVAL_MS',
            'MACROSS_GEOM_CPU_PROBE',
            'MACROSS_GEOM_CPU_PROBE_OUTPUT',
            'MACROSS_GEOM_CPU_PROBE_FROM_MS',
            'MACROSS_GEOM_CPU_PROBE_DURATION_MS',
            'MACROSS_GEOM_CPU_PROBE_INTERVAL_MS',
            'MACROSS_GEOM_C5B0_CAPTURE',
            'MACROSS_GEOM_C5B0_CAPTURE_DIR',
            'MACROSS_GEOM_C5B0_CAPTURE_GATE',
            'MACROSS_PPU_WAIT_BACKOFF',
            'MACROSS_PPU_WAIT_TRACE',
            'ATRAC_STREAM_DIAG',
            'ATRAC_PCM_IDENTITY',
            'ATRAC_PCM_IDENTITY_FULL',
            'AUDIO_ADD_TRACE',
            'MACROSS_POSTTASK_F11_LUT',
            'LD_GPU_TIMING',
            'GCM_FIFO_DRAIN_SPLIT',
            'MACROSS_FRAME_VBLANKS',
            'MACROSS_VBLANK_PROBE',
            'LD_VERTEX_RAW28',
            'LD_PRESENT_INTERVAL',
            'LD_PRESENT_TIMING',
            'LD_CPU_DRAW_PHASES',
            'MACROSS_VBLANK_INDEPENDENT',
            'MACROSS_REALTIME_STEP',
            'MACROSS_REALTIME_STEP_PROBE',
            'MACROSS_VARIABLE_STEP',
            'LD_GPU_TIMING_OUTPUT',
            'LD_GPU_TIMING_GATE',
            'LD_GPU_TIMING_MAX_BATCHES',
            'LD_RESOURCE_CONSUMER_TRACE',
            'LD_RESOURCE_CONSUMER_TRACE_GATE',
            'LD_RESOURCE_CONSUMER_OFFSET',
            'LD_RESOURCE_CONSUMER_BYTES',
            'SPU_JOB_FP_CACHE',
            'PS3_FTRACE', 'PS3_FTRACE_MIN_US', 'PS3_FTRACE_MERGE_US', 'PS3_FTRACE_CAP',
            'MACROSS_FRAME_INTERVAL_CSV',
            'LD_SHADER_METADATA_CACHE', 'LD_SHADER_METADATA_VERIFY',
            'LD_FP_VARIANT_PROFILE', 'LD_ASYNC_FRAME_RESOURCES',
            'LD_ASYNC_LABEL_BATCH',
            'LD_ASYNC_GPU_WAKE',
            'FIFO_PARK_PATCH_WAKE',
            'LD_MAIN_ALIAS_AUDIT', 'LD_MAIN_ALIAS_AUDIT_GATE',
            'LD_MAIN_ALIAS_AUDIT_ROW',
            'LD_TEXTURE_HASH',
            'SPU_COH_HOLDER_INDEX',
            'RSX_DRAW_CSV_REQUEST_FILE',
            'LD_TEXRGBA_DUMP', 'LD_TEXRGBA_DUMP_OFFSET', 'LD_TEXRGBA_DUMP_DIR',
            'LD_TEX_DESCRIPTOR_TRACE', 'LD_TEX_DESCRIPTOR_TRACE_ALT',
            'GCM_A2O_TRACE', 'GCM_A2O_TRACE_CAP',
            'GCM_TEXTURE_PACKET_TRACE',
            'GCM_M2MF_TRACE',
            'FP_CAL_RET_TRACE', 'FP_CAL_RET_DUMP', 'LD_PRIMITIVE_REJECT_TRACE',
            'PPU_WVAL', 'PPU_WVAL_GATE', 'PPU_WVAL_LO', 'PPU_WVAL_HI',
            'PPU_WWATCH', 'PPU_WWATCH_LEN', 'PPU_WWATCH_MAX',
            'PPU_RWATCH',
            'SPURS_JOB_PROBE_IMAGE', 'SPURS_JOB_PROBE_ONLY', 'SPURS_JOB_DUMP',
            'RSX_DRAW_CSV_FROM', 'RSX_DRAW_CSV_LIMIT', 'RSX_VERTEX_DIAG_DIR', 'LD_HLSL_DUMP') if k in env})
        (run/'process.json').write_text(json.dumps(dict(pid=child.pid,command=command,started=start,
            settings=settings,exe_sha256=hashlib.sha256(Path(command[0]).read_bytes()).hexdigest()),indent=2),encoding='utf-8')
        print('Native game PID:',child.pid,flush=True)
        external_limit = False
        stopped_for_new_job = False
        while child.poll() is None:
            if (run/'stop.request').exists():
                child.terminate()
                break
            if args.stop_on_job_miss and any(misses.glob('*.bin')):
                child.terminate()
                stopped_for_new_job=True
                break
            if time.time()-start > args.seconds+3 or (run/'stderr.log').stat().st_size > 64*1024*1024:
                child.terminate()
                external_limit = True
                break
            time.sleep(0.5)
        code = child.wait(timeout=10)
    finally:
        # Also close the exact child if logging, polling or user interruption fails.
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=5)
result = dict(exit_code=code,elapsed_seconds=round(time.time()-start,2),external_limit=external_limit,
              captured_frames=[str(p.relative_to(root)) for p in frames.iterdir() if p.suffix.lower() in ('.bmp', '.ppm', '.png')],
              game_rendering_verified=False,process_exited=child.poll() is not None)
result['stopped_for_new_job']=stopped_for_new_job
result['raw_spu_jobs']=[str(p.relative_to(root)) for p in misses.glob('*.bin')]
(run/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps(result,indent=2))
