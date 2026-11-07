"""Select validated title-menu-v11 for the next guarded normal launch."""
import json
import os
from pathlib import Path

from install_korean_ui import bundle_targets, preflight
from stage_ui_images import sha, sha_file
from stage_title_menu_v11 import check_epoch

ROOT=Path(__file__).resolve().parents[2]
B=ROOT/'localization/image_assets/title-menu-v11'
load=lambda p:json.loads(p.read_text(encoding='utf8'))
proof=load(B/'final-pack-readback.json');paths=load(B/'ready-paths.json')
assert proof['status']=='PASS' and len(proof['nested_title_readbacks'])==2 and proof['unknown_variants']==0
assert proof['all_old_pack_bytes_preserved_except_secondary_mirror_fields']
assert proof['archive_prefix_preserved_except_two_entry_fields']
assert load(B/'mask-negative-fixture.json')['status']=='PASS'
check_epoch(paths['source_epoch'])
files=[]
for name,p in paths['files'].items():
    path=Path(p).resolve();assert path.is_relative_to(B.resolve())
    digest=sha_file(path);assert digest==proof['staged_hashes'][name]
    files.append(dict(filename=name,candidate=path.relative_to(B.resolve()).as_posix(),sha256=digest,source_sha256=paths['source_epoch'][name]))
elf=Path(paths['elf']);assert elf.resolve().is_relative_to(B.resolve())
assert sha_file(elf)==proof['ELFsha256']
elfdoc=dict(path=elf.relative_to(B).as_posix(),sha256=proof['ELFsha256'])
version=sha(json.dumps(dict(files=files,elf=elfdoc),sort_keys=True).encode())
manifests=['final-pack-readback.json','source-plan.json','candidate/authority.json','mask-negative-fixture.json']
doc=dict(status='verified title-menu candidate; awaiting guarded installation',version=version,files=files,elf=elfdoc,
         manifests=[dict(path=p,sha256=sha_file(B/p)) for p in manifests],
         previous_bundle=dict(name='menu-title-v10',version=load(ROOT/'localization/image_assets/menu-title-v10/ready.json')['version']),
         active_resources_modified=False,in_game_verified=False)
(B/'ready.json').write_bytes((json.dumps(doc,ensure_ascii=False,indent=2)+'\n').encode('utf8'))
pending=preflight(bundle_targets(B))
(B/'installer-preflight.json').write_text(json.dumps(dict(status='PASS',pending_files=len(pending),version=version),indent=2)+'\n',encoding='utf8')
pointer=ROOT/'localization/image_assets/active-ui.json'
before=B/'active-ui.before-v11.json'
if not before.exists():before.write_bytes(pointer.read_bytes())
assert load(pointer)['bundle'] in ('menu-title-v10','title-menu-v11')
tmp=pointer.with_suffix('.title-menu.tmp');tmp.write_text(json.dumps(dict(bundle=B.name))+'\n',encoding='utf8');os.replace(tmp,pointer)
print(json.dumps(dict(ready=True,pending_files=len(pending),version=version)))
