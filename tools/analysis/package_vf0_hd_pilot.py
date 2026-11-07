"""Package a generated VF-0A pilot atlas as BC1 DDS; never install it."""
import hashlib
import io
import json
import math
import struct
from pathlib import Path

import numpy as np
from PIL import Image,ImageDraw

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'experiments/vf0-hd-v1'
SOURCE=ROOT/'analysis/vf0-hd-pilot/source'


def sha(data):return hashlib.sha256(data).hexdigest()


def dds(im,template):
    size=im.width;assert im.height==size and size&(size-1)==0
    levels=int(math.log2(size))+1;header=bytearray(template[:128])
    struct.pack_into('<II',header,12,size,size)
    struct.pack_into('<I',header,20,max(1,(size+3)//4)**2*8)
    struct.pack_into('<I',header,28,levels)
    blocks=[];sizes=[]
    for level in range(levels):
        n=max(1,size>>level);mip=im.resize((n,n),Image.Resampling.LANCZOS).convert('RGB')
        f=io.BytesIO();mip.save(f,format='DDS',pixel_format='DXT1');raw=f.getvalue()
        assert raw[84:88]==b'DXT1'
        expected=max(1,(n+3)//4)**2*8;assert len(raw)==128+expected
        blocks.append(raw[128:]);sizes.append(expected)
    return bytes(header)+b''.join(blocks),sizes


def main():
    original=(SOURCE/'mb_00f.dds').read_bytes();model=(SOURCE/'mb_00f.mdl').read_bytes()
    assert b'mb_00f.dds' in model
    source=Image.open(SOURCE/'mb_00f.dds').convert('RGB')
    master=Image.open(OUT/'mb_00f-generated-master.png').convert('RGB')
    # Preserve native generated pixels. DDS hardware dimensions are explicit
    # resampling outputs, never misreported as native 2048 image generation.
    products=[]
    for size,name in [(2048,'mb_00f-hd-2048.dds'),(512,'mb_00f-detail-control-512.dds')]:
        data,mips=dds(master.resize((size,size),Image.Resampling.LANCZOS),original)
        target=OUT/name;target.write_bytes(data)
        decoded=Image.open(target);assert decoded.size==(size,size)
        assert struct.unpack_from('<I',data,28)[0]==len(mips)
        products.append(dict(path=str(target.relative_to(ROOT)),width=size,height=size,mips=len(mips),bytes=len(data),sha256=sha(data),compression='BC1/DXT1',mip_payload_bytes=mips))
    small=master.resize(source.size,Image.Resampling.LANCZOS)
    a=np.asarray(source).max(axis=2)>12;b=np.asarray(small).max(axis=2)>12
    intersection=int((a&b).sum());union=int((a|b).sum())
    # This is an image-space warning metric, NOT proof of UV compatibility.
    iou=intersection/union
    review=Image.new('RGB',(1056,560),(22,25,29));draw=ImageDraw.Draw(review)
    draw.text((12,10),'Original game atlas: 512 x 512',fill='white')
    draw.text((540,10),f'Generated candidate: {master.width} x {master.height} (comparison scaled)',fill='white')
    review.paste(source,(12,36));review.paste(small,(540,36));review.save(OUT/'comparison.png')
    elf=(ROOT/'analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
    assert elf[0x917200:].split(b'\0',1)[0]==b'VF-0A'
    assert elf[0x917230:].split(b'\0',1)[0]==b'MB_00F'
    doc=dict(status='EXPERIMENTAL_NOT_INSTALLED',craft='VF-0A',machine_row=1,
        source_name='mb_00f.dds',source_sha256=sha(original),source_model_sha256=sha(model),
        machine_name_literal_offset='0x917200',model_key_literal_offset='0x917230',
        source_size=list(source.size),source_mips=10,generated_native_size=list(master.size),
        master_sha256=sha((OUT/'mb_00f-generated-master.png').read_bytes()),products=products,
        image_space_foreground_IOU=iou,UV_correspondence_verified=False,
        note='AI restoration can shift UV island boundaries. Image-space IOU is diagnostic only. Do not install until mesh/seam review and actual guest loading checks pass.',
        source_normal_and_decal_maps_unchanged=True,active_packs_modified=False,
        live_game_verified=False,renderer_texture_override_available=False,
        vram_BC1_estimate_ratio=products[0]['bytes']/len(original))
    (OUT/'manifest.json').write_text(json.dumps(doc,indent=2),encoding='utf8')
    print(json.dumps(dict(products=len(products),generated_size=list(master.size),foreground_IOU=round(iou,4),installed=False)))


if __name__=='__main__':main()
