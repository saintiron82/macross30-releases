"""Read-only runtime-proof inheritance for catalog-data-only candidates.

Never claims the new executable was run, and never publishes/launches anything.
"""
import argparse
import hashlib
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
FLAGS=('reaction_display_verified','reaction_cleared_verified','original_story_priority_verified','test_processes_closed','player_checkpoint_unchanged')
def sha(path):
 h=hashlib.sha256()
 with Path(path).open('rb') as f:
  for b in iter(lambda:f.read(4*1024*1024),b''):h.update(b)
 return h.hexdigest()
def load(path):return json.loads(Path(path).read_text(encoding='utf8'))
def check(value,message):
 if not value:raise ValueError(message)
def path(value):
 p=Path(value)
 return (p if p.is_absolute() else ROOT/p).resolve()
def guarded(p,expected):
 check(p.is_file() and sha(p)==expected,'SHA mismatch: '+str(p))
def catalog_object(p):return Path(p).name.endswith('battle_voice_catalog.cpp.obj')
def build(folder):
 folder=path(folder);plan=load(folder/'plan.json');result=load(folder/'build-result.json')
 check(path(plan['output'])==folder,'Build output path mismatch')
 check(result.get('status')=='compiled_and_linked_candidate_only' and all(result.get(k) is True for k in ('original_outputs_preserved','reused_objects_unchanged','frozen_sources_unchanged')),'Build preservation guards failed')
 check(result['original_outputs']==plan['preserved_original_outputs'],'Build baseline differs from plan')
 exe=path(plan['products']['exe']);product=result['products']['exe']
 check(exe.parent==folder and path(product['path'])==exe,'Build executable identity mismatch');guarded(exe,product['sha256'])
 return plan,result,exe

def evidence(proof_path,seen=()):
 proof_path=path(proof_path);check(proof_path.is_relative_to(ROOT),'Proof outside workspace')
 check(str(proof_path) not in seen and len(seen)<16,'Proof ancestry cycle/depth')
 p=load(proof_path);check(p.get('status')=='GENERIC_BRIDGE_LIVE_VALIDATED' and all(p.get(k) is True for k in FLAGS),'Prior live proof flags failed')
 check(bool(p.get('evidence')),'Empty live evidence')
 result=[]
 for row in p['evidence']:
  file=path(row['path']);check(file.is_relative_to(ROOT),'Evidence outside workspace');guarded(file,row['sha256'])
  result.append(dict(path=str(file),sha256=row['sha256']))
 ancestor=p.get('inherited_original_story_priority_proof')
 if ancestor:
  ap=path(ancestor);check(any(path(r['path'])==ap for r in p['evidence']),'Unpinned ancestor proof')
  _,extra=evidence(ap,seen+(str(proof_path),));result+=extra
 return p,result

