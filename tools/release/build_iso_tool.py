"""Build pinned PS3Dec with private key-file input and a one-sector key probe."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
UPSTREAM='https://github.com/al3xtjames/PS3Dec.git'
REVISION='7d1d27f028aa86cd961a89795d0d19a9b3771446'
MBEDTLS='60fbd5bdf05c223b641677204469b53c2ff39d4e'


def run(argv,**kwargs):
    return subprocess.check_output(argv,**kwargs)


def patch_source(original,probe):
    source=original.decode('utf-8')
    start='/*else if( argv[2][0]==\'f\' || argv[2][0]==\'F\' )'
    end='fclose(in_file);\n  }*/'
    if source.count(start)!=1 or source.count(end)!=1:
        raise ValueError('Pinned key-file patch anchors differ')
    source=source.replace(start,start[2:]).replace(end,'fclose(in_file);\n  }')
    # TYPE_FILE contains a final disc key, not the older pre-transformed D1 value.
    anchor='if(type==TYPE_3K3Y || type==TYPE_D1 || type==TYPE_FILE )'
    if source.count(anchor)!=1:raise ValueError('Pinned key-conversion anchor differs')
    source=source.replace(anchor,'if(type==TYPE_3K3Y || type==TYPE_D1)')
    anchor='  fprintf(stdout, "Decryption key:");hex_fprintf(stdout, key, 16);'
    if source.count(anchor)!=1:raise ValueError('Pinned output patch anchor differs')
    source=source.replace(anchor,'  /* Key material is never printed. */')
    anchor='  fprintf(stderr, "PS3Dec r%d (compiled to use %d threads for enc/dec)\\n\\n", rev, THREAD_COUNT);'
    if source.count(anchor)!=1:raise ValueError('Pinned probe patch anchor differs')
    return source.replace(anchor,probe+'\n'+anchor).encode('utf-8')


def build(cache,output):
    cache,output=Path(cache).resolve(),Path(output).resolve()
    source=cache/'ps3dec-source';work=cache/'ps3dec-build'
    if not source.exists():
        source.parent.mkdir(parents=True,exist_ok=True)
        subprocess.run(['git','clone','--recurse-submodules',UPSTREAM,str(source)],check=True)
    if run(['git','-C',str(source),'rev-parse','HEAD']).decode().strip()!=REVISION:
        raise ValueError('Unexpected PS3Dec revision; use a separate pinned tool cache')
    if run(['git','-C',str(source/'external/mbedtls'),'rev-parse','HEAD']).decode().strip()!=MBEDTLS:
        raise ValueError('Unexpected mbedTLS revision')
    original=run(['git','-C',str(source),'show',REVISION+':src/PS3Dec.c'])
    probe=(Path(__file__).with_name('ps3dec_probe.inc')).read_text(encoding='utf-8')
    patched=patch_source(original,probe)
    target=source/'src/PS3Dec.c'
    current=target.read_bytes().replace(b'\r\n',b'\n')
    previous=output/'tool-manifest.json'
    known_previous=json.loads(previous.read_text()).get('source_sha256') if previous.is_file() else None
    if current not in (original.replace(b'\r\n',b'\n'),patched) and hashlib.sha256(current).hexdigest()!=known_previous:
        raise ValueError('Tool source has unrelated modifications; preserved for review')
    target.write_bytes(patched)
    subprocess.run(['cmake','-S',str(source),'-B',str(work),'-A','x64',
                    '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_POLICY_VERSION_MINIMUM=3.5'],check=True)
    subprocess.run(['cmake','--build',str(work),'--config','Release','--target','PS3Dec','--parallel','4'],check=True)
    built=list(work.rglob('PS3Dec.exe'))
    if len(built)!=1:raise ValueError('Expected exactly one PS3Dec build output')
    output.mkdir(parents=True,exist_ok=True)
    executable=output/'PS3Dec.exe';shutil.copyfile(built[0],executable)
    vswhere=Path('C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe')
    vs=Path(run([str(vswhere),'-latest','-products','*','-property','installationPath']).decode().strip())
    redists=sorted((vs/'VC/Redist/MSVC').glob('[0-9]*'),key=lambda p:tuple(map(int,p.name.split('.'))))
    dependencies={}
    for directory in (redists[-1]/'x64').iterdir():
        if directory.name.endswith(('.CRT','.OpenMP')):
            for dependency in directory.glob('*.dll'):
                shutil.copyfile(dependency,output/dependency.name)
                dependencies[dependency.name]=hashlib.sha256(dependency.read_bytes()).hexdigest()
    notices='PS3Dec: '+UPSTREAM+'\nPinned commit: '+REVISION+'\n\n'
    notices+=original.decode().split('/***********************************************************************/',2)[1]
    license_file=source/'external/mbedtls/LICENSE'
    if license_file.is_file():notices+='\n\nmbedTLS\n'+license_file.read_text(encoding='utf-8')
    (output/'THIRD-PARTY-NOTICES.txt').write_text(notices,encoding='utf-8')
    manifest=dict(schema_version=1,tool='PS3Dec',upstream=UPSTREAM,revision=REVISION,mbedtls_revision=MBEDTLS,
                  source_sha256=hashlib.sha256(patched).hexdigest(),probe_sha256=hashlib.sha256(probe.encode()).hexdigest(),
                  executable='PS3Dec.exe',sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                  dependencies=dependencies,
                  features=['key-file-input','no-key-output','sector-key-probe'])
    (output/'tool-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache',type=Path,default=ROOT/'.tools')
    parser.add_argument('--output',type=Path,default=ROOT/'.tools/iso-tools')
    args=parser.parse_args()
    print(json.dumps(build(args.cache,args.output),indent=2))
