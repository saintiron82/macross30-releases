"""Launch the isolated aircraft texture test on public 0.3.4, using copied saves."""
import json
import shutil
import subprocess
import sys
import argparse
from pathlib import Path
from datetime import datetime,timezone

ROOT=Path(__file__).resolve().parents[2]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reuse-test-state',type=Path)
    parser.add_argument('--candidate-dir',default='aircraft-hd-034-v3',choices=['vf0-hd-v1','vf0-hd-v2','vf0-hd-v2-r2','vf0-hd-viewer-v3','vf0-native1024-v4','aircraft-hd-options-v2','aircraft-hd-034-v3'])
    options=parser.parse_args()
    from native_binary_launch import select_native_binary
    candidate=options.candidate_dir;select_native_binary(ROOT,candidate)
    run=ROOT/'native/analysis'/('vf0-hd-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
    run.mkdir();state=run/'state'
    if candidate in ('aircraft-hd-options-v2','aircraft-hd-034-v3'):
        settings=ROOT/'native/play/settings.ini'
        lines=settings.read_text(encoding='utf8').splitlines() if settings.exists() else []
        lines=[line for line in lines if line.partition('=')[0].strip()!='hd_textures']
        (run/'settings.ini').write_text('\n'.join(lines+['hd_textures=1'])+'\n',encoding='utf8')
    # These are full copies. No HDD/pack/save hardlinks reach the real player.
    if options.reuse_test_state:
        previous=options.reuse_test_state.resolve()
        if not previous.is_relative_to(ROOT/'native/analysis') or not previous.name.startswith('vf0-hd-'):
            raise ValueError('Only a prior VF-0 diagnostic state can be reused')
        state=previous/'state';saves=previous/'gamedata'
    else:
        shutil.copytree(ROOT/'native/state-ko-full/hdd0',state/'hdd0')
        (state/'hdd1').mkdir();shutil.copytree(ROOT/'native/play/gamedata',run/'gamedata')
        saves=run/'gamedata'
    pad=run/'review-pad.txt';pad.write_text('',encoding='ascii')
    active=json.loads((ROOT/'localization/image_assets/active-ui.json').read_text(encoding='utf8'))
    bundle=ROOT/'localization/image_assets'/active['bundle']
    ready=json.loads((bundle/'ready.json').read_text(encoding='utf8'))
    stamp=state/'ui-bundle-installed.json'
    installed=json.loads(stamp.read_text(encoding='utf8')) if stamp.exists() else {}
    if installed.get('version')!=ready.get('version') or installed.get('bundle')!=bundle.name:
        subprocess.run([sys.executable,str(ROOT/'tools/localization/install_korean_ui.py'),
                        '--bundle',bundle.name,'--diagnostic-state',str(state),'--apply'],cwd=ROOT,check=True)
    chosen=active.get('elf_override',ready['elf']);elf=bundle/chosen['path']
    args=[sys.executable,str(ROOT/'tools/play/play_native.py'),'--language','ko-KR',
          '--elf',str(elf),'--vfs',str(ROOT/'build/game-ko-full'),
          '--candidate-dir',candidate,'--run-directory',str(run)]
    values=dict(PS3_HDD0_ROOT=str(state/'hdd0'),PS3_HDD1_ROOT=str(state/'hdd1'),
                PS3_HDD0_GAME_ROOT=str(state/'hdd0/game'),PS3_GAMEDATA_ROOT=str(saves),
                MACROSS_VF0_HD_DIR=str(ROOT/'experiments'/(candidate if candidate in ['vf0-hd-viewer-v3','vf0-native1024-v4'] else 'vf0-hd-v1')),PAD_FILE=str(pad),PAD_FILE_DIAG='1')
    for k,v in values.items():args+=['--env',k+'='+v]
    with (run/'launcher.log').open('wb') as stream:
        child=subprocess.Popen(args,cwd=ROOT,stdout=stream,stderr=stream,creationflags=subprocess.CREATE_NO_WINDOW)
    record=dict(run=str(run),launcher_pid=child.pid,candidate=candidate,command=args,original_saves_modified=False)
    (ROOT/'experiments/vf0-hd-v1/latest-run.json').write_text(json.dumps(record,indent=2),encoding='utf8')
    print(json.dumps(record))


if __name__=='__main__':main()
