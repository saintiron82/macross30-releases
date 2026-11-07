"""Stage the three exact-SHA KO nested copies missed by the older route comparison."""
import json,shutil,struct
from pathlib import Path
from stage_gop_primary_mirrors import pidx
from stage_nested_ui import region,fsts_entries,decoded,rebuild
from stage_ui_images import sha_file
import hashlib
ROOT=Path(__file__).resolve().parents[2]
def digest(b):return hashlib.sha256(b).hexdigest()
def main():
    load=lambda p:json.loads(p.read_text(encoding='utf8'))
    active_path=ROOT/'localization/image_assets/active-ui.json';active=load(active_path)
    assert active['bundle']=='viewer-guide-menu-v19'
    previous=ROOT/'localization/image_assets'/active['bundle'];ready=load(previous/'ready.json')
    out=ROOT/'localization/image_assets/menu-nested-repair-v20';assert not out.exists();out.mkdir()
    source=ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK'
    authorities=[load(ROOT/'localization/itembox-tabs/manifest.json'),load(ROOT/'localization/quest-badges/manifest.json'),load(ROOT/'localization/viewer-guide/manifest.json')['ko-KR']]
    names={a['name']:a for a in authorities};_,_,archives=pidx(source/'pack.idx')
    staged={'pack.idx':out/'pack.idx'};shutil.copyfile(source/'pack.idx',staged['pack.idx']);proof=[]
    for archive in archives:
        name=Path(archive['pack']).name.lower();original=source/name
        if name not in ('data.dat','data2.dat','fileset0.dat','fileset2.dat'):continue
        blob=region(original,archive['offset'],archive['size']);patches={}
        for e in fsts_entries(blob):
            a=names.get(Path(e['path']).name)
            if a is None:continue
            raw,_=decoded(blob,e)
            if digest(raw)==a['replacement_sha256']:continue
            assert digest(raw)==a['source_sha256']
            new=Path(a['replacement']).read_bytes();assert digest(new)==a['replacement_sha256']
            patches[e['index']]=(a['source_sha256'],new)
        if not patches:continue
        rebuilt,entries=rebuild(blob,patches)
        if name not in staged:
            staged[name]=out/name;shutil.copyfile(original,staged[name])
        destination=staged[name];_,_,embedded=pidx(destination)
        aliases=[r for r in embedded if all(r[k]==archive[k] for k in ('name','pack','offset','size'))];assert len(aliases)==1
        with destination.open('ab') as f:
            f.write(b'\0'*(-f.tell()%2048));offset=f.tell();f.write(rebuilt)
        for path,row in [(staged['pack.idx'],archive),(destination,aliases[0])]:
            with path.open('r+b') as f:f.seek(row['record']+8);f.write(struct.pack('<II',offset,len(rebuilt)))
        proof+=entries
    assert len(proof)==3
    _,_,check=pidx(staged['pack.idx']);readbacks=[]
    for archive in check:
        name=Path(archive['pack']).name.lower()
        if name not in staged:continue
        blob=region(staged[name],archive['offset'],archive['size'])
        for e in fsts_entries(blob):
            a=names.get(Path(e['path']).name)
            if a:
                raw,_=decoded(blob,e);assert digest(raw)==a['replacement_sha256'];readbacks.append(e['path'])
    assert len(readbacks)==3
    chosen=active.get('elf_override',ready['elf']);shutil.copyfile(previous/chosen['path'],out/chosen['path'])
    doc=dict(files=[dict(filename=n,candidate=p.name,source_sha256=sha_file(source/n),sha256=sha_file(p)) for n,p in staged.items()],elf=chosen,manifests=[],status='prepared nested menu repair',in_game_verified=False)
    doc['version']=digest(json.dumps(doc,sort_keys=True).encode())
    for name,obj in [('ready.json',doc),('previous-active.json',active),('review.json',dict(status='PASS',fixed_nested_entries=readbacks,proof=proof,root_cause='archive pack path compared against basename without normalization'))]:
        (out/name).write_text(json.dumps(obj,ensure_ascii=False,indent=2),encoding='utf8')
    active_path.write_text(json.dumps(dict(bundle=out.name),indent=2),encoding='utf8')
    print(json.dumps(dict(bundle=out.name,nested_readbacks=len(readbacks),installed=False)))
if __name__=='__main__':main()
