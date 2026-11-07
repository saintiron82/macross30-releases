"""Exact-SHA multi-variant UI pack planner. Default audit only; active inputs immutable.

Uses stage_ui_images.apply for one independent copy per touched pack and a chain
of master indexes. Never selects a DDS by basename alone or guesses a variant.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import struct

import sys as _sys, pathlib as _pl
_sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]
import stage_ui_images as ui
from stage_nested_ui import pidx as secondary_pidx, region, decoded

ROOT=Path(__file__).resolve().parents[2]
BUNDLE=ROOT/'localization/image_assets/menu-ui-v7'
DISC=ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'
HDD=ROOT/'native/state-ko-full/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'

def doc(path): return json.loads(Path(path).read_text(encoding='utf-8-sig'))
def dump(path,value):
    Path(path).parent.mkdir(parents=True,exist_ok=True)
    Path(path).write_text(json.dumps(value,ensure_ascii=False,indent=2),encoding='utf8')
def resolve(path):
    p=Path(path)
    return p.resolve() if p.is_absolute() else (ROOT/p).resolve()
def primary(path):
    with Path(path).open('rb') as f:
        head=f.read(40)
        if head[:4]!=b'PIDX':raise ValueError('Missing PIDX')
        fields=struct.unpack_from('<9I',head,4);info,count,ns,nsz=fields[2],fields[3],fields[7],fields[8]
        end=max(40,info+count*24,ns+nsz)
        if end>32*1024*1024:raise ValueError('Unbounded primary metadata')
        ui.bounded_range(0,end,Path(path).stat().st_size);f.seek(0);b=f.read(end)
    def text(o):
        ui.bounded_range(o,1,nsz);stop=b.find(b'\0',ns+o,ns+nsz)
        if stop<0:raise ValueError('Unterminated name')
        return b[ns+o:stop].decode('utf8')
    rows=[]
    for i in range(count):
        at=info+i*24;folder,no,pn,off,size,zs=struct.unpack_from('<6I',b,at)
        if not folder:rows.append(dict(name=text(no),pack=Path(text(pn)).name.lower(),offset=off,size=size,compressed_size=zs,record=at,record_sha256=ui.sha(b[at:at+24])))
    return b,rows
def tuple_key(r):return (r['name'],Path(r['pack']).name.lower(),r['offset'],r['size'],r['compressed_size'])
def payload(pack,r):
    size,zs=r['size'],r['compressed_size']
    if size<=0 or not 0<=zs<size:raise ValueError('Unsupported source encoding')
    encoded=region(pack,r['offset'],zs or size)
    return (ui.unpack(encoded,size) if zs else encoded),ui.sha(encoded)

def normalize(extra=()):
    authorities=[];nested=[];inputs=[]
    def add(row,manifest):
        name=row.get('name',row.get('path',''))
        if Path(name).name!=name or not name.endswith('.dds'):raise ValueError('Flat DDS authority basename required')
        source=row.get('source_original_sha256',row.get('source_sha256',row.get('original_sha256')))
        replacement=resolve(row['replacement']);raw=replacement.read_bytes();ui.validate_dds(raw)
        expected=row['replacement_sha256']
        if ui.sha(raw)!=expected or not isinstance(source,str) or len(source)!=64:raise ValueError('Authority hash mismatch')
        authorities.append(dict(name=name,source_sha256=source,replacement=str(replacement),replacement_sha256=expected,proof_manifest=str(manifest)))
    m=BUNDLE/'candidate-family-manifest.json';inputs.append(m)
    for a in doc(m)['targets']:add(a,m)
    m=BUNDLE/'gallery-title-replacements/stage-inputs.json';inputs.append(m)
    for a in doc(m)['entries']:
        add(a,m)
        for c in a['actual_copy_inventory']:
            if c['kind']=='nested':nested.append(dict(type='DDS',path=c['name'],original_sha256=a['source_original_sha256'],replacement=a['replacement'],replacement_sha256=a['replacement_sha256']))
    m=BUNDLE/'misc-replacements-v1/misc-replacements-manifest.json';inputs.append(m)
    for a in doc(m)['assets']:add(a,m)
    n=BUNDLE/'misc-replacements-v1/nested-extra-map.json';inputs.append(n);nested+=doc(n)['entries']
    for m in extra:
        m=resolve(m);inputs.append(m);d=doc(m)
        for a in d.get('authorities',d.get('assets',d.get('entries',d.get('replacements',[])))):add(a,m)
        nested+=d.get('nested_entries',[])
    seen={}
    for a in authorities:
        key=a['name'],a['source_sha256']
        if key in seen and seen[key]!=a['replacement_sha256']:raise ValueError('Conflicting replacement authority')
        seen[key]=a['replacement_sha256']
    authorities=list({(a['name'],a['source_sha256']):a for a in authorities}.values())
    known={(a['name'],a['source_sha256']):a for a in authorities};merged={}
    for n in nested:
        key=(Path(n['path']).name,n['original_sha256'])
        if key not in known or n['replacement_sha256']!=known[key]['replacement_sha256']:raise ValueError('Nested map differs from primary authority')
        full=(n['path'],n['original_sha256'])
        n=dict(n,replacement=str(resolve(n['replacement'])))
        if full in merged and merged[full]!=n:raise ValueError('Conflicting nested fullpath map')
        merged[full]=n
    return dict(status='NORMALIZED_REVIEWED_AUTHORITIES_NOT_STAGED',authorities=authorities,nested_entries=list(merged.values()),input_manifests=[dict(path=str(p),sha256=ui.sha(p.read_bytes())) for p in inputs])

def choose(authorities,name,raw):
    candidates=[a for a in authorities if a['name']==name and a['source_sha256']==ui.sha(raw)]
    if len(candidates)!=1:raise ValueError(f'Unknown/ambiguous original variant: {name} SHA={ui.sha(raw)}')
    a=candidates[0];replacement=Path(a['replacement']).read_bytes()
    if ui.sha(replacement)!=a['replacement_sha256']:raise ValueError('Replacement changed after review')
    ui.validate_dds(raw);ui.validate_dds(replacement)
    if len(raw)!=len(replacement) or raw[:128]!=replacement[:128]:raise ValueError('Replacement header/length differs')
    return a

def make_plan(pack,index,authorities,pack_hash,index_hash):
    pack,index=Path(pack).resolve(),Path(index).resolve()
    if ui.sha_file(pack)!=pack_hash or ui.sha_file(index)!=index_hash:raise ValueError('Source full hash mismatch')
    mb,mrows=primary(index);eb,erows=primary(pack);names={a['name'] for a in authorities}
    selected=[r for r in mrows if r['name'] in names and r['pack']==pack.name.lower()]
    if not selected:raise ValueError('No selected source records for pack')
    embedded=defaultdict(list);master=defaultdict(list)
    for r in erows:
        if r['name'] in names and r['pack']==pack.name.lower():embedded[tuple_key(r)].append(r)
    for r in selected:master[tuple_key(r)].append(r)
    if set(embedded)!=set(master) or any(len(embedded[k])!=len(v) for k,v in master.items()):raise ValueError('Master/embedded source tuple cardinality mismatch')
    cursor=pack.stat().st_size;changes=[]
    for key,masters in master.items():
        raw,encoded_sha=payload(pack,masters[0]);a=choose(authorities,masters[0]['name'],raw)
        for m,e in zip(sorted(masters,key=lambda r:r['record']),sorted(embedded[key],key=lambda r:r['record'])):
            cursor=(cursor+ui.ALIGN-1)//ui.ALIGN*ui.ALIGN
            if cursor+len(raw)>0xffffffff:raise ValueError('PIDX32 overflow')
            changes.append(dict(name=m['name'],pack=m['pack'],replacement=a['replacement'],sha256=a['replacement_sha256'],offset=cursor,size=len(raw),compressed_size=0,master_field=m['record']+12,embedded_field=e['record']+12,source_tuple=dict(m,raw_sha256=ui.sha(raw),encoded_sha256=encoded_sha),master_record_sha256=m['record_sha256'],embedded_record_sha256=e['record_sha256']))
            cursor+=len(raw)
    return dict(kind='exact-SHA all-record UI variant plan',data2=str(pack),index=str(index),pack_filename=pack.name,source_pack_sha256=pack_hash,source_data2_sha256=pack_hash,source_index_sha256=index_hash,original_length=pack.stat().st_size,final_length=cursor,changes=changes,source_modified=False)

def audit_nested(source,normalized,discover_reviewed=False):
    _,archives=secondary_pidx(Path(source)/'pack.idx');names={a['name'] for a in normalized['authorities']}
    allowed={(r['path'],r['original_sha256']):r for r in normalized['nested_entries']};matches=[]
    for archive in archives:
        pack=Path(source)/archive['pack'];base=archive['offset'];h=region(pack,base,20)
        if h[:4]!=b'FSTS':raise ValueError('Unknown secondary archive format')
        count,table,ns,nsz=struct.unpack_from('<4I',h,4)
        ui.bounded_range(table,count*16,archive['size']);ui.bounded_range(ns,nsz,archive['size'])
        tab=region(pack,base+table,count*16);strings=region(pack,base+ns,nsz)
        for i in range(count):
            no,off,size,zs=struct.unpack_from('<4I',tab,i*16)
            ui.bounded_range(no,1,len(strings));end=strings.find(b'\0',no)
            if end<0:raise ValueError('Unterminated nested path')
            name=strings[no:end].decode('utf8')
            if Path(name).name not in names:continue
            ui.bounded_range(off,zs or size,archive['size'])
            raw,encoded_sha=payload(pack,dict(offset=base+off,size=size,compressed_size=zs))
            authority=choose(normalized['authorities'],Path(name).name,raw)
            if (name,ui.sha(raw)) not in allowed:
                if not discover_reviewed:raise ValueError(f'Unknown nested fullpath/variant {name} SHA={ui.sha(raw)}')
                observed=dict(type='DDS',path=name,original_sha256=ui.sha(raw),replacement=authority['replacement'],replacement_sha256=authority['replacement_sha256'])
                allowed[(name,ui.sha(raw))]=observed
                normalized['nested_entries'].append(observed)
            matches.append(dict(archive=archive['path'],pack=archive['pack'],archive_offset=base,archive_size=archive['size'],entry_index=i,path=name,offset=off,size=size,compressed_size=zs,raw_sha256=ui.sha(raw),encoded_sha256=encoded_sha))
    found={(r['path'],r['raw_sha256']) for r in matches}
    if found!=set(allowed):raise ValueError('Reviewed nested fullpath map target missing')
    return dict(metadata_archives=len(archives),unknown_variants=0,unknown_fullpaths=0,matches=matches)

def audit(source,hdd,normalized,discover_reviewed_nested=False):
    source,hdd=Path(source),Path(hdd);names={a['name'] for a in normalized['authorities']}
    _,rows=primary(source/'pack.idx');selected=[r for r in rows if r['name'] in names]
    found=set();records=[]
    for r in selected:
        raw,encoded=payload(source/r['pack'],r);a=choose(normalized['authorities'],r['name'],raw)
        found.add((r['name'],ui.sha(raw)));records.append(dict(r,raw_sha256=ui.sha(raw),encoded_sha256=encoded,replacement_sha256=a['replacement_sha256']))
    if found!={(a['name'],a['source_sha256']) for a in normalized['authorities']}:raise ValueError('Reviewed authority missing primary source record')
    files=sorted({'pack.idx','fileset0.dat','fileset2.dat',*(r['pack'] for r in selected)})
    # DATA2/FILESET0/FILESET2 contain prior KO DDS/GOP/font baselines too.
    expected=doc(ROOT/'localization/image_assets/data-ui-v6/final-pack-readback.json')['output_hashes']
    checks=[]
    for name in files:
        disc_hash=ui.sha_file(source/name);hdd_hash=ui.sha_file(hdd/name)
        if disc_hash!=hdd_hash:raise ValueError(f'Disc/HDD epoch mismatch {name}')
        if name in expected and disc_hash!=expected[name]:raise ValueError(f'Current pack differs from installed v6 epoch {name}')
        checks.append(dict(filename=name,disc=str(source/name),hdd=str(hdd/name),sha256=disc_hash,bytes=(source/name).stat().st_size,v6_expected_match=(disc_hash==expected[name]) if name in expected else None))
    nested=audit_nested(source,normalized,discover_reviewed_nested)
    plans=[];hashes={r['filename']:r['sha256'] for r in checks}
    for name in sorted({r['pack'] for r in selected}):plans.append(make_plan(source/name,source/'pack.idx',normalized['authorities'],hashes[name],hashes['pack.idx']))
    prior_path=ROOT/'localization/image_assets/data-ui-v6/final-pack-readback.json';prior=doc(prior_path)
    protected_names={r['name'] for r in prior['prior_primary_DDS']+prior['new_primary_DDS']+prior['primary_GOPs']}
    protected_records={r['record'] for r in rows if r['name'] in protected_names}
    changed_records={c['master_field']-12 for plan in plans for c in plan['changes']}
    if protected_records & changed_records:raise ValueError('New target would modify prior KO primary record')
    preservation=dict(baseline_proof=str(prior_path),baseline_proof_sha256=ui.sha(prior_path.read_bytes()),prior_DDS=prior['prior_primary_DDS_count'],additional_v6_DDS=prior['new_primary_DDS_count'],primary_GOPs=prior['primary_GOP_count'],primary_GOP_KO_strings=prior['primary_GOP_KO_strings'],nested_battletext_archives=prior['prior_nested_battletext_count'],font_glyphs=prior['font_glyphs'],prior_KO_primary_record_changes=0,basis='Whole current DATA/DATA2/FILESET2/IDX equals validated v6 hashes; every old primary KO record is disjoint from new triplets. apply checks all original prefix bytes except these exact triplets. Secondary pointers/payloads, font and nested GOP are untouched in primary stage. Future nested stage must preserve all unselected encoded payloads.')
    return dict(status='READ_ONLY_COMPLETE_SOURCE_PLAN',source=str(source.resolve()),source_epoch=checks,primary_records=records,nested=nested,pack_plans=plans,preserved_prior_KO=preservation,unknown_primary_variants=0,authority_variants=len(found))

def apply_all(audit_doc,normalized,output):
    output=Path(output).resolve()
    if output.exists() or BUNDLE.resolve() not in output.parents:raise ValueError('Require NEW output inside menu-ui-v7')
    output.mkdir();index=Path(audit_doc['source'])/'pack.idx';files=[];stages=[]
    for n,old in enumerate(audit_doc['pack_plans']):
        # Earlier pack changes affect only master fields; re-plan against the chain.
        plan=make_plan(old['data2'],index,normalized['authorities'],old['source_pack_sha256'],ui.sha_file(index))
        folder=output/f'stage-{n:02d}';result=ui.apply(plan,folder)
        final=output/plan['pack_filename'];(folder/plan['pack_filename']).rename(final)
        index=folder/'pack.idx';files.append(dict(filename=final.name,path=str(final),sha256=result['output_pack_sha256']))
        stages.append(result)
    index.rename(output/'pack.idx');files.append(dict(filename='pack.idx',path=str(output/'pack.idx'),sha256=ui.sha_file(output/'pack.idx')))
    result=dict(status='ISOLATED_PRIMARY_CANDIDATE_NOT_INSTALLED',files=files,stages=stages,source_epoch=audit_doc['source_epoch'],unknown_original_variants=0,nested_still_pending=True,preservation='stage_ui_images.apply checks complete source pack/index prefixes except exact selected 12-byte primary triplets; all previous payload bytes and unselected records remain bit-identical.')
    dump(output/'primary-bundle-result.json',result)
    return result

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-dir',type=Path,default=DISC);ap.add_argument('--hdd-source-dir',type=Path,default=HDD)
    ap.add_argument('--extra-manifest',type=Path,action='append',default=[])
    ap.add_argument('--artifacts',type=Path,default=BUNDLE/'stage-preparation')
    ap.add_argument('--discover-reviewed-nested',action='store_true',help='Admit only actually observed FSTS fullpaths with rawSHA in the reviewed primary authority pool')
    ap.add_argument('--apply',action='store_true');ap.add_argument('--output',type=Path)
    a=ap.parse_args();normalized=normalize(a.extra_manifest)
    result=audit(a.source_dir,a.hdd_source_dir,normalized,a.discover_reviewed_nested)
    dump(a.artifacts/'normalized-authorities.json',normalized)
    dump(a.artifacts/'nested-extra-map.json',dict(entries=normalized['nested_entries']))
    dump(a.artifacts/'source-plan.json',result)
    if a.apply:
        if not a.output:ap.error('--apply requires --output NEW DIRECTORY')
        result=apply_all(result,normalized,a.output)
    print(json.dumps(dict(status=result['status'],authorities=len(normalized['authorities']),nested_maps=len(normalized['nested_entries']),primary_records=len(result.get('primary_records',[])))))

if __name__=='__main__':main()
