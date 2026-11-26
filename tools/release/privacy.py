"""Remove local build provenance without changing executable instructions."""
import hashlib
import json
from pathlib import PureWindowsPath
import re
import struct

LOCAL_PATH = re.compile(rb'(?i)[A-Z]:[\\/]+(?:GameDev|Game)[\\/]+(?:Macross|mAC30)[^\x00\r\n\"<>|]{0,220}')
SOURCE_PATH = re.compile(r'(?i)[A-Z]:[\\/]+(?:GameDev|Game)[\\/]+[A-Za-z0-9_./\\-]+')
TOKEN = re.compile(rb'(?:gh[pousr]_[A-Za-z0-9]{30,255}|github_pat_[A-Za-z0-9_]{40,255}|https://discord(?:app)?\.com/api/webhooks/[0-9]{15,22}/[A-Za-z0-9_-]{40,}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----[\r\nA-Za-z0-9+/= ]{80,})')

def digest(data):return hashlib.sha256(data).hexdigest()

def sanitize_pe_pdb(data):
    """Replace RSDS path by its basename; fail on signed/unknown debug data.

    Byte differences are restricted to the bounded PDB path. All PE headers,
    sections, code, imports, relocations and the GUID/age remain unchanged.
    A populated PE checksum is also updated after the metadata edit.
    """
    import pefile
    pe=pefile.PE(data=data,fast_load=True)
    if pe.OPTIONAL_HEADER.DATA_DIRECTORY[4].Size:raise ValueError('Signed PE requires a new signed build')
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DEBUG']])
    out=bytearray(data);allowed=set();paths=0
    for entry in getattr(pe,'DIRECTORY_ENTRY_DEBUG',[]):
        if entry.struct.Type!=2:continue
        start=entry.struct.PointerToRawData;end=start+entry.struct.SizeOfData
        if not 0<=start<end<=len(data) or data[start:start+4]!=b'RSDS':raise ValueError('Unknown CodeView layout')
        first=start+24;last=data.find(b'\0',first,end)
        if last<first:raise ValueError('Unterminated PDB path')
        name=data[first:last].decode('utf-8')
        clean=PureWindowsPath(name).name.encode('utf-8')
        if clean==data[first:last]:continue
        if not clean.endswith(b'.pdb') or len(clean)>last-first:raise ValueError('Invalid PDB basename')
        out[first:last+1]=clean.ljust(last-first+1,b'\0')
        allowed.update(range(first,last+1));paths+=1
    checksum_offset=pe.OPTIONAL_HEADER.get_field_absolute_offset('CheckSum')
    if pe.OPTIONAL_HEADER.CheckSum:
        updated=pefile.PE(data=bytes(out),fast_load=True).generate_checksum()
        struct.pack_into('<I',out,checksum_offset,updated);allowed.update(range(checksum_offset,checksum_offset+4))
    changed=[i for i,(a,b) in enumerate(zip(data,out)) if a!=b]
    if not all(i in allowed for i in changed):raise ValueError('Unexpected PE edit')
    sections=[]
    for section in pe.sections:
        if section.Characteristics & 0x20000000:
            start=section.PointerToRawData;end=start+section.SizeOfRawData
            assert data[start:end]==out[start:end]
            sections.append(dict(name=section.Name.rstrip(b'\0').decode(),sha256=digest(data[start:end])))
    clean=bytes(out)
    if LOCAL_PATH.search(clean):raise ValueError('Unremoved local path remains in PE')
    return clean,dict(pdb_paths_generalized=paths,changed_bytes=len(changed),same_length=len(data)==len(clean),
                      executable_sections_identical=sections,non_metadata_bytes_identical=True,
                      before_sha256=digest(data),after_sha256=digest(clean))

def sanitize_shader_manifest(data):
    doc=json.loads(data);old=doc.pop('manifest_sha256')
    assert digest(json.dumps(doc,indent=2,sort_keys=True).encode())==old
    rows=0
    for entry in doc.get('pso_inventory',{}).get('inputs',[]):
        if 'file' in entry:
            entry['file']='assets/'+PureWindowsPath(entry['file']).name;rows+=1
    doc['manifest_sha256']=digest(json.dumps(doc,indent=2,sort_keys=True).encode())
    return (json.dumps(doc,indent=2,sort_keys=True)+'\n').encode(),dict(input_paths_generalized=rows,manifest_sha256=doc['manifest_sha256'])

def relative_source_paths(text):
    """Public legacy scripts are invoked from repo root; preserve path suffixes."""
    def replace(match):
        raw=re.sub(r'/+','/',match.group().replace('\\','/'))
        rest=re.sub(r'(?i)^[A-Z]:/(?:GameDev|Game)/','',raw)
        first,slash,tail=rest.partition('/')
        low=first.lower()
        if low=='macross':base='.'
        elif low=='mac30':base='build/tests/installed-game'
        elif low.startswith('macross-release-'):
            digits=re.match(r'macross-release-(\d+)',low)
            if digits and len(digits[1])>=3:
                v=digits[1];base=f'build/releases/{int(v[0])}.{int(v[1])}.{int(v[2:])}'
            else:base='build/releases/'+first
        elif low.startswith('macross-dist'):base='build/releases/legacy-dist'+first[len('Macross-dist'):]
        elif low.startswith('macross-local-build'):base='build/releases/local-build'+first[len('Macross-local-build'):]
        else:base='build/tests/'+first
        return base+('/'+tail if slash else '')
    return SOURCE_PATH.sub(replace,text)

def check_owned_bytes(data):
    if LOCAL_PATH.search(data) or TOKEN.search(data):raise ValueError('Private path or credential candidate in owned release data')
