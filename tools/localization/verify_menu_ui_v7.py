"""Read-only v7 full readback. No installation, selection, or game control."""
import argparse,json,hashlib
from pathlib import Path
from verify_data_ui_v6 import Reader,verify,ROOT,V6,load,sha
from stage_nested_ui import fsts_entries,decoded
from stage_ui_images import validate_dds,sha_file
B=ROOT/'localization/image_assets/menu-ui-v7'
def check_dds(original,replacement,expected):
 validate_dds(original);validate_dds(replacement)
 if sha(replacement)!=expected or len(original)!=len(replacement) or original[:128]!=replacement[:128]:raise ValueError('DDS authority/header mismatch')
def run(index,sources,artifacts,check_original_epoch=True):
 norm=load(artifacts/'normalized-authorities.json');plan=load(artifacts/'source-plan.json')
 authorities={(a['name'],a['source_sha256']):a for a in norm['authorities']}
 if len(authorities)!=86 or plan['unknown_primary_variants'] or plan['nested']['unknown_variants']:raise ValueError('Require exact 86 authorities / unknown0')
 r=Reader(index,sources);primary=[];seen=set()
 for p in plan['pack_plans']:
  for c in p['changes']:
   key=(c['name'],c['source_tuple']['raw_sha256']);a=authorities[key]
   new,proof=r.primary(c['master_field']-12,c['pack'],a['replacement_sha256'],c['embedded_field'])
   from stage_menu_ui_bundle import payload
   old,_=payload(Path(plan['source'])/c['pack'],c['source_tuple'])
   if sha(old)!=key[1]:raise ValueError('Original appended-prefix DDS changed')
   check_dds(old,new,a['replacement_sha256']);seen.add(key);primary.append(dict(name=c['name'],**proof))
 if seen!=set(authorities):raise ValueError('Missing v7 primary authority')
 nested=[]
 _,archives=__import__('stage_nested_ui').pidx(index)
 for c in plan['nested']['matches']:
  found=[a for a in archives if a['path']==c['archive'] and a['pack']==c['pack']]
  if len(found)!=1:raise ValueError('Nested route missing/ambiguous')
  ar,where=r.archive(found[0]);entry=fsts_entries(ar)[c['entry_index']]
  if entry['path']!=c['path']:raise ValueError('Nested path changed')
  raw,_=decoded(ar,entry);a=authorities[(Path(c['path']).name,c['raw_sha256'])]
  if sha(raw)!=a['replacement_sha256']:raise ValueError('Nested DDS missing replacement')
  validate_dds(raw);nested.append(dict(path=c['path'],archive=c['archive'],sha256=sha(raw)))
 protected=[];target_names={a['name'] for a in norm['authorities']}
 ledger=load(ROOT/'analysis/localization/region-name-audit/all-current-copy-audit.json')
 for c in ledger['copies']:
  if c['name'] in target_names or c['kind']!='primary':continue
  _,proof=r.primary(c['master_record'],c['pack'],c['raw_sha256'],c['embedded_record']+12)
  protected.append(dict(name=c['name'],**proof))
 prior=verify(index,sources,V6/'staged-primary/ui-images-manifest.json',V6/'nested-staged/nested-ui-manifest.json')
 for name,path in r.used.items():
  prior['backing_files'][name]=path;prior['output_hashes'][name]=sha_file(Path(path))
 unchanged=[]
 for e in plan['source_epoch'] if check_original_epoch else []:
  for key in ('disc','hdd'):
   if sha_file(Path(e[key]))!=e['sha256']:raise ValueError('Active source epoch changed')
  unchanged.append(dict(filename=e['filename'],sha256=e['sha256']))
 admission=load(V6/'pool-static-admission.json');elf=V6/'EBOOT.catalog-pool.elf'
 if sha_file(elf)!=admission['ELFsha256'] or not admission['ELFsha256'].startswith('3311'):raise ValueError('ELF v6 pool changed')
 return dict(status='PASS',static_only=True,in_game_verified=False,authority_variants=86,primary_readbacks=primary,nested_readbacks=nested,protected_English_background_other_primary_readbacks=protected,unknown_variants=0,missing=0,source_epoch_unchanged=unchanged,prior_v6=prior,ELF=str(elf.resolve()),ELFsha256=admission['ELFsha256'],active_modified=False)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--index',type=Path,required=True);p.add_argument('--source-dir',type=Path,action='append',required=True);p.add_argument('--artifacts',type=Path,default=B/'stage-preparation-all');p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 if a.output.exists() or B.resolve() not in a.output.resolve().parents:raise ValueError('Require new proof inside v7')
 result=run(a.index,a.source_dir,a.artifacts);a.output.write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf8');print('PASS v7 86 / prior v6 fully preserved / source unchanged')
if __name__=='__main__':main()
