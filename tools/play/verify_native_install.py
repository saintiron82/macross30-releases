"""Check files written by the native installer against the original ISO manifest."""
import hashlib
import json
from pathlib import Path

root=Path(__file__).resolve().parents[2]
manifest=json.loads((root/'native/vfs/source_manifest.json').read_text(encoding='utf-8'))
original={item['path'].casefold():item for item in manifest['files']}
installed=root/'native/state/hdd0/game/BLJS10184_INSTALL'
results=[]
unmapped=[]
for target in sorted(installed.rglob('*')):
    if not target.is_file(): continue
    relative=target.relative_to(installed).as_posix()
    source=original.get(('PS3_GAME/'+relative).casefold())
    if source is None:
        unmapped.append(relative)
        continue
    digest=hashlib.sha256()
    with target.open('rb') as stream:
        for block in iter(lambda:stream.read(8*1024*1024),b''):digest.update(block)
    actual=digest.hexdigest()
    row=dict(path=relative,size=target.stat().st_size,sha256=actual,
             matches_original=(actual==source['source_sha256'] and target.stat().st_size==source['size']))
    results.append(row)
    print(relative,row['matches_original'],flush=True)
report=dict(files=results,checked=len(results),all_checked_match=bool(results) and all(x['matches_original'] for x in results),
            unmapped_files=unmapped,
            scope='Only installed files present and mapped to original disc entries; not proof of full installation or boot')
(root/'native/evidence/install_verification.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print('Checked',report['checked'],'all match:',report['all_checked_match'])
