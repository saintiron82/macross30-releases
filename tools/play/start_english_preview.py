"""Open the prepared partial English preview without changing the normal language."""
import json
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--review-input',action='store_true',help='Enable the existing native PAD_FILE review interface in this isolated run only')
    parser.add_argument('--candidate-dir',help='Use a separately built native candidate with the prepared English resources')
    args=parser.parse_args()
    active = ROOT / 'localization/en-US/preview-active.json'
    manifest_path = Path(json.loads(active.read_text(encoding='utf8'))['manifest']) if active.exists() else ROOT / 'build/english-preview/preview.json'
    if not manifest_path.resolve().is_relative_to(ROOT / 'build'):
        raise ValueError('English preview manifest outside build directory')
    manifest = json.loads(manifest_path.read_text(encoding='utf8'))
    run = Path(manifest['run_directory'])
    if (run / 'process.json').exists():
        from datetime import datetime, timezone
        import shutil
        fresh = ROOT / 'native/analysis' / ('english-preview-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
        fresh.mkdir(parents=True)
        if (run / 'gamedata').exists():
            shutil.copytree(run / 'gamedata', fresh / 'gamedata')
        manifest['command'][manifest['command'].index('--run-directory') + 1] = str(fresh)
        manifest['command'] = [s.replace('PS3_GAMEDATA_ROOT=' + str(run / 'gamedata'),
                                          'PS3_GAMEDATA_ROOT=' + str(fresh / 'gamedata')) for s in manifest['command']]
        run = fresh
    command = [sys.executable] + manifest['command'][1:]
    if args.candidate_dir:
        from native_binary_launch import select_native_binary
        select_native_binary(ROOT,args.candidate_dir)
        if '--candidate-dir' in command:
            command[command.index('--candidate-dir')+1]=args.candidate_dir
        else:
            command += ['--candidate-dir',args.candidate_dir]
    command += ['--env','MACROSS_UI_LANGUAGE=en','--env','MACROSS_GAME_LANGUAGE=en-US']
    if args.review_input:
        pad=run/'review-pad.txt';pad.write_text('',encoding='ascii')
        command += ['--env','PAD_FILE='+str(pad),'--env','PAD_FILE_DIAG=1']
    with (run / 'launcher.log').open('ab') as log:
        child = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=log,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
    print(json.dumps(dict(launcher_pid=child.pid, run=str(run))))
