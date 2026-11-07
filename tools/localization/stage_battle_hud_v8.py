"""Offline exact-source HUD stage; never writes active data or controls game."""
import argparse,json,struct,subprocess,sys
from pathlib import Path
from stage_menu_ui_bundle import primary,payload,make_plan,choose
from stage_nested_ui import pidx,region,fsts_entries,decoded
import stage_ui_images as ui
ROOT=Path(__file__).resolve().parents[2];B=ROOT/'localization/image_assets/battle-hud-v8';V7=ROOT/'localization/image_assets/menu-ui-v7'
DISC=ROOT/'build/game-ko-full/PS3_GAME/USRDIR/DATA/PACK';HDD=ROOT/'native/state-ko-full/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack'
load=lambda p:json.loads(Path(p).read_text(encoding='utf-8-sig'))
def dump(p,d):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(d,ensure_ascii=False,indent=2),encoding='utf8')
def validate_mask(a):
 old=Path(a['source_DDS']).read_bytes();new=Path(a['replacement']).read_bytes();mask=Path(a['allowed_pixel_mask']).read_bytes()
 ui.validate_dds(old);ui.validate_dds(new)
 if ui.sha(old)!=a['source_sha256'] or ui.sha(mask)!=a['allowed_mask_sha256'] or ui.sha(new)!=a['replacement_sha256']:raise ValueError('HUD source/mask/candidate hash changed')
 h,w=struct.unpack_from('<II',old,12)
 if len(old)!=len(new) or old[:128]!=new[:128] or len(old)!=128+w*h*4 or len(mask)!=w*h:raise ValueError('HUD header/dimensions/mask invalid')
 changed=0
 for n,m in enumerate(mask):
  x,y=n%w,n//w
  if m not in (0,1):raise ValueError('Invalid allowed mask value')
  inside=any(xx<=x<xx+ww and yy<=y<yy+hh for xx,yy,ww,hh in a['ROI_xywh'])
  if m and not inside:raise ValueError('Mask escapes reviewed special-text ROIs')
  delta=old[128+n*4:132+n*4]!=new[128+n*4:132+n*4]
  if delta and not m:raise ValueError('HUD panel/digits/background RGBA changed outside mask')
  changed+=bool(delta)
 x,y,ww,hh=a['protected_digits_xywh']
 for yy in range(y,y+hh):
  q=128+(yy*w+x)*4
  if old[q:q+ww*4]!=new[q:q+ww*4]:raise ValueError('HUD digits changed')
 if not changed:raise ValueError('Empty HUD patch')
 return dict(header_changed_bytes=0,outside_mask_RGBA_changed_pixels=0,protected_digits_RGBA_changed_bytes=0,changed_pixels=changed,mask_sha256=ui.sha(mask))
def prepare(authority,source=DISC,hdd=HDD):
 a=load(authority);a=a.get('authority',a)
 if a['name']!='hud_cockpit_01.dds':raise ValueError('Single exact HUD authority required')
 replacement=Path(a['replacement']).resolve();raw=replacement.read_bytes();ui.validate_dds(raw)
 if ui.sha(raw)!=a['replacement_sha256']:raise ValueError('Replacement changed')
 maskproof=validate_mask(a);a=dict(a,replacement=str(replacement));epoch=[]
 for e in load(V7/'ready.json')['files']:
  name=e['filename'];h=ui.sha_file(source/name)
  if h!=e['sha256'] or ui.sha_file(hdd/name)!=h:raise ValueError('Installed v7 source epoch differs '+name)
  epoch.append(dict(filename=name,sha256=h,disc=str(source/name),hdd=str(hdd/name)))
 _,rows=primary(source/'pack.idx');selected=[r for r in rows if r['name']==a['name']]
 if not selected:raise ValueError('HUD primary missing')
 records=[]
 for r in selected:
  original,_=payload(source/r['pack'],r);choose([a],r['name'],original);records.append(dict(r,raw_sha256=ui.sha(original)))
 _,archives=pidx(source/'pack.idx');nested=[];mapping={}
 for ar in archives:
  pack=source/ar['pack'];header=region(pack,ar['offset'],20)
  if header[:4]!=b'FSTS':raise ValueError('Unknown secondary archive')
  count,table,ns,nsz=struct.unpack_from('<4I',header,4);ui.bounded_range(ns,nsz,ar['size'])
  names=region(pack,ar['offset']+ns,nsz)
  if a['name'].encode() not in names:continue
  archive=region(pack,ar['offset'],ar['size'])
  for entry in fsts_entries(archive):
   if Path(entry['path']).name!=a['name']:continue
   original,_=decoded(archive,entry);choose([a],a['name'],original)
   mapping[entry['path']]=dict(type='DDS',path=entry['path'],original_sha256=a['source_sha256'],replacement=a['replacement'],replacement_sha256=a['replacement_sha256'])
   nested.append(dict(archive=ar,entry_index=entry['index'],path=entry['path']))
 result=dict(status='EXACT_V7_SOURCE_PREPARED',authority=a,candidate_mask_proof=maskproof,source_epoch=epoch,primary=records,nested=nested,metadata_archives=len(archives),unknown_variants=0,active_modified=False)
 dump(B/'source-plan.json',result);dump(B/'nested-extra-map.json',dict(entries=list(mapping.values())))
 return result
def apply(plan):
 out=B/'staged-primary'
 if out.exists():raise ValueError('Require fresh primary output')
 out.mkdir(parents=True);idx=DISC/'pack.idx';stages=[]
 for n,name in enumerate(sorted({r['pack'] for r in plan['primary']})):
  p=make_plan(DISC/name,idx,[plan['authority']],ui.sha_file(DISC/name),ui.sha_file(idx));result=ui.apply(p,out/f'stage-{n:02d}')
  (out/f'stage-{n:02d}'/name).rename(out/name);idx=out/f'stage-{n:02d}'/'pack.idx';stages.append(result)
 idx.rename(out/'pack.idx');dump(out/'primary-bundle-result.json',dict(stages=stages,active_modified=False))
 if plan['nested']:
  subprocess.run([sys.executable,str(ROOT/'tools/localization/stage_nested_ui.py'),'--source-dir',str(out),'--fallback-source-dir',str(DISC),'--only-extra-map','--extra-map',str(B/'nested-extra-map.json'),'--output',str(B/'nested-staged'),'--artifacts',str(B/'nested-artifacts'),'--apply'],check=True)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--authority',type=Path,required=True);p.add_argument('--apply',action='store_true');n=p.parse_args();plan=prepare(n.authority)
 if n.apply:apply(plan)
 print(json.dumps(dict(primary=len(plan['primary']),nested=len(plan['nested']),unknown_variants=0,applied=n.apply)))
if __name__=='__main__':main()
