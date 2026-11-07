"""Synchronize explicitly reviewed primary GOP mirrors with existing secondary KO authority.

Default is read-only prepare. --apply creates independent copies in gop-ui-v5;
no existing game, index, archive, ELF or HDD input is modified.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import shutil
import struct
import uuid

import sys as _sys, pathlib as _pl
if not getattr(_sys, "frozen", False):
    _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]
from analyze_archives import unpack
from gop_strings import parse, TOKEN
from stage_hud_guide import sha, sha_file, bounded_range, compare_prefix

ROOT = Path(__file__).resolve().parents[2]
OUTPUT_ROOT = ROOT/'localization/image_assets/gop-ui-v5'
ALIGN = 2048  # Also satisfies the original 16-byte payload alignment.


def pidx(path):
    path = Path(path)
    with path.open('rb') as f:
        header = f.read(40)
        if len(header) != 40 or header[:4] != b'PIDX':
            raise ValueError('Missing PIDX header')
        _, _, info, count, _, secondary, secondary_size, names, names_size = struct.unpack_from('<9I', header, 4)
        end = max(40, info+count*24, secondary+secondary_size, names+names_size)
        if end > 32*1024*1024:
            raise ValueError('Unbounded PIDX metadata')
        bounded_range(0,end,path.stat().st_size)
        f.seek(0); data=f.read(end)
    def text(offset):
        bounded_range(offset,1,names_size)
        stop=data.find(b'\0',names+offset,names+names_size)
        if stop < 0: raise ValueError('Unterminated PIDX name')
        return data[names+offset:stop].decode('utf8')
    primary=[]
    for i in range(count):
        at=info+i*24; folder,no,pn,off,size,zs=struct.unpack_from('<6I',data,at)
        if not folder: primary.append(dict(name=text(no),pack=text(pn),offset=off,size=size,compressed_size=zs,record=at))
    archives=[]
    if secondary_size:
        bounded_range(secondary,4,len(data)); n=struct.unpack_from('<I',data,secondary)[0]
        bounded_range(secondary+4,4*n,secondary+secondary_size)
        for i in range(n):
            at=secondary+struct.unpack_from('<I',data,secondary+4+i*4)[0]
            bounded_range(at,16,secondary+secondary_size)
            no,pn,off,size=struct.unpack_from('<4I',data,at)
            archives.append(dict(name=text(no),pack=text(pn),offset=off,size=size,record=at))
    return data,primary,archives


def read_payload(pack, row):
    zs=row.get('compressed_size',0); size=row['size']
    if not size or zs < 0 or zs > size: raise ValueError('Invalid GOP payload encoding')
    stored=zs if 0 < zs < size else size
    bounded_range(row['offset'],stored,Path(pack).stat().st_size)
    with Path(pack).open('rb') as f:
        f.seek(row['offset']); encoded=f.read(stored)
    decoded=unpack(encoded,size) if 0 < zs < size else encoded
    return decoded,sha(encoded)


def fsts_entries(blob):
    if blob[:4] != b'FSTS' or len(blob)<20: raise ValueError('Missing secondary FSTS')
    count,table,names,names_size=struct.unpack_from('<4I',blob,4)
    bounded_range(table,count*16,len(blob));bounded_range(names,names_size,len(blob))
    entries=[]
    for i in range(count):
        at=table+i*16; no,off,size,zs=struct.unpack_from('<4I',blob,at)
        bounded_range(no,1,names_size); stop=blob.find(b'\0',names+no,names+names_size)
        if stop<0:raise ValueError('Unterminated FSTS entry')
        name=blob[names+no:stop].decode('utf8');stored=zs if 0<zs<size else size
        if not size or zs>size:raise ValueError('Invalid FSTS encoding')
        bounded_range(off,stored,len(blob)); encoded=blob[off:off+stored]
        decoded=unpack(encoded,size) if 0<zs<size else encoded
        entries.append(dict(name=name,entry=i,offset=off,size=size,compressed_size=zs,payload=decoded))
    return entries


def section_layout(data):
    if data[:8]!=b'ENDIBIGE':raise ValueError('Expected big-endian GOP')
    pos=16
    while pos+16<=len(data):
        size=struct.unpack_from('>I',data,pos+8)[0];end=pos+16+size
        bounded_range(pos,16+size,len(data))
        if data[pos:pos+8]==b'GENESTRT':
            base=pos+16;count,table,values,_=struct.unpack_from('>4I',data,base)
            bounded_range(base+table,count*4,end)
            if not 16<=table<=values<=size:raise ValueError('Invalid GENESTRT layout')
            return base+table,count,base+values,end
        pos=end
    raise ValueError('No GENESTRT section')


def validate_gop(original, korean, catalog):
    """Strict type-specific validator; does not use or bypass DDS validation."""
    if sha(original)!=catalog['source_sha256']:raise ValueError('Catalog original SHA mismatch')
    before=parse(original);after=parse(korean)
    layout=section_layout(original)
    if len(original)!=len(korean) or layout!=section_layout(korean) or len(before)!=len(after):
        raise ValueError('GOP count/layout/size changed')
    table,count,values,end=layout
    if any(a!=b and not(table<=i<table+4*count or values<=i<end) for i,(a,b) in enumerate(zip(original,korean))):
        raise ValueError('Non-string GOP bytes changed')
    translations={}
    for item in catalog['entries']:
        if not item.get('translation'):continue
        n=item['index']
        if n in translations or not 0<=n<len(before):raise ValueError('Duplicate/outside catalog index')
        if before[n]['text']!=item['source'] or before[n]['offset']!=item['offset'] or before[n]['metadata']:
            raise ValueError('Catalog source/index/offset mismatch')
        if TOKEN.findall(item['source'])!=TOKEN.findall(item['translation']):raise ValueError('Format tokens changed')
        if item['source'].count('\\n')!=item['translation'].count('\\n') or item['source'].count('\n')!=item['translation'].count('\n'):
            raise ValueError('Line break count changed')
        translations[n]=item['translation']
    if not translations:raise ValueError('Empty reviewed Korean catalog')
    for n,(a,b) in enumerate(zip(before,after)):
        if b['text']!=translations.get(n,a['text']):raise ValueError('Authority is not exact all-catalog Korean / unchanged metadata')
    return dict(total_strings=count,korean_catalog_entries=len(translations),non_string_sections_unchanged=True,
                format_tokens_preserved=True,full_prose_preserved=True)


def prepare(pack,index,catalog_paths,pack_hash,index_hash,authority_prefix='data/gop/'):
    pack,index=Path(pack).resolve(),Path(index).resolve()
    for p,h in ((pack,pack_hash),(index,index_hash)):
        if len(h)!=64 or sha_file(p)!=h.lower():raise ValueError('Full source SHA mismatch: '+str(p))
    _,master,archives=pidx(index);_,embedded,embedded_archives=pidx(pack)
    arc=[r for r in archives if r['name']=='gop' and Path(r['pack']).name.lower()==pack.name.lower()]
    earc=[r for r in embedded_archives if r['name']=='gop' and Path(r['pack']).name.lower()==pack.name.lower()]
    shape=lambda r:tuple(r[k] for k in ('name','pack','offset','size'))
    if len(arc)!=1 or [shape(r) for r in arc]!=[shape(r) for r in earc]:raise ValueError('Ambiguous/different master and embedded secondary gop authority')
    authority_blob,_=read_payload(pack,arc[0]);authority=fsts_entries(authority_blob)
    cursor=pack.stat().st_size;changes=[];payloads={};targets=set()
    for cp in catalog_paths:
        cp=Path(cp).resolve();catalog=json.loads(cp.read_text(encoding='utf8'));name=Path(catalog['source_file']).name
        if name in targets or not name.startswith('gop_') or not name.endswith('.gop'):raise ValueError('Duplicate/invalid exact target name')
        targets.add(name);original_path=(ROOT/Path(catalog['source_file'])).resolve();original=original_path.read_bytes()
        # Secondary entries use their full reviewed resource path, whereas
        # primary PIDX keys are standalone filenames. Never basename-match.
        authority_name=authority_prefix+name
        choices=[r for r in authority if r['name']==authority_name]
        if len(choices)!=1:raise ValueError('Secondary authority exact entry missing/ambiguous: '+name)
        ko=choices[0]['payload'];validation=validate_gop(original,ko,catalog)
        ms=[r for r in master if r['name']==name];es=[r for r in embedded if r['name']==name]
        signature=lambda r:tuple(r[k] for k in ('name','pack','offset','size','compressed_size'))
        if not ms or Counter(map(signature,ms))!=Counter(map(signature,es)):raise ValueError('Missing/different master and embedded primary aliases: '+name)
        aliases=[]
        for group,rs in (('master',ms),('embedded',es)):
            for row in rs:
                if Path(row['pack']).name.lower()!=pack.name.lower():raise ValueError('Same-name primary alias in different pack; explicit multi-pack review required')
                old,encoded_sha=read_payload(pack,row)
                if old!=original and old!=ko:raise ValueError('Unknown primary GOP variant; stopping: '+name)
                aliases.append(dict(row,role=group,payload_sha256=sha(old),encoded_sha256=encoded_sha,
                                    known_variant='original_JP' if old==original else 'authority_KO'))
        cursor=(cursor+ALIGN-1)//ALIGN*ALIGN
        if cursor+len(ko)>0xffffffff:raise ValueError('PIDX32 append overflow')
        changes.append(dict(name=name,catalog=str(cp),catalog_sha256=sha_file(cp),original_sha256=sha(original),
                            authority_sha256=sha(ko),authority_archive=arc[0],authority_entry={k:v for k,v in choices[0].items() if k!='payload'},
                            offset=cursor,size=len(ko),compressed_size=0,aliases=aliases,validation=validation))
        payloads[name]=ko;cursor+=len(ko)
    if not changes:raise ValueError('No explicit reviewed target catalogs')
    plan=dict(kind='isolated GOP primary mirrors from existing secondary KO authority',pack=str(pack),index=str(index),
              source_pack_sha256=pack_hash.lower(),source_index_sha256=index_hash.lower(),original_length=pack.stat().st_size,
              final_length=cursor,alignment=ALIGN,changes=changes,active_modified=False,secondary_FSTS_modified=False)
    return plan,payloads


def apply(plan,payloads,output):
    output=Path(output).resolve()
    if not output.is_relative_to(OUTPUT_ROOT.resolve()) or output==OUTPUT_ROOT.resolve():raise ValueError('Output must be a new child directory inside gop-ui-v5')
    if output.exists():raise FileExistsError(output)
    output.parent.mkdir(parents=True,exist_ok=True)
    pack,index=Path(plan['pack']),Path(plan['index'])
    if sha_file(pack)!=plan['source_pack_sha256'] or sha_file(index)!=plan['source_index_sha256']:raise ValueError('Source changed after prepare')
    staging=output.with_name(output.name+'.tmp-'+uuid.uuid4().hex);staging.mkdir()
    try:
        data=staging/pack.name;idx=staging/index.name
        if data==idx:raise ValueError('Pack/index output names collide')
        shutil.copyfile(pack,data);shutil.copyfile(index,idx)
        if os.path.samefile(pack,data) or os.path.samefile(index,idx):raise ValueError('Output copy aliases original')
        if sha_file(data)!=plan['source_pack_sha256'] or sha_file(idx)!=plan['source_index_sha256']:raise ValueError('Clone hash mismatch')
        master_fields=[];embedded_fields=[]
        with data.open('r+b') as df,idx.open('r+b') as mf:
            for change in plan['changes']:
                ko=payloads[change['name']]
                if sha(ko)!=change['authority_sha256']:raise ValueError('Authority snapshot changed')
                df.seek(0,2);gap=change['offset']-df.tell()
                if gap<0:raise ValueError('Invalid append order')
                df.write(bytes(gap));df.write(ko)
                triplet=struct.pack('<3I',change['offset'],change['size'],0)
                for alias in change['aliases']:
                    at=alias['record']+12
                    if alias['role']=='master':mf.seek(at);mf.write(triplet);master_fields.append((at,12))
                    else:df.seek(at);df.write(triplet);embedded_fields.append((at,12))
            df.flush();mf.flush();os.fsync(df.fileno());os.fsync(mf.fileno())
        if data.stat().st_size!=plan['final_length']:raise ValueError('Final length mismatch')
        compare_prefix(pack,data,plan['original_length'],embedded_fields)
        compare_prefix(index,idx,index.stat().st_size,master_fields)
        _,ms,_=pidx(idx);_,es,_=pidx(data)
        for c in plan['changes']:
            for rows,role in ((ms,'master'),(es,'embedded')):
                aliases=[r for r in rows if r['name']==c['name']]
                expected=sum(a['role']==role for a in c['aliases'])
                if len(aliases)!=expected:raise ValueError('Alias count changed')
                for row in aliases:
                    if (row['offset'],row['size'],row['compressed_size'])!=(c['offset'],c['size'],0):raise ValueError('Alias tuple roundtrip mismatch')
                    raw,_=read_payload(data,row)
                    if sha(raw)!=c['authority_sha256']:raise ValueError('All-alias Korean roundtrip mismatch')
        # Prefix proof covers every secondary/FSTS original byte, not just target GOPs.
        result=dict(plan,output=str(output),independent_copies=True,output_pack_sha256=sha_file(data),
                    output_index_sha256=sha_file(idx),non_target_primary_changed_bytes=0,
                    secondary_and_FSTS_changed_bytes=0,outside_primary_triplets_original_prefix_changed_bytes=0)
        (staging/'gop-primary-mirrors-manifest.json').write_text(json.dumps(result,indent=2),encoding='utf8')
        staging.rename(output);return result
    finally:
        if staging.exists():
            if not staging.resolve().is_relative_to(OUTPUT_ROOT.resolve()) or not staging.name.startswith(output.name+'.tmp-'):
                raise ValueError('Unsafe staging cleanup target')
            shutil.rmtree(staging)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pack',type=Path,required=True);ap.add_argument('--index',type=Path,required=True)
    ap.add_argument('--pack-sha256',required=True);ap.add_argument('--index-sha256',required=True)
    ap.add_argument('--catalogs',type=Path,nargs='+',required=True);ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--authority-prefix',default='data/gop/',help='Exact secondary resource-path prefix; no basename guessing')
    ap.add_argument('--plan-output',type=Path);ap.add_argument('--apply',action='store_true');args=ap.parse_args()
    plan,payloads=prepare(args.pack,args.index,args.catalogs,args.pack_sha256,args.index_sha256,args.authority_prefix)
    result=apply(plan,payloads,args.output) if args.apply else dict(plan,dry_run=True)
    if args.plan_output:
        report=args.plan_output.resolve()
        if not report.is_relative_to(OUTPUT_ROOT.resolve()) or report in (args.pack.resolve(),args.index.resolve()):
            raise ValueError('Diagnostic JSON must stay inside gop-ui-v5, separate from inputs')
        report.parent.mkdir(parents=True,exist_ok=True)
        report.write_text(json.dumps(result,indent=2),encoding='utf8')
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
