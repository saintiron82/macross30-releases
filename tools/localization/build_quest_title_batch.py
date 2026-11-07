"""DDS codec batch for reviewed GDI-rendered quest title RGBA; no image editing."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from build_quest_title_bc3 import encode, decode, png
from stage_ui_images import validate_dds


def sha(b): return hashlib.sha256(b).hexdigest()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--plan',type=Path,required=True)
    ap.add_argument('--rgba',type=Path,default=Path('localization/image_assets/caution-ui-v4/quest-title-rgba'))
    ap.add_argument('--output',type=Path,default=Path('localization/image_assets/caution-ui-v4/replacements'))
    ap.add_argument('--replace',action='store_true')
    a=ap.parse_args();doc=json.loads(a.plan.read_text(encoding='utf8'));reports=[]
    assert len(doc['rows'])==37
    if doc.get('ready') is False or any(r.get('blocked_for_glyph_regeneration') for r in doc['rows']):
        raise ValueError('Quest title plan contains blocked glyph/mapping corrections')
    a.output.mkdir(parents=True,exist_ok=True)
    for row in doc['rows']:
        name=row['source_filename'];assert name not in ('quest_title_01_01.dds','quest_title_01_02.dds') and not row.get('keep_original_English')
        target=a.output/name
        if target.exists() and not a.replace: raise FileExistsError(target)
        old=Path(row['source_dds']).read_bytes();assert sha(old)==row['source_sha256'];validate_dds(old)
        w,h=row['width'],row['height'];raw=(a.rgba/(Path(name).stem+'.rgba')).read_bytes();assert len(raw)==w*h*4
        x,y,rw,rh=row['bbox_xywh'];new=bytearray(old);allowed=set()
        if row['format']=='BGRA32':
            for yy in range(y,y+rh):
                for xx in range(x,x+rw):
                    p=(yy*w+xx)*4;q=128+p;r,g,b,alpha=raw[p:p+4]
                    new[q:q+4]=bytes((b,g,r,alpha));allowed.update(range(q,q+4))
            pixels=[(new[i+2],new[i+1],new[i],new[i+3]) for i in range(128,len(new),4)]
        elif row['format']=='BC3':
            l,t,r,b=row['blockaligned_roi_xyxy'];bpr=(w+3)//4
            for by in range(t//4,b//4):
                for bx in range(l//4,r//4):
                    values=[tuple(raw[((by*4+dy)*w+bx*4+dx)*4:((by*4+dy)*w+bx*4+dx)*4+4]) for dy in range(4) for dx in range(4)]
                    q=128+(by*bpr+bx)*16;new[q:q+16]=encode(values);allowed.update(range(q,q+16))
            pixels=[(0,0,0,0)]*(w*h)
            for by in range((h+3)//4):
                for bx in range(bpr):
                    q=128+(by*bpr+bx)*16
                    for i,v in enumerate(decode(new[q:q+16])):
                        xx,yy=bx*4+i%4,by*4+i//4
                        if xx<w and yy<h:pixels[yy*w+xx]=v
        else: raise ValueError('Unsupported original format')
        assert len(old)==len(new) and old[:128]==new[:128]
        assert all(before==after or i in allowed for i,(before,after) in enumerate(zip(old,new)))
        validate_dds(new);target.write_bytes(new);png(target.with_suffix('.png'),pixels,w,h)
        reports.append(dict(source_filename=name,JP=row['JP'],KO=row['KO'],original_sha256=sha(old),sha256=sha(new),format=row['format'],bbox_xywh=row['bbox_xywh'],blockaligned_roi_xyxy=row['blockaligned_roi_xyxy'],header_changed_bytes=0,outside_allowed_changed_bytes=0,provenance=row['provenance'],in_game_verified=False))
    (a.output/'quest-title-batch-manifest.json').write_text(json.dumps(dict(count=len(reports),textures=reports,preserved=['quest_title_01_01.dds','quest_title_01_02.dds','quest_title_03_02.dds','quest_title_06_02.dds']),ensure_ascii=False,indent=2),encoding='utf8')
    print(json.dumps(dict(completed=len(reports))))


if __name__=='__main__':main()
