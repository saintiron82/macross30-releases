"""Offline best-fit permutation of reviewed display literal slots and pointers.

No guest address space, ELF header, executable code or heap is added/changed.
Only one Korean NUL string is placed in each guarded original display slot.
"""
import argparse
import bisect
import hashlib
import json
from pathlib import Path
import struct
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from build_ui_catalog_segment import PRINTF


def build(original, base, inventory, repertoire):
    sha=lambda b:hashlib.sha256(b).hexdigest()
    if sha(original)!=inventory['source_sha256'] or len(base)!=len(original):
        raise ValueError('source SHA/length guard')
    rows=[r for r in inventory['entries'] if r.get('translation_approved') and not r.get('installation_excluded') and not r.get('inplace_excluded')]
    byea=sorted(rows,key=lambda r:int(r['guest_ea'],16));starts=[int(r['guest_ea'],16) for r in byea]
    expected_refs={int(r['guest_ea'],16):{int(a['pointer_file_offset'],16) for a in r['refs']} for r in rows}
    actual_refs={ea:set() for ea in starts}
    for off in range(0,len(original)-3,4):
        v=struct.unpack_from('>I',original,off)[0]
        if v in actual_refs:actual_refs[v].add(off)
    if actual_refs!=expected_refs:
        raise ValueError('not every aligned literal start reference is reviewed typed field')
    intervals=[]
    for r in rows:
        f=int(r['file_offset'],16);cap=r['verified_capacity'];old=r['source'].encode()+b'\0';target=r['translation_draft']
        if not len(old)<=cap<=len(old)+2 or original[f:f+cap]!=old.ljust(cap,b'\0') or base[f:f+cap]!=original[f:f+cap]:
            raise ValueError('source/base slot/padding guard')
        if '\0' in target or target.count('\n')!=r['source'].count('\n') or PRINTF.findall(target)!=PRINTF.findall(r['source']):
            raise ValueError('format guard')
        if any(ord(c) not in repertoire for c in target if c not in '\r\n\t'):
            raise ValueError('font guard')
        for ref in expected_refs[int(r['guest_ea'],16)]:
            if base[ref:ref+4]!=original[ref:ref+4]:raise ValueError('base typed pointer guard')
        intervals.append((f,f+cap))
    intervals.sort()
    if any(a[1]>b[0] for a,b in zip(intervals,intervals[1:])):
        raise ValueError('overlapping source slots')
    free=sorted(rows,key=lambda r:(r['verified_capacity'],int(r['file_offset'],16)))
    assignments=[]
    for r in sorted(rows,key=lambda r:(-len(r['translation_draft'].encode()),int(r['file_offset'],16))):
        raw=r['translation_draft'].encode()+b'\0'
        index=next((i for i,s in enumerate(free) if s['verified_capacity']>=len(raw)),None)
        if index is None:raise ValueError('best-fit permutation impossible')
        dest=free.pop(index);assignments.append((r,dest,raw))
    out=bytearray(base);allowed=set();proof=[]
    for src,dest,raw in assignments:
        f=int(dest['file_offset'],16);cap=dest['verified_capacity'];ea=int(dest['guest_ea'],16)
        out[f:f+cap]=raw.ljust(cap,b'\0');allowed.update(range(f,f+cap))
        refs=sorted(expected_refs[int(src['guest_ea'],16)])
        for ref in refs:
            struct.pack_into('>I',out,ref,ea);allowed.update(range(ref,ref+4))
        proof.append(dict(source_file_offset=src['file_offset'],destination_file_offset=dest['file_offset'],destination_guest_ea=hex(ea),slot_bytes=cap,target_nul_bytes=len(raw),source=src['source'],target=src['translation_draft'],typed_pointer_file_offsets=[hex(n) for n in refs]))
    changed={i for i,(a,b) in enumerate(zip(base,out)) if a!=b}
    if changed-allowed:raise AssertionError('out-of-scope modifications')
    sh=struct.unpack_from('>Q',original,40)[0];es,num=struct.unpack_from('>HH',original,58)
    for n in range(num):
        q=sh+n*es;flags=struct.unpack_from('>Q',original,q+8)[0];f,z=struct.unpack_from('>QQ',original,q+24)
        if flags&4 and out[f:f+z]!=base[f:f+z]:raise AssertionError('executable code modified')
    po=struct.unpack_from('>Q',original,32)[0];ps,pn=struct.unpack_from('>HH',original,54)
    if out[:po+ps*pn]!=base[:po+ps*pn]:raise AssertionError('ELF/program headers modified')
    for p in proof:
        f=int(p['destination_file_offset'],16);raw=p['target'].encode()+b'\0'
        assert out[f:f+len(raw)]==raw
        for ref in p['typed_pointer_file_offsets']:assert struct.unpack_from('>I',out,int(ref,16))[0]==int(p['destination_guest_ea'],16)
    return bytes(out),dict(status='offline candidate; runtime rendering not verified',source_sha256=sha(original),base_sha256=sha(base),output_sha256=sha(out),translated_literals=len(proof),typed_pointer_count=sum(len(r['typed_pointer_file_offsets']) for r in proof),source_slot_bytes=sum(r['verified_capacity'] for r in rows),Korean_nul_bytes=sum(len(r['translation_draft'].encode())+1 for r in rows),outside_slot_and_typed_pointer_changes=0,headers_executable_code_and_original_BSS_unchanged=True,prior_base_UI_outside_pool_preserved=True,heap_or_segment_additions=False,all_aligned_start_refs_exactly_reviewed=True,font_coverage_missing=0,printf_and_newlines_preserved=True,assignments=proof)


def main():
    a=argparse.ArgumentParser(description=__doc__)
    for name in ('original','base','inventory','font-repertoire','output'):a.add_argument('--'+name,type=Path,required=True)
    n=a.parse_args();load=lambda p:json.loads(p.read_text(encoding='utf8'))
    if n.output.exists():raise SystemExit('choose a new offline output')
    out,proof=build(n.original.read_bytes(),n.base.read_bytes(),load(n.inventory),set(load(n.font_repertoire)['codepoints']))
    n.output.parent.mkdir(parents=True,exist_ok=True);n.output.write_bytes(out);n.output.with_suffix('.validation.json').write_text(json.dumps(proof,ensure_ascii=False,indent=2)+'\n',encoding='utf8');print('PASS',proof['translated_literals'],proof['output_sha256'])


if __name__=='__main__':main()
