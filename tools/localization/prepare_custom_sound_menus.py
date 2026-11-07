"""Menu-only localization. Music titles and credits remain original Japanese."""
import json
from pathlib import Path
import struct
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from english_pack import ROOT, digest, write
from build_quest_title_bc3 import encode


def main():
    folder=ROOT/'localization/custom-sound';folder.mkdir(exist_ok=True)
    assets=[]
    for name,rows in [
        ('cst_bg.dds',[(92,56,320,44,'커스텀 사운드',34)]),
        ('hanger_guide.dds',[
            (0,224,576,28,'↑↓ 선택   ○ 결정   × 뒤로   △ 미리듣기   □ 초기화',17),
            (0,252,576,28,'L1/R1 목록 선택   START 목록 확정   SELECT 전체 초기화',17)])]:
        original=ROOT/'analysis/localization/custom-sound-review'/name
        raw=original.read_bytes();im=Image.open(original).convert('RGBA');before=np.array(im)
        changed=np.zeros(before.shape[:2],bool)
        for x,y,w,h,text,size in rows:
            changed[y:y+h,x:x+w]=True
            if name=='cst_bg.dds':
                # Same per-scanline interpolation as the reviewed English title.
                for yy in range(y,y+h):
                    a=np.array(im.getpixel((x-1,yy)));b=np.array(im.getpixel((x+w,yy)))
                    for xx in range(x,x+w):im.putpixel((xx,yy),tuple(np.rint(a+(b-a)*(xx-x+1)/(w+1)).astype(int)))
                color=(255,255,255,255)
            else:
                ImageDraw.Draw(im).rectangle((x,y,x+w-1,y+h-1),fill=(0,0,0,0));color=(60,255,180,255)
            font=ImageFont.truetype('C:/Windows/Fonts/malgunbd.ttf',size)
            assert ImageDraw.Draw(im).textbbox((0,0),text,font=font)[2]<=w-4
            ImageDraw.Draw(im).text((x+w/2,y+h/2),text,font=font,fill=color,anchor='mm')
        p=np.array(im);assert np.array_equal(p[~changed],before[~changed])
        if raw[84:88]==b'DXT5':
            new=bytearray(raw)
            for by in range(im.height//4):
                for bx in range(im.width//4):
                    if not changed[by*4:by*4+4,bx*4:bx*4+4].any():continue
                    assert changed[by*4:by*4+4,bx*4:bx*4+4].all()
                    at=128+(by*(im.width//4)+bx)*16
                    new[at:at+16]=encode([tuple(int(v) for v in c) for c in p[by*4:by*4+4,bx*4:bx*4+4].reshape(16,4)])
            new=bytes(new)
        else:
            assert len(raw)==128+im.width*im.height*4
            new=raw[:128]+p[:,:,[2,1,0,3]].tobytes()
        target=folder/('ko-'+name);target.write_bytes(new)
        bg=Image.new('RGBA',im.size,(15,25,35,255));bg.alpha_composite(im);bg.convert('RGB').save(target.with_suffix('.png'))
        assets.append(dict(name=name,source_sha256=digest(raw),replacement=str(target),replacement_sha256=digest(new),rows=rows,outside_ROIs_unchanged=True))
    write(folder/'menus.json',dict(scope='menu only; original song titles and credits retained in both locales',assets=assets,game_render_verified=False))
    # Undo the previously translated English music catalog, checking each slot
    # against its reviewed English text before restoring original bytes.
    path=ROOT/'localization/en-US/build/EBOOT.full-english.elf';base=path.read_bytes();out=bytearray(base)
    source=(ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
    proofpath=ROOT/'localization/en-US/review-v10/supplemental-elf-review.json'
    proof=json.loads(proofpath.read_text(encoding='utf8'));retained=[]
    for row in proof['entries']:
        off=int(row['offset'],16)
        if not 0x925918<=off<0x928cc8:continue
        cap=row['capacity'];old=base[off:off+cap]
        if old!=source[off:off+cap]:
            assert old.split(b'\0',1)[0].decode()==row['target']
            out[off:off+cap]=source[off:off+cap]
        retained.append(dict(offset=row['offset'],source_ja=row['source'],policy='original music title/credit retained'))
        row['target']=row['source'];row['target_nul_bytes']=len(row['source'].encode())+1
        row['review']='Original music text retained by user instruction'
    path.write_bytes(out)
    proof.update(output_sha256=digest(out),music_original_retained=len(retained),translated=len(proof['entries'])-len(retained));write(proofpath,proof)
    manifest=path.with_name('full-elf-manifest.json');doc=json.loads(manifest.read_text(encoding='utf8'));doc.update(output_sha256=digest(out),music_original_retained=len(retained));write(manifest,doc)
    write(folder/'music-policy.json',dict(policy='Preserve original song titles and credits in ko-KR and en-US; translate menus only',original_music_literals=retained,english_working_elf_sha256=digest(out)))
    print(json.dumps(dict(menu_images=len(assets),restored_original_music_literals=len(retained))))


if __name__=='__main__':main()
