"""Localize the nested-only aircraft-viewer guide; keep controller artwork."""
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from english_pack import ROOT,digest,write


def main():
    source=ROOT/'analysis/localization/viewer-menu-review/viewer_guide.dds'
    raw=source.read_bytes();original=Image.open(source).convert('RGBA')
    folder=ROOT/'localization/viewer-guide';folder.mkdir(exist_ok=True)
    labels=[((282,34,49,37),'회전','Rotate',14),((388,34,42,37),'축소','Out',14),
            ((499,34,44,37),'확대','In',14),((600,34,44,37),'변형','Mode',14),
            ((690,34,237,37),'카메라·모델 회전 초기화','Reset Camera / Model Rotation',13),
            ((961,28,55,23),'뒤로','Back',14),((961,51,128,23),'메뉴 켜기/끄기','Menu ON/OFF',14)]
    assets={}
    for locale,index in [('ko-KR',1),('en-US',2)]:
        im=original.copy();layer=Image.new('RGBA',im.size);d=ImageDraw.Draw(layer)
        allowed=np.zeros((128,1280),bool)
        for row in labels:
            (x,y,w,h)=row[0];allowed[y:y+h,x:x+w]=True;allowed[70:120,x:x+w]=True
            ImageDraw.Draw(im).rectangle((x,y,x+w-1,y+h-1),fill=(0,0,0,0))
            ImageDraw.Draw(im).rectangle((x,70,x+w-1,119),fill=(0,0,0,0))
            font=ImageFont.truetype('C:/Windows/Fonts/malgunbd.ttf' if locale=='ko-KR' else 'C:/Windows/Fonts/arialbd.ttf',row[3])
            assert d.textbbox((0,0),row[index],font=font)[2]<=w
            d.text((x+w/2,y+h/2),row[index],font=font,fill=(190,203,180,255),anchor='mm',stroke_width=0)
        im.alpha_composite(layer)
        # Original guide also reflects its labels below the controller icons.
        reflected=layer.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        a=np.asarray(reflected).copy();a[:,:,3]=(a[:,:,3].astype(float)*0.18).astype('uint8')
        reflection=Image.fromarray(a);im.alpha_composite(reflection.crop((0,0,1280,112)),(0,16))
        # Reflection falls inside the broad per-label guide rectangles.
        after=np.asarray(im);before=np.asarray(original)
        assert np.array_equal(after[~allowed],before[~allowed])
        assert len(raw)==128+1280*128*4
        target=folder/(locale+'.dds');payload=raw[:128]+after[:,:,[2,1,0,3]].tobytes();target.write_bytes(payload)
        bg=Image.new('RGBA',im.size,(75,78,80,255));bg.alpha_composite(im);bg.save(folder/(locale+'-review.png'))
        assets[locale]=dict(name='viewer_guide.dds',source_sha256=digest(raw),replacement=str(target),replacement_sha256=digest(payload),labels=[r[index] for r in labels],nested_path='data/menu/viewer/viewer_guide.dds',controller_art_preserved=True,game_render_verified=False)
    write(folder/'manifest.json',assets)
    print('Viewer guide KO/EN candidates prepared')


if __name__=='__main__':main()
