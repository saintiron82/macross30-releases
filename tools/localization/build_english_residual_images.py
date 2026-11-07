"""Remove Japanese remnants found by inspecting the installed English snapshots."""
import json
import math
from pathlib import Path
import struct
import subprocess
import numpy as np
from english_pack import ROOT,digest,write
from encode_misc_menu_replacements import pixels

if __name__=='__main__':
    folder=ROOT/'localization/en-US/images/residual-v11';folder.mkdir(parents=True,exist_ok=True)
    registry=json.loads((ROOT/'tools/localization/catalogs/image_registry.json').read_text(encoding='utf8'))
    names={'hangar_text01.dds','gallery_menu01.dds','loadpict035.dds','loadpict035_s.dds','loadpict083.dds','loadpict083_s.dds'}
    assets=[]
    for r in registry['assets']:
        if r['name'] not in names:continue
        name=r['name'];base=ROOT/r['source_files']['en-US'];raw=base.read_bytes();assert digest(raw)==r['english_sha256']
        src=folder/(r['id']+'-baseline.dds')
        if src.exists():
            raw=src.read_bytes() # Preserve the reviewed v10 baseline on reruns.
        else:src.write_bytes(raw)
        h,w=struct.unpack_from('<II',raw,12);assert raw[84:88]==bytes(4)
        bgra=folder/(r['id']+'.bgra');bgra.write_bytes(raw[128:]);assert len(raw)==128+w*h*4
        a=dict(name=name,source_DDS=str(src),source_BGRA=str(bgra),source_sha256=r['original_sha256'],baseline_sha256=digest(raw),width=w,height=h,format='BGRA8',mode='clear',color=[255,255,255],output_stem=str(folder/'rendered'/r['id']),primary_records=[])
        if name=='hangar_text01.dds':
            a['rows']=[dict(text=t,rect=box,size=s,color=c) for t,box,s,c in [
                ('Consum.',[0,0,30,16],9,[0,0,0]),('Parts',[35,0,40,16],12,[0,0,0]),
                ('Gear / Delivery',[0,16,64,16],10,[0,0,0]),('Key Items',[0,32,64,16],12,[0,0,0]),
                ('Left/Right: Switch',[0,48,106,16],10,[255,215,65])]]
        elif name=='gallery_menu01.dds':
            a['mode']='panel';a['rows']=[dict(text=t,rect=[85,y,52,18],size=13) for t,y in zip(['Hunt','Delivery','Bounty','Escort','Other'],[28,48,68,88,108])]
        else:
            n='035' if '035' in name else '083';a['mode']='art';a['generated_edit']=str(ROOT/f'localization/en-US/review-v10/sharon-{n}-clean.png')
            x,y=int(512*w/1280),int(558*h/720);right,bottom=math.ceil(782*w/1280),math.ceil(622*h/720)
            a['rows']=[dict(text='Japanese name remnant removed; English SHARON APPLE retained',rect=[x,y,right-x,bottom-y],size=10)]
            a['feather']=max(1,4*w/1280)
        a['replacement']=a['output_stem']+'.dds';assets.append(a)
    assert len(assets)==6
    write(folder/'composition-plan.json',dict(assets=assets))
    subprocess.run(['pwsh','-NoProfile','-File',str(ROOT/'tools/localization/render_extra_english_images.ps1'),'-Plan',str(folder/'composition-plan.json')],check=True)
    reports=[]
    for a in assets:
        original=Path(a['source_DDS']).read_bytes();w,h=a['width'],a['height']
        raw=Path(a['output_stem']+'.rgba').read_bytes();p=np.frombuffer(raw,dtype=np.uint8).reshape(h,w,4)
        allowed=np.zeros((h,w),dtype=bool)
        for row in a['rows']:
            x,y,rw,rh=row['rect'];allowed[y:y+rh,x:x+rw]=True
        old=np.frombuffer(original[128:],dtype=np.uint8).reshape(h,w,4)[:,:,[2,1,0,3]]
        assert np.array_equal(old[~allowed],p[~allowed])
        target=original[:128]+p[:,:,[2,1,0,3]].tobytes();assert target!=original and len(target)==len(original)
        Path(a['replacement']).write_bytes(target)
        reports.append(dict(name=a['name'],source_sha256=a['source_sha256'],replacement=a['replacement'],replacement_sha256=digest(target),baseline_sha256=a['baseline_sha256'],labels=a['rows'],dimensions=[w,h],outside_revision_rectangles_unchanged=True,game_render_verified=False))
    write(folder/'manifest.json',dict(assets=reports,asset_count=6,status='prepared_not_installed'))
    print(dict(revised_images=6,outside_v10_revision_rectangles='PASS'))
