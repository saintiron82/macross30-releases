"""DDS format conversion only: encode GDI-produced pixels in approved BC3 blocks."""
import hashlib
import json
from pathlib import Path
import struct
from build_quest_title_bc3 import decode, encode, png

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'localization/image_assets/menu-ui-v7/replacements'
manifest = json.loads((OUT/'options9-composition-manifest.json').read_text(encoding='utf-8-sig'))
proofs = []
for name, rois in [('opt_logo00', [(4,8,188,56)]), ('opt_brighttext', [(0,0,512,48)])]:
    original = (ROOT/'analysis/assets/data'/f'{name}.dds').read_bytes()
    h,w = struct.unpack_from('<II', original, 12)
    assert original[84:88] == b'DXT5' and len(original) == 128+w*h
    bgra = (OUT/f'{name}.composited.bgra').read_bytes()
    assert len(bgra) == w*h*4
    result = bytearray(original)
    allowed = set()
    for left,top,right,bottom in rois:
        assert all(v%4 == 0 for v in (left,top,right,bottom))
        for by in range(top//4,bottom//4):
            for bx in range(left//4,right//4):
                at = 128+16*(by*(w//4)+bx)
                pixels = []
                for dy in range(4):
                    for dx in range(4):
                        i = 4*((by*4+dy)*w+bx*4+dx)
                        b,g,r,a = bgra[i:i+4]
                        pixels.append((r,g,b,a))
                result[at:at+16] = encode(pixels)
                allowed.update(range(at,at+16))
    assert original[:128] == result[:128]
    assert all(a==b or i in allowed for i,(a,b) in enumerate(zip(original,result)))
    # The OPTION text begins at source x188. Its entire BC3 block columns are exact.
    if name == 'opt_logo00':
        for by in range(h//4):
            for bx in range(47,w//4):
                at=128+16*(by*(w//4)+bx)
                assert original[at:at+16] == result[at:at+16]
    (OUT/f'{name}.dds').write_bytes(result)
    actual = [(0,0,0,0)]*(w*h)
    for by in range(h//4):
        for bx in range(w//4):
            at=128+16*(by*(w//4)+bx)
            for k,p in enumerate(decode(result[at:at+16])):
                actual[(by*4+k//4)*w+bx*4+k%4]=p
    png(OUT/f'{name}.png',actual,w,h)
    proofs.append(dict(asset=name+'.dds',source_sha256=hashlib.sha256(original).hexdigest(),
                       output_sha256=hashlib.sha256(result).hexdigest(),
                       allowed_block_ROIs_ltrb=rois,header_changed_bytes=0,nonROI_block_changed_bytes=0,
                       unchanged_English_OPTION=(name=='opt_logo00'),
                       changed_bytes=sum(a!=b for a,b in zip(original,result))))
for name in ['opt_text00','opt_text01']:
    before=(ROOT/'analysis/assets/data'/f'{name}.dds').read_bytes()
    after=(OUT/f'{name}.dds').read_bytes()
    assert before[:128]==after[:128] and len(before)==len(after)
    proofs.append(dict(asset=name+'.dds',source_sha256=hashlib.sha256(before).hexdigest(),
                       output_sha256=hashlib.sha256(after).hexdigest(),header_changed_bytes=0,
                       composition_manifest=('options9' if name=='opt_text00' else 'controller27')+'-composition-manifest.json'))
(OUT/'final-options-four-DDS-proof.json').write_text(json.dumps(dict(status='OFFLINE_DDS_CANDIDATES_NOT_INSTALLED',results=proofs),indent=2),encoding='utf8')
print(json.dumps(proofs))
