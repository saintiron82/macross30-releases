"""Interactive launch with the required local inputs; no scripted controller."""
import json
import os
from pathlib import Path
import subprocess
import msvcrt
import argparse
from datetime import datetime, timezone
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
# Launcher pipes and Windows consoles must agree even under a cp949 system locale.
for _stream in (_sys.stdout, _sys.stderr):
    if hasattr(_stream, 'reconfigure'):
        _stream.reconfigure(encoding='utf-8', errors='backslashreplace')
from localization_launch import prepare_localization
from language_packs import available_languages, saved_language, saved_partial_opt_in, resolve_language, apply_catalog_environment, preview_notice
from performance_mode import apply_performance_mode, performance_settings, effective_performance_mode
from native_binary_launch import select_native_binary, battle_subtitles_default
from native_run_directory import prepare_run_directory

parser = argparse.ArgumentParser()
parser.add_argument('--elf', type=Path, help='use a separately prepared localized ELF')
parser.add_argument('--vfs', type=Path, help='use a separately prepared game resource root')
parser.add_argument('--video-mode', choices=['480', '720'])
parser.add_argument('--uncapped', action='store_true', help='60Hz gate with experimental elapsed-time mission step')
parser.add_argument('--movie-stop-probe', action='store_true')
parser.add_argument('--performance-mode', choices=('trusted','verify'), default='trusted', help='validated native geometry by default; verify runs every SPU reference job')
parser.add_argument('--legacy-vblank', action='store_true', help='publish VBlank from the render loop (old jittery 16 ms tick)')
parser.add_argument('--vblank-probe', action='store_true', help='diagnostic: log VBlank callback and frame-gate wait timestamps')
parser.add_argument('--env', action='append', default=[], metavar='KEY=VALUE', help='diagnostic: set an environment variable for the game (repeatable)')
parser.add_argument('--frame-dump', help='diagnostic: dump presented frames and decoded movie frames to this directory')
parser.add_argument('--run-directory', type=Path, help='fresh diagnostic logs/settings directory beneath native/analysis')
parser.add_argument('--candidate-dir', help='diagnostic only: test a built candidate without changing normal launch selection')
parser.add_argument('--language', choices=available_languages(Path(__file__).resolve().parents[2]), help='installed language pack; resource changes require restart')
parser.add_argument('--allow-partial-language', action='store_true', help='explicitly allow an installed partial_test language pack and its reported fallback')
args = parser.parse_args()
if args.candidate_dir and not args.run_directory:parser.error('--candidate-dir requires --run-directory')

root = Path(__file__).resolve().parents[2]
exe = select_native_binary(root,args.candidate_dir)
language = args.language or saved_language(root)
allow_partial=args.allow_partial_language or (args.language is None and saved_partial_opt_in(root))
profile = resolve_language(root, language, allow_partial=allow_partial) if not (args.elf or args.vfs) else None
if profile and profile.get('candidate_dir') and not args.candidate_dir:
    exe=select_native_binary(root,profile['candidate_dir'])
elf = (args.elf.resolve() if args.elf else
       root/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf' if args.vfs else
       (profile['elf'] if profile else prepare_localization(root)))
vfs = args.vfs.resolve() if args.vfs else (profile['vfs'] if profile else root/'build/game-ko-full')
state_root = profile['state'] if profile else root/'native/state-ko-full'
for path in (exe, elf, vfs):
    if not path.exists():
        raise SystemExit(f'Missing required game input: {path}')
lease = (root / 'native/runner.lock').open('a+b')
lease.seek(0)
try:
    msvcrt.locking(lease.fileno(), msvcrt.LK_NBLCK, 1)
except OSError:
    raise SystemExit('Another game/test is running. Close it first.')
baseline = json.loads((root / 'native/analysis/frame-rate-priority/wait-owner-clean-proof/process.json').read_text(encoding='utf-8'))
env = dict(os.environ)
prefixes = ('MACROSS_', 'SPU_', 'SPURS_', 'PS3_', 'PPU_', 'LD_', 'RSX_', 'GCM_', 'FIFO_', 'AUDIO_', 'ATRAC_', 'PAD_', 'VP_', 'FP_', 'CELLMARK_', 'RTT_', 'D3D12_')
for key in list(env):
    if key.startswith(prefixes) or key in ('PERF', 'FP_CONSTBUF', 'SCREEN_AS_TEX'):
        del env[key]
