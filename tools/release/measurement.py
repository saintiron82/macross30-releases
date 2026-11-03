"""Prepare and explicitly launch the private 0.3.5 measurement composition."""
import argparse
import csv
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import time
import zipfile

_adjacent_manifest = Path(__file__).resolve().with_name('measurement.json')
ROOT = (Path(json.loads(_adjacent_manifest.read_text(encoding='utf-8'))['project_root'])
        if _adjacent_manifest.exists() else Path(__file__).resolve().parents[2])
DEFAULT = Path('D:/Game/Macross-0.3.5-measurement-20261101')
PREFIXES = ('MACROSS_', 'SPU_', 'SPURS_', 'PS3_', 'PPU_', 'LD_', 'RSX_', 'GCM_',
            'FIFO_', 'AUDIO_', 'ATRAC_', 'PAD_', 'VP_', 'FP_', 'CELLMARK_', 'RTT_', 'D3D12_', 'EDGE_', 'RIBBON_')
RELEASE_ARCHIVE = Path('D:/Game/Macross-release-034-20261101-30fps-4k/artifacts/Macross30-program-0.3.4.zip')
RELEASE_ARCHIVE_SHA = '20d314bfaaa1b97222563be2bb769c1a8d591b3d30702632982f2284d807b64b'
REQUIRED_RENDER_SETTINGS = dict(LD_ASYNC_ALLOW_MOVIE='1',LD_ASYNC_FRAME_RESOURCES='1',
    LD_ASYNC_GPU_WAKE='1',LD_ASYNC_IMMEDIATE_LABELS='1',LD_SHADER_METADATA_CACHE='1',
    LD_TEXTURE_HASH='xxh3',LD_VERTEX_RAW28='1',LD_VERTEX_RAWN='1',EDGE_NATIVE='1',
    EDGE_SKIN='1',EDGE_VERIFY='0',EDGE_ANIM='0',RIBBON_NATIVE='0',
    MACROSS_POSTFX_HOST='0',MACROSS_NATIVE_ZLIB='0',SCREEN_AS_TEX='0')


def read(path): return json.loads(Path(path).read_text(encoding='utf-8-sig'))
def write(path, value):
    path = Path(path); path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
def sha(path):
    with Path(path).open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()
def stamp(path):
    s = Path(path).stat()
    return dict(bytes=s.st_size, mtime_ns=s.st_mtime_ns)
def profiles():
    return {name+'-hd'+str(hd):dict(render_scale=scale, hd_textures=hd, fps_limit=30)
            for name, scale in [('720p','1'), ('1080p','1.5'), ('1440p','2'), ('2160p','3')] for hd in (0,1)}
def production_environment(archive=RELEASE_ARCHIVE):
    if sha(archive)!=RELEASE_ARCHIVE_SHA:raise ValueError('Public 0.3.4 archive identity mismatch')
    with zipfile.ZipFile(archive) as source:raw=source.read('program/base-game/Macross30.cmd')
    text=raw.decode('utf-8-sig')
    section=text[text.index('rem Baseline settings'):text.index('rem settings.ini')]
    environment={}
    for line in section.splitlines():
        found=re.fullmatch(r'set "([^=]+)=(.*)"',line)
        if found and '%' not in found[2]:environment[found[1]]=found[2]
    validate_render_environment(environment)
    return environment,dict(archive=str(archive),archive_sha256=RELEASE_ARCHIVE_SHA,
        script='program/base-game/Macross30.cmd',script_sha256=hashlib.sha256(raw).hexdigest())
def validate_render_environment(environment):
    wrong=[key for key,value in REQUIRED_RENDER_SETTINGS.items() if environment.get(key)!=value]
    if wrong:raise ValueError('Incomplete production render environment: '+', '.join(wrong))
def runtime_file(manifest,runtime,hd):
    if runtime=='base034':
        if hd:raise ValueError('Public 0.3.4 control has no HD option; use HD OFF')
        return manifest['control034']['file']
    return 'bin/Macross30Recompiled.exe'
