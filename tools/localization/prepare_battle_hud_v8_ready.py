"""Materialize verified offline v8 candidates inside bundle; never installs/selects."""
import argparse,json,shutil
from pathlib import Path
from stage_battle_hud_v8 import B,load,dump
from stage_ui_images import sha_file
def local_copy(source,expected,destination):
 source=Path(source).resolve()
 if sha_file(source)!=expected:raise ValueError('Readback candidate changed')
 if B.resolve() in source.parents:return source
 destination.parent.mkdir(parents=True,exist_ok=True)
 if not destination.exists():
  with source.open('rb') as f,destination.open('xb') as g:shutil.copyfileobj(f,g,8*1024*1024)
 if destination.stat().st_nlink!=1 or sha_file(destination)!=expected:raise ValueError('Independent bundle copy mismatch')
 return destination.resolve()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--proof',type=Path,required=True);a=p.parse_args();proof=load(a.proof)
 if proof['status']!='PASS' or proof['unknown_variants']:raise ValueError('Complete readback required')
 old=proof['v7_preservation'];files=[];epoch={e['filename']:e for e in proof['source_epoch_unchanged']}
 for name,path in old['prior_v6']['backing_files'].items():
  h=old['prior_v6']['output_hashes'][name];candidate=local_copy(path,h,B/'preserved-baseline'/name)
  files.append(dict(filename=name,candidate=candidate.relative_to(B.resolve()).as_posix(),sha256=h,source_sha256=epoch[name]['sha256']))
 h=old['prior_v6']['output_hashes']['pack.idx'];candidate=local_copy(old['prior_v6']['index'],h,B/'preserved-baseline/pack.idx')
 files.append(dict(filename='pack.idx',candidate=candidate.relative_to(B.resolve()).as_posix(),sha256=h,source_sha256=epoch['pack.idx']['sha256']))
 elf=local_copy(old['ELF'],old['ELFsha256'],B/'EBOOT.catalog-pool.elf')
 result=dict(status='OFFLINE_REVIEWABLE_CANDIDATE',files=files,ELF=elf.relative_to(B.resolve()).as_posix(),ELFsha256=old['ELFsha256'],active_modified=False,installed=False,in_game_verified=False,source_version='ae496b6cd76e2211ddc04ddff2630b00900e495a56b741e41ab79219f2710071')
 dump(B/'ready-paths.json',result);print('PASS bundle-local independent copies; install/version/selector remain root-owned')
if __name__=='__main__':main()