def inherit(previous,candidate,live_proof):
 old,old_result,old_exe=build(previous);new,new_result,new_exe=build(candidate)
 check(old_exe!=new_exe,'Require distinct candidates')
 proof,files=evidence(live_proof)
 check(proof['candidate_sha256']==sha(old_exe),'Prior live proof names wrong executable SHA')
 run_path=path(live_proof).parent/'result.json'
 check(any(path(r['path'])==run_path for r in proof['evidence']),'Live run result not pinned')
 run=load(run_path)
 check(run.get('status')=='BOUNDED_RUN_CLOSED' and run.get('game_process_remaining') is False and run.get('player_checkpoint_unchanged') is True,'Prior live run not safely closed')
 command=run['process']['command'];check(bool(command) and path(command[0])==old_exe,'Live run command names wrong old executable')
 if proof.get('tested_ELF'):
  elf=path(proof['tested_ELF']);guarded(elf,proof['tested_ELF_sha256'])
  check(len(command)>1 and path(command[1])==elf,'Live ELF command differs')
 baseline=old['preserved_original_outputs'];check(bool(baseline) and baseline==new['preserved_original_outputs'],'Baseline epoch differs')
 for name,digest in baseline.items():guarded(path(name),digest)
 catalog=ROOT/'native/macross30/battle_voice_catalog.cpp';inc=ROOT/'native/macross30/battle_voice_catalog.inc';bridge=ROOT/'native/macross30/battle_voice_subtitles.cpp'
 check(len(new['compile'])==1 and path(new['compile'][0]['source'])==catalog,'Candidate is not catalog-data-only compile')
 row=new['compile'][0];guarded(catalog,row['source_sha256'])
 check(len([p for p in old['response_objects'] if catalog_object(p)])==1 and len([p for p in new['response_objects'] if catalog_object(p)])==1,'Missing/ambiguous catalog object')
 check(catalog_object(row['output']) and new['response_objects'].count(row['output'])==1,'Compiled catalog output not linked exactly once')
 shared=[p for p in old['response_objects'] if not catalog_object(p)]
 check(shared==[p for p in new['response_objects'] if not catalog_object(p)],'Ordered runtime object list changed')
 check(old['response_libraries']==new['response_libraries'],'Response libraries changed')
 def link_options(plan):
  return [a for a in plan['link'] if a!='@'+plan['response_file'] and not a.lower().startswith(('--intdir=','/out:','/pdb:','/map:','/implib:'))]
 check(link_options(old)==link_options(new),'Link options changed')
 old_catalog=[r for r in old['compile'] if path(r['source'])==catalog]
 check(len(old_catalog)==1,'No unique prior catalog compiler contract')
 check(old_catalog[0]['source_sha256']==sha(catalog),'Prior catalog implementation SHA differs')
 check(new['source_guards'].get(str(inc))==sha(inc),'New generated catalog is not frozen')
 def compiler_options(command):return [a for a in command if not a.lower().startswith(('/fo','/fd'))]
 check(compiler_options(old_catalog[0]['command'])==compiler_options(row['command']),'Catalog compiler contract changed')
 # Source/header/compiler/provenance guards from the actual tested build remain
 # frozen. The generated translation table is the sole allowed changed source.
 for name,digest in old['source_guards'].items():
  if path(name)!=inc:guarded(path(name),digest)
 for name,digest in new['source_guards'].items():guarded(path(name),digest)
 check(old['source_guards'].get(str(catalog))==new['source_guards'].get(str(catalog))==sha(catalog),'Catalog implementation changed')
 check(sha(bridge)==proof['bridge_sha256']==new['source_guards'].get(str(bridge)),'Bridge source differs from prior proof')
 objects=[]
 for name in shared:
  obj=path(name);stamp=[obj.stat().st_size,obj.stat().st_mtime_ns]
  check(old['reused_object_stamps'].get(name)==new['reused_object_stamps'].get(name)==stamp,'Shared runtime object stamp changed: '+name)
  digest=sha(obj);historical=old['source_guards'].get(name)
  if historical is not None:check(historical==digest,'Shared runtime object SHA changed: '+name)
  objects.append(dict(path=str(obj),sha256=digest,stamp=stamp,historical_sha_guard=historical is not None))
 old_dll=old_exe.parent/'SDL2.dll';new_dll=new_exe.parent/'SDL2.dll'
 check(sha(old_dll)==sha(new_dll),'SDL2 dependency changed')
 pins=[]
 for p in (path(previous)/'plan.json',path(previous)/'build-result.json',path(candidate)/'plan.json',path(candidate)/'build-result.json',path(live_proof),run_path,old_exe,new_exe,old_dll,new_dll,bridge,inc):
  pins.append(dict(path=str(p),sha256=sha(p)))
 return dict(status='DATA_ONLY_BRIDGE_RUNTIME_INHERITED',bridge_runtime_inherited_verified=True,candidate_live_game_verified=False,
  candidate=str(new_exe),candidate_sha256=sha(new_exe),ancestor_proof=str(path(live_proof)),ancestor_proof_sha256=sha(path(live_proof)),
  prior_tested_exe=str(old_exe),prior_tested_exe_sha256=sha(old_exe),bridge_sha256=sha(bridge),shared_runtime_objects=objects,
  pins=pins,evidence=files,catalog_generated_sha256=sha(inc),catalog_test_checked=False,
  scope='Existing generic bridge display/clear/story-priority evidence inherited only; this new executable was NOT live tested. Translation accuracy/runtime reachability of new catalog entries remains unverified.',
  object_history_basis='Identical old/new ordered runtime paths and frozen size/mtime; historical SHA guards checked where available; current SHA recorded for every shared object.')
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--previous',required=True,type=Path);p.add_argument('--candidate',required=True,type=Path);p.add_argument('--live-proof',required=True,type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
 result=inherit(a.previous,a.candidate,a.live_proof);text=json.dumps(result,indent=2)+'\n'
 if a.output:
  target=path(a.output);check(target.is_relative_to(ROOT) and target.suffix=='.json','Output must be workspace JSON');check(not target.exists(),'Refuse overwriting existing proof');target.parent.mkdir(parents=True,exist_ok=True);target.write_text(text,encoding='utf8')
 print(text)
if __name__=='__main__':main()
