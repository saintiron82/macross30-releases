"""Exercise the merged release sources with hidden Win32 controls and fake input."""
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'build/tests/runtime040-integration'
SRC = ROOT/'build/releases/0.4.0/runtime-ui-nof8-20261126/src'


def main():
    OUT.mkdir(exist_ok=True)
    vswhere = Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
    vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-property', 'installationPath'], text=True).strip()
    compiler = Path(vs)/'VC/Tools/Llvm/x64/bin/clang-cl.exe'
    for name in ['test_pad_mouse.c', 'test_mouse_settings.c', 'test_mouse_capture.c',
                 'test_mouse_button_pad.c', 'test_mouse_button_binding.c']:
        text = (ROOT/'tools/tests'/name).read_text('utf8')
        for group, source in [('input','pad_mouse.h'),('input','cellPad.c'),('video','rsx_null_backend.c')]:
            text = text.replace('../../vendor/ps3recomp-master/libs/'+group+'/'+source, (SRC/source).as_posix())
        if name == 'test_mouse_settings.c':
            text=text.replace('ID_OPT_MOUSE_X','ID_KEY_MOUSE_X').replace('ID_OPT_MOUSE_Y','ID_KEY_MOUSE_Y')
            text=text.replace('ID_OPT_MOUSE_INVERT','ID_KEY_MOUSE_INVERT').replace('ID_OPT_MOUSE','ID_KEY_MOUSE')
            text=text.replace('null_opt_wndproc(NULL,WM_COMMAND,MAKEWPARAM(ID_KEY_MOUSE_',
                              'null_key_wndproc(NULL,WM_COMMAND,MAKEWPARAM(ID_KEY_MOUSE_')
            text=text.replace('GetDlgItemTextW(s_opt_hwnd,ID_KEY_MOUSE_', 'GetDlgItemTextW(s_key_hwnd,ID_KEY_MOUSE_')
            text=text.replace('null_opt_wndproc(NULL,WM_COMMAND,MAKEWPARAM(ID_KEY_MOUSE',
                              'null_key_wndproc(NULL,WM_COMMAND,MAKEWPARAM(ID_KEY_MOUSE')
            text=text.replace('assert(GetDlgItem(s_opt_hwnd,ID_KEY_MOUSE)); assert(GetDlgItem(s_opt_hwnd,ID_KEY_MOUSE_INVERT));',
                              'assert(!GetDlgItem(s_opt_hwnd,ID_KEY_MOUSE)); assert(!GetDlgItem(s_opt_hwnd,ID_KEY_MOUSE_INVERT));')
            marker='    wchar_t shown[32];'
            text=text.replace(marker,'''    null_open_keys(s_opt_hwnd);assert(s_key_hwnd);
    assert(!GetDlgItem(s_opt_hwnd,ID_KEY_MOUSE_X) && !GetDlgItem(s_opt_hwnd,ID_KEY_MOUSE_Y));
    assert(GetDlgItem(s_key_hwnd,ID_KEY_MOUSE_X) && GetDlgItem(s_key_hwnd,ID_KEY_MOUSE_Y));
    assert(GetDlgItem(s_key_hwnd,ID_KEY_MOUSE) && GetDlgItem(s_key_hwnd,ID_KEY_MOUSE_INVERT));
'''+marker)
            text = text.replace('int main(void) {',
                'void cellPad_set_rumble_enabled(int enabled) {(void)enabled;}\n'
                'void cellPad_set_rumble_strength(unsigned percent) {(void)percent;}\n'
                'static int fps_got_mode;\nstatic int test_fps_apply(int mode) {fps_got_mode=mode;return mode;}\n'
                'static int test_fps_reject(int mode) {(void)mode;return 0;}\n'
                'int main(void) {')
            text = text.replace('    DestroyWindow(s_opt_hwnd); DestroyWindow(s_state.hwnd);', '''
    assert(ID_KEY_MOUSE_INVERT < ID_KEY_BASE);
    assert(GetDlgItem(s_opt_hwnd,ID_OPT_RUMBLE) && GetDlgItem(s_opt_hwnd,ID_OPT_RUMBLE_STRENGTH));
    RECT client, last; GetClientRect(s_key_hwnd,&client);
    GetWindowRect(GetDlgItem(s_key_hwnd,ID_KEY_MOUSE_Y),&last);
    MapWindowPoints(NULL,s_key_hwnd,(POINT*)&last,2);
    assert(last.bottom <= client.bottom);
    s_set.rumble=1; s_set.rumble_strength=80; null_settings_save();
    s_mouse_sx=37; null_settings_save(); s_set.rumble=0; s_set.rumble_strength=100;
    null_settings_load(); assert(s_set.rumble==1 && s_set.rumble_strength==80 && s_mouse_sx==37);
    FILE* custom=fopen("settings.ini","a"); assert(custom); fputs("custom_option=preserve\\n",custom); fclose(custom);
    s_set.rumble_strength=90; assert(null_settings_save_rumble_strength());
    null_settings_load(); assert(s_mouse_sx==37 && s_set.rumble_strength==90);
    custom=fopen("settings.ini","r"); char all[2048]={0}; fread(all,1,sizeof(all)-1,custom); fclose(custom);
    assert(strstr(all,"custom_option=preserve") && strstr(all,"mouse_sensitivity_x=0.37"));
    HWND fps=GetDlgItem(s_opt_hwnd,ID_OPT_UNLOCK); assert(fps);
    assert(SendMessageW(fps,CB_GETCOUNT,0,0)==3);
    wchar_t unlimited[64]; SendMessageW(fps,CB_GETLBTEXT,2,(LPARAM)unlimited);
    assert(!wcscmp(unlimited,macross_ui_string("options.fps_limit.unlimited")));
    s_fps_apply=test_fps_apply;
    const int limits[]={30,60,0}, modes[]={0,2,1};
    for(int index=0;index<3;index++) {
        SendMessageW(fps,CB_SETCURSEL,index,0);
        null_opt_wndproc(s_opt_hwnd,WM_COMMAND,MAKEWPARAM(ID_OPT_UNLOCK,CBN_SELCHANGE),(LPARAM)fps);
        assert(s_set.fps_limit==limits[index] && fps_got_mode==modes[index]);
        assert(s_set.rumble_strength==90 && s_mouse_sx==37);
        s_set.fps_limit=30; null_settings_load(); assert(s_set.fps_limit==limits[index]);
    }
    s_fps_apply=test_fps_reject;SendMessageW(fps,CB_SETCURSEL,1,0);
    null_opt_wndproc(s_opt_hwnd,WM_COMMAND,MAKEWPARAM(ID_OPT_UNLOCK,CBN_SELCHANGE),(LPARAM)fps);
    assert(s_set.fps_limit==30 && SendMessageW(fps,CB_GETCURSEL,0,0)==0);
    const char* legacy[]={"fps_unlock=0\\n","fps_unlock=1\\n","fps_limit=60\\nfps_unlock=1\\n",
        "fps_unlock=0\\nfps_limit=60\\n","fps_limit=120\\n","fps_limit=60bad\\n"};
    const int migrated[]={30,0,60,60,30,30};
    for(int i=0;i<6;i++) {
        custom=fopen("settings.ini","w");fputs(legacy[i],custom);fclose(custom);
        null_settings_load();assert(s_set.fps_limit==migrated[i]);
    }
    DestroyWindow(s_opt_hwnd); DestroyWindow(s_state.hwnd);''')
        if name=='test_mouse_button_binding.c':
            text=text.replace('    DestroyWindow(s_key_hwnd); DestroyWindow(owner);', '''
    memset(held,0,sizeof held);test_foreground=s_key_hwnd;choose(12);assert(s_key_capture==12);
    HWND sensitivity=GetDlgItem(s_key_hwnd,ID_KEY_MOUSE_X);assert(sensitivity);
    SetWindowTextW(sensitivity,L"0.75");
    null_key_wndproc(s_key_hwnd,WM_COMMAND,MAKEWPARAM(ID_KEY_MOUSE_X,CBN_EDITCHANGE),(LPARAM)sensitivity);
    assert(s_key_capture==-1 && s_mouse_sx==75 && got_x==75);
    choose(11);assert(s_key_capture==11);
    HWND enable=GetDlgItem(s_key_hwnd,ID_KEY_MOUSE);SendMessageW(enable,BM_SETCHECK,BST_CHECKED,0);
    null_key_wndproc(s_key_hwnd,WM_COMMAND,MAKEWPARAM(ID_KEY_MOUSE,BN_CLICKED),(LPARAM)enable);
    assert(s_key_capture==-1 && got_on && s_mouse_enabled);
    HWND invert=GetDlgItem(s_key_hwnd,ID_KEY_MOUSE_INVERT);SendMessageW(invert,BM_SETCHECK,BST_CHECKED,0);
    null_key_wndproc(s_key_hwnd,WM_COMMAND,MAKEWPARAM(ID_KEY_MOUSE_INVERT,BN_CLICKED),(LPARAM)invert);
    assert(got_inv && s_mouse_invert);
    DestroyWindow(s_key_hwnd);null_open_keys(owner);assert(s_key_hwnd);
    wchar_t restored[32];GetDlgItemTextW(s_key_hwnd,ID_KEY_MOUSE_X,restored,32);
    assert(!wcscmp(restored,L"0.75"));
    assert(SendMessageW(GetDlgItem(s_key_hwnd,ID_KEY_MOUSE),BM_GETCHECK,0,0)==BST_CHECKED);
    assert(SendMessageW(GetDlgItem(s_key_hwnd,ID_KEY_MOUSE_INVERT),BM_GETCHECK,0,0)==BST_CHECKED);
    s_mouse_sx=100;null_settings_load();assert(s_mouse_sx==75);
    DestroyWindow(s_key_hwnd); DestroyWindow(owner);''')
        (OUT/name).write_text(text,encoding='utf8')
    include = [SRC]+[ROOT/'vendor/ps3recomp-master'/p for p in ['include','runtime','runtime/platform','runtime/ppu','runtime/spu',
        'libs/video','libs/input','libs/misc','libs/system','libs/filesystem']]
    cases = []
    for name in ['test_pad_mouse','test_mouse_settings','test_mouse_capture','test_mouse_button_pad','test_mouse_button_binding']:
        exe = OUT/(name+'.exe')
        cmd = [str(compiler),'/nologo','/utf-8','/std:c17','/O1','/Gy','/DWIN32']
        cmd += ['/I'+str(p) for p in include]
        cmd += [str(OUT/(name+'.c')), '/Fe:'+str(exe), '/link','/OPT:REF','user32.lib','gdi32.lib',
                'shell32.lib','ole32.lib','uuid.lib','Xinput9_1_0.lib']
        with (OUT/(name+'-compile.log')).open('w',encoding='utf8') as log:
            subprocess.run(cmd,cwd=OUT,stdout=log,stderr=subprocess.STDOUT,check=True)
        for locale in (['ko-KR','en-US','ja-JP','ru-RU','es-ES'] if name=='test_mouse_settings' else ['en-US']):
            case = OUT/(name+'-'+locale);case.mkdir(exist_ok=True)
            for n in ['settings.ini','keymap.ini']:
                if (case/n).exists(): (case/n).unlink()
            run = subprocess.run([str(exe)],cwd=case,env=dict(os.environ,MACROSS_GAME_LANGUAGE=locale),capture_output=True,text=True,timeout=20)
            (case/'output.txt').write_text(run.stdout+run.stderr,encoding='utf8')
            assert run.returncode==0,(name,locale,run.stdout,run.stderr)
            cases.append(dict(test=name,locale=locale,passed=True))
    pacing=OUT/'fps-limit.exe'
    cmd=[str(compiler),'/nologo','/std:c++20','/O1','/I'+str(SRC),
         str(ROOT/'tools/tests/fps_limit040_fixture.cpp'),'/Fe:'+str(pacing)]
    with (OUT/'fps-limit-compile.log').open('w',encoding='utf8') as log:
        subprocess.run(cmd,cwd=OUT,stdout=log,stderr=subprocess.STDOUT,check=True)
    run=subprocess.run([str(pacing)],cwd=OUT,capture_output=True,text=True,timeout=15)
    (OUT/'fps-limit-output.txt').write_text(run.stdout+run.stderr,encoding='utf8')
    assert run.returncode==0,(run.stdout,run.stderr)
    cases.append(dict(test='fps_limit_guest_gate_and_guard',passed=True))
    report=dict(passed=True,cases=cases,mouse_sensitivity_window='key_mouse_bindings',all_mouse_controls_in_bindings=True,
                sensitivity_edit_cancels_binding_capture=True,fps_limit_choices=[30,60,0],fps_legacy_migration_verified=True,
                game_started=False,visible_windows_created=False,
                actual_hardware_input_checked=False)
    (OUT/'RESULT.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf8')
    print(json.dumps(report,indent=2))


if __name__=='__main__': main()
