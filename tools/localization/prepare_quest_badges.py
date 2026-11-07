"""Remove residual Japanese under Korean quest labels; preserve capsule edges."""
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from english_pack import ROOT,digest,write


def main():
    folder=ROOT/'localization/quest-badges';folder.mkdir(exist_ok=True)
    registry=ROOT/'localization/image-registry/assets/gallery_menu01-2ec7a077bc40'
    raw=(registry/'ko-KR.dds').read_bytes()
    assert digest(raw)=='5d05923c9c989cb4fcbc309c5a6c424d533fd12a87255aad85763b305eb6cd2d'
    original=Image.open(registry/'ja-JP.dds').convert('RGBA')
    im=Image.open(registry/'ko-KR.dds').convert('RGBA');before=np.array(im);allowed=np.zeros(before.shape[:2],bool)
    rows=[('토벌',(85,28,52,18)),('납품',(85,48,52,18)),('현상범',(85,68,52,18)),
          ('호위',(85,88,52,18)),('기타',(85,108,52,18)),('완료',(140,75,74,18))]
    for text,(x,y,w,h) in rows:
        allowed[y:y+h,x:x+w]=True
        # Restore the original rim, then reconstruct the face from unlettered
        # side donors on each scanline. Remove all old Japanese and KO ink.
        tile=original.crop((x,y,x+w,y+h))
        for yy in range(h):
            a=np.array(tile.getpixel((2,yy)),float);b=np.array(tile.getpixel((w-3,yy)),float)
            for xx in range(3,w-3):tile.putpixel((xx,yy),tuple(np.rint(a+(b-a)*(xx-2)/(w-5)).astype(int)))
        big=tile.resize((w*4,h*4),Image.Resampling.NEAREST)
        font=ImageFont.truetype('C:/Windows/Fonts/malgunbd.ttf',48)
        ImageDraw.Draw(big).text((w*2,h*2-1),text,font=font,fill=(255,255,255,255),anchor='mm')
        tile=big.resize((w,h),Image.Resampling.LANCZOS)
        im.paste(tile,(x,y))
    after=np.array(im);assert np.array_equal(before[~allowed],after[~allowed])
    assert len(raw)==128+288*128*4
    new=raw[:128]+after[:,:,[2,1,0,3]].tobytes();target=folder/'gallery_menu01.dds';target.write_bytes(new)
    bg=Image.new('RGBA',im.size,(30,35,40,255));bg.alpha_composite(im);bg.resize((1152,512)).save(folder/'review.png')
    write(folder/'manifest.json',dict(name=target.name,source_sha256=digest(raw),replacement=str(target),replacement_sha256=digest(new),labels=rows,outside_ROIs_unchanged=True,game_render_verified=False))
    print('Six quest badges prepared; old JP ink removed')


if __name__=='__main__':main()
