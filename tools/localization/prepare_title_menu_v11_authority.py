"""Verify exact sprite boundaries and export authority for title-only staging."""
import hashlib
import json
from pathlib import Path
import struct

ROOT=Path(__file__).resolve().parents[2]
B=ROOT/'localization/image_assets/title-menu-v11'
SRC=ROOT/'analysis/localization/title-menu-v11-audit/nested'
sha=lambda b:hashlib.sha256(b).hexdigest()
rows=[]
for name,expected,rects,protected,labels in (
 ('title_text.dds','53e4815f0c924c78a3baaf14bd2ea7f6b869a8b103c318bd7ea178aa0b37d44e',
  [[0,0,256,48],[0,48,256,48],[0,96,256,48]],[[0,144,256,112]],['START 버튼을 누르세요','게임 시작','불러오기']),
 ('title_logo02.dds','4efd619c1bea941f5f8a51be5e39761845810fbad1aa9e7a13faa19fc8de9973',
  [[0,0,384,64]],[],['은하를 잇는 노랫소리'])):
 old=(SRC/name).read_bytes();path=B/'candidate'/name;new=path.read_bytes()
 assert sha(old)==expected and old[:128]==new[:128] and len(old)==len(new)
 h,w=struct.unpack_from('<2I',old,12)
 maskpath=path.with_suffix('.mask');mask=maskpath.read_bytes()
 assert len(mask)==w*h and set(mask)<={0,1}
 changed=0
 for i,m in enumerate(mask):
  x,y=i%w,i//w
  assert bool(m)==any(rx<=x<rx+rw and ry<=y<ry+rh for rx,ry,rw,rh in rects)
  a=old[128+i*4:132+i*4];b=new[128+i*4:132+i*4]
  if not m:assert a==b,'Outside sprite changed'
  changed+=a!=b
 assert changed>0
 for x,y,rw,rh in rects:
  assert any(new[128+4*(yy*w+xx)+3]>128 for yy in range(y,y+rh) for xx in range(x,x+rw))
 rows.append(dict(name=name,path='data/menu/title/'+name,source_DDS=str(SRC/name),
                  source_sha256=expected,replacement=str(path),replacement_sha256=sha(new),
                  allowed_pixel_mask=str(maskpath),allowed_mask_sha256=sha(mask),
                  ROI_xywh=rects,protected_xywh=protected,labels=labels,changed_pixels=changed,
                  outside_mask_changed_pixels=0,header_changed_bytes=0))
doc=dict(status='OFFLINE_CANDIDATE',entries=rows,glyph_source=str(ROOT/'analysis/localization/title-menu-v11-audit/generated-korean-sheet.png'),
         installed=False,in_game_verified=False,protected='Background, VF silhouette, brand logo, glow effects, ARK layouts and copyright remain unchanged')
(B/'candidate/authority.json').write_bytes((json.dumps(doc,ensure_ascii=False,indent=2)+'\n').encode('utf8'))
print('PASS exact headers/ROI/protected pixels; 2 DDS / 4 text phrases')
