"""Check 0.4.0 through the shipped launcher bridge without starting the game."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zipfile
from PyInstaller.archive.readers import CArchiveReader

ROOT=Path(__file__).resolve().parents[2]
sys.dont_write_bytecode=True
OUT=ROOT/'build/tests/release040'
ASSETS=Path(os.environ.get('MACROSS_RELEASE040_ASSETS',str(ROOT/'build/releases/0.4.0/candidate/assets'))).resolve()
assert ASSETS.is_relative_to(ROOT/'build/releases/0.4.0')
sys.path.insert(0,str(ROOT/'tools/release'))
from pipeline import sha,write
from privacy import check_owned_bytes


def main():
    OUT.mkdir(exist_ok=True)
    boot=json.loads((ASSETS/'BOOT-INSTALL.json').read_text('utf8'))
    launcher=ROOT/'build/downloads/release-privacy-20261115/Macross30Launcher-0.3.9.exe'
    assert sha(launcher)=='12c83880df2b7bd15b8c2a7e797b662c4559f588d7de8d6c55ce8225bfa7c9ba'
    bridge=OUT/'public-039-bridge.exe'
    bridge.write_bytes(CArchiveReader(str(launcher)).extract('bridge.exe'))
    root=OUT/'installation 한글';root.mkdir(exist_ok=True)
    for name,content in [('bin/Macross30Recompiled.exe',b'MZ old fixture'),('play/settings.ini',b'render_scale=3\nfullscreen=0\nvsync=1\nrumble=1\nrumble_strength=80\nmouse_sensitivity_x=0.37\n'),
                         ('play/gamedata/fixture/save.bin',b'PRIVATE TEST SAVE'),('data/hdd0/home/00000001/savedata/fixture/save.bin',b'PRIVATE TEST SAVE')]:
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(content)
    protected={n:sha(root/n) for n in ['play/settings.ini','play/gamedata/fixture/save.bin','data/hdd0/home/00000001/savedata/fixture/save.bin']}
    write(root/'release-profile.json',dict(mode='user-original-eboot',release_version='0.3.10',exe_sha256=sha(root/'bin/Macross30Recompiled.exe'),language_views={}))
    def call(op,locale='en-US',**kwargs):
        request=OUT/'bridge-request.json';write(request,dict(op=op,root=str(root),locale=locale,**kwargs))
        r=subprocess.run([str(bridge),str(request)],capture_output=True,text=True,encoding='utf8',creationflags=subprocess.CREATE_NO_WINDOW,timeout=60)
        result=json.loads(r.stdout);assert r.returncode==0 and result['ok'],result
        return result['result']
    row=boot['files']['program']
    program=Path(call('extract',archive=str(ASSETS/row['file']),artifact=row)['path'])
    call('stage',program=str(program),version='0.4.0',artifact=row)
    assert call('inspect',manifest=boot)['runtime_status']==2
    sys.path[:0]=[str(program/'tools/local_build')]
    import prebuilt_finalize
    languages=[]
    for locale in ['ko-KR','en-US']:
        row=boot['language_packs'][locale]
        payload=Path(call('extract-language',locale=locale,archive=str(ASSETS/row['file']),artifact=row)['path'])
        prebuilt_finalize.prepare_elf(root,payload,ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf',locale=locale)
        assert sha(root/('bin/EBOOT.'+locale+'.elf'))==row['elf_sha256']
        state=root/('view-'+locale)/'data';state.mkdir(parents=True,exist_ok=True)
        (state/'installed-version.txt').write_text(row['data_id'],encoding='utf8')
        profile=json.loads((root/'release-profile.json').read_text('utf-8-sig'))
        profile['language_views'][locale]=dict(root='view-'+locale,elf='bin/EBOOT.'+locale+'.elf',files=[],**{k:row[k] for k in ['data_id','elf_sha256','plan_sha256']})
        write(root/'release-profile.json',profile)
        assert call('inspect',locale=locale,manifest=boot)['language_status']==2
        languages.append(locale)
    frozen=ROOT/'build/releases/0.3.12/display-privacy-20261115/public-source'
    sys.path[:0]=[str(frozen/p) for p in ['tools/play','tools/localization','tools/launcher-python','tools/analysis']]
    from pack_registry import import_pack,validate_pack
    packs=[]
    for locale,row in boot['locale_packs'].items():
        admitted=import_pack(root,ASSETS/row['file'])
        assert sha(admitted['manifest'])==row['manifest_sha256']
        doc,resources=validate_pack(root,Path(admitted['manifest']).parent)
        catalog=Path(admitted['manifest']).parent/doc['resources']['native_ui']
        expected=ROOT/'localization/native-ui/0.4.0'/('en-US-fix.tsv' if locale=='en-US' else locale+'.tsv')
        assert catalog.read_bytes()==expected.read_bytes()
        packs.append(locale)
    for name,expected in protected.items(): assert sha(root/name)==expected,name
    command=root/'Macross30.cmd'
    command.write_bytes(prebuilt_finalize.launcher_command((program/'base-game/Macross30.cmd').read_text('utf8')))
    old=(root/'play/settings.ini').read_bytes()
    environments=[]
    for settings,limit in [('fps_unlock=0\n',30),('fps_unlock=1\n',0),('fps_limit=30\n',30),('fps_limit=60\n',60),('fps_limit=0\n',0),('fps_limit=60\nfps_unlock=1\n',60),('fps_unlock=0\nfps_limit=60\n',60)]:
        (root/'play/settings.ini').write_bytes(old+settings.encode())
        run=subprocess.run(['cmd.exe','/d','/c',str(command),'--print-env'],cwd=root,capture_output=True,text=True,errors='replace',timeout=30)
        assert run.returncode==0,(run.stdout,run.stderr)
        env=dict(re.findall(r'^([A-Z][A-Z0-9_]*)=([^\r\n]*)',run.stdout,re.M))
        for key,expected in dict(MACROSS_FPS_LIMIT=str(limit),MACROSS_FRAME_VBLANKS=str(2 if limit==30 else 1),MACROSS_DT_STEP='1',EDGE_ANIM='1',EDGE_ANIM_VERIFY='0',LD_RENDER_SCALE='3').items():
            assert env.get(key)==expected,(key,env.get(key),expected)
        assert 'MACROSS_MEASURE' not in env
        environments.append(dict(settings=settings,fps_limit=limit,delta_enabled=True,game_started=False))
    (root/'play/settings.ini').write_bytes(old)
    for name,expected in protected.items(): assert sha(root/name)==expected,name
    members=0
    for p in ASSETS.glob('*.zip'):
        with zipfile.ZipFile(p) as z:
            for name in z.namelist():
                if name.endswith('/'): continue
                data=z.read(name);check_owned_bytes(data)
                assert not re.search(rb'(?i)[A-Z]:[\\/]+Users[\\/]+saint',data),name
                members+=1
    report=dict(passed=True,public_launcher_bridge='0.3.9',program_stage_and_inspect=True,
        language_elf_and_update_status=languages,additional_pack_imports=packs,new_native_ui_bytes_verified=True,
        command_environments=environments,private_path_and_credential_scan_members=members,
        synthetic_saves_and_settings_unchanged=True,game_started=False,full_gui_update_tested=False,
        actual_hardware_playtest=False)
    write(OUT/'ACCEPTANCE.json',report)
    print(json.dumps(report,ensure_ascii=False),flush=True)


if __name__=='__main__':main()
