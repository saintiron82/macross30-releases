"""Stage exact-SHA UI DDS replacements inside nested FSTS copies; never edit inputs.

Uses reviewed primary PIDX rows as the Korean payload authority and original ISO
rows as the Japanese SHA authority. No basename-only replacement is permitted.
FSTS clones retain all original bytes except selected entry offset/size/zs fields;
new same-codec payloads append at 16B boundaries. Whole archives relocate at2048B,
with master and same-pack embedded secondary PIDX records updated together.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import uuid
import zlib
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from analyze_archives import unpack
from build_korean_font_archive import encode_segmented
from inspect_iso import inventory, read_file
from stage_hud_guide import bounded_range, compare_prefix, sha_file
from stage_ui_images import validate_dds
from stage_gop_primary_mirrors import validate_gop


def sha(b): return hashlib.sha256(b).hexdigest()

def region(path, off, size):
    bounded_range(off, size, Path(path).stat().st_size)
    with Path(path).open('rb') as f:
        f.seek(off); b=f.read(size)
    if len(b)!=size: raise ValueError('Short read')
    return b

def pidx(path):
    h=region(path,0,40)
    if h[:4]!=b'PIDX': raise ValueError('Expected PIDX')
    f=struct.unpack_from('<9I',h,4); info,count,sec,secsz,ns,nsz=f[2],f[3],f[5],f[6],f[7],f[8]
    total=max(40,info+count*24,sec+secsz,ns+nsz)
    if total>32*1024*1024: raise ValueError('Unbounded PIDX')
    b=region(path,0,total); strings=b[ns:ns+nsz]
    def st(o):
        bounded_range(o,1,len(strings)); end=strings.find(b'\0',o)
        if end<0: raise ValueError('Unterminated PIDX name')
        return strings[o:end].decode('utf8')
    rows=[]
    for i in range(struct.unpack_from('<I',b,sec)[0]):
        at=sec+struct.unpack_from('<I',b,sec+4+i*4)[0]
        bounded_range(at,16,len(b)); no,pn,off,size=struct.unpack_from('<4I',b,at)
        rows.append(dict(path=st(no),pack=Path(st(pn)).name.lower(),offset=off,size=size,field=at+8))
    return b, rows

def fsts_entries(b):
    if b[:4]!=b'FSTS': raise ValueError('Expected FSTS')
    count,tab,ns,nsz=struct.unpack_from('<4I',b,4)
    bounded_range(tab,count*16,len(b)); bounded_range(ns,nsz,len(b)); names=b[ns:ns+nsz]
    rows=[]
    for k in range(count):
        at=tab+16*k;no,off,size,zs=struct.unpack_from('<4I',b,at)
        bounded_range(no,1,len(names));end=names.find(b'\0',no)
        if end<0:raise ValueError('Unterminated FSTS name')
        length=zs if 0<zs<size else size;bounded_range(off,length,len(b))
        rows.append(dict(index=k,path=names[no:end].decode('utf8'),offset=off,size=size,zs=zs,field=at+4))
    return rows

def decoded(b,r):
    e=b[r['offset']:r['offset']+(r['zs'] if 0<r['zs']<r['size'] else r['size'])]
    return (unpack(e,r['size']) if 0<r['zs']<r['size'] else e),e

def rebuild(b,patches,gop_catalogs=None):
    """patches maps index -> exact-original-SHA/bytes. Everything else bit-exact."""
    out=bytearray(b);rows=fsts_entries(b);proof=[];allowed=[];gop_catalogs=gop_catalogs or {}
    for r in rows:
        if r['index'] not in patches: continue
        expected,payload=patches[r['index']];old,enc=decoded(b,r)
        if sha(old)!=expected:raise ValueError('Source SHA changed')
        validation=None
        if r['path'].lower().endswith('.gop'):
            if r['index'] not in gop_catalogs:raise ValueError('GOP requires reviewed full-text catalog')
            validation=validate_gop(old,payload,gop_catalogs[r['index']])
        elif r['path'].lower().endswith('.dds'):
            validate_dds(old);validate_dds(payload)
            if old[:128]!=payload[:128] or len(old)!=len(payload):raise ValueError('DDS header/length differs')
        codec='segs' if enc.startswith(b'segs') else 'zlib' if 0<r['zs']<r['size'] else 'raw'
        new=encode_segmented(payload) if codec=='segs' else zlib.compress(payload,9) if codec=='zlib' else payload
        nz=len(new) if codec!='raw' else 0
        if codec!='raw' and len(new)>=len(payload):raise ValueError('Same-codec payload not compressed')
        out.extend(bytes(-len(out)%16));off=len(out);out.extend(new)
        struct.pack_into('<3I',out,r['field'],off,len(payload),nz);allowed.append((r['field'],12))
        proof.append(dict(**r,old_sha256=expected,replacement_sha256=sha(payload),codec=codec,new_offset=off,new_size=len(payload),new_zs=nz,GOP_validation=validation))
    if len(proof)!=len(patches):raise ValueError('Missing patch index')
    out.extend(bytes(-len(out)%16));allowed_bytes={i for a,n in allowed for i in range(a,a+n)}
    if any(x!=y and i not in allowed_bytes for i,(x,y) in enumerate(zip(b,out))):raise ValueError('Unexpected original archive change')
    for before,after in zip(rows,fsts_entries(out)):
        original,encoded_original=decoded(b,before);actual,encoded_actual=decoded(out,after)
        if before['path']!=after['path']:raise ValueError('Entry order/name changed')
        want=patches.get(before['index'],(None,original))[1]
        if actual!=want:raise ValueError('Payload roundtrip failed')
        if before['index'] not in patches and encoded_original!=encoded_actual:raise ValueError('Unselected encoded payload changed')
    return bytes(out),proof

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-dir',type=Path,required=True)
    ap.add_argument('--reviewed-rows',type=Path)
    ap.add_argument('--iso',type=Path)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--artifacts',type=Path,required=True)
    ap.add_argument('--extra-map',type=Path,help='Reviewed additional exact-SHA raw payload mappings, e.g. ABF alias')
    ap.add_argument('--only-extra-map',action='store_true',help='Use only exact mappings in extra-map; no original primary/ISO map')
    ap.add_argument('--fallback-source-dir',type=Path,action='append',default=[],help='Read missing pack files from immutable sources, in supplied order; repeatable; source-dir always wins')
    ap.add_argument('--apply',action='store_true')
    args=ap.parse_args();src=args.source_dir.resolve();out=args.output.resolve();art=args.artifacts.resolve()
    if args.only_extra_map and not args.extra_map:ap.error('--only-extra-map requires --extra-map')
    if not args.only_extra_map and (args.iso is None or args.reviewed_rows is None):ap.error('Normal mode requires --iso and --reviewed-rows')
    fallbacks=[p.resolve() for p in args.fallback_source_dir]
    fallback=fallbacks[0] if fallbacks else None
    def pack_source(name):
        if Path(name).name!=name:raise ValueError('Pack basename required')
        path=src/name
        if path.is_file():return path
        for folder in fallbacks:
            if (folder/name).is_file():return folder/name
        raise FileNotFoundError(f'Missing pack {name}')
    for folder in fallbacks:
        if out==folder or out in folder.parents or folder in out.parents:raise ValueError('Output overlaps fallback source')
    if out.exists() or art.exists() or out==src or src in out.parents or out in src.parents:raise ValueError('Require new independent output/artifact paths')
    art.mkdir(parents=True);master,archives=pidx(src/'pack.idx')
    reviewed=[] if args.only_extra_map else json.loads(args.reviewed_rows.read_text(encoding='utf-8-sig'))['readbacks'];mapping={};primary=[];required_paths={};catalog_by_key={}
    if not args.only_extra_map:
        with args.iso.open('rb') as iso:
            inv=inventory(iso);packs={Path(x['path']).name.lower():x for x in inv};idx=read_file(iso,next(x for x in inv if x['path'].endswith('/PACK.IDX')))
            ff=struct.unpack_from('<9I',idx,4);strings=idx[ff[7]:ff[7]+ff[8]]
            def st(o):return strings[o:strings.index(b'\0',o)].decode('utf8')
            for r in reviewed:
                at=r['master_record'];folder,no,pn,off,size,zs=struct.unpack_from('<6I',idx,at)
                if folder or st(no)!=r['name']:raise ValueError('Original primary provenance mismatch')
                old=read_file(iso,packs[Path(st(pn)).name.lower()],off,zs if 0<zs<size else size);old=unpack(old,size) if 0<zs<size else old
                fields=struct.unpack_from('<6I',master,at)
                if fields[0] or fields[4]!=size:raise ValueError('Current primary row mismatch')
                actual=region(pack_source(r['pack']),fields[3],fields[5] if 0<fields[5]<fields[4] else fields[4]);actual=unpack(actual,fields[4]) if 0<fields[5]<fields[4] else actual
                if sha(actual)!=r['sha256']:raise ValueError('Reviewed Korean primary SHA mismatch')
                validate_dds(old);validate_dds(actual)
                if old[:128]!=actual[:128]:raise ValueError('Reviewed original DDS header mismatch')
                key=(r['name'],sha(old));prev=mapping.get(key)
                if prev is not None and prev!=actual:raise ValueError('Ambiguous original SHA mapping')
                mapping[key]=actual;primary.append(dict(name=r['name'],original_sha256=sha(old),replacement_sha256=sha(actual),source_record=at))
    if args.extra_map:
        for r in json.loads(args.extra_map.read_text(encoding='utf-8-sig'))['entries']:
            actual=Path(r['replacement']).read_bytes()
            if sha(actual)!=r['replacement_sha256']:raise ValueError('Extra replacement SHA mismatch')
            key=(Path(r['path']).name,r['original_sha256'])
            if key in mapping and mapping[key]!=actual:raise ValueError('Ambiguous extra mapping')
            kind=r.get('type',Path(r['path']).suffix.lstrip('.')).upper()
            if kind=='GOP':
                if not r['path'].lower().endswith('.gop') or 'catalog' not in r:raise ValueError('GOP type requires.gop path and catalog')
                cat=json.loads(Path(r['catalog']).read_text(encoding='utf-8-sig'))
                if cat['source_sha256']!=r['original_sha256']:raise ValueError('GOP source authority SHA mismatch')
                validate_gop(Path(cat['source_file']).read_bytes(),actual,cat)
                catalog_by_key[key]=cat
            elif kind not in ('DDS','ABF'):raise ValueError('Unsupported extra-map payload type')
            mapping[key]=actual;required_paths[key]=r['path']
            primary.append(dict(name=key[0],original_sha256=key[1],replacement_sha256=sha(actual),extra_manifest=str(args.extra_map),exact_entry_path=r['path']))
    changes=[];mismatches=[];already_translated=[];candidate_count=0;embedded={};lengths={}
    known_outputs={(name,sha(payload)):key for key,payload in mapping.items() for name in [key[0]]}
    targets={name for name,h in mapping}
    for a in archives:
        path=pack_source(a['pack']);head=region(path,a['offset'],20)
        if head[:4]!=b'FSTS':continue
        count,tab,ns,nsz=struct.unpack_from('<4I',head,4)
        bounded_range(ns,nsz,a['size']);names=region(path,a['offset']+ns,nsz)
        if not any(n.encode() in names for n in targets):continue
        blob=region(path,a['offset'],a['size']);patches={};patch_catalogs={}
        for r in fsts_entries(blob):
            name=Path(r['path']).name
            if name not in targets:continue
            candidate_count+=1;raw,_=decoded(blob,r);key=(name,sha(raw))
            output_key=known_outputs.get(key)
            if key not in mapping and output_key is not None and (output_key not in required_paths or r['path']==required_paths[output_key]):
                already_translated.append(dict(archive=a,path=r['path'],entry_index=r['index'],sha256=key[1],reason='Exact reviewed Korean outputSHA; untouched'));continue
            if key not in mapping or (key in required_paths and r['path']!=required_paths[key]):
                mismatches.append(dict(archive=a,path=r['path'],sha256=sha(raw),reason='No reviewed exact Japanese SHA; skip (possibly already Korean or distinct original)'));continue
            patches[r['index']]=(key[1],mapping[key])
            if key in catalog_by_key:patch_catalogs[r['index']]=catalog_by_key[key]
        if not patches:continue
        rebuilt,proof=rebuild(blob,patches,patch_catalogs)
        if a['pack'] not in embedded:embedded[a['pack']]=pidx(path)[1]
        matches=[x for x in embedded[a['pack']] if all(x[k]==a[k] for k in ('path','pack','offset','size'))]
        if len(matches)!=1:raise ValueError('Embedded secondary provenance missing/ambiguous')
        start=(lengths.get(a['pack'],path.stat().st_size)+2047)//2048*2048;lengths[a['pack']]=start+len(rebuilt)
        artifact=art/(str(len(changes))+'-'+a['path']+'.fsts');artifact.write_bytes(rebuilt)
        changes.append(dict(original=a,source_archive_sha256=sha(blob),artifact=str(artifact),artifact_sha256=sha(rebuilt),new_offset=start,new_size=len(rebuilt),embedded_field=matches[0]['field'],master_field=a['field'],entries=proof,unselected_payloads_verified=count-len(proof),original_prefix_changed_only_selected_table_fields=True))
    if not changes:raise ValueError('No exact-SHA nested matches')
    inputs=sorted({c['original']['pack'] for c in changes}|{'pack.idx'});source_state={n:dict(path=str(pack_source(n)),bytes=pack_source(n).stat().st_size,sha256=sha_file(pack_source(n))) for n in inputs}
    if region(src/'pack.idx',0,len(master))!=master:raise ValueError('Source master changed during scan')
    for c in changes:
        if sha(region(pack_source(c['original']['pack']),c['original']['offset'],c['original']['size']))!=c['source_archive_sha256']:raise ValueError('Source archive changed during scan')
    plan=dict(source_dir=str(src),fallback_source_dir=str(fallback) if fallback else None,fallback_source_dirs=[str(p) for p in fallbacks],only_extra_map=args.only_extra_map,reviewed_primary=primary,candidates=candidate_count,unmatched=mismatches,already_translated=already_translated,archives=changes,source_state=source_state,final_lengths=lengths,applied=False)
    (art/'plan.json').write_text(json.dumps(plan,indent=2),encoding='utf8')
    if args.apply:
        staging=out.parent/('.nested-ui-'+uuid.uuid4().hex);staging.mkdir(parents=True)
        try:
            for n in inputs:
                dst=staging/n
                with pack_source(n).open('rb') as f,dst.open('xb') as g:shutil.copyfileobj(f,g,8*1024*1024)
                if dst.stat().st_nlink!=1 or sha_file(dst)!=source_state[n]['sha256']:raise ValueError('Independent source copy mismatch')
            with (staging/'pack.idx').open('r+b') as idxout:
                for c in changes:
                    payload=Path(c['artifact']).read_bytes()
                    if sha(payload)!=c['artifact_sha256']:raise ValueError('Artifact changed')
                    with (staging/c['original']['pack']).open('r+b') as f:
                        f.seek(0,2);pad=c['new_offset']-f.tell()
                        if not 0<=pad<2048:raise ValueError('Append offset mismatch')
                        f.write(bytes(pad));f.write(payload)
                        record=struct.pack('<2I',c['new_offset'],c['new_size']);f.seek(c['embedded_field']);f.write(record)
                    idxout.seek(c['master_field']);idxout.write(record)
            for n in inputs:
                ranges=[(c['master_field'],8) if n=='pack.idx' else (c['embedded_field'],8) for c in changes if n=='pack.idx' or n==c['original']['pack']]
                compare_prefix(pack_source(n),staging/n,source_state[n]['bytes'],ranges)
                if sha_file(pack_source(n))!=source_state[n]['sha256']:raise ValueError('Source changed during stage')
            _,newmaster=pidx(staging/'pack.idx')
            for c in changes:
                for rows in (newmaster,pidx(staging/c['original']['pack'])[1]):
                    found=[r for r in rows if r['field']==(c['master_field'] if rows is newmaster else c['embedded_field'])]
                    if len(found)!=1 or found[0]['offset']!=c['new_offset'] or found[0]['size']!=c['new_size']:raise ValueError('Index mirror roundtrip mismatch')
                if sha(region(staging/c['original']['pack'],c['new_offset'],c['new_size']))!=c['artifact_sha256']:raise ValueError('Archive write roundtrip mismatch')
            for n,length in lengths.items():
                if (staging/n).stat().st_size!=length:raise ValueError('Final pack length mismatch')
            plan['applied']=True;plan['output_hashes']={n:sha_file(staging/n) for n in inputs};plan['active_written']=False
            (staging/'nested-ui-manifest.json').write_text(json.dumps(plan,indent=2),encoding='utf8');staging.rename(out)
        finally:
            if staging.exists():shutil.rmtree(staging)
    print(json.dumps(dict(archives=len(changes),matched_entries=sum(len(c['entries']) for c in changes),unmatched=len(mismatches),already_translated=len(already_translated),candidates=candidate_count,applied=args.apply,artifacts=str(art),output=str(out)),indent=2))

if __name__=='__main__':main()
