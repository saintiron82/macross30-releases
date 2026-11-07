"""Prepare exact-source standalone text and bounded HUD atlas additions."""
import json
import struct
from pathlib import Path
import numpy as np
from english_pack import ROOT,digest,write
from stage_gop_primary_mirrors import pidx,read_payload
from encode_misc_menu_replacements import pixels

if __name__=='__main__':
    out=ROOT/'localization/en-US/images/extra-ui-v1';out.mkdir(parents=True,exist_ok=True)
    pack=ROOT/'native/vfs/PS3_GAME/USRDIR/DATA/PACK';_,master,_=pidx(pack/'PACK.IDX');paths={p.name.lower():p for p in pack.iterdir()}
    names={'hanger_guide.dds','minititle_dlc.dds','hud_menu00.dds','placename_b03_title01_01.dds','placename_c00_title06.dds'}
    names|={f'placename_c{i:02}_title02.dds' for i in range(1,10)}|{f'title_credit{i:02}.dds' for i in range(7)}
    rows=[];seen=set();bands={}
    for r in master:
        if r['name'] not in names:continue
        data,_=read_payload(paths[Path(r['pack']).name.lower()],r);sha=digest(data)
        if (r['name'],sha) in seen:continue
        seen.add((r['name'],sha));h,w=struct.unpack_from('<II',data,12)
        stem=Path(r['name']).stem+'-'+sha[:8];source=out/(stem+'.dds');source.write_bytes(data)
        decoded=pixels(data,w,h);bgra=out/(stem+'.bgra');bgra.write_bytes(bytes(v for red,g,b,a in decoded for v in (b,g,red,a)))
        item=dict(name=r['name'],source_DDS=str(source),source_sha256=sha,width=w,height=h,format='BC3' if data[84:88]==b'DXT5' else 'BGRA8',source_BGRA=str(bgra),output_stem=stem,primary_records=[q['record'] for q in master if q['name']==r['name'] and q['offset']==r['offset'] and q['pack']==r['pack']],labels=[])
        if r['name'].startswith('title_credit'):
            alpha=np.asarray([p[3] for p in decoded],dtype=np.uint8).reshape(h,w)
            ys=np.flatnonzero((alpha>=64).sum(axis=1)>3)
            item['bands']=[[int(g[0]),int(g[-1])+1] for g in np.split(ys,np.flatnonzero(np.diff(ys)>6)+1)]
            bands[r['name']]=item['bands']
        rows.append(item)
    write(out/'sources.json',dict(assets=rows));write(out/'credit-bands.json',bands)
    print(json.dumps(dict(assets=len(rows),credits={k:len(v) for k,v in bands.items()})))
