"""User-supplied disc-key diagnostics and verified local preparation."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
import sys

if not getattr(sys,'frozen',False):
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'release'))
from iso import inspect
from decrypt_iso import disc_regions,eboot_sector,key_bytes,probe,prepare,ORIGINAL_ELF


def fingerprint(path):
    path=Path(path).resolve(strict=True);st=path.stat()
    return (str(path),st.st_size,st.st_mtime_ns,st.st_ino)


def analyze(path,key_file,tool_dir,runner,known_hash=None):
    if not path:return dict(kind='missing',sha1='',key_status='none')
    try:result=inspect(path,False)
    except ValueError:result={'installer_header_supported':False,'invalid_iso':True}
    if known_hash:result['sha1']=known_hash
    else:
        with Path(path).open('rb') as stream:result['sha1']=hashlib.file_digest(stream,'sha1').hexdigest()
    result.update(kind='ready' if result['installer_header_supported'] else 'unsupported',key_status='not-needed')
    if result.get('invalid_iso'):return result
    if result['kind']=='ready':return result
    try:
        regions=disc_regions(path);lba,_=eboot_sector(path)
        if any(start<=lba<=end for start,end in regions):return result
    except ValueError:return result
    result.update(kind='encrypted',key_status='missing')
    if not key_file or not Path(key_file).is_file():return result
    try:key_bytes(key_file)
    except (OSError,ValueError):
        result['key_status']='invalid-format';return result
    try:probe(path,key_file,tool_dir,runner=runner)
    except ValueError as error:
        if 'does not produce' not in str(error):raise
        result['key_status']='mismatch'
    else:result['key_status']='matched'
    return result


def verify_elf(iso,rpcs3,runner):
    if not Path(rpcs3).is_file():raise ValueError('Original preparation tool is missing')
    from inspect_iso import inventory,read_file
    with Path(iso).open('rb') as stream:
        entry=next((r for r in inventory(stream) if r['path'].casefold()=='ps3_game/usrdir/eboot.bin'),None)
        if entry is None or entry['size']>32<<20:raise ValueError('Supported EBOOT not found')
        raw=read_file(stream,entry)
    if raw[:4]==b'\x7fELF':value=hashlib.sha256(raw).hexdigest()
    else:
        if raw[:4]!=b'SCE\0':raise ValueError('Prepared ISO EBOOT is not a supported format')
        with tempfile.TemporaryDirectory(prefix='macross-original-') as td:
            work=Path(td);(work/'EBOOT.BIN').write_bytes(raw)
            result=runner([str(Path(rpcs3).resolve()),'--decrypt',str(work/'EBOOT.BIN')],cwd=work,input=b'',
                          stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90,
                          creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            target=work/'EBOOT.elf'
            if result.returncode or not target.is_file():raise ValueError('Original EBOOT verification failed')
            with target.open('rb') as stream:value=hashlib.file_digest(stream,'sha256').hexdigest()
    if value!=ORIGINAL_ELF:raise ValueError('ISO does not contain the supported Japanese game version')
    return value
