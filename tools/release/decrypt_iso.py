"""Prepare a separate verified ISO using pinned PS3Dec; preserve the source image."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/play'))
from inspect_iso import inventory,read_file

ORIGINAL_ELF='44d7112cbb5527314eb6192e59d861dacfcd4099f301a6e82ffc1681b0782b5b'
UPSTREAM_REVISION='7d1d27f028aa86cd961a89795d0d19a9b3771446'
NO_WINDOW=getattr(subprocess,'CREATE_NO_WINDOW',0)


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def key_bytes(path):
    raw=Path(path).read_bytes()
    if len(raw)==16:return raw
    try:
        text=raw.decode('utf-8-sig').strip()
        if len(text)!=32 or any(c not in '0123456789abcdefABCDEF' for c in text):raise ValueError()
        return bytes.fromhex(text)
    except (UnicodeError,ValueError):raise ValueError('Disc key must contain 16 binary bytes or 32 hex characters') from None


def load_tool(directory):
    directory=Path(directory).resolve(strict=True)
    manifest=json.loads((directory/'tool-manifest.json').read_text())
    if manifest.get('revision')!=UPSTREAM_REVISION or manifest.get('executable')!='PS3Dec.exe':
        raise ValueError('Unexpected ISO tool provenance')
    if not {'key-file-input','no-key-output','sector-key-probe'}.issubset(manifest.get('features',[])):
        raise ValueError('ISO tool lacks required private-key support')
    executable=directory/'PS3Dec.exe'
    if sha(executable)!=manifest['sha256']:raise ValueError('ISO tool hash mismatch')
    for name,expected in manifest.get('dependencies',{}).items():
        if Path(name).name!=name or sha(directory/name)!=expected:raise ValueError('ISO tool runtime hash mismatch')
    return executable,manifest


def disc_regions(path):
    size=Path(path).stat().st_size
    if size<4096 or size%2048:raise ValueError('ISO size must be a multiple of 2048 bytes')
    with Path(path).open('rb') as stream:header=stream.read(4096)
    count=struct.unpack_from('>I',header)[0]
    if not 1<=count<=(4096-8)//8:raise ValueError('Invalid PS3 disc region count')
    regions=[struct.unpack_from('>II',header,8+i*8) for i in range(count)]
    if regions[0][0]!=0 or regions[-1][1]!=size//2048-1:raise ValueError('Disc region boundaries disagree with ISO size')
    previous=-1
    for begin,end in regions:
        if begin<=previous or begin>end or end>=size//2048:raise ValueError('Invalid or overlapping plain disc regions')
        previous=end
    return regions


def eboot_sector(path):
    with Path(path).open('rb') as stream:
        entry=next((r for r in inventory(stream) if r['path'].casefold()=='ps3_game/usrdir/eboot.bin'),None)
        if entry is None or entry['size']<2048 or entry['size']>32<<20:raise ValueError('Supported EBOOT not found')
        extent=entry['extents'][0]
        if extent['offset']%2048 or extent['size']<2048:raise ValueError('Unaligned EBOOT extent')
        return extent['offset']//2048,read_file(stream,entry,0,2048)


def probe(source,key_file,tool_directory,runner=None):
    lba,sector=eboot_sector(source)
    if sector[:4] in (b'SCE\0',b'\x7fELF'):
        return dict(key_required=False,recognized_header=True,magic=sector[:4].hex(),lba=lba)
    tool,manifest=load_tool(tool_directory)
    regions=disc_regions(source)
    if any(begin<=lba<=end for begin,end in regions):
        raise ValueError('Unrecognized EBOOT lies in a plain region; disc decryption is not the indicated fix')
    if key_file is None:raise ValueError('Encrypted ISO requires a matching .dkey or binary key file')
    secret=key_bytes(key_file)
    with tempfile.TemporaryDirectory(prefix='macross-key-probe-') as td:
        work=Path(td);(work/'key.bin').write_bytes(secret)
        result=(runner or subprocess.run)([str(tool),'--probe','key.bin',str(lba)],cwd=work,input=sector,
                              stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,creationflags=NO_WINDOW,timeout=15)
    if result.returncode!=0 or result.stdout not in (b'SCE\0',b'\x7fELF'):
        raise ValueError('Disc key does not produce a recognized EBOOT header')
    return dict(key_required=True,recognized_header=True,magic=result.stdout.hex(),lba=lba,tool_sha256=manifest['sha256'])


def verify_original(path,rpcs3):
    # Reuse the existing exact supported-ELF gate; no game is launched.
    sys.path.insert(0,str(ROOT/'tools/local_build'))
    import original_elf
    with tempfile.TemporaryDirectory(prefix='macross-original-verify-') as td:
        elf=original_elf.extract(path,Path(td),rpcs3)
        value=sha(elf)
    if value!=ORIGINAL_ELF:raise ValueError('Prepared ISO has an unsupported original EBOOT')
    return value


def prepare(source,output,key_file,tool_directory,rpcs3,progress=None,runner=None,popen=None,verifier=None):
    source=Path(source).resolve(strict=True);output=Path(output).resolve()
    if output==source or output.exists():raise ValueError('Output must be a new path distinct from the source ISO')
    checked=probe(source,key_file,tool_directory,runner=runner)
    verify=verifier or verify_original
    source_hash=sha(source)
    if not checked['key_required']:
        return dict(verified=True,reused_source=True,path=str(source),source_sha256=source_hash,
                    original_elf_sha256=verify(source,rpcs3))
    output.parent.mkdir(parents=True,exist_ok=True)
    if shutil.disk_usage(output.parent).free<source.stat().st_size+(1<<30):raise ValueError('Insufficient space for a separate decrypted ISO')
    tool,manifest=load_tool(tool_directory)
    secret=key_bytes(key_file)
    with tempfile.TemporaryDirectory(prefix='.iso-decrypt-',dir=output.parent) as td:
        work=Path(td);(work/'key.bin').write_bytes(secret)
        image=work/'image.iso'
        # Only ASCII relative names go to the older C file APIs; stdin handles Unicode source paths.
        with source.open('rb') as original:
            child=(popen or subprocess.Popen)([str(tool),'d','file','key.bin','-','image.iso'],cwd=work,stdin=original,
                                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=NO_WINDOW)
            started=time.monotonic();last=0
            try:
                while child.poll() is None:
                    if time.monotonic()-started>3600:raise TimeoutError('ISO decryption exceeded one hour')
                    if progress and time.monotonic()-last>=5:
                        done=image.stat().st_size if image.exists() else 0
                        progress(dict(stage='decrypting',bytes=done,total=source.stat().st_size))
                        last=time.monotonic()
                    time.sleep(.2)
                if child.returncode:raise RuntimeError('ISO tool failed (exit='+str(child.returncode)+'); no output was published')
            finally:
                if child.poll() is None:child.kill();child.wait(timeout=10)
        if not image.is_file() or image.stat().st_size!=source.stat().st_size:raise ValueError('Decrypted ISO size mismatch')
        if progress:progress(dict(stage='verifying-original'))
        elf_hash=verify(image,rpcs3)
        output_hash=sha(image)
        if sha(source)!=source_hash:raise ValueError('Source ISO changed during preparation')
        # Same-volume hard-link creation is atomic and refuses an existing output name.
        os.link(image,output)
        result=dict(verified=True,reused_source=False,path=str(output),bytes=output.stat().st_size,
                    source=str(source),source_sha256=source_hash,sha256=output_hash,
                    source_preserved=True,original_elf_sha256=elf_hash,
                    key_probe=checked,tool_sha256=manifest['sha256'],tool_revision=manifest['revision'])
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--key-file',type=Path)
    parser.add_argument('--tool-dir',type=Path,default=ROOT/'.tools/iso-tools')
    parser.add_argument('--rpcs3',type=Path)
    parser.add_argument('--probe',action='store_true')
    parser.add_argument('--report',type=Path)
    args=parser.parse_args()
    if args.key_file is None:
        args.key_file=next((p for p in (args.source.with_suffix('.dkey'),args.source.with_suffix('.key')) if p.is_file()),None)
    if args.probe:result=probe(args.source,args.key_file,args.tool_dir)
    else:
        if not args.output:parser.error('--output is required for preparation')
        result=prepare(args.source,args.output,args.key_file,args.tool_dir,args.rpcs3,
                       progress=lambda data:print(json.dumps(data),flush=True))
    if args.report:
        args.report.parent.mkdir(parents=True,exist_ok=True)
        args.report.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