def regular_tree(path):
    path = Path(path)
    for current, dirs, files in os.walk(path, followlinks=False):
        for name in dirs+files:
            p = Path(current)/name
            if p.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                raise ValueError('Input tree contains a reparse point: '+str(p))
    if path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
        raise ValueError('Input root is a reparse point')


def prepare(output):
    output = Path(output).absolute()
    if output.exists(): raise ValueError('Use a fresh measurement destination')
    candidate = ROOT/'native/subtitle-candidate/aircraft-hd-034-v3'
    product = read(candidate/'build-result.json')['products']['exe']
    if sha(candidate/'Macross30Recompiled.exe') != product['sha256']: raise ValueError('Candidate changed')
    asset = read(ROOT/'native/assets/aircraft-hd-manifest.json')
    if sha(ROOT/'native/assets/aircraft-hd.pack') != asset['pack_sha256']: raise ValueError('HD pack changed')
    output.mkdir(parents=True)
    (output/'bin').mkdir(); (output/'assets').mkdir()
    for name in ('Macross30Recompiled.exe','SDL2.dll'):
        shutil.copy2(candidate/name, output/'bin'/name)
    shutil.copy2(ROOT/'native/assets/aircraft-hd.pack', output/'assets/aircraft-hd.pack')
    binary_pins = {str(p.relative_to(output)):sha(p) for p in
                   [output/'bin/Macross30Recompiled.exe', output/'bin/SDL2.dll', output/'assets/aircraft-hd.pack']}
    (output/'control034').mkdir()
    with zipfile.ZipFile(RELEASE_ARCHIVE) as archive, (output/'control034/Macross30Recompiled.exe').open('wb') as target:
        with archive.open('program/bin/Macross30Recompiled.exe') as source:shutil.copyfileobj(source,target)
    shutil.copy2(output/'bin/SDL2.dll',output/'control034/SDL2.dll')
    for name in ('Macross30Recompiled.exe','SDL2.dll'):binary_pins['control034/'+name]=sha(output/'control034'/name)
    if binary_pins['control034/Macross30Recompiled.exe']!='62619ca694a6c545e6c72447f4a49ea3e3138d31b027c78034a752e107c174cf':
        raise ValueError('Public 0.3.4 control changed')
    ko_bundle = ROOT/'localization/image_assets/hud-quest-badges-v21'
    ko = read(ko_bundle/'ready.json'); en = read(ROOT/'localization/en-US/pack-ready.json')
    if 'images-v14' not in en['vfs']: raise ValueError('Expected collected English v14')
    inputs = {
        'ko-KR':dict(vfs=str(ROOT/'build/game-ko-full'), hdd0=str(ROOT/'native/state-ko-full/hdd0'),
                     elf=str(ko_bundle/ko['elf']['path']), subtitles=str(ROOT/'localization/subtitles/runtime')),
        'en-US':dict(vfs=str(ROOT/en['vfs']), hdd0=str(ROOT/en['state']/'hdd0'),
                     elf=str(ROOT/en['elf']), subtitles=str(ROOT/en['subtitles']), battle_catalog=str(ROOT/en['battle_catalog']))}
    metadata = [('ko-ready',ko_bundle/'ready.json'), ('ko-review',ko_bundle/'review.json'),
                ('en-ready',ROOT/'localization/en-US/pack-ready.json'),
                ('menu-audit',ROOT/'analysis/pending-menu-images-audit.json'),
                ('hd-manifest',ROOT/'native/assets/aircraft-hd-manifest.json'),
                ('hd-baseline',candidate/'baseline-audit.json'), ('hd-build',candidate/'build-result.json')]
    for name, path in metadata: write(output/'evidence'/(name+'.json'), read(path))
    for locale, values in inputs.items():
        values['pins'] = {}
        elf = Path(values['elf'])
        expected_elf = ko['elf']['sha256'] if locale=='ko-KR' else next(r['sha256'] for r in en['proofs'] if r['path']==en['elf'])
        if sha(elf) != expected_elf: raise ValueError('Localized ELF changed')
        # Freeze ELF/subtitles; original disc assets stay read-only shared inputs.
        target = output/'languages'/locale; target.mkdir(parents=True)
        shutil.copy2(elf,target/'EBOOT.elf'); values['elf']=str(target/'EBOOT.elf')
        values['elf_sha256']=expected_elf
        regular_tree(values['subtitles']); shutil.copytree(values['subtitles'],target/'subtitles')
        values['subtitles']=str(target/'subtitles')
        if 'battle_catalog' in values:
            shutil.copy2(values['battle_catalog'],target/'battle.tsv');values['battle_catalog']=str(target/'battle.tsv')
        expected = {r['filename'].lower():r['sha256'] for r in ko['files']} if locale=='ko-KR' else {
            Path(r['path']).name.lower():r['sha256'] for r in en['proofs']
            if '/DATA/PACK/' in r['path']}
        regular_tree(values['hdd0'])
        for base in (Path(values['vfs'])/'PS3_GAME/USRDIR/DATA/PACK',
                     Path(values['hdd0'])/'game/BLJS10184_INSTALL/USRDIR/data/pack'):
            present={p.name.lower():p for p in base.iterdir() if p.is_file()}
            for name,digest in expected.items():
                p=present[name]
                print('VERIFY_INPUT '+locale+' '+str(p),flush=True)
                if sha(p)!=digest:raise ValueError('Prepared language pack differs from authority: '+str(p))
                values['pins'][str(p)]=dict(sha256=digest,**stamp(p))
    saves=ROOT/'native/play/gamedata';regular_tree(saves)
    shutil.copytree(saves,output/'baseline-gamedata')
    save_pins={str(p.relative_to(output/'baseline-gamedata')):sha(p) for p in (output/'baseline-gamedata').rglob('*') if p.is_file()}
    clean,environment_provenance=production_environment()
    manifest=dict(identity='0.3.5-measurement-r2', status='prepared_not_launched_not_published',project_root=str(ROOT),
                  game_target='0.3.5', source_base_version='0.3.4', source_base_commit='c955a89057161a06f1c3ae42ea0ac09af5a9729a',
                  launcher_version='0.3.7', binary_pins=binary_pins,control034=dict(file='control034/Macross30Recompiled.exe',version='0.3.4'),inputs=inputs,profiles=profiles(),
                  baseline_save_pins=save_pins, environment=clean,environment_provenance=environment_provenance,default_profile='720p-hd0',
                  experimental_delta_included=False, true_widescreen_included=False,
                  frame_metric='Host observations of completed-frame counter, not GPU presentation timestamps',
                  input_policy='Disc resources referenced read-only and pinned; per-run HDD and saves are full private copies',
                  created_at=datetime.now(timezone.utc).isoformat())
    write(output/'measurement.json',manifest)
    shutil.copy2(__file__,output/'measurement.py')
    print(json.dumps(dict(prepared=str(output),identity=manifest['identity'],profiles=list(profiles()),game_launched=False),indent=2))
    return manifest


