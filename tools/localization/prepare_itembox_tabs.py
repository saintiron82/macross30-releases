"""Translate the five Korean item-box labels, preserving all other texels."""
import json
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from english_pack import ROOT,digest,write


def main():
    folder=ROOT/'localization/itembox-tabs';folder.mkdir(exist_ok=True)
    source=ROOT/'analysis/localization/itembox-menu-review/current-hangar_text01.dds'
    raw=source.read_bytes();assert digest(raw)=='93affed80d019989517411cb130b0e678e55aaca5131243a6fac51e53de8667e'
    im=Image.open(source).convert('RGBA');old=np.array(im);allowed=np.zeros(old.shape[:2],bool)
    rows=[('소모품',(0,0,30,16),9),('부품',(35,0,40,16),12),('장비·납품',(0,16,64,16),11),
          ('귀중품',(0,32,64,16),12)]
    # Supersampling keeps the compact 30px category readable at 720p.
    for text,(x,y,w,h),size in rows:
        allowed[y:y+h,x:x+w]=True
        tile=Image.new('RGBA',(w*4,h*4))
        font=ImageFont.truetype('C:/Windows/Fonts/malgunbd.ttf',size*4)
        color=(255,215,65,255) if y==48 else (0,0,0,255)
        d=ImageDraw.Draw(tile);assert d.textbbox((0,0),text,font=font)[2]<=w*4
        d.text((w*2,h*2),text,font=font,fill=color,anchor='mm')
        im.paste(tile.resize((w,h),Image.Resampling.LANCZOS),(x,y))
    new=np.array(im);assert np.array_equal(old[~allowed],new[~allowed])
    assert len(raw)==128+128*64*4
    payload=raw[:128]+new[:,:,[2,1,0,3]].tobytes();target=folder/'hangar_text01.dds';target.write_bytes(payload)
    preview=Image.new('RGBA',im.size,(230,195,70,255));preview.alpha_composite(im)
    preview.convert('RGB').resize((512,256)).save(folder/'review.png')
    write(folder/'manifest.json',dict(name=target.name,source_sha256=digest(raw),replacement=str(target),replacement_sha256=digest(payload),labels=rows,source_ark='item_box00.ark TXOS 95..98',outside_ROIs_unchanged=True,game_render_verified=False))
    print('Korean item-box labels: 4 prepared; existing guide preserved')


if __name__=='__main__':main()
