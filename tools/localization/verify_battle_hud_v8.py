"""Offline v8 HUD + complete v7/v6 preservation readback; no installation."""
import argparse,json,shutil
from pathlib import Path
from verify_menu_ui_v7 import run as verify_v7
from verify_data_ui_v6 import Reader
from stage_nested_ui import fsts_entries,decoded
from stage_ui_images import sha,sha_file,validate_dds
from stage_battle_hud_v8 import B,V7,DISC,HDD,load,dump,ROOT
def verify(index,sources):
 plan=load(B/'source-plan.json');a=plan['authority'];r=Reader(index,sources);primary=[];nested=[]
 for c in plan['primary']:
  # Find exact source embedded record in the applied primary plan.
  changes=[x for s in load(B/'staged-primary/primary-bundle-result.json')['stages'] for x in s['changes'] if x['master_field']-12==c['record']]
  if len(changes)!=1:raise ValueError('HUD primary mirror plan missing')
  raw,p=r.primary(c['record'],c['pack'],a['replacement_sha256'],changes[0]['embedded_field']);validate_dds(raw);primary.append(p)
 for c in plan['nested']:
  rawarchive,where=r.archive(c['archive']);entry=fsts_entries(rawarchive)[c['entry_index']]
  raw,_=decoded(rawarchive,entry)
  if entry['path']!=c['path'] or sha(raw)!=a['replacement_sha256']:raise ValueError('HUD nested mismatch')
  validate_dds(raw);nested.append(dict(archive=where['path'],path=c['path'],sha256=sha(raw)))
 old=verify_v7(index,sources,V7/'stage-preparation-all',check_original_epoch=False)
 for e in plan['source_epoch']:
  for key in ('disc','hdd'):
   if sha_file(Path(e[key]))!=e['sha256']:raise ValueError('Active v7 source changed')
 for name,path in r.used.items():old['prior_v6']['backing_files'][name]=path;old['prior_v6']['output_hashes'][name]=sha_file(Path(path))
 return dict(status='PASS',static_only=True,in_game_verified=False,HUD_primary=primary,HUD_nested=nested,HUD_replacement_sha256=a['replacement_sha256'],v7_preservation=old,unknown_variants=0,source_epoch_unchanged=plan['source_epoch'],active_modified=False)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--index',type=Path,required=True);p.add_argument('--source-dir',type=Path,action='append',required=True);p.add_argument('--output',type=Path,required=True);n=p.parse_args()
 if n.output.exists() or B.resolve() not in n.output.resolve().parents:raise ValueError('New proof inside v8 required')
 result=verify(n.index,n.source_dir);dump(n.output,result);print('PASS HUD allcopies /v7 86/v6 preserved/source unchanged')
if __name__=='__main__':main()
