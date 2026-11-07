"""Open the complete Russian text draft with unchanged English UI images."""
import json,subprocess,sys,shutil
from pathlib import Path
from datetime import datetime,timezone
from language_packs import resolve_language
from native_binary_launch import select_native_binary
ROOT=Path(__file__).resolve().parents[2]
def main():
    profile=resolve_language(ROOT,'ru-RU');select_native_binary(ROOT,profile['candidate_dir'])
    run=ROOT/'native/analysis'/('russian-preview-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'));run.mkdir()
    saves=ROOT/'native/play/gamedata'
    if saves.exists():shutil.copytree(saves,run/'gamedata')
    command=[sys.executable,str(ROOT/'tools/play/play_native.py'),'--language','ru-RU','--run-directory',str(run),
             '--env','PS3_GAMEDATA_ROOT='+str(run/'gamedata')]
    with (run/'launcher.log').open('wb') as log:
        child=subprocess.Popen(command,cwd=ROOT,stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW)
    print(json.dumps(dict(launcher_pid=child.pid,run=str(run),locale='ru-RU',image_locale='en-US')))
if __name__=='__main__':main()
