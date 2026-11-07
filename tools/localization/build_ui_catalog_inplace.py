"""Build only reviewed, capacity-fitting catalog literals on an existing UI ELF.

No headers, pointers, code, overflow strings, or active paths are changed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from build_ui_catalog_segment import PRINTF


def build(original, base, inventory, points):
    sha=lambda b:hashlib.sha256(b).hexdigest()
    if sha(original)!=inventory['source_sha256'] or len(original)!=len(base):
        raise ValueError('source identity/length mismatch')
    out=bytearray(base);allowed=set();changes=[];skipped=[];alias=[]
    shoff=struct.unpack_from('>Q',original,40)[0]
    ents,num=struct.unpack_from('>HH',original,58)
    code=[]
    for n in range(num):
        q=shoff+n*ents;flags=struct.unpack_from('>Q',original,q+8)[0]
        off,size=struct.unpack_from('>QQ',original,q+24)
        if flags&4:code.append((off,off+size))
    for r in inventory['entries']:
        if not r.get('translation_approved') or r.get('installation_excluded') or r.get('inplace_excluded') or r.get('overflow'):
            skipped.append(r['file_offset']);continue
        off=int(r['file_offset'],16);old=r['source'].encode()+b'\0';new=r['translation_draft'].encode()+b'\0'
        size=r['original_nul_bytes']+r['original_verified_zero_padding_up_to2']
        if not len(old)<=size<=len(old)+2 or len(new)>size:
            raise ValueError('capacity guard')
        expected=old.ljust(size,b'\0')
        if original[off:off+size]!=expected or base[off:off+size]!=expected:
            raise ValueError('original literal/padding/base guard')
        if any(a<off+size and off<b for a,b in code):
            raise ValueError('executable section overlap')
        if PRINTF.findall(r['source'])!=PRINTF.findall(r['translation_draft']) or r['source'].count('\n')!=r['translation_draft'].count('\n'):
            raise ValueError('format guard')
        if any(ord(c) not in points for c in r['translation_draft'] if c not in '\r\n\t'):
            raise ValueError('font guard')
        span=set(range(off,off+size))
        if span&allowed:raise ValueError('overlapping literals')
        typed={x['pointer_file_offset'].lower() for x in r['refs']}
        extra=sorted(set(x.lower() for x in r['all_aligned_literal_pointer_refs'])-typed)
        if extra:alias.append(dict(file_offset=r['file_offset'],source=r['source'],additional_aligned_refs=extra))
        out[off:off+size]=new.ljust(size,b'\0');allowed|=span
        changes.append(dict(file_offset=r['file_offset'],source=r['source'],target=r['translation_draft'],slot_bytes=size,original_slot_sha256=sha(expected),shared_literal_pointer_refs=r['all_aligned_literal_pointer_refs']))
    changed={i for i,(a,b) in enumerate(zip(base,out)) if a!=b}
    assert not changed-allowed
    return bytes(out),dict(status='offline candidate; not installed or game-rendered',source_sha256=sha(original),base_sha256=sha(base),output_sha256=sha(out),translated_literals=len(changes),skipped_literals=len(skipped),outside_slot_changes=0,header_code_pointers_and_overflow_literals_unchanged=True,prior_UI_bytes_outside_catalog_slots_preserved=True,font_coverage_complete=True,printf_preserved=True,newline_preserved=True,shared_literal_alias_audit=alias,changes=changes,skipped_file_offsets=skipped)


def main():
    a=argparse.ArgumentParser(description=__doc__)
    for n in ('original','base','inventory','font-repertoire','output'):a.add_argument('--'+n,type=Path,required=True)
    n=a.parse_args();load=lambda p:json.loads(p.read_text(encoding='utf8'))
    if n.output.exists():raise SystemExit('choose a new offline output path')
    out,proof=build(n.original.read_bytes(),n.base.read_bytes(),load(n.inventory),set(load(n.font_repertoire)['codepoints']))
    n.output.parent.mkdir(parents=True,exist_ok=True);n.output.write_bytes(out);n.output.with_suffix('.validation.json').write_text(json.dumps(proof,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    print('PASS',proof['translated_literals'],proof['output_sha256'])


if __name__=='__main__':main()
