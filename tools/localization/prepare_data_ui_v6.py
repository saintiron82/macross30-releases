from pathlib import Path
import json,hashlib,sys,struct
sys.path.insert(0,'tools')
import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from stage_ui_images import plan,apply,sha_file
b=Path('localization/image_assets/data-ui-v6');o=Path('native/analysis/develop-ui-localization');m=json.loads((o/'original-extraction-manifest.json').read_text(encoding='utf-8'))
names=sorted(p.name for p in (b/'replacements').glob('*.dds'));sources=[r for r in m['sources'] if r['path'] in names]
assert len(names)==len(sources)==7
(b/'source-provenance.json').write_text(json.dumps({'sources':sources,'scope':'Seven exact original primary DATA DDS rows; no active writes'},indent=2),encoding='utf-8')
menu=json.loads((o/'menu01-final-rois.json').read_text(encoding='utf-8'));other=json.loads((b/'other-final-rois.json').read_text(encoding='utf-8'))
rois={'hangar_menu01.dds':[r['edit_roi_xywh'] for r in menu['labels']]}
for r in other['labels']:rois.setdefault(r['asset'],[]).append(r['edit_roi'])
proof=[]
for name in names:
 src=(o/name).read_bytes();dst=(b/'replacements'/name).read_bytes();h,w=struct.unpack_from('<II',src,12);allowed=set()
 for x,y,rw,rh in rois[name]:
  for yy in range(y,y+rh):
   for xx in range(x,x+rw):allowed.add(yy*w+xx)
 protected_src=bytearray();protected_dst=bytearray();outside=0
 for i,(a,c) in enumerate(zip(src,dst)):
  if i<128 or (i-128)//4 not in allowed:
   protected_src.append(a);protected_dst.append(c);outside+=a!=c
 assert src[:128]==dst[:128] and len(src)==len(dst) and outside==0
 proof.append(dict(name=name,source_sha256=hashlib.sha256(src).hexdigest(),output_sha256=hashlib.sha256(dst).hexdigest(),dimensions=[w,h],protected_bytes=len(protected_src),protected_source_sha256=hashlib.sha256(protected_src).hexdigest(),protected_output_sha256=hashlib.sha256(protected_dst).hexdigest(),outside_roi_changed_bytes=outside,header_changed_bytes=0,rois=rois[name]))
(b/'image-composite-validation.json').write_text(json.dumps({'status':'PASS','assets':proof,'labels':45,'protected_header_and_outside_ROI':True,'active_written':False},indent=2),encoding='utf-8')
font=json.loads((b/'font-incremental/nested-font-map.json').read_text(encoding='utf-8'))['entries']
entries=[dict(type='DDS',path='data/menu/hangar/'+r['path'],original_sha256=r['sha256'],replacement=str(b/'replacements'/r['path']),replacement_sha256=sha_file(b/'replacements'/r['path'])) for r in sources]+font
(b/'nested-extra-map.json').write_text(json.dumps({'entries':entries},indent=2),encoding='utf-8')
p=Path('localization/image_assets/gop-ui-v5/staged-primary/data.dat');idx=Path('localization/image_assets/gop-ui-v5/nested-staged/pack.idx')
doc,_,_=plan(p,idx,b/'replacements',{'sources':sources},sha_file(p),sha_file(idx))
(b/'primary-plan.json').write_text(json.dumps(doc,indent=2),encoding='utf-8')
print(json.dumps({'validated':7,'changes':len(doc['changes']),'source_data':doc.get('source_pack_sha256'),'final_length':doc['final_length']},indent=2))
if '--apply' in sys.argv:
 result=apply(doc,b/'staged-primary');print('APPLIED '+str(b/'staged-primary'))
