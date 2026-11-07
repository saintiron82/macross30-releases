"""Build additional English text sprites with bounded native font composition."""
import json
import argparse
from pathlib import Path
import struct
import subprocess
import numpy as np
from english_pack import ROOT,digest,write
from build_quest_title_bc3 import encode,pack565
from encode_misc_menu_replacements import pixels,constrained

def mono_bc3(rgba,w,h,color):
    a=np.frombuffer(rgba,dtype=np.uint8).reshape(h,w,4)[:,:,3]
    assert w%4==h%4==0
    blocks=a.reshape(h//4,4,w//4,4).transpose(0,2,1,3).reshape(-1,16).astype(np.int16)
    palette=np.array([255,0,218,182,145,109,72,36],dtype=np.int16)
    indices=np.abs(blocks[:,:,None]-palette).argmin(axis=2).astype(np.uint64)
    bits=(indices<<(3*np.arange(16,dtype=np.uint64))).sum(axis=1)
    output=np.zeros((len(blocks),16),dtype=np.uint8);output[:,0]=255
    for n in range(6):output[:,2+n]=(bits>>(8*n))&255
    c=pack565(tuple(color));output[:,8]=output[:,10]=c&255;output[:,9]=output[:,11]=c>>8
    return output.tobytes()

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--folder',type=Path,default=ROOT/'localization/en-US/images/extra-ui-v1');args=parser.parse_args()
    folder=args.folder.resolve()
    sources=json.loads((folder/'sources.json').read_text(encoding='utf8'))
    labels=json.loads((ROOT/'tools/localization/catalogs/extra-image-labels.json').read_text(encoding='utf8'))
    places=json.loads((ROOT/'analysis/localization/region-name-audit/b-c-f-s-definitive.json').read_text(encoding='utf8'))['rows']
    english=json.loads((ROOT/'localization/en-US/images/region-labels.json').read_text(encoding='utf8'))
    caves={r['family']:english[r['JP']] for r in places if r['family'].startswith('placename_c')}
    for a in sources['assets']:
        name=a['name'];w,h=a['width'],a['height'];a['color']=[255,255,255];a['mode']='text';rows=[]
        if name.startswith('title_credit'):
            if len(a['bands'])!=len(labels['credits'][name]):raise ValueError('Credit row count mismatch: '+name)
            rows=[dict(text=t,rect=[8,max(0,y0-2),w-16,min(h,y1+2)-max(0,y0-2)],size=24) for t,(y0,y1) in zip(labels['credits'][name],a['bands'])]
            a['name_readings_review']=labels['credit_name_readings_review']
            if name=='title_credit06.dds':a['color']=[0,0,0]
        elif name=='hanger_guide.dds':
            rows=[dict(text=t,rect=[29,i*28,w-36,28],size=14) for i,t in enumerate(labels[name])];a['color']=[90,255,180]
        elif name=='minititle_dlc.dds':rows=[dict(text='DLC AIRCRAFT',rect=[1,0,w-2,20],size=12)];a['color']=[255,225,50]
        elif name=='placename_b03_title01_01.dds':rows=[dict(text='Fortress',rect=[1,0,w-2,h],size=24)];a['color']=[220,220,220]
        elif name.startswith('placename_c'):
            family=name[:13];text=caves.get(family,caves['placename_c01'])
            rows=[dict(text=text,rect=[4,0,w-8,h],size=42)];a['color']=[210,210,210];a['mode']='scan'
        elif name=='hud_menu00.dds':
            a['mode']='panel'
            rows=[dict(text=t,rect=[465,y+2,42,14],size=12) for t,y in zip(['Hunt','Delivery','Bounty','Escort','Other'],[146,166,186,206,226])]
            rows.append(dict(text='Cleared',rect=[171,238,62,15],size=13))
        else:raise ValueError(name)
        a['rows']=rows;a['output_stem']=str(folder/'rendered'/a['output_stem']);a['replacement']=a['output_stem']+'.dds'
    write(folder/'composition-plan.json',sources)
    subprocess.run(['pwsh','-NoProfile','-File',str(ROOT/'tools/localization/render_extra_english_images.ps1'),'-Plan',str(folder/'composition-plan.json')],check=True)
    reports=[]
    for a in sources['assets']:
        source=Path(a['source_DDS']).read_bytes();assert digest(source)==a['source_sha256'];w,h=a['width'],a['height']
        raw=Path(a['output_stem']+'.rgba').read_bytes();assert len(raw)==w*h*4
        if a['mode']=='panel':
            target=bytearray(source);points=np.frombuffer(raw,dtype=np.uint8).reshape(h,w,4);old=pixels(source,w,h);allowed=set()
            for row in a['rows']:
                x,y,rw,rh=row['rect'];allowed.update(yy*w+xx for yy in range(y,y+rh) for xx in range(x,x+rw))
            for i in range(w*h):
                if i not in allowed:assert tuple(points[i//w,i%w])==old[i]
            if a['format']=='BC3':
                blocks={(i%w//4,i//w//4) for i in allowed}
                for bx,by in blocks:
                    at=128+(by*(w//4)+bx)*16;p=[tuple(int(c) for c in points[by*4+j//4,bx*4+j%4]) for j in range(16)]
                    editable={j for j in range(16) if (by*4+j//4)*w+bx*4+j%4 in allowed}
                    target[at:at+16]=constrained(source[at:at+16],p,editable) if len(editable)<16 else encode(p)
            else:
                for i in allowed:r,g,b,alpha=points[i//w,i%w];target[128+i*4:128+i*4+4]=bytes((b,g,r,alpha))
            decoded=pixels(bytes(target),w,h)
            assert all(old[i]==decoded[i] for i in range(w*h) if i not in allowed)
        elif a['format']=='BC3':target=source[:128]+mono_bc3(raw,w,h,a['color'])
        else:
            p=np.frombuffer(raw,dtype=np.uint8).reshape(-1,4);target=source[:128]+p[:,[2,1,0,3]].tobytes()
        target=bytes(target);assert target[:128]==source[:128] and len(target)==len(source) and target!=source
        Path(a['replacement']).write_bytes(target)
        reports.append(dict(name=a['name'],source_sha256=a['source_sha256'],replacement=a['replacement'],replacement_sha256=digest(target),header_unchanged=True,dimensions=[w,h],labels=a['rows'],primary_records=a['primary_records'],name_readings_review=a.get('name_readings_review'),game_render_verified=False))
    write(folder/'manifest.json',dict(assets=reports,status='prepared_not_installed',asset_count=len(reports)))
    print(json.dumps(dict(additional_images=len(reports),headers_and_dimensions='PASS',outside_HUD_ROIs='PASS',credit_reading_review='pending')))