for key, value in baseline['settings'].items():
    if key.startswith(('PAD_', 'CELLMARK_')) or any(word in key for word in ('TRACE', 'PROBE', 'DUMP', 'PERF', 'CAPTURE')) or key in ('MACROSS_RUN_SECONDS', 'MACROSS_FRAME_INTERVAL_CSV', 'MACROSS_FIFO_IDLE_MS_FILE'):
        continue
    env[key] = str(value)
# xxh3 texture-content hash: same once-per-frame refresh policy, ~15.0->15.4 FPS (2026-10-24).
env.setdefault('LD_TEXTURE_HASH', 'xxh3')
env.setdefault('AUDIO_BLOCK_PACING', '1')
# Frames in flight (2026-10-24): labels written at FIFO time, prefix submits,
# presents one vblank after the flip; verified HUD-complete on every present.
# LD_ASYNC_FRAME_RESOURCES=0 restores the synchronous renderer.
env.setdefault('LD_ASYNC_FRAME_RESOURCES', '1')
env.setdefault('LD_ASYNC_IMMEDIATE_LABELS', '1')
env.setdefault('LD_ASYNC_ALLOW_MOVIE', '1')
env.setdefault('LD_ASYNC_GPU_WAKE', '1')   # wake the host loop on GPU completion
env.setdefault('LD_SHADER_METADATA_CACHE', '1')   # per-program FP/VP analysis cache (verified equal to the per-draw parse)
env.setdefault('LD_VERTEX_RAW28', '1')   # Edge 28-byte vertices decoded in the vertex shader (GPU) instead of on the CPU
# Every other per-vertex format decoded on the GPU too (rsx_vertex_rawn.h). Save point,
# uncapped, two runs each in both orders: 33.87->34.60 and 33.30->33.89 FPS, same image (2026-10-25).
env.setdefault('LD_VERTEX_RAWN', '1')
env.setdefault('LD_RENDER_SCALE', '1.5')   # internal 1920x1080 render of the 1280x720 canvas (window 1920x1080); 1 = native 720p
env.update(MACROSS_RENDERER='live', RSX_LIVE_DRAW='1', MACROSS_SAIL_PLAYBACK='1',
    PS3_HDD0_ROOT=str(state_root/'hdd0'),
    PS3_HDD1_ROOT=str(state_root/'hdd1'),
    PS3_DEV_FLASH=str(root/'runtime/rpcs3/dev_flash'),
    MACROSS_VFS_ROOT=str(vfs), MACROSS_FFMPEG_DIR=str(root/'runtime/rpcs3'))
if args.video_mode:
    env['MACROSS_VIDEO_MODE'] = args.video_mode
if args.vblank_probe:
    env['MACROSS_VBLANK_PROBE'] = '1'
if not args.legacy_vblank:
    # Exact 60 Hz VBlank on its own timer; the 30 FPS cap (2 VBlanks) is unchanged.
    env['MACROSS_VBLANK_INDEPENDENT'] = '1'
# Options window (F9 in the game window) saves native/play/settings.ini;
# resolution and the 30 FPS unlock apply from here, at the next start.
settings_path = root / 'native/play/settings.ini'
saved = {}
if settings_path.exists():
    for line in settings_path.read_text(encoding='utf-8', errors='replace').splitlines():
        key, _, value = line.partition('=')
        saved[key.strip()] = value.strip()
if saved.get('render_scale') in ('1', '1.25', '1.5', '2', '2.5', '3'):
    env['LD_RENDER_SCALE'] = saved['render_scale']
# Keep the original 30 FPS cap unless the saved experimental unlock is enabled.
if saved.get('fps_unlock', '0') == '1':
    args.uncapped = True
candidate_plan=exe.parent/'plan.json'
release_locked=(candidate_plan.exists() and json.loads(candidate_plan.read_text(encoding='utf8')).get('release_policy',{}).get('fps_unlock_available') is False)
if release_locked:
    args.uncapped=False
    env.update(MACROSS_FRAME_VBLANKS='2',MACROSS_DT_STEP='0',MACROSS_VBLANK_INDEPENDENT='0')
