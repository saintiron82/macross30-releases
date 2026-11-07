"""Close Japanese UI image gaps discovered from original ARK texture references."""
import json
import struct
import subprocess
from pathlib import Path
import numpy as np
from english_pack import ROOT,digest,write
from stage_gop_primary_mirrors import pidx,read_payload
from encode_misc_menu_replacements import pixels,constrained
from build_quest_title_bc3 import encode
from build_extra_english_images import mono_bc3

if __name__=='__main__':
    folder=ROOT/'localization/en-US/images/discovery-v10';folder.mkdir(parents=True,exist_ok=True)
    pack=ROOT/'native/vfs/PS3_GAME/USRDIR/DATA/PACK';_,master,_=pidx(pack/'PACK.IDX');paths={p.name.lower():p for p in pack.iterdir()}
    names={f'chapter_t_{i:03}.dds' for i in range(1,10)}|{'hud_select_off.dds','wanted_name000.dds','cst_bg.dds','ransher_back.dds'}
    assets=[];seen=set()
    for r in master:
        if r['name'] not in names:continue
        source,_=read_payload(paths[Path(r['pack']).name.lower()],r);sha=digest(source)
        if (r['name'],sha) in seen:continue
        seen.add((r['name'],sha));h,w=struct.unpack_from('<II',source,12);name=r['name'];stem=Path(name).stem+'-'+sha[:8]
        src=folder/(stem+'.dds');src.write_bytes(source)
        bgra=folder/(stem+'.bgra');bgra.write_bytes(bytes(v for red,g,b,a in pixels(source,w,h) for v in (b,g,red,a)))
        a=dict(name=name,source_DDS=str(src),source_BGRA=str(bgra),source_sha256=sha,width=w,height=h,format='BC3' if source[84:88]==b'DXT5' else 'BGRA8',color=[255,255,255],mode='text',output_stem=str(folder/'rendered'/stem),primary_records=[r['record']])
        if name.startswith('chapter_t_'):
            n=int(name[-7:-4]);a['rows']=[dict(text=f'CHAPTER {n}',rect=[0,0,w,h],size=20)];a['color']=[255,235,145]
        elif name=='hud_select_off.dds':
            a['mode']='panel';a['rows']=[dict(text='SELECT: Release',rect=[116,0,88,16],size=10)];a['color']=[180,180,180]
        elif name=='wanted_name000.dds':a['rows']=[dict(text='Bartholomew Roberts',rect=[0,0,w,h],size=13)]
        elif name=='cst_bg.dds':a['mode']='panel';a['rows']=[dict(text='CUSTOM SOUND',rect=[92,56,320,44],size=38)]
        else:
            a['mode']='art';a['generated_edit']=str(ROOT/'localization/en-US/review-v10/ransher-back-generated.png')
            a['rows']=[dict(text='LOVE YOU',rect=[0,0,348,310],size=20)]
        a['mip_levels']=max(1,struct.unpack_from('<I',source,28)[0]);a['replacement']=a['output_stem']+'.dds';assets.append(a)
    assert len(assets)==13
    write(folder/'composition-plan.json',dict(assets=assets))
    subprocess.run(['pwsh','-NoProfile','-File',str(ROOT/'tools/localization/render_extra_english_images.ps1'),'-Plan',str(folder/'composition-plan.json')],check=True)
    reports=[]
    for a in assets:
        source=Path(a['source_DDS']).read_bytes();w,h=a['width'],a['height'];raw=Path(a['output_stem']+'.rgba').read_bytes();assert len(raw)==w*h*4
        p=np.frombuffer(raw,dtype=np.uint8).reshape(h,w,4)
        if a['mode'] in ('panel','art'):
            target=bytearray(source);old=pixels(source,w,h);allowed=set()
            for row in a['rows']:
                x,y,rw,rh=row['rect'];allowed.update(yy*w+xx for yy in range(y,y+rh) for xx in range(x,x+rw))
            if a['format']=='BC3':
                for bx,by in {(i%w//4,i//w//4) for i in allowed}:
                    at=128+(by*(w//4)+bx)*16;ps=[tuple(int(c) for c in p[by*4+j//4,bx*4+j%4]) for j in range(16)]
                    editable={j for j in range(16) if (by*4+j//4)*w+bx*4+j%4 in allowed}
                    target[at:at+16]=constrained(source[at:at+16],ps,editable) if len(editable)<16 else encode(ps)
            else:
                for i in allowed:red,g,b,alpha=p[i//w,i%w];target[128+i*4:128+i*4+4]=bytes((b,g,red,alpha))
            decoded=pixels(bytes(target),w,h)
            assert all(old[i]==decoded[i] for i in range(w*h) if i not in allowed)
            if a['mip_levels']>1:
                assert a['format']=='BC3'
                mip_at=128+((w+3)//4)*((h+3)//4)*16
                for level in range(1,a['mip_levels']):
                    mw,mh=max(1,w>>level),max(1,h>>level);size=((mw+3)//4)*((mh+3)//4)*16
                    pts=np.frombuffer(Path(a['output_stem']+f'.mip{level}.rgba').read_bytes(),dtype=np.uint8).reshape(mh,mw,4)
                    old_mip=source[:128]+source[mip_at:mip_at+size];old_pixels=pixels(old_mip,mw,mh);changed=set()
                    for row in a['rows']:
                        x,y,rw,rh=row['rect'];scale=1<<level
                        changed.update(yy*mw+xx for yy in range(max(0,y//scale-1),min(mh,(y+rh+scale-1)//scale+1)) for xx in range(max(0,x//scale-1),min(mw,(x+rw+scale-1)//scale+1)))
                    for bx,by in {(i%mw//4,i//mw//4) for i in changed}:
                        at=mip_at+(by*((mw+3)//4)+bx)*16
                        ps=[tuple(int(c) for c in pts[min(mh-1,by*4+j//4),min(mw-1,bx*4+j%4)]) for j in range(16)]
                        editable={j for j in range(16) if by*4+j//4<mh and bx*4+j%4<mw and (by*4+j//4)*mw+bx*4+j%4 in changed}
                        target[at:at+16]=constrained(source[at:at+16],ps,editable) if len(editable)<16 else encode(ps)
                    actual=pixels(source[:128]+target[mip_at:mip_at+size],mw,mh)
                    assert all(old_pixels[i]==actual[i] for i in range(mw*mh) if i not in changed)
                    mip_at+=size
                assert mip_at==len(source)
        elif a['format']=='BC3':target=source[:128]+mono_bc3(raw,w,h,a['color'])
        else:target=source[:128]+p.reshape(-1,4)[:,[2,1,0,3]].tobytes()
        target=bytes(target);assert len(target)==len(source) and target[:128]==source[:128] and target!=source
        Path(a['replacement']).write_bytes(target)
        reports.append(dict(name=a['name'],source_sha256=a['source_sha256'],replacement=a['replacement'],replacement_sha256=digest(target),labels=a['rows'],dimensions=[w,h],header_unchanged=True,outside_edit_rectangles_unchanged=True,updated_mip_levels=a['mip_levels'],primary_records=a['primary_records'],game_render_verified=False))
    write(folder/'manifest.json',dict(assets=reports,asset_count=len(reports),status='prepared_not_installed'))
    print(dict(additional_images=len(reports),source_dimensions_headers_and_outside_ROIs='PASS'))
