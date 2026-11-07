"""Read-only final v6 pack composition verification; never installs/selects data.

Search order: final nested, v6 primary, v5 nested, v5 primary, active fallback.
All reads follow exact reviewed PIDX records or FSTS indices, never basename
selection for a resource. Pack basenames only resolve physical backing files.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from analyze_archives import unpack
from stage_nested_ui import pidx, region, fsts_entries, decoded
from stage_gop_primary_mirrors import validate_gop
from stage_hud_guide import sha_file

ROOT=Path(__file__).resolve().parents[2]
V5=ROOT/'localization/image_assets/gop-ui-v5'
V6=ROOT/'localization/image_assets/data-ui-v6'
sha=lambda b:hashlib.sha256(b).hexdigest()
load=lambda p:json.loads(Path(p).read_text(encoding='utf-8-sig'))


class Reader:
    def __init__(self,index,sources):
        self.index,self.archives=pidx(index)
        self.sources=[Path(p) for p in sources]
        self.used={}

    def pack(self,name):
        key=Path(name.replace('\\','/')).name.lower()
        for directory in self.sources:
            p=directory/key
            if p.is_file():
                self.used[key]=str(p.resolve());return p
        raise FileNotFoundError('Missing backing pack '+key)

    def primary(self,record,pack,expected,embedded_field=None):
        if record<40 or record+24>len(self.index):raise ValueError('Invalid master record')
        folder,no,pn,off,size,zs=struct.unpack_from('<6I',self.index,record)
        if folder:raise ValueError('Expected primary file record')
        encoded=region(self.pack(pack),off,zs if 0<zs<size else size)
        raw=unpack(encoded,size) if 0<zs<size else encoded
        if sha(raw)!=expected:raise ValueError('Primary payload mismatch at '+hex(record))
        if embedded_field is not None and region(self.pack(pack),embedded_field,12)!=self.index[record+12:record+24]:
            raise ValueError('Primary/embedded mirror differs')
        return raw,dict(master_record=record,pack=pack,offset=off,size=size,zs=zs,sha256=expected,embedded_mirror_checked=embedded_field is not None)

    def archive(self,original):
        field=original['field']
        if field<0 or field+8>len(self.index):raise ValueError('Secondary field out of bounds')
        found=[r for r in self.archives if r['field']==field]
        if len(found)!=1 or found[0]['path']!=original['path'] or found[0]['pack']!=Path(original['pack']).name.lower():
            raise ValueError('Secondary exact path/pack/field changed')
        r=found[0]
        return region(self.pack(r['pack']),r['offset'],r['size']),r


def verify(index,sources,primary_manifest,nested_manifest):
    reader=Reader(index,sources)
    old=load(ROOT/'localization/image_assets/caution-ui-v4/final-verification.json')
    images=[];authority={}
    for r in old['primary_readbacks']:
        raw,proof=reader.primary(r['master_record'],r['pack'],r['sha256'],r['embedded_record']+12)
        images.append(dict(name=r['name'],**proof))
        authority[r['name']]=r['sha256']
    if len(images)!=62:raise ValueError('Expected 62 prior primary DDS rows')
    gops=[]
    gp=load(V5/'staged-primary/gop-primary-mirrors-manifest.json')
    for c in gp['changes']:
        masters=[a for a in c['aliases'] if a['role']=='master'];embedded=[a for a in c['aliases'] if a['role']=='embedded']
        if len(masters)!=1 or len(embedded)!=1:raise ValueError('Unexpected GOP mirror multiplicity')
        raw,proof=reader.primary(masters[0]['record'],masters[0]['pack'],c['authority_sha256'],embedded[0]['record']+12)
        catalog=load(c['catalog']);validation=validate_gop((ROOT/catalog['source_file']).read_bytes(),raw,catalog)
        gops.append(dict(name=c['name'],validation=validation,**proof))
    if len(gops)!=12 or sum(g['validation']['korean_catalog_entries'] for g in gops)!=13536:
        raise ValueError('Expected all 12 GOP / 13536 KO strings')
    battle=[];catalog=load(ROOT/'localization/ko-KR/structured/gop_battletext.json');original=(ROOT/catalog['source_file']).read_bytes()
    for a in load(V5/'nested-staged/nested-ui-manifest.json')['archives']:
        rawarchive,where=reader.archive(a['original']);entries=fsts_entries(rawarchive)
        for e in a['entries']:
            r=entries[e['index']]
            if r['path']!=e['path']:raise ValueError('Battletext path/order changed')
            raw,_=decoded(rawarchive,r)
            if sha(raw)!=e['replacement_sha256']:raise ValueError('Prior nested battletext SHA changed')
            validation=validate_gop(original,raw,catalog)
            battle.append(dict(archive=where['path'],index=e['index'],sha256=sha(raw),validation=validation))
    if len(battle)!=40:raise ValueError('Expected 40 prior nested battletext copies')
    primary=load(primary_manifest);new=[]
    for c in primary['changes']:
        raw,proof=reader.primary(c['master_field']-12,c.get('pack',primary['pack_filename']),c['sha256'],c['embedded_field'])
        new.append(dict(name=c['name'],**proof));authority[Path(c['name']).name]=c['sha256']
    if len(new)!=7:raise ValueError('Expected 7 new primary DDS rows')
    fontmap=load(V6/'font-incremental/nested-font-map.json')
    for e in fontmap['entries']:authority[e['path']]=e['replacement_sha256']
    m=load(nested_manifest)
    if m.get('unmatched'):raise ValueError('Unmatched nested entries remain')
    nested=[];fontcounts={e['path']:0 for e in fontmap['entries']}
    for a in m['archives']:
        archive,where=reader.archive(a['original']);entries=fsts_entries(archive)
        for e in a['entries']:
            r=entries[e['index']]
            if r['path']!=e['path']:raise ValueError('New nested path/order changed')
            raw,_=decoded(archive,r);h=sha(raw)
            if h!=e['replacement_sha256']:raise ValueError('New nested payload mismatch')
            expected=authority.get(r['path'],authority.get(Path(r['path']).name))
            if expected!=h:raise ValueError('Nested output differs from reviewed DDS/font authority')
            if r['path'] in fontcounts:fontcounts[r['path']]+=1
            nested.append(dict(archive=where['path'],index=e['index'],path=r['path'],sha256=h))
    # Existing exact-output copies are independently read back too.
    already=[]
    for a in m.get('already_translated',[]):
        archive,where=reader.archive(a['archive']);entries=fsts_entries(archive);r=entries[a['entry_index']]
        raw,_=decoded(archive,r)
        if r['path']!=a['path'] or sha(raw)!=a['sha256']:raise ValueError('Already-translated payload changed')
        expected=authority.get(r['path'],authority.get(Path(r['path']).name))
        if expected!=sha(raw):raise ValueError('Existing output differs from reviewed authority')
        if r['path'] in fontcounts:fontcounts[r['path']]+=1
        already.append(dict(archive=where['path'],path=r['path'],sha256=sha(raw)))
    if any(v<1 for v in fontcounts.values()):raise ValueError('Both font payload types must be read back')
    hashes={name:sha_file(Path(path)) for name,path in reader.used.items()}
    hashes['pack.idx']=sha_file(Path(index))
    return dict(status='PASS',scope='Offline pack composition only; game rendering/install not verified',index=str(Path(index).resolve()),backing_files=reader.used,output_hashes=hashes,prior_primary_DDS_count=len(images),prior_primary_DDS=images,primary_GOP_count=len(gops),primary_GOP_KO_strings=13536,primary_GOPs=gops,prior_nested_battletext_count=len(battle),prior_nested_battletext=battle,new_primary_DDS_count=len(new),new_primary_DDS=new,nested_changed_readbacks=nested,nested_already_translated_readbacks=already,font_payload_copy_counts=fontcounts,font_glyphs=8175,active_or_selector_modified=False)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--index',type=Path,required=True)
    p.add_argument('--source-dir',type=Path,action='append',required=True,help='Repeat in highest-to-lowest priority order')
    p.add_argument('--primary-manifest',type=Path,required=True)
    p.add_argument('--nested-manifest',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    n=p.parse_args()
    if n.output.exists():raise SystemExit('Output proof already exists; choose a new path')
    protected=[ROOT/'native',ROOT/'build']
    if any(q.resolve()==n.output.resolve() or q.resolve() in n.output.resolve().parents for q in protected):
        raise SystemExit('Proof output must be outside active/runtime/build trees')
    proof=verify(n.index,n.source_dir,n.primary_manifest,n.nested_manifest)
    n.output.parent.mkdir(parents=True,exist_ok=True);n.output.write_text(json.dumps(proof,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    print('PASS prior DDS62 / GOP12 / nested battle40 / new DDS7 / font8175')


if __name__=='__main__':main()