# Never wait a third vblank for a frame that already took more than two
# (a 34-50 ms frame showed as 20 FPS); the 30 FPS cap itself is unchanged.
env.setdefault('MACROSS_FREE_RUN', '1')
# Uncapped: one frame-owned clock drives elapsed-time consumers, cached
# animation steps and original 30 Hz timers. Asset time units stay constant.
# This remains experimental; full combat/cutscene pacing is not yet validated.
if args.uncapped:
    env.update(MACROSS_FRAME_VBLANKS='1', MACROSS_VBLANK_INDEPENDENT='1', MACROSS_FREE_RUN='1',
               MACROSS_DT_STEP='1')
env['MACROSS_SUBTITLE_DIR'] = str(profile['subtitles'] if profile else root/'localization/subtitles/runtime')
env['MACROSS_GAME_LANGUAGE']=language
env['MACROSS_UI_LANGUAGE']=language
apply_catalog_environment(env,profile)
env.setdefault('MACROSS_HD_TEXTURE_PACK', str(root/'native/assets/aircraft-hd.pack'))
env['MACROSS_BATTLE_SUBTITLES'] = '1' if (profile is None or profile['battle_subtitles']) and battle_subtitles_default(root,exe) else '0'
if profile and profile.get('battle_catalog'):
    env['MACROSS_BATTLE_CATALOG_FILE']=str(profile['battle_catalog'])
    env['MACROSS_BATTLE_SUBTITLES']='1'
apply_performance_mode(env,args.performance_mode)
for item in args.env:
    key, _, value = item.partition('=')
    env[key] = value
run = prepare_run_directory(root, args.run_directory)
misses = run/'spu-misses'
misses.mkdir(exist_ok=True)
env['SPU_DUMP_MISS'] = str(misses)
if args.frame_dump:
    Path(args.frame_dump).mkdir(parents=True, exist_ok=True)
    env.update(LD_FRAME_DUMP=str(Path(args.frame_dump).resolve()), LD_FRAME_DUMP_EVERY='30', MACROSS_SAIL_DUMP=str(Path(args.frame_dump).resolve()))
if args.movie_stop_probe:
    env['MACROSS_MOVIE_STOP_PROBE'] = '1'
if (run/'stdout.log').exists():
    import shutil
    history = root/'native/play-history'/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    history.mkdir(parents=True)
    for name in ('stdout.log', 'stderr.log', 'process.json'):
        if (run/name).exists(): shutil.copy2(run/name, history/name)
print('Macross 30: Enter=Start, Z=Cross, X=Circle. Close the game window to exit.', flush=True)
print(f'Logs: {run}', flush=True)
notice=preview_notice(profile)
if notice:print(notice,flush=True)
with (run/'stdout.log').open('wb') as out, (run/'stderr.log').open('wb') as err:
    child = subprocess.Popen([str(exe), str(elf), str(vfs)], cwd=run, env=env, stdout=out, stderr=err)
    (run/'process.json').write_text(json.dumps({'pid':child.pid,'command':[str(exe),str(elf),str(vfs)],
        'language':language,'language_switch_requires_restart':True,'video_mode':args.video_mode,'uncapped':args.uncapped,'automatic_timeout':False,
        'movie_stop_probe':args.movie_stop_probe,'spu_miss_directory':str(misses),
        'started':datetime.now(timezone.utc).isoformat(),
        'performance_mode':effective_performance_mode(env),
        'settings':performance_settings(env)},indent=2),encoding='utf-8')
    try:
        if args.run_directory is None:
            code = child.wait()
        else:
            # Only a diagnostic's own fresh directory can request this stop.
            # Popen retains the actual child handle; no process-name/PID kill.
            while child.poll() is None:
                if (run/'stop.request').exists():
                    child.terminate()
                    break
                try:
                    child.wait(timeout=.25)
                except subprocess.TimeoutExpired:
                    pass
            code = child.wait()
    except KeyboardInterrupt:
        child.terminate()
        child.wait()
        code = 0
if code:
    print((run/'stderr.log').read_text(encoding='utf-8', errors='replace')[-5000:])
raise SystemExit(code)
