"""Read-only ready-path report; does not copy, install or select a bundle."""
import argparse,json
from pathlib import Path
from stage_ui_images import sha_file
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--proof',type=Path,required=True);a=p.parse_args();d=json.loads(a.proof.read_text(encoding='utf8'))
 if d['status']!='PASS' or d['authority_variants']!=86 or d['missing'] or d['unknown_variants']:raise ValueError('Complete final proof required')
 if sha_file(Path(d['ELF']))!=d['ELFsha256']:raise ValueError('ELF changed')
 files=[]
 for name,path in d['prior_v6']['backing_files'].items():
  h=d['prior_v6']['output_hashes'][name]
  if sha_file(Path(path))!=h:raise ValueError('Candidate changed '+name)
  files.append(dict(filename=name,candidate=path,sha256=h))
 index=d['prior_v6']['index'];h=d['prior_v6']['output_hashes']['pack.idx']
 if sha_file(Path(index))!=h:raise ValueError('Index changed')
 files.append(dict(filename='pack.idx',candidate=index,sha256=h))
 print(json.dumps(dict(status='PASS_READ_ONLY_PATHS',ELF=d['ELF'],ELFsha256=d['ELFsha256'],files=files,installed=False,in_game_verified=False),ensure_ascii=False,indent=2))
if __name__=='__main__':main()
