"""Drive the real native title to its intro sequence, with bounded cleanup.

No guest state is forced. The known cyan-mecha reference is an INTRO CUTSCENE,
not interactive gameplay. P6/BMP reading uses only the standard library.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
from datetime import datetime

ROOT = Path(__file__).resolve().parents[2]


def read_rgb(path):
    data = Path(path).read_bytes()
    if data.startswith(b'P6'):
        pos, tokens = 2, []
        while len(tokens) < 3:
            while data[pos:pos+1].isspace():
                pos += 1
            if data[pos:pos+1] == b'#':
                pos = data.index(b'\n', pos) + 1
                continue
            end = pos
            while end < len(data) and not data[end:end+1].isspace():
                end += 1
            tokens.append(int(data[pos:end]))
            pos = end
        width, height, maximum = tokens
        if maximum != 255 or not data[pos:pos+1].isspace():
            raise ValueError('Unsupported P6 header')
        pos += 2 if data[pos:pos+2] == b'\r\n' else 1
        rgb = data[pos:]
        if width <= 0 or height <= 0 or len(rgb) != width*height*3:
            raise ValueError('Incomplete P6 frame')
        return width, height, rgb
    if data.startswith(b'BM'):
        offset = struct.unpack_from('<I', data, 10)[0]
        width, signed_height = struct.unpack_from('<ii', data, 18)
        bits = struct.unpack_from('<H', data, 28)[0]
        compression = struct.unpack_from('<I', data, 30)[0]
        if width <= 0 or bits not in (24, 32) or compression != 0:
            raise ValueError('Only uncompressed 24/32-bit BMP is supported')
        height, bpp = abs(signed_height), bits//8
        pitch = (width*bpp+3)&~3
        if len(data) < offset+height*pitch:
            raise ValueError('Incomplete BMP frame')
        rgb = bytearray(width*height*3)
        for y in range(height):
            source_y = height-1-y if signed_height > 0 else y
            for x in range(width):
                p = offset+source_y*pitch+x*bpp
                q = (y*width+x)*3
                rgb[q:q+3] = data[p:p+3][::-1]
        return width, height, bytes(rgb)
    raise ValueError('Expected P6 PPM or BMP')


def pixel(image, x, y):
    width, height, data = image
    p = (min(height-1, y*height//720)*width + min(width-1, x*width//1280))*3
    return data[p:p+3]


def anchors(image, rect, threshold=120):
    x0, y0, x1, y1 = rect
    return [(x, y) for y in range(y0, y1, 2) for x in range(x0, x1, 2)
            if sum(pixel(image, x, y))/3 >= threshold]


class ScreenObserver:
    def __init__(self, title_path, menu_path, prompt_path, battle_paths=(), hud_path=None):
        self.title = read_rgb(title_path)
        self.menu_anchors = anchors(read_rgb(menu_path), (545, 553, 739, 580))
        self.prompt_anchors = anchors(read_rgb(prompt_path), (535, 518, 746, 548), 95)
        if len(self.menu_anchors) < 30 or len(self.prompt_anchors) < 30:
            raise ValueError('Reference image has insufficient menu/prompt glyphs')
        self.battles=[]
        self.hud = read_rgb(hud_path) if hud_path else None
        # Match the fixed gauge rims, excluding changing ammunition digits,
        # health fills and weapon heat. Both lower corners must be represented.
        self.hud_points = [(x,y)
                           for cx,cy,lo,hi in ((161,523,65,75),(1085,558,34,41))
                           for y in range(cy-hi,cy+hi,2) for x in range(cx-hi,cx+hi,2)
                           if lo*lo<=(x-cx)**2+(y-cy)**2<=hi*hi and
                           sum(pixel(self.hud,x,y))/3>=120] if self.hud else []
        if self.hud and len(self.hud_points) < 100:
            raise ValueError('HUD reference has insufficient bright gauge pixels')
        for path in battle_paths:
            if not Path(path).exists():continue
            image=read_rgb(path)
            points=[(x,y) for y in range(110,480,5) for x in range(150,1120,5)
                    if (lambda p:p[2]>60 and p[2]>p[0]*1.3)(pixel(image,x,y))]
            if len(points)>=20:self.battles.append((image,points,str(path)))

    def inspect(self, image):
        positions = [(x, y) for y in range(195, 475, 8) for x in range(340, 970, 8)]
        logo_error = sum(sum(abs(a-b) for a,b in zip(pixel(image,x,y),pixel(self.title,x,y)))
                         for x,y in positions)/(len(positions)*3)
        def score(points):
            return sum(sum(pixel(image,x,y))/3 >= 75 for x,y in points)/len(points)
        menu_score, prompt_score = score(self.menu_anchors), score(self.prompt_anchors)
        logo = logo_error < 45
        is_menu = logo and menu_score >= .72
        is_title = logo and not is_menu and prompt_score >= .55
        samples = [pixel(image,x,y) for y in range(110,480,8) for x in range(150,1120,8)]
        nonblack = sum(max(p)>28 for p in samples)/len(samples)
        colors = len({tuple(c//16 for c in p) for p in samples})
        # Broad visual evidence only: excludes logo screens, black loading,
        # and the lower dialogue portrait region. It cannot verify gameplay.
        visual_scene = not logo and nonblack > .20 and colors > 100
        battle_score=0.
        for reference,points,_ in self.battles:
            matched=sum(sum(abs(a-b) for a,b in zip(pixel(image,x,y),pixel(reference,x,y)))/3<38
                        and pixel(image,x,y)[2]>45 for x,y in points)/len(points)
            battle_score=max(battle_score,matched)
        hud_score = (sum(sum(abs(a-b) for a,b in zip(pixel(image,x,y),pixel(self.hud,x,y)))/3 < 35
                         for x,y in self.hud_points)/len(self.hud_points)) if self.hud_points else 0.
        return dict(hud_reference_score=round(hud_score,3),logo_error=round(logo_error,3),menu_score=round(menu_score,3),
                    prompt_score=round(prompt_score,3),title=is_title,menu=is_menu,
                    nonblack=round(nonblack,3),quantized_colors=colors,
                    visual_scene_candidate=visual_scene,battle_reference_score=round(battle_score,3),
                    battle_template_match=not logo and battle_score>=.60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds',type=int,default=120,choices=range(20,601))
    parser.add_argument('--frame-every',type=int,default=30,choices=range(1,121))
    parser.add_argument('--capture-interval',type=float,default=.4,
                        help='Seconds between requested world captures; raise for performance measurements')
    parser.add_argument('--vfs',type=Path,default=ROOT/'build/game-ko-full')
    parser.add_argument('--state',type=Path,default=ROOT/'native/state-ko-full')
    parser.add_argument('--title-reference',type=Path,default=ROOT/'native/analysis/battle-preview-templates/title.ppm')
    parser.add_argument('--menu-reference',type=Path,default=ROOT/'native/evidence/title-menu.bmp')
    parser.add_argument('--prompt-reference',type=Path,default=ROOT/'native/evidence/title-screen.bmp')
    parser.add_argument('--inspect',type=Path,help='Classify an existing frame; do not launch anything')
    parser.add_argument('--battle-reference',type=Path,action='append')
    parser.add_argument('--hud-reference',type=Path,help='Known combat HUD reference for an optional test pulse')
    parser.add_argument('--hud-pulse',type=lambda s:int(s,0),help='Button mask sent once after two distinct matching HUD frames')
    parser.add_argument('--hud-polls',type=int,default=8,choices=range(1,41))
    parser.add_argument('--hud-axes',help='Optional lx,ly,rx,ry values (0..255) for the HUD test pulse')
    parser.add_argument('--hud-axes-once',action='store_true',
                        help='Use HUD axes only on the first pulse, then release them for later pulses')
    parser.add_argument('--hud-pulse-limit',type=int,default=1,choices=range(1,17),
                        help='Maximum combat HUD pulses; each needs two fresh matching captures')
    parser.add_argument('--hud-repeat-seconds',type=float,default=35,
                        help='Minimum wall seconds between optional repeated HUD pulses')
    parser.add_argument('--hud-pause-file',type=Path,
                        help='Pause automatic HUD pulses while this file exists; default: per-run hud.pause')
    parser.add_argument('--stop-on-scene-candidate','--stop-on-candidate',action='store_true',
                        help='Stop after two known INTRO CUTSCENE matches; does not verify interactive gameplay')
    parser.add_argument('--skip-intro',action='store_true',
                        help='On the first known intro match press START once; wait for manual confirmation of any new prompt')
    parser.add_argument('--continue-to-gameplay',action='store_true',
                        help='Keep running past the intro for manual gameplay inspection; no HUD/gameplay detector is claimed')
    args=parser.parse_args()
    if not .4<=args.capture_interval<=30:
        parser.error('--capture-interval must be between .4 and 30 seconds')
    if args.hud_pulse is not None and (not args.hud_reference or not 0<args.hud_pulse<=0xffff):
        parser.error('--hud-pulse needs a HUD reference and a nonzero 16-bit mask')
    hud_axes=None
    if args.hud_axes:
        try:
            hud_axes=[int(value) for value in args.hud_axes.split(',')]
            if len(hud_axes)!=4 or any(value<0 or value>255 for value in hud_axes):raise ValueError
        except ValueError:
            parser.error('--hud-axes needs four comma-separated integers between 0 and 255')
        if args.hud_pulse is None:parser.error('--hud-axes needs --hud-pulse')
    if args.hud_axes_once and not hud_axes:parser.error('--hud-axes-once needs --hud-axes')
    if not 5<=args.hud_repeat_seconds<=120:
        parser.error('--hud-repeat-seconds must be between 5 and 120 seconds')
    battle_paths=args.battle_reference or [ROOT/'native/evidence/live-owned-cube-swizzled/field.ppm',
                                          ROOT/'native/evidence/background-canonical-field/field.ppm']
    observer=ScreenObserver(args.title_reference,args.menu_reference,args.prompt_reference,battle_paths,args.hud_reference)
    if args.inspect:
        print(json.dumps(observer.inspect(read_rgb(args.inspect)),indent=2));return 0
    if os.name != 'nt':
        parser.error('Native preview orchestration requires Windows')
    output=ROOT/'native/analysis/battle-preview-runs'/datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    output.mkdir(parents=True)
    run=ROOT/'native/run'; pad=output/'pad-input.txt'
    hud_pause_path=args.hud_pause_file or output/'hud.pause'
    pad.write_text('',encoding='ascii')
    capture_request=output/'capture.request'
    capture_request.write_text('',encoding='ascii')
    env=dict(os.environ)
    env.update(MACROSS_RENDERER='live',PAD_FILE=str(pad),PAD_SCRIPT='',PAD_CLOCK='',
               LD_FRAME_DUMP=str(run/'frames'),LD_FRAME_DUMP_FROM='0',
               LD_FRAME_DUMP_EVERY=str(args.frame_every),
               LD_FRAME_DUMP_REQUEST_FILE=str(capture_request))
    command=[sys.executable,str(ROOT/'tools/play/run_native.py'),'--vfs',str(args.vfs),
             '--state',str(args.state),'--seconds',str(args.seconds),'--frame-limit','256',
             '--frame-every',str(args.frame_every),'--stop-on-job-miss']
    events=[]
    def note(kind,**details):
        item=dict(time=round(time.monotonic()-started,3),kind=kind,**details)
        events.append(item)
        with (output/'events.jsonl').open('a',encoding='utf8') as f:f.write(json.dumps(item)+'\n')
        print(json.dumps(item),flush=True)
    started=time.monotonic(); launch_wall=time.time()
    native_handle=None; native_pid=None; native_start=None; runner=None
    k32=ctypes.WinDLL('kernel32',use_last_error=True)
    k32.OpenProcess.argtypes=[ctypes.c_ulong,ctypes.c_int,ctypes.c_ulong]
    k32.OpenProcess.restype=ctypes.c_void_p
    k32.GetExitCodeProcess.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)]
    k32.TerminateProcess.argtypes=[ctypes.c_void_p,ctypes.c_uint]
    k32.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_ulong]
    k32.CloseHandle.argtypes=[ctypes.c_void_p]
    state='title'; action_at=0.; action_frame=''; confirms=0; candidate_frames=0
    seen=set(); candidate_names=set()
    last_confirm=0.; last_ack=0; reason='bounded_run_ended'
    capture_requests=0; last_capture_request=0.; intro_skip_sent=False
    hud_names=set(); hud_pulse_sent=False
    hud_pulse_count=0; last_hud_pulse=0.
    last_hud_paused=False
    title_start_retries=0
    def pulse(mask,frame):
        nonlocal action_at,action_frame
        if pad.exists() and pad.read_text(encoding='ascii').strip():return False
        pad.write_text(f'0x{mask:04X} 3',encoding='ascii')
        action_at=time.monotonic();action_frame=frame
        note('input',mask=f'0x{mask:04X}',polls=3,frame=frame,state=state)
        return True
    try:
        with (output/'runner.log').open('wb') as runner_log:
            runner=subprocess.Popen(command,cwd=ROOT,env=env,stdin=subprocess.DEVNULL,
                                    stdout=runner_log,stderr=subprocess.STDOUT)
            while runner.poll() is None and time.monotonic()-started < args.seconds+15:
                process_path=run/'process.json'
                if native_handle is None:
                    try:
                        process=json.loads(process_path.read_text())
                        if process['started'] < launch_wall-1:time.sleep(.1);continue
                        native_pid=process['pid'];native_start=process['started']
                        native_handle=k32.OpenProcess(0x100001|0x1000,False,native_pid)
                        if not native_handle:raise RuntimeError('Cannot retain exact native process handle')
                        note('native_process',pid=native_pid,pad_file=str(pad),
                             hud_pause_file=str(hud_pause_path),
                             capture_request_file=str(capture_request))
                    except (FileNotFoundError,json.JSONDecodeError):time.sleep(.1);continue
                stdout=(run/'stdout.log').read_text(errors='replace')
                observed_ack=stdout.count('[cellPad] PAD_FILE press')
                if observed_ack!=last_ack:
                    note('pad_acknowledgements',count=observed_ack,previous=last_ack)
                    last_ack=observed_ack
                stderr=(run/'stderr.log').read_text(errors='replace')
                owned_world=('[owned-cube] copied' in stderr and
                             ('0x027B0000' in stderr or '0x027B1000' in stderr))
                # Ask the renderer for its next real presented frame rather
                # than waiting another thirty slow world frames. This must run
                # before the no-new-frame shortcuts below. An empty file is ACK.
                now=time.monotonic()
                if (owned_world and capture_requests<(120 if args.skip_intro or args.continue_to_gameplay else 30) and now-last_capture_request>=args.capture_interval and
                    (not capture_request.exists() or capture_request.stat().st_size==0)):
                    capture_request.write_text('capture\n',encoding='ascii')
                    capture_requests+=1;last_capture_request=now
                    note('capture_request',number=capture_requests)
                files=sorted((run/'frames').glob('*.ppm'),key=lambda p:p.stat().st_mtime_ns)
                if not files:time.sleep(.1);continue
                frame=files[-1]
                stamp=(frame.name,frame.stat().st_size,frame.stat().st_mtime_ns)
                if stamp in seen:time.sleep(.1);continue
                try:observation=observer.inspect(read_rgb(frame))
                except (ValueError,IndexError):time.sleep(.1);continue
                seen.add(stamp)
                note('frame',frame=frame.name,state=state,**observation)
                now=time.monotonic()
                fresh=frame.name!=action_frame
                consumed=not pad.exists() or not pad.read_text(encoding='ascii').strip()
                hud_paused=hud_pause_path.exists()
                if hud_paused!=last_hud_paused:
                    note('hud_pulses_paused',paused=hud_paused,file=str(hud_pause_path))
                    last_hud_paused=hud_paused;hud_names.clear()
                if observation['hud_reference_score']<.90:
                    hud_names.clear()
                if (args.hud_pulse is not None and not hud_paused and hud_pulse_count<args.hud_pulse_limit and
                    observation['hud_reference_score']>=.90 and
                    (not hud_pulse_sent or now-last_hud_pulse>=args.hud_repeat_seconds)):
                    hud_names.add(frame.name)
                    if len(hud_names)>=2 and consumed:
                        pulse_axes=hud_axes if not args.hud_axes_once or hud_pulse_count==0 else None
                        axes_suffix=' '+ ' '.join(map(str,pulse_axes)) if pulse_axes else ''
                        pad.write_text(f'0x{args.hud_pulse:04X} {args.hud_polls}'+axes_suffix,encoding='ascii')
                        hud_pulse_sent=True
                        hud_pulse_count+=1;last_hud_pulse=now;hud_names.clear()
                        shutil.copy2(frame,output/'hud-before-pulse.ppm')
                        shutil.copy2(frame,output/f'hud-before-pulse-{hud_pulse_count:02d}.ppm')
                        note('hud_test_pulse',mask=f'0x{args.hud_pulse:04X}',polls=args.hud_polls,frame=frame.name,
                             pulse_number=hud_pulse_count,pulse_limit=args.hud_pulse_limit,
                             axes=pulse_axes,
                             reference_match_only=True,gameplay_verified=False)
                if state=='title' and observation['title']:
                    if pulse(0x0008,frame.name):state='menu'
                elif state=='menu' and observation['title'] and consumed and fresh and now-action_at>=2 and title_start_retries<3:
                    if pulse(0x0008,frame.name):
                        title_start_retries+=1
                        note('title_start_retry',attempt=title_start_retries,frame=frame.name)
                elif state=='menu' and observation['menu'] and consumed and now-action_at>=.8:
                    if pulse(0x0010,frame.name):state='select_game_start'
                elif state=='select_game_start' and observation['menu'] and fresh and consumed and now-action_at>=1.2:
                    if pulse(0x2000,frame.name):state='entering';last_confirm=now;confirms=1
                elif state=='entering' and consumed and fresh and not observation['title'] and not observation['menu']:
                    if owned_world and observation['battle_template_match']:
                        if frame.name not in candidate_names:
                            candidate_names.add(frame.name)
                            candidate_frames+=1
                            shutil.copy2(frame,output/f'scene-candidate-{candidate_frames:02d}.ppm')
                            note('scene_candidate',frame=frame.name,verified=False,
                                 scene_candidate_kind='intro_sequence',interactive_gameplay_verified=False)
                        if args.skip_intro and not intro_skip_sent:
                            if pulse(0x0008,frame.name):
                                intro_skip_sent=True;state='skip_intro_waiting_for_prompt'
                                note('intro_skip_requested',frame=frame.name,
                                     next_action='Observe the new screen; use PAD_FILE for a confirmed prompt')
                        elif args.continue_to_gameplay:
                            state='awaiting_gameplay_inspection'
                            note('manual_gameplay_inspection',frame=frame.name,pad_file=str(pad),
                                 interactive_gameplay_verified=False)
                        elif args.stop_on_scene_candidate and candidate_frames>=2:
                            reason='intro_sequence_candidate_requires_review';break
                    elif confirms<4 and now-last_confirm>=3 and observation['nonblack']>.025:
                        if pulse(0x2000,frame.name):confirms+=1;last_confirm=now
                time.sleep(.1)
    except KeyboardInterrupt:
        reason='interrupted'
    finally:
        if runner is not None and runner.poll() is None:
            # Only request stop for the exact run whose process handle we own.
            if native_handle:
                (run/'stop.request').write_text('battle preview finished',encoding='ascii')
            # If startup failed before retaining the game handle, leave the
            # bounded runner alive to perform its own mandatory child cleanup.
            try:runner.wait(timeout=12 if native_handle else args.seconds+15)
            except subprocess.TimeoutExpired:
                if native_handle:
                    code=ctypes.c_ulong()
                    if k32.GetExitCodeProcess(native_handle,ctypes.byref(code)) and code.value==259:
                        k32.TerminateProcess(native_handle,1);k32.WaitForSingleObject(native_handle,5000)
                try:runner.wait(timeout=8)
                except subprocess.TimeoutExpired:runner.kill();runner.wait(timeout=5)
        native_exited=None
        if native_handle:
            code=ctypes.c_ulong()
            native_exited=bool(k32.GetExitCodeProcess(native_handle,ctypes.byref(code)) and code.value!=259)
            k32.CloseHandle(native_handle)
        # Keep logs tied to this exact child when the next run archives native/run.
        process_path=run/'process.json'
        if native_exited and process_path.exists():
            process_record=json.loads(process_path.read_text(encoding='utf-8'))
            if process_record.get('pid')==native_pid:
                for name in ('process.json','stdout.log','stderr.log'):
                    source=run/name
                    if source.exists():shutil.copy2(source,output/name)
        result=dict(reason=reason,state=state,native_pid=native_pid,native_started=native_start,
                    native_process_exited=native_exited,runner_exit_code=runner.returncode if runner else None,
                    elapsed_seconds=round(time.monotonic()-started,2),scene_candidates=candidate_frames,
                    capture_requests=capture_requests,
                    capture_interval_seconds=args.capture_interval,
                    hud_test_pulse_sent=hud_pulse_sent,
                    hud_test_pulse_count=hud_pulse_count,
                    title_start_retries=title_start_retries,
                    scene_candidate_kind='intro_sequence' if candidate_frames else None,
                    intro_skip_requested=intro_skip_sent,
                    interactive_gameplay_verified=False,
                    game_rendering_verified=False,pad_acknowledgements=last_ack)
        (output/'result.json').write_text(json.dumps(result,indent=2),encoding='utf8')
        print(json.dumps(result,indent=2),flush=True)
    return 0 if native_exited and runner.returncode in (0,1) else 1


if __name__=='__main__':
    raise SystemExit(main())