def verify(output):
    output=Path(output);manifest=read(output/'measurement.json')
    validate_render_environment(manifest['environment'])
    for name,digest in manifest['binary_pins'].items():
        if sha(output/name)!=digest:raise ValueError('Frozen binary changed: '+name)
    for locale,values in manifest['inputs'].items():
        if sha(values['elf'])!=values['elf_sha256']:raise ValueError('Frozen ELF changed: '+locale)
        for name,pin in values['pins'].items():
            if stamp(name)!={k:pin[k] for k in ('bytes','mtime_ns')}:
                raise ValueError('Shared input changed; prepare a new baseline: '+name)
    for name,digest in manifest['baseline_save_pins'].items():
        if sha(output/'baseline-gamedata'/name)!=digest:raise ValueError('Baseline save changed')
    return manifest


def run(output,profile,locale,runtime='candidate035'):
    import msvcrt
    output=Path(output);manifest=verify(output);values=manifest['inputs'][locale];mode=manifest['profiles'][profile]
    executable=output/runtime_file(manifest,runtime,mode['hd_textures'])
    # Refuse simultaneous games before copying the private HDD.
    query=subprocess.check_output(['powershell','-NoProfile','-Command',
        "@(Get-Process -Name Macross30Recompiled -ErrorAction SilentlyContinue).Count"],creationflags=subprocess.CREATE_NO_WINDOW)
    if int(query.strip() or b'0'):raise ValueError('A game is already running')
    session=output/'runs'/(datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')+'-'+locale+'-'+profile+'-'+runtime)
    session.mkdir(parents=True);regular_tree(values['hdd0'])
    shutil.copytree(values['hdd0'],session/'hdd0');shutil.copytree(output/'baseline-gamedata',session/'gamedata')
    (session/'hdd1').mkdir();verify(output)
    settings=f"render_scale={mode['render_scale']}\nfullscreen=0\nvsync=1\nfps_unlock=0\nbloom=1\nsubtitles=1\nhd_textures={mode['hd_textures']}\n"
    (session/'settings.ini').write_text(settings,encoding='ascii')
    env={k:v for k,v in os.environ.items() if not k.startswith(PREFIXES) and k not in ('PERF','FP_CONSTBUF','SCREEN_AS_TEX')}
    env.update(manifest['environment'])
    env.update(MACROSS_RENDERER='live',RSX_LIVE_DRAW='1',MACROSS_SAIL_PLAYBACK='1',AUDIO_BLOCK_PACING='1',
        LD_RENDER_SCALE=mode['render_scale'],MACROSS_FRAME_VBLANKS='2',MACROSS_VBLANK_INDEPENDENT='0',
        MACROSS_DT_STEP='0',MACROSS_VARIABLE_STEP='0',MACROSS_REALTIME_STEP='0',MACROSS_ACTOR_DT='0',
        MACROSS_FRAME_TIMING='1',MACROSS_FRAME_TIMING_REPORTS='0',MACROSS_FRAME_INTERVAL_CSV=str(session/'frames.csv'),
        MACROSS_HD_TEXTURE_PACK=str(output/'assets/aircraft-hd.pack'),MACROSS_GAME_LANGUAGE=locale,
        MACROSS_UI_LANGUAGE='ko' if locale=='ko-KR' else 'en',MACROSS_SUBTITLE_DIR=values['subtitles'],
        MACROSS_BATTLE_SUBTITLES='1',MACROSS_VFS_ROOT=values['vfs'],
        PS3_HDD0_ROOT=str(session/'hdd0'),PS3_HDD0_GAME_ROOT=str(session/'hdd0/game'),
        PS3_HDD1_ROOT=str(session/'hdd1'),PS3_GAMEDATA_ROOT=str(session/'gamedata'),
        PS3_DEV_FLASH=str(ROOT/'runtime/rpcs3/dev_flash'),MACROSS_FFMPEG_DIR=str(ROOT/'runtime/rpcs3'),
        EDGE_NATIVE='1',EDGE_SKIN='1',EDGE_VERIFY='0',MACROSS_FREE_RUN='1')
    if values.get('battle_catalog'):env['MACROSS_BATTLE_CATALOG_FILE']=values['battle_catalog']
    env['RSX_SHADER_CACHE_DIR']=str(output/'shader-cache')
    (output/'shader-cache').mkdir(exist_ok=True)
    validate_render_environment(env)
    env['PATH']=str(output/'bin')+os.pathsep+str(ROOT/'runtime/rpcs3')+os.pathsep+env.get('PATH','')
    command=[str(executable),values['elf'],values['vfs']]
    query=subprocess.check_output(['powershell','-NoProfile','-Command',
        "@(Get-Process -Name Macross30Recompiled -ErrorAction SilentlyContinue).Count"],creationflags=subprocess.CREATE_NO_WINDOW)
    if int(query.strip() or b'0'):raise ValueError('A game started during preparation; this session was not launched')
    with (ROOT/'native/runner.lock').open('r+b') as lease:
        lease.seek(0);msvcrt.locking(lease.fileno(),msvcrt.LK_NBLCK,1)
        with (session/'stdout.log').open('wb') as stdout,(session/'stderr.log').open('wb') as stderr:
            child=subprocess.Popen(command,cwd=session,env=env,stdout=stdout,stderr=stderr,creationflags=subprocess.CREATE_NO_WINDOW)
            record=dict(identity=manifest['identity'],pid=child.pid,profile=profile,language=locale,runtime=runtime,exe_sha256=sha(executable),
                        command=command,environment={k:v for k,v in env.items() if k.startswith(PREFIXES) or k in ('SCREEN_AS_TEX','PERF','FP_CONSTBUF')},
                        automatic_timeout=False,original_saves_read_only=True,started_at=datetime.now(timezone.utc).isoformat())
            write(session/'process.json',record);print('RUN '+str(session),flush=True)
            try:code=child.wait()
            except KeyboardInterrupt:
                child.terminate();code=child.wait()
        record.update(exit_code=code,ended_at=datetime.now(timezone.utc).isoformat(),settings_changed=(session/'settings.ini').read_text()!=settings)
        write(session/'process.json',record)
    print('EXIT '+str(code))


def analyze(path,skip=20.0,duration=60.0):
    with Path(path).open(newline='') as stream:rows=list(csv.DictReader(stream))
    if not rows:raise ValueError('No frame observations')
    origin=int(rows[0]['qpc']);freq=int(rows[0]['frequency'])
    selected=[r for r in rows if skip<=(int(r['qpc'])-origin)/freq<skip+duration]
    singles=sorted(float(r['observed_interval_ms']) for r in selected if int(r['frame_delta'])==1)
    if not singles:raise ValueError('No single-frame observations in requested window')
    def percentile(p):return singles[min(len(singles)-1,max(0,math.ceil(p*len(singles))-1))]
    return dict(metric='host completed-frame observations; not GPU presentation latency',skip_seconds=skip,requested_seconds=duration,
                window_complete=(int(rows[-1]['qpc'])-origin)/freq>=skip+duration,
                sample_count=len(singles),coalesced_observations=sum(int(r['frame_delta'])!=1 for r in selected),
                median_ms=percentile(.5),p95_ms=percentile(.95),p99_ms=percentile(.99),max_ms=max(singles),
                over_50ms=sum(v>50 for v in singles),over_100ms=sum(v>100 for v in singles),instrumentation_enabled=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,default=DEFAULT)
    sub=parser.add_subparsers(dest='command',required=True)
    sub.add_parser('prepare');sub.add_parser('verify')
    launch=sub.add_parser('run');launch.add_argument('--profile',choices=list(profiles()),default='720p-hd0')
    launch.add_argument('--language',choices=('ko-KR','en-US'),default='ko-KR')
    launch.add_argument('--runtime',choices=('candidate035','base034'),default='candidate035')
    report=sub.add_parser('analyze');report.add_argument('csv',type=Path);report.add_argument('--skip',type=float,default=20)
    report.add_argument('--duration',type=float,default=60)
    args=parser.parse_args()
    if args.command=='prepare':prepare(args.output)
    elif args.command=='verify':verify(args.output);print('PASS: frozen binaries, ELF, saves and shared input fingerprints')
    elif args.command=='run':run(args.output,args.profile,args.language,args.runtime)
    else:print(json.dumps(analyze(args.csv,args.skip,args.duration),indent=2))


if __name__=='__main__':
    main()
