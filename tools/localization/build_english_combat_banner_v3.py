"""Build continuous English words, then slice for the original ope60 animation."""
import hashlib
import json
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'localization/en-US/images/combat-banner-v3'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'native/analysis/combat-caution-ui/ope60_title.dds'
    old = source.read_bytes()
    assert sha(old) == '2f2d5f14f687a822bf183abef14c4c245083628d90413ca77f54eddf51e314f5'
    ark = (ROOT / 'analysis/ui_assets/ope60.ark').read_bytes()
    rects = [struct.unpack_from('<4i', ark, 160+(6+i)*48+12) for i in range(14)]
    rects += [(254,292,115,95),(378,292,115,95)]
    # Four independent sprite tracks: 103, 103, 104 pixel steps; a 115px
    # source rectangle therefore overlaps its neighbour. Never centre chunks
    # independently or stretch a short suffix to fill a square kanji cell.
    tracks = [[struct.unpack_from('<13i', ark, p+8+j*52) for j in range(4)]
              for p in (0xe9c,0xf74,0x104c,0x1124)]
    positions = [t[1][5] for t in tracks]
    assert positions == [427,530,633,737]
    assert all(t[1][12] == 100 and t[1][11] == 100 for t in tracks)
    font = ImageFont.truetype('C:/Windows/Fonts/arialbd.ttf', 192)
    mask = Image.new('L',(512,512))
    words = [('BATTLE',0),('START',2),('FINISH',4),('MISSION',6),
             ('OBJECTIVE',8),('MET',11),('FAILED',12),('CONTINUE',14)]
    proofs=[]
    for word, first in words:
        count = 3 if word == 'OBJECTIVE' else 1 if word == 'MET' else 2
        width = sum([103,103,104][:count])
        box = font.getbbox(word)
        ink = Image.new('L',(box[2]-box[0],box[3]-box[1]))
        ImageDraw.Draw(ink).text((-box[0],-box[1]),word,font=font,fill=255)
        # Same cap height for every word. Fit long words horizontally only.
        height = 42
        natural = round(ink.width*height/ink.height)
        ink = ink.resize((min(width-18,natural),height),Image.Resampling.LANCZOS)
        line = Image.new('L',(width,95))
        line.paste(ink,((width-ink.width)//2,26))
        offset=0
        for j in range(count):
            i=first+j; x,y,w,h=rects[i]
            step=[103,103,104][j]
            # Unique strips with six transparent texels on either side avoid
            # double-blending the 12px UV overlap at the final 100% scale.
            tile=Image.new('L',(w,h))
            tile.paste(line.crop((offset,0,offset+step,95)),(6,0))
            assert tile.getbbox() and y+tile.getbbox()[3] < 384
            mask.paste(tile,(x,y));offset+=step
        proofs.append(dict(text=word,first_cell=first,cells=count,cap_height=height,width=ink.width))
    raw=bytearray(old)
    alpha=mask.tobytes()
    for y in range(384):
        for x in range(512):raw[128+(y*512+x)*4+3]=alpha[y*512+x]
    new=bytes(raw)
    assert old[:128]==new[:128] and old[128+512*384*4:]==new[128+512*384*4:]
    assert all(a==b for i,(a,b) in enumerate(zip(old,new)) if i<128 or (i-128)%4!=3)
    target=OUT/'ope60_title.dds';target.write_bytes(new)
    Image.merge('RGBA',(Image.new('L',mask.size,255),)*3+(mask,)).save(OUT/'atlas.png')
    phrases=[('BATTLE START',[0,1,2,3]),('BATTLE FINISH',[0,1,4,5]),
             ('MISSION FINISH',[6,7,4,5]),('OBJECTIVE MET',[8,9,10,11]),
             ('MISSION FAILED',[6,7,12,13]),('CONTINUE',[14,15])]
    review=Image.new('RGB',(640,6*110),(10,25,30))
    for row,(name,indices) in enumerate(phrases):
        line=Image.new('RGBA',(440,95))
        for slot,i in enumerate(indices):
            x,y,w,h=rects[i]
            tile=Image.new('RGBA',(w,h),(55,245,245,0));tile.putalpha(mask.crop((x,y,x+w,y+h)))
            line.alpha_composite(tile,(positions[slot]-positions[0],0))
        review.paste(line,(100,row*110+8),line)
    review.save(OUT/'assembled-review.png')
    doc=dict(status='PASS',name='ope60_title.dds',source_sha256=sha(old),replacement=str(target),
             replacement_sha256=sha(new),ark_sha256=sha(ark),words=proofs,
             sprite_positions=positions,animation_tracks=tracks,
             proof='Original ARK settled 100% sprite positions, not a game capture',
             live_review='pending',protected_header_rgb_lower_rows=True)
    (OUT/'manifest.json').write_text(json.dumps(doc,indent=2),encoding='utf8')
    print(json.dumps(dict(output=str(target),sha256=sha(new),review=str(OUT/'assembled-review.png'))))


if __name__=='__main__':main()
