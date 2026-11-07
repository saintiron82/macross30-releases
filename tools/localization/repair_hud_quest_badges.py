"""Translate the actual HUD quest badges in both exact source variants and routes."""
import hashlib,json,shutil,struct
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from stage_menu_ui_bundle import primary,make_plan,audit_nested
from stage_ui_images import apply,sha_file
from stage_gop_primary_mirrors import pidx,read_payload
from stage_nested_ui import region,fsts_entries,decoded,rebuild
ROOT=Path(__file__).resolve().parents[2]
def digest(b):return hashlib.sha256(b).hexdigest()
def main():
    load=lambda p:json.loads(p.read_text(encoding='utf8'))
    pointer=ROOT/'localization/image_assets/active-ui.json';active=load(pointer);assert active['bundle']=='menu-nested-repair-v20'
    prior=ROOT/'localization/image_assets'/active['bundle'];ready=load(prior/'ready.json')
    out=ROOT/'localization/image_assets/hud-quest-badges-v21';assert not out.exists();out.mkdir()
    source=ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK';_,rows,_=pidx(source/'pack.idx')
    authorities=[];expected={'d55af8a25e71d5fba038a84a1d7eecfab3c44b5665709d6f33f7454f6a81ccde','0b9f9e7cde611d77e11c4c16d45f284195dac84921ab805d9839c5881583d0f6'}
    for r in [r for r in rows if r['name']=='hud_menu00.dds']:
        raw,_=read_payload(source/Path(r['pack']).name,r);old=digest(raw);assert old in expected
        import io
        im=Image.open(io.BytesIO(raw)).convert('RGBA');before=np.array(im);mask=np.zeros(before.shape[:2],bool)
        labels=[('토벌',(455,146,52,18)),('납품',(455,166,52,18))]
        if old.startswith('d55a'):labels += [('현상범',(459,186,52,18)),('호위',(459,206,52,18)),('기타',(459,226,52,18)),('완료',(165,237,74,18))]
        for text,(x,y,w,h) in labels:
            tile=im.crop((x,y,x+w,y+h));pixels=np.array(tile)
            for yy in range(h):
                a=pixels[yy,2].astype(float);b=pixels[yy,w-3].astype(float)
                for xx in range(3,w-3):tile.putpixel((xx,yy),tuple(np.rint(a+(b-a)*(xx-2)/(w-5)).astype(int)))
            big=tile.resize((w*4,h*4),Image.Resampling.NEAREST)
            ImageDraw.Draw(big).text((w*2,h*2-1),text,font=ImageFont.truetype('C:/Windows/Fonts/malgunbd.ttf',48),fill=(255,255,255,255),anchor='mm')
            im.paste(big.resize((w,h),Image.Resampling.LANCZOS),(x,y));mask[y:y+h,x:x+w]=True
        after=np.array(im);assert np.array_equal(before[~mask],after[~mask]);new=raw[:128]+after[:,:,[2,1,0,3]].tobytes()
        assert len(new)==len(raw);target=out/('hud-'+old[:12]+'.dds');target.write_bytes(new)
        bg=Image.new('RGBA',im.size,(30,35,40,255));bg.alpha_composite(im);bg.resize((1024,1024)).save(target.with_suffix('.png'))
        authorities.append(dict(name='hud_menu00.dds',source_sha256=old,replacement=str(target),replacement_sha256=digest(new),labels=labels))
    assert len(authorities)==2
    nested=audit_nested(source,dict(authorities=authorities,nested_entries=[]),discover_reviewed=True)
    staged={};index=source/'pack.idx'
    for i,name in enumerate(sorted({Path(r['pack']).name.lower() for r in rows if r['name']=='hud_menu00.dds'})):
        plan=make_plan(source/name,index,authorities,sha_file(source/name),sha_file(index));folder=out/f'stage-{i}';apply(plan,folder);staged[name]=folder/name;index=folder/'pack.idx'
    staged['pack.idx']=index;_,_,archives=pidx(source/'pack.idx');nested_proof=[]
    for archive in archives:
        if not any(m['archive_offset']==archive['offset'] and Path(m['pack']).name.lower()==Path(archive['pack']).name.lower() for m in nested['matches']):continue
        name=Path(archive['pack']).name.lower();blob=region(source/name,archive['offset'],archive['size']);patches={}
        for e in fsts_entries(blob):
            if Path(e['path']).name!='hud_menu00.dds':continue
            raw,_=decoded(blob,e);a=next(a for a in authorities if a['source_sha256']==digest(raw));patches[e['index']]=(a['source_sha256'],Path(a['replacement']).read_bytes())
        rebuilt,proof=rebuild(blob,patches)
        if name not in staged:staged[name]=out/name;shutil.copyfile(source/name,staged[name])
        destination=staged[name];_,_,embedded=pidx(destination);alias=next(r for r in embedded if all(r[k]==archive[k] for k in ('name','pack','offset','size')))
        with destination.open('ab') as f:f.write(b'\0'*(-f.tell()%2048));off=f.tell();f.write(rebuilt)
        for path,row in [(index,archive),(destination,alias)]:
            with path.open('r+b') as f:f.seek(row['record']+8);f.write(struct.pack('<II',off,len(rebuilt)))
        nested_proof+=proof
    selected_records={a['record'] for a in pidx(source/'pack.idx')[2] if any(m['archive_offset']==a['offset'] and Path(m['pack']).name.lower()==Path(a['pack']).name.lower() for m in nested['matches'])}
    _,actual,archives=pidx(index);replacement_hashes={a['replacement_sha256'] for a in authorities};primary_checks=[];nested_checks=[]
    for r in actual:
        if r['name']=='hud_menu00.dds':
            raw,_=read_payload(staged[Path(r['pack']).name.lower()],r);assert digest(raw) in replacement_hashes;primary_checks.append(digest(raw))
    for archive in archives:
        if archive['record'] not in selected_records:continue
        name=Path(archive['pack']).name.lower();blob=region(staged[name],archive['offset'],archive['size'])
        for e in fsts_entries(blob):
            if Path(e['path']).name=='hud_menu00.dds':assert digest(decoded(blob,e)[0]) in replacement_hashes;nested_checks.append(e['path'])
    assert len(primary_checks)==2 and len(nested_checks)==len(nested['matches'])
    chosen=active.get('elf_override',ready['elf']);shutil.copyfile(prior/chosen['path'],out/chosen['path'])
    doc=dict(files=[dict(filename=n,candidate=str(p.relative_to(out)),source_sha256=sha_file(source/n),sha256=sha_file(p)) for n,p in staged.items()],elf=chosen,manifests=[],status='prepared actual HUD quest badges',in_game_verified=False)
    doc['version']=digest(json.dumps(doc,sort_keys=True).encode())
    for name,obj in [('ready.json',doc),('previous-active.json',active),('review.json',dict(status='PASS',primary_readbacks=primary_checks,nested_readbacks=nested_checks,authorities=authorities,game_render_verified=False))]:
        (out/name).write_text(json.dumps(obj,ensure_ascii=False,indent=2),encoding='utf8')
    pointer.write_text(json.dumps(dict(bundle=out.name),indent=2),encoding='utf8')
    print(json.dumps(dict(bundle=out.name,primary_readbacks=len(primary_checks),nested_readbacks=len(nested_checks),installed=False)))
if __name__=='__main__':main()
