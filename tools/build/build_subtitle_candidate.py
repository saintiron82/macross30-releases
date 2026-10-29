"""Reuse existing native objects and link a separate subtitle candidate only.

Default prepares a frozen plan. --build compiles only --source files into the
candidate directory. No original EXE/PDB/MAP/object/SDK-library output is used.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT=Path(__file__).resolve().parents[2]
BUILD=ROOT/'native/build'
BASE=ROOT/'native/subtitle-candidate'


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda:f.read(8*1024*1024),b''):h.update(b)
    return h.hexdigest()


def args_windows(command):
    shell=ctypes.windll.shell32;kernel=ctypes.windll.kernel32
    shell.CommandLineToArgvW.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)]
    shell.CommandLineToArgvW.restype=ctypes.POINTER(ctypes.c_wchar_p)
    kernel.LocalFree.argtypes=[ctypes.c_void_p]
    kernel.LocalFree.restype=ctypes.c_void_p
    count=ctypes.c_int();ptr=shell.CommandLineToArgvW(command,ctypes.byref(count))
    if not ptr:raise ValueError('Cannot parse Windows command')
    try:return [ptr[i] for i in range(count.value)]
    finally:kernel.LocalFree(ptr)


def unescape_ninja(text):
    return re.sub(r'\$([ :$])',r'\1',text)


def cache():
    result={}
    for line in (BUILD/'CMakeCache.txt').read_text().splitlines():
        if line and not line.startswith(('#','//')) and '=' in line and ':' in line.split('=',1)[0]:
            key,val=line.split('=',1);result[key.split(':',1)[0]]=val
    return result


def checked_output(path,out):
    resolved=path.resolve()
    if not resolved.is_relative_to(out.resolve()) or not out.resolve().is_relative_to(BASE.resolve()):
        raise ValueError('Output escaped native/subtitle-candidate')
    if resolved.is_relative_to(BUILD.resolve()):raise ValueError('Original build output prohibited')
    return resolved


def local_env(compiler):
    # Match the installed toolchain without running `set` or recording env secrets.
    vs=next(p for p in compiler.resolve().parents if (p/'VC/Tools/MSVC').is_dir())
    vc=sorted((vs/'VC/Tools/MSVC').glob('*'))[-1]
    sdk=Path(os.environ.get('ProgramFiles(x86)','C:/Program Files (x86)'))/'Windows Kits/10'
    versions=[p.name for p in (sdk/'Lib').iterdir() if p.is_dir() and (sdk/'Include'/p.name/'um').is_dir()]
    version=sorted(versions,key=lambda s:tuple(int(x) for x in s.split('.')))[-1]
    includes=[vc/'include']+[sdk/'Include'/version/x for x in ('ucrt','shared','um','winrt','cppwinrt')]
    libs=[vc/'lib/x64',sdk/'Lib'/version/'ucrt/x64',sdk/'Lib'/version/'um/x64']
    if not all(p.is_dir() for p in includes+libs):raise ValueError('Incomplete MSVC/SDK directories')
    env=dict(os.environ);env['INCLUDE']=';'.join(map(str,includes));env['LIB']=';'.join(map(str,libs))
    return env,dict(MSVC=str(vc),WindowsSDK=str(sdk),WindowsSDK_version=version)


def plan(out,sources,guards):
    cfg=cache();ninja=cfg['CMAKE_MAKE_PROGRAM']
    text=(BUILD/'build.ninja').read_text();lines=text.splitlines()
    at=next(i for i,l in enumerate(lines) if l.startswith('build Macross30Recompiled.exe:'))
    edge=lines[at].split(': ',1)[1].split();objects=[]
    for token in edge[1:]:
        if token in ('|','||'):break
        path=Path(unescape_ninja(token));objects.append((BUILD/path).resolve() if not path.is_absolute() else path.resolve())
    variables={}
    for line in lines[at+1:]:
        if line and not line.startswith(' '):break
        if '=' in line:
            key,value=line.strip().split('=',1);variables[key.strip()]=unescape_ninja(value.strip())
    if 'wholearchive' in (variables['LINK_FLAGS']+variables['LINK_LIBRARIES']).lower():
        raise ValueError('Whole-archive SDK linkage cannot be safely overridden')
    compdb=json.loads(subprocess.check_output([ninja,'-t','compdb'],cwd=BUILD,text=True))
    byfile={str(Path(r['file']).resolve()).lower():r for r in compdb if Path(r['file']).is_absolute() and ' -c ' in r['command']}
    cxx=byfile[str((ROOT/'native/macross30/movie_subtitle_timeline.cpp').resolve()).lower()]
    selected=[];replacement={};extras=[]
    for index,source in enumerate(sources):
        source=source.resolve()
        if not source.is_relative_to(ROOT) or not source.is_file() or source.suffix not in ('.c','.cpp'):
            raise ValueError('Explicit workspace C/C++ sources only')
        row=byfile.get(str(source).lower())
        if row is None and source.suffix=='.c':raise ValueError('C source absent from compile database')
        command=args_windows((row or cxx)['command'])
        output=checked_output(out/'objects'/f'{index:02}-{source.name}.obj',out)
        old_output=None;changed=[]
        for arg in command:
            if arg.startswith('/Fo'):
                old_output=Path(arg[3:]);old_output=(BUILD/old_output).resolve() if not old_output.is_absolute() else old_output.resolve();changed.append('/Fo'+str(output))
            elif arg.startswith('/Fd'):changed.append('/Fd'+str(checked_output(out/'objects'/'compile.pdb',out)))
            elif arg==str(Path((row or cxx)['file'])):changed.append(str(source))
            else:changed.append(arg)
        # Compiler input is the final argument following -- in these guarded rules.
        if '--' not in changed or changed[-2]!='--':raise ValueError('Unexpected compile input layout')
        changed[-1]=str(source)
        if not old_output:raise ValueError('No compiler output switch')
        if row and old_output in objects:replacement[str(old_output)]=str(output)
        else:extras.append(str(output))
        selected.append(dict(source=str(source),source_sha256=sha(source),output=str(output),command=changed))
    final_objects=[replacement.get(str(p),str(p)) for p in objects]+extras
    new_outputs={r['output'] for r in selected}
    missing=[p for p in final_objects if p not in new_outputs and not Path(p).is_file()]
    if missing:raise ValueError('Missing existing objects: '+repr(missing[:5]))
    libraries=args_windows(variables['LINK_LIBRARIES']);resolved_libs=[]
    for lib in libraries:
        p=Path(lib)
        if '\\' in lib or '/' in lib:
            p=(BUILD/p).resolve() if not p.is_absolute() else p.resolve();resolved_libs.append(str(p))
        else:resolved_libs.append(lib)
    flags=[f for f in args_windows(variables['LINK_FLAGS']) if not f.lower().startswith(('/map:','/pdb:','/out:','/implib:'))]
    products={k:str(checked_output(out/('Macross30Recompiled.'+k),out)) for k in ('exe','pdb','map','lib')}
    rsp=checked_output(out/'link.rsp',out)
    link=[cfg['CMAKE_COMMAND'],'-E','vs_link_exe','--msvc-ver=1950',
          '--intdir='+str(checked_output(out/'link-temp',out)),
          '--rc='+cfg['CMAKE_RC_COMPILER'],'--mt='+cfg['CMAKE_MT'],'--manifests','--',cfg['CMAKE_LINKER'],
          '/nologo','@'+str(rsp),'/out:'+products['exe'],'/implib:'+products['lib'],
          '/pdb:'+products['pdb'],'/version:0.0',*flags,'/MAP:'+products['map'],'/PDB:'+products['pdb']]
    guarded=list(dict.fromkeys([p.resolve() for p in sources+guards]+[ROOT/'native/macross30/CMakeLists.txt',BUILD/'build.ninja']))
    before={str(p):sha(p) for p in guarded}
    originals=[BUILD/('Macross30Recompiled.'+x) for x in ('exe','pdb','map')]+[BUILD/'ps3recomp_sdk/ps3recomp_runtime.lib']
    preserved={str(p):sha(p) for p in originals if p.exists()}
    reused=[Path(p) for p in final_objects if p not in new_outputs]
    stamps={str(p):[p.stat().st_size,p.stat().st_mtime_ns] for p in reused}
    return dict(status='prepared_not_built',output=str(out),compile=selected,link=link,products=products,
        response_file=str(rsp),response_objects=final_objects,response_libraries=resolved_libs,
        source_guards=before,preserved_original_outputs=preserved,reused_object_stamps=stamps,
        original_objects_written=False,original_SDK_library_written=False,original_EXE_PDB_MAP_written=False,
        additional_explicit_objects=extras,source_compile_count=len(selected),reused_object_count=len(reused),game_started=False)


def validate_plan(p):
    out=Path(p['output'])
    checked_output(out/'plan.json',out)
    expected={k:str(checked_output(out/('Macross30Recompiled.'+k),out)) for k in ('exe','pdb','map','lib')}
    if p['products']!=expected:raise ValueError('Candidate products must use exact isolated paths')
    checked_output(Path(p['response_file']),out)
    for r in p['compile']:
        checked_output(Path(r['output']),out)
        if Path(r['command'][-1]).resolve()!=Path(r['source']).resolve() or not Path(r['source']).resolve().is_relative_to(ROOT):
            raise ValueError('Unexpected compile source')
        fos=[x[3:] for x in r['command'] if x.startswith('/Fo')]
        if fos!=[r['output']]:raise ValueError('Compiler object output escaped candidate')
        for arg in r['command']:
            if arg.startswith('/Fd'):checked_output(Path(arg[3:]),out)
    seen=set()
    for arg in p['link']:
        low=arg.lower()
        for switch,key in (('/out:','exe'),('/pdb:','pdb'),('/map:','map'),('/implib:','lib')):
            if low.startswith(switch):
                if str(Path(arg[len(switch):]).resolve())!=expected[key]:raise ValueError('Linker target escaped candidate')
                seen.add(key)
        if low.startswith('--intdir='):checked_output(Path(arg.split('=',1)[1]),out)
    if seen!={'exe','pdb','map','lib'}:raise ValueError('Every linker output must be explicit')
    if any(x.startswith(('/','-','@')) for x in p['response_libraries']):raise ValueError('Unexpected response-library switches')
    if any(Path(x).suffix.lower()!='.obj' for x in p['response_objects']):raise ValueError('Unexpected response object')
    return True


def execute(p):
    validate_plan(p)
    out=Path(p['output']);exe=Path(p['products']['exe'])
    if exe.exists():raise ValueError('Candidate EXE already exists; choose a fresh version directory')
    for path,h in p['source_guards'].items():
        if sha(Path(path))!=h:raise ValueError('Frozen source changed '+path)
    for path,h in p['preserved_original_outputs'].items():
        if sha(Path(path))!=h:raise ValueError('Original output changed since prepare '+path)
    env,toolchain=local_env(Path(cache()['CMAKE_CXX_COMPILER']))
    (out/'objects').mkdir(exist_ok=True);(out/'link-temp').mkdir(exist_ok=True)
    started=time.monotonic()
    for r in p['compile']:
        result=subprocess.run(r['command'],cwd=BUILD,env=env,capture_output=True,text=True,errors='replace')
        (out/'objects'/(Path(r['output']).stem+'.log')).write_text(result.stdout+result.stderr,encoding='utf-8')
        if result.returncode:raise ValueError('Candidate compile failed '+r['source'])
    rsp='\n'.join(subprocess.list2cmdline([s]) for s in p['response_objects']+p['response_libraries'])
    Path(p['response_file']).write_text(rsp+'\n',encoding='utf-8')
    result=subprocess.run(p['link'],cwd=BUILD,env=env,capture_output=True,text=True,errors='replace')
    (out/'link.log').write_text(result.stdout+result.stderr,encoding='utf-8')
    if result.returncode:raise ValueError('Candidate link failed; see link.log')
    changed=[path for path,h in p['preserved_original_outputs'].items() if sha(Path(path))!=h]
    reused_changed=[path for path,stamp in p['reused_object_stamps'].items() if [Path(path).stat().st_size,Path(path).stat().st_mtime_ns]!=stamp]
    source_changed=[path for path,h in p['source_guards'].items() if sha(Path(path))!=h]
    if changed or reused_changed or source_changed:raise ValueError('Concurrent/shared change detected '+repr(changed+reused_changed+source_changed))
    sdl=ROOT/'vendor/SDL2/lib/x64/SDL2.dll'
    if sdl.exists():shutil.copyfile(sdl,checked_output(out/'SDL2.dll',out))
    return dict(status='compiled_and_linked_candidate_only',elapsed_seconds=time.monotonic()-started,
        products={k:dict(path=v,sha256=sha(Path(v)),bytes=Path(v).stat().st_size) for k,v in p['products'].items() if Path(v).exists()},
        toolchain=toolchain,original_outputs_preserved=True,reused_objects_unchanged=True,
        frozen_sources_unchanged=True,game_started=False,game_closed=False,original_outputs=p['preserved_original_outputs'])


def main():
    a=argparse.ArgumentParser(description=__doc__)
    a.add_argument('--output',type=Path,default=BASE)
    a.add_argument('--source',type=Path,action='append',default=[])
    a.add_argument('--guard',type=Path,action='append',default=[])
    a.add_argument('--build',action='store_true')
    a.add_argument('--frozen-plan',type=Path)
    n=a.parse_args();out=n.output.resolve();checked_output(out/'plan.json',out)
    out.mkdir(parents=True,exist_ok=True)
    if n.frozen_plan:
        p=json.loads(n.frozen_plan.read_text(encoding='utf-8'))
        if Path(p['output']).resolve()!=out:raise ValueError('Frozen plan output mismatch')
    else:
        if not n.source:a.error('Provide explicit --source files or --frozen-plan')
        p=plan(out,n.source,n.guard);(out/'plan.json').write_text(json.dumps(p,indent=2)+'\n',encoding='utf-8')
    if n.build:
        try:r=execute(p)
        except Exception as e:
            (out/'build-result.json').write_text(json.dumps(dict(status='failed',error=str(e),game_started=False),indent=2)+'\n',encoding='utf-8');raise
        (out/'build-result.json').write_text(json.dumps(r,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status=r['status'],products=list(r['products']))))
    else:print(json.dumps(dict(status=p['status'],compile=p['source_compile_count'],reused=p['reused_object_count'],output=p['output'])))


if __name__=='__main__':main()
